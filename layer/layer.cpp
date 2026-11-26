#include "panel.hpp"
#include <openxr/openxr_loader_negotiation.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <vector>
#include <mutex>
#include <unistd.h>

namespace edmo {
void log(const char* format, ...) noexcept {
    struct Output {
        FILE* file{};
        Output() {
            const char* path = std::getenv("EDMO_XR_LOG");
            if (path && *path)
                file = !std::strcmp(path, "-") ? stderr : std::fopen(path, "ae");
        }
        ~Output() {
            if (file && file != stderr)
                std::fclose(file);
        }
    };
    static const Output output;
    auto* sink = output.file;
    if (!sink)
        return;
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::fprintf(sink, "edmo_xr pid=%ld %s\n", long(getpid()), message);
    std::fflush(sink);
}

namespace {
struct Instance {
    XrInstance handle{};
    PFN_xrGetInstanceProcAddr get{};
    PFN_xrDestroyInstance destroy{};
    PFN_xrCreateSession createSession{};
    PFN_xrDestroySession destroySession{};
    PFN_xrEndFrame endFrame{};
    PanelApi api{};
    bool panel{}, live{}, curved{};
};
struct Session {
    XrSession handle{};
    Instance* owner{};
    Panel panel{};
    bool firstFrame{true};
    bool firstPanel{true};
    bool graphicsEnabled{};
};
std::list<Instance> instances;
std::list<Session> sessions;
std::mutex registry;

// OpenXR requires callers to synchronize destruction with all use of the handle and its children.
// The short registry lock protects container membership. Downstream calls never hold this lock.
template <class T, class H> T* lookup(std::list<T>& entries, H handle) noexcept {
    std::lock_guard lock(registry);
    for (auto& entry : entries)
        if (entry.handle == handle)
            return &entry;
    return nullptr;
}

template <class T> bool resolve(Instance& inst, const char* name, T& out) noexcept {
    PFN_xrVoidFunction fn{};
    auto result = inst.get(inst.handle, name, &fn);
    out = reinterpret_cast<T>(fn);
    return XR_SUCCEEDED(result) && out;
}

const char* bindingName(XrStructureType type) noexcept {
    switch (type) {
    case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR:
        return "Vulkan";
    case XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR:
        return "GLX";
    case XR_TYPE_GRAPHICS_BINDING_OPENGL_XCB_KHR:
        return "OpenGL_XCB";
    case XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR:
        return "OpenGL_Wayland";
    case XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR:
        return "WGL";
    case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR:
        return "D3D11";
    case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR:
        return "D3D12";
    default:
        return "other";
    }
}

XrResult XRAPI_CALL createSession(XrInstance handle, const XrSessionCreateInfo* info, XrSession* out) {
    auto* inst = lookup(instances, handle);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    std::list<Session> pending;
    try {
        pending.emplace_back();
    } catch (const std::bad_alloc&) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
    const auto result = inst->createSession(handle, info, out);
    if (XR_FAILED(result))
        return result;
    auto& session = pending.front();
    session.handle = *out;
    session.owner = inst;
    log("event=session_created session=%p system=%llu", static_cast<void*>(*out),
        static_cast<unsigned long long>(info->systemId));
    for (auto* node = static_cast<const XrBaseInStructure*>(info->next); node; node = node->next) {
        log("event=session_binding type=%d binding=%s", int(node->type), bindingName(node->type));
        if (node->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            const auto& vk = *reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(node);
            log("event=vulkan_queue family=%u index=%u", vk.queueFamilyIndex, vk.queueIndex);
            if (inst->panel) {
                session.panel.configure(inst->api, handle, info->systemId, *out, vk, inst->live, inst->curved);
                session.graphicsEnabled = true;
            }
        }
    }
    std::lock_guard lock(registry);
    sessions.splice(sessions.end(), pending);
    return result;
}

XrResult XRAPI_CALL endFrame(XrSession handle, const XrFrameEndInfo* info) {
    auto* session = lookup(sessions, handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    auto next = session->owner->endFrame;
    if (session->firstFrame) {
        session->firstFrame = false;
        log("event=end_frame session=%p layers=%u", static_cast<void*>(handle), info ? info->layerCount : 0);
    }
    std::array<const XrCompositionLayerBaseHeader*, 64> layers;
    if (!session->graphicsEnabled || !info || info->type != XR_TYPE_FRAME_END_INFO || !info->layers ||
        !info->layerCount || info->layerCount >= layers.size())
        return next(handle, info);
    const auto last = info->layers + info->layerCount;
    const auto projection = std::find_if(info->layers, last, [](const auto* layer) {
            return layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        });
    if (projection == last)
        return next(handle, info);
    auto* quad = session->panel.frame(info->layerCount);
    if (!quad)
        return next(handle, info);
    std::copy(info->layers, last, layers.begin());
    layers[info->layerCount] = session->panel.composition((*projection)->space);
    XrFrameEndInfo frame = *info;
    frame.layers = layers.data();
    ++frame.layerCount;
    const auto result = next(handle, &frame);
    if (XR_SUCCEEDED(result) && session->firstPanel) {
        session->firstPanel = false;
        log("event=panel_submitted session=%p layers=%u xr=%d", static_cast<void*>(handle), frame.layerCount,
            int(result));
    } else if (XR_FAILED(result)) {
        log("event=panel_disabled call=xrEndFrame xr=%d", int(result));
        session->graphicsEnabled = false;
    }
    return result;
}

XrResult XRAPI_CALL destroySession(XrSession handle) {
    auto* session = lookup(sessions, handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    session->graphicsEnabled = false;
    session->panel.reset();
    auto result = session->owner->destroySession(handle);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard lock(registry);
        sessions.remove_if([handle](const Session& s) { return s.handle == handle; });
    }
    log("event=session_destroyed session=%p xr=%d", static_cast<void*>(handle), int(result));
    return result;
}

XrResult XRAPI_CALL destroyInstance(XrInstance handle) {
    auto* inst = lookup(instances, handle);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    // Also support implicit destruction of live child sessions. Splice them out before XR/Vulkan cleanup.
    std::list<Session> children;
    {
        std::lock_guard lock(registry);
        for (auto it = sessions.begin(); it != sessions.end();) {
            auto current = it++;
            if (current->owner == inst)
                children.splice(children.end(), sessions, current);
        }
    }
    for (auto& s : children) {
        s.panel.reset();
        s.graphicsEnabled = false;
    }
    const auto result = inst->destroy(handle);
    std::lock_guard lock(registry);
    if (XR_SUCCEEDED(result))
        instances.remove_if([handle](const Instance& i) { return i.handle == handle; });
    else
        sessions.splice(sessions.end(), children);
    log("event=instance_destroyed xr=%d", int(result));
    return result;
}

XrResult XRAPI_CALL getProc(XrInstance handle, const char* name, PFN_xrVoidFunction* fn) {
    if (!fn || !name)
        return XR_ERROR_VALIDATION_FAILURE;
    *fn = nullptr;
    if (!std::strcmp(name, "xrGetInstanceProcAddr")) {
        *fn = reinterpret_cast<PFN_xrVoidFunction>(getProc);
        return XR_SUCCESS;
    }
    auto* inst = lookup(instances, handle);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    const auto result = inst->get(handle, name, fn);
    if (XR_FAILED(result) || !*fn)
        return result;
    if (!std::strcmp(name, "xrCreateSession"))
        *fn = reinterpret_cast<PFN_xrVoidFunction>(createSession);
    else if (!std::strcmp(name, "xrEndFrame"))
        *fn = reinterpret_cast<PFN_xrVoidFunction>(endFrame);
    else if (!std::strcmp(name, "xrDestroySession"))
        *fn = reinterpret_cast<PFN_xrVoidFunction>(destroySession);
    else if (!std::strcmp(name, "xrDestroyInstance"))
        *fn = reinterpret_cast<PFN_xrVoidFunction>(destroyInstance);
    return result;
}

XrResult XRAPI_CALL createLayer(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer,
                                XrInstance* out) {
    if (!info || !out || !layer || layer->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layer->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layer->structSize < sizeof(*layer) || !layer->nextInfo)
        return XR_ERROR_INITIALIZATION_FAILED;
    const auto* link = layer->nextInfo;
    if (link->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        link->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION || link->structSize < sizeof(*link) ||
        std::strncmp(link->layerName, layerName, XR_MAX_API_LAYER_NAME_SIZE) ||
        !link->nextGetInstanceProcAddr || !link->nextCreateApiLayerInstance)
        return XR_ERROR_INITIALIZATION_FAILED;
    std::list<Instance> pending;
    try {
        pending.emplace_back();
    } catch (const std::bad_alloc&) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
    auto forwarded = *layer;
    forwarded.nextInfo = link->next;
    auto& inst = pending.front();
    inst.get = link->nextGetInstanceProcAddr;
    auto requested = *info;
    const char* shape = std::getenv("EDMO_SHAPE");
    const char* active = std::getenv("EDMO_XR_MODE");
    std::vector<const char*> names;
    bool added = false;
    if (active && !std::strcmp(active, "live") && (std::getenv("EDMO_DISPLAY_FILE") || (shape && !std::strcmp(shape, "curved")))) {
        inst.curved = true;
        try {
            for (uint32_t n = 0; n < info->enabledExtensionCount; ++n)
                names.push_back(info->enabledExtensionNames[n]);
            const char* ext = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
            if (std::none_of(names.begin(), names.end(), [ext](auto name){ return !std::strcmp(name, ext); })) {
                names.push_back(ext);
                requested.enabledExtensionNames = names.data();
                requested.enabledExtensionCount = names.size();
                added = true;
            }
        } catch (const std::bad_alloc&) { return XR_ERROR_OUT_OF_MEMORY; }
    }
    auto result = link->nextCreateApiLayerInstance(&requested, &forwarded, out);
    if (added && result == XR_ERROR_EXTENSION_NOT_PRESENT) {
        inst.curved = false;
        log("event=cylinder_unavailable fallback=flat");
        result = link->nextCreateApiLayerInstance(info, &forwarded, out);
    }
    if (XR_FAILED(result))
        return result;
    inst.handle = *out;
    if (!resolve(inst, "xrDestroyInstance", inst.destroy) ||
        !resolve(inst, "xrCreateSession", inst.createSession) ||
        !resolve(inst, "xrDestroySession", inst.destroySession) ||
        !resolve(inst, "xrEndFrame", inst.endFrame)) {
        if (inst.destroy)
            inst.destroy(*out);
        *out = XR_NULL_HANDLE;
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    const char* mode = std::getenv("EDMO_XR_MODE");
    inst.live = mode && !std::strcmp(mode, "live");
    inst.panel = mode && (inst.live || !std::strcmp(mode, "panel")) && inst.api.load(*out, inst.get);
    log("event=instance_created app=%.*s mode=%s", XR_MAX_APPLICATION_NAME_SIZE,
        info->applicationInfo.applicationName, inst.panel ? mode : "trace");
    std::lock_guard lock(registry);
    instances.splice(instances.end(), pending);
    return result;
}
} // namespace
} // namespace edmo

extern "C" __attribute__((visibility("default"))) XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loader, const char* name,
                                   XrNegotiateApiLayerRequest* req) {
    using namespace edmo;
    constexpr auto version = XR_MAKE_VERSION(1, 0, 0);
    if (!loader || !req || !name || std::strcmp(name, layerName) ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize < sizeof(*loader) ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        req->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || req->structSize < sizeof(*req) ||
        loader->minInterfaceVersion > 1 || loader->maxInterfaceVersion < 1 ||
        loader->minApiVersion > version || loader->maxApiVersion < version)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->layerInterfaceVersion = 1;
    req->layerApiVersion = version;
    req->getInstanceProcAddr = getProc;
    req->createApiLayerInstance = createLayer;
    log("event=negotiated interface=1 api=1.0");
    return XR_SUCCESS;
}
