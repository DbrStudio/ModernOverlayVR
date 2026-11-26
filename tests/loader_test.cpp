#include "test_gpu.hpp"
#include "../layer/texture.hpp"
#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
TestGpu gpu;
// A second image uses the same Vulkan device in the live swapchain.
TestGpu otherImage;
bool live{}, acquired{};
unsigned imageIndex{};

int instanceToken, sessionToken, spaceToken, swapchainToken;
const auto inst = reinterpret_cast<XrInstance>(&instanceToken);
const auto session = reinterpret_cast<XrSession>(&sessionToken);
const auto space = reinterpret_cast<XrSpace>(&spaceToken);
const auto sc = reinterpret_cast<XrSwapchain>(&swapchainToken);
bool spaceAlive{}, swapAlive{}, released{}, panelExpected{}, firstWait{};
bool sawGeometry{};
unsigned testWidth = edmo::liveWidth, testHeight = edmo::liveHeight;
unsigned creates{}, acquires{}, releases{}, quads{}, calls{};
XrResult endResult = XR_SUCCESS;
VkFormat offered = VK_FORMAT_R8G8B8A8_SRGB;
const XrFrameEndInfo* original{};

XrResult extensions(const char*, uint32_t n, uint32_t* count, XrExtensionProperties* props) {
    *count = 1;
    if (n) {
        std::strcpy(props->extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        props->extensionVersion = XR_KHR_vulkan_enable_SPEC_VERSION;
    }
    return XR_SUCCESS;
}
XrResult createInstance(const XrInstanceCreateInfo* info, XrInstance* out) {
    for (uint32_t n = 0; n < info->enabledExtensionCount; ++n)
        if (!strcmp(info->enabledExtensionNames[n], XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) && getenv("EDMO_TEST_NO_CYLINDER"))
            return XR_ERROR_EXTENSION_NOT_PRESENT;
    *out = inst;
    return XR_SUCCESS;
}
XrResult destroyInstance(XrInstance i) {
    CHECK(i == inst && !spaceAlive && !swapAlive);
    return XR_SUCCESS;
}
XrResult getSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* system) {
    *system = 7;
    return XR_SUCCESS;
}
XrResult createSession(XrInstance, const XrSessionCreateInfo* info, XrSession* out) {
    CHECK(info->systemId == 7);
    *out = session;
    return XR_SUCCESS;
}
XrResult destroySession(XrSession s) {
    CHECK(s == session && !spaceAlive && !swapAlive);
    return XR_SUCCESS;
}
XrResult properties(XrInstance, XrSystemId, XrSystemProperties* props) {
    props->graphicsProperties = {2048, 2048, 3};
    return XR_SUCCESS;
}
XrResult createSpace(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* out) {
    CHECK(!spaceAlive && info->referenceSpaceType == (live && getenv("EDMO_ANCHOR") ? XR_REFERENCE_SPACE_TYPE_LOCAL : XR_REFERENCE_SPACE_TYPE_VIEW) &&
          info->poseInReferenceSpace.orientation.w == 1);
    spaceAlive = true;
    *out = space;
    return XR_SUCCESS;
}
XrResult destroySpace(XrSpace s) {
    CHECK(spaceAlive && s == space);
    spaceAlive = false;
    return XR_SUCCESS;
}
XrResult formats(XrSession, uint32_t n, uint32_t* count, int64_t* out) {
    *count = 1;
    if (n)
        *out = offered;
    return XR_SUCCESS;
}
XrResult makeSwap(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
    CHECK(!swapAlive && info->createFlags == (live ? 0 : XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT));
    CHECK(info->format == offered && info->width == (live ? testWidth : edmo::width) && info->height == (live ? testHeight : edmo::height));
    CHECK(info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT);
    gpu.makeImage(*info);
    if (live) {
        otherImage.binding = gpu.binding;
        otherImage.makeImage(*info);
    }
    ++creates;
    swapAlive = true;
    released = false;
    firstWait = true;
    *out = sc;
    return XR_SUCCESS;
}
XrResult destroySwap(XrSwapchain s) {
    CHECK(s == sc && swapAlive);
    gpu.freeImage();
    if (live) {
        otherImage.freeImage();
        otherImage.binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    }
    swapAlive = false;
    return XR_SUCCESS;
}
XrResult images(XrSwapchain, uint32_t n, uint32_t* count, XrSwapchainImageBaseHeader* out) {
    *count = live ? 2 : 1;
    if (!n) return XR_SUCCESS;
    CHECK(n == *count && out->type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR);
    reinterpret_cast<XrSwapchainImageVulkanKHR*>(out)->image = gpu.image;
    if (live) reinterpret_cast<XrSwapchainImageVulkanKHR*>(out)[1].image = otherImage.image;
    return XR_SUCCESS;
}
XrResult acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    ++acquires;
    CHECK(!acquired);
    acquired = true;
    imageIndex = live ? acquires % 2 : 0;
    *index = imageIndex;
    return XR_SUCCESS;
}
XrResult wait(XrSwapchain, const XrSwapchainImageWaitInfo* info) {
    CHECK(info->timeout == 0);
    if (firstWait) {
        firstWait = false;
        return XR_TIMEOUT_EXPIRED;
    }
    return XR_SUCCESS;
}
XrResult release(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
    CHECK(acquired);
    acquired = false;
    released = true;
    ++releases;
    return XR_SUCCESS;
}
XrResult endFrame(XrSession s, const XrFrameEndInfo* frame) {
    ++calls;
    CHECK(s == session && frame->displayTime == original->displayTime && frame->next == original->next);
    CHECK(frame->environmentBlendMode == original->environmentBlendMode);
    if (frame == original)
        return endResult;
    CHECK(panelExpected && released && frame->layerCount == original->layerCount + 1);
    for (uint32_t i = 0; i < original->layerCount; ++i)
        CHECK(frame->layers[i] == original->layers[i]);
    XrSpace expectedSpace = space;
    if (live && getenv("EDMO_ANCHOR")) {
        for (uint32_t i = 0; i < original->layerCount; ++i)
            if (original->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                expectedSpace = original->layers[i]->space;
    }
    if (getenv("EDMO_TEST_SETTINGS") && frame->layers[frame->layerCount-1]->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
        auto* c = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(frame->layers[frame->layerCount-1]);
        CHECK(c->space == original->layers[1]->space && c->pose.position.x == .3f &&
              c->pose.position.y == -.2f && std::abs(c->centralAngle - 1.5707963f) < .001f &&
              std::abs(c->centralAngle*c->radius - 2.f) < .001f &&
              std::abs(c->pose.position.z - c->radius + 2.f) < .001f);
        sawGeometry = true;
        ++quads;
        return endResult;
    }
    if (live && getenv("EDMO_SHAPE") && !getenv("EDMO_TEST_NO_CYLINDER")) {
        auto* c = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(frame->layers[frame->layerCount-1]);
        CHECK(c->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR && c->space == expectedSpace &&
              c->subImage.swapchain == sc && c->radius > 0 && c->centralAngle > 0 &&
              c->aspectRatio == float(testWidth)/testHeight);
        ++quads;
        return endResult;
    }
    auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(frame->layers[frame->layerCount - 1]);
    CHECK(quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quad->space == expectedSpace &&
          quad->subImage.swapchain == sc);
    CHECK(quad->eyeVisibility == XR_EYE_VISIBILITY_BOTH && quad->pose.position.z < 0);
    CHECK(quad->layerFlags == (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                               XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT));
    ++quads;
    return endResult;
}

XrResult get(XrInstance, const char* name, PFN_xrVoidFunction* out) {
    struct Entry {
        const char* name;
        PFN_xrVoidFunction fn;
    };
#define ENTRY(command, callback)                                                                             \
    {                                                                                                        \
        #command, reinterpret_cast<PFN_xrVoidFunction>(callback)                                             \
    }
    static const Entry entries[] = {
        ENTRY(xrGetInstanceProcAddr, get),
        ENTRY(xrEnumerateInstanceExtensionProperties, extensions),
        ENTRY(xrCreateInstance, createInstance),
        ENTRY(xrDestroyInstance, destroyInstance),
        ENTRY(xrGetSystem, getSystem),
        ENTRY(xrCreateSession, createSession),
        ENTRY(xrEndFrame, endFrame),
        ENTRY(xrDestroySession, destroySession),
        ENTRY(xrGetSystemProperties, properties),
        ENTRY(xrCreateReferenceSpace, createSpace),
        ENTRY(xrDestroySpace, destroySpace),
        ENTRY(xrEnumerateSwapchainFormats, formats),
        ENTRY(xrCreateSwapchain, makeSwap),
        ENTRY(xrDestroySwapchain, destroySwap),
        ENTRY(xrEnumerateSwapchainImages, images),
        ENTRY(xrAcquireSwapchainImage, acquire),
        ENTRY(xrWaitSwapchainImage, wait),
        ENTRY(xrReleaseSwapchainImage, release),
    };
#undef ENTRY
    for (auto& entry : entries)
        if (!std::strcmp(entry.name, name)) {
            *out = entry.fn;
            return XR_SUCCESS;
        }
    *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

template <class T> T sym(void* lib, const char* name) {
    auto fn = reinterpret_cast<T>(dlsym(lib, name));
    CHECK(fn);
    return fn;
}
template <class T> T proc(PFN_xrGetInstanceProcAddr loader, XrInstance i, const char* name) {
    PFN_xrVoidFunction fn{};
    CHECK(loader(i, name, &fn) == XR_SUCCESS && fn);
    return reinterpret_cast<T>(fn);
}
void* open(const char* path) {
    auto* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        std::fprintf(stderr, "%s\n", dlerror());
    CHECK(lib);
    return lib;
}
} // namespace

// The Python driver owns the real demo process and acknowledges lifecycle changes.
void rendererCommand(const char* command) {
    std::printf("renderer %s\n", command);
    std::fflush(stdout);
    CHECK(std::getchar() == '\n');
}

template<class Submit> void exerciseLive(Submit submit) {
    auto until = [&](auto done) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            submit();
            if (done()) return;
            std::this_thread::sleep_for(2ms);
        } while (std::chrono::steady_clock::now() < deadline);
        CHECK(false);
    };
    auto displayed = [] { return (imageIndex ? otherImage : gpu).readPixels(); };
    CHECK(!swapAlive && !quads);
    rendererCommand("start");
    if (getenv("EDMO_TEST_SETTINGS")) {
        auto deadline = std::chrono::steady_clock::now() + 400ms;
        while (std::chrono::steady_clock::now() < deadline) { submit(); std::this_thread::sleep_for(2ms); }
        CHECK(quads == 0 && creates == 0);
        rendererCommand("enable");
    }
    until([] { return releases == 1; });
    const auto first = displayed();
    CHECK(first.front() == 0);
    // BGRA8 on the GPU; the renderer supplies straight RGBA8.
    CHECK(first[(testHeight - 5) * testWidth + 3] == 0xffff78be);
    CHECK((first[3 * testWidth + 3] >> 24) == 160);
    unsigned before = quads;
    for (int n = 0; n < 16; ++n) submit();
    CHECK(quads == before + 16 && acquires == 1 && releases == 1);
    until([] { return releases == 2; });
    CHECK(displayed() != first);
    rendererCommand("kill");
    until([&] { bool hidden = quads == before; before = quads; return hidden; });
    CHECK(acquires == 2);
    rendererCommand("start");
    until([] { return releases == 3; });
    CHECK(displayed() == first && creates == 1);
    if (getenv("EDMO_TEST_SETTINGS")) {
        rendererCommand("geometry");
        until([] { return sawGeometry; });
        rendererCommand("disable");
        before = quads;
        until([&]{ bool hidden = before == quads; before = quads; return hidden; });
        rendererCommand("kill");
        testWidth = 640; testHeight = 480;
        rendererCommand("resize");
        rendererCommand("start");
        rendererCommand("enable");
        until([] { return creates == 2 && released; });
        CHECK(displayed().size() == 640*480);
    }
    rendererCommand("kill");
}

int main(int argc, char** argv) {
    CHECK(argc == 4);
    if (auto size = getenv("EDMO_RESOLUTION")) CHECK(std::sscanf(size, "%ux%u", &testWidth, &testHeight) == 2);
    live = !std::strcmp(argv[3], "live");
    panelExpected = live || !std::strcmp(argv[3], "panel");
    const bool unsupported = !std::strcmp(argv[3], "unsupported");
    if (unsupported)
        setenv("EDMO_XR_MODE", "panel", 1);
    if (panelExpected && !gpu.init()) {
        std::fprintf(stderr, "No Vulkan graphics device\n");
        return 77;
    }
    auto* runtime = open(argv[2]);
    sym<void (*)(PFN_xrGetInstanceProcAddr)>(runtime, "setTestDispatch")(get);
    auto* loader = open(argv[1]);
    auto enumerate = sym<PFN_xrEnumerateApiLayerProperties>(loader, "xrEnumerateApiLayerProperties");
    uint32_t n{};
    CHECK(enumerate(0, &n, nullptr) == XR_SUCCESS && n);
    std::vector<XrApiLayerProperties> layers(n, {XR_TYPE_API_LAYER_PROPERTIES});
    CHECK(enumerate(n, &n, layers.data()) == XR_SUCCESS);
    bool found{};
    for (auto& layer : layers)
        found |= !std::strcmp(layer.layerName, edmo::layerName);
    CHECK(found);
    auto gipa = sym<PFN_xrGetInstanceProcAddr>(loader, "xrGetInstanceProcAddr");
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    std::strcpy(ci.applicationInfo.applicationName, "EDMO loader test");
    const char* extension = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &extension;
    XrInstance i{};
    CHECK(sym<PFN_xrCreateInstance>(loader, "xrCreateInstance")(&ci, &i) == XR_SUCCESS);
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system{};
    CHECK(proc<PFN_xrGetSystem>(gipa, i, "xrGetSystem")(i, &systemInfo, &system) == XR_SUCCESS);
    auto create = proc<PFN_xrCreateSession>(gipa, i, "xrCreateSession");
    auto end = proc<PFN_xrEndFrame>(gipa, i, "xrEndFrame");
    if (!panelExpected)
        CHECK(end != endFrame); // Prove that the no-op hook actually loaded.
    auto destroy = proc<PFN_xrDestroySession>(gipa, i, "xrDestroySession");
    XrBaseInStructure other{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR, nullptr};
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.systemId = system;
    sci.next = unsupported ? static_cast<const void*>(&other) : &gpu.binding;
    for (unsigned cycle = 1; cycle <= (live ? 1u : 2u); ++cycle) {
        if (cycle == 2 || live)
            offered = VK_FORMAT_B8G8R8A8_SRGB;
        XrSession s{};
        CHECK(create(i, &sci, &s) == XR_SUCCESS);
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad prior{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* headers[] = {
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&prior),
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection),
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&prior)};
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
        frame.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frame.displayTime = 123;
        original = &frame;
        int seatedOrigins[2]{};
        auto submit = [&] {
            projection.space = reinterpret_cast<XrSpace>(&seatedOrigins[(quads / 8) % 2]);
            auto before = calls;
            CHECK(end(s, &frame) == endResult && calls == before + 1);
        };
        submit(); // Empty frame.
        frame.layers = headers;
        frame.layerCount = 1;
        submit(); // No projection.
        frame.layerCount = 3;
        submit(); // Runtime maxLayerCount.
        CHECK(!swapAlive);
        frame.layerCount = 2;
        auto before = quads;
        submit(); // First wait times out.
        if (live) {
            exerciseLive(submit);
        } else if (panelExpected) {
            CHECK(quads == before);
            auto deadline = std::chrono::steady_clock::now() + 3s;
            while (quads == before && std::chrono::steady_clock::now() < deadline) {
                submit();
                std::this_thread::sleep_for(1ms);
            }
            CHECK(quads == before + 1);
            submit();
            CHECK(quads == before + 2 && acquires == cycle && creates == cycle && releases == cycle);
            gpu.checkPixels();
        } else
            CHECK(acquires == 0 && creates == 0 && quads == 0);
        endResult = XR_ERROR_TIME_INVALID;
        submit();
        endResult = XR_SUCCESS;
        if (cycle == 1)
            CHECK(destroy(s) == XR_SUCCESS);
        // Second session intentionally left to xrDestroyInstance.
    }
    CHECK(proc<PFN_xrDestroyInstance>(gipa, i, "xrDestroyInstance")(i) == XR_SUCCESS);
    dlclose(loader);
    dlclose(runtime);
    std::fprintf(stderr, "Verified %u downstream frame calls, %u appended quads, %u uploads\n", calls, quads,
                 releases);
}
