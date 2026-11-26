#include "panel.hpp"
#include "texture.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>

namespace edmo {
namespace {
float setting(const char* key, float fallback, float low, float high) {
    const char* raw = std::getenv(key);
    if (!raw) return fallback;
    char* end{};
    float value = std::strtof(raw, &end);
    return end != raw && !*end && std::isfinite(value) && value >= low && value <= high ? value : fallback;
}

bool xrOk(XrResult result, const char *op) noexcept {
    if (result == XR_SUCCESS)
        return true;
    log("event=panel_error call=%s xr=%d", op, int(result));
    return false;
}
bool vkOk(VkResult result, const char *op) noexcept {
    if (result == VK_SUCCESS)
        return true;
    log("event=panel_error call=%s vk=%d", op, int(result));
    return false;
}
template <class T>
bool resolve(XrInstance instance, PFN_xrGetInstanceProcAddr get, const char *name, T &out) noexcept {
    PFN_xrVoidFunction fn{};
    const auto result = get(instance, name, &fn);
    out = reinterpret_cast<T>(fn);
    return xrOk(result, name) && out;
}
} // namespace

bool PanelApi::load(XrInstance instance, PFN_xrGetInstanceProcAddr get) noexcept {
    return resolve(instance, get, "xrGetSystemProperties", systemProperties) &&
           resolve(instance, get, "xrCreateReferenceSpace", createSpace) &&
           resolve(instance, get, "xrDestroySpace", destroySpace) &&
           resolve(instance, get, "xrEnumerateSwapchainFormats", formats) &&
           resolve(instance, get, "xrCreateSwapchain", createSwapchain) &&
           resolve(instance, get, "xrDestroySwapchain", destroySwapchain) &&
           resolve(instance, get, "xrEnumerateSwapchainImages", images) &&
           resolve(instance, get, "xrAcquireSwapchainImage", acquire) &&
           resolve(instance, get, "xrWaitSwapchainImage", wait) &&
           resolve(instance, get, "xrReleaseSwapchainImage", release);
}

void Panel::configure(const PanelApi &api, XrInstance instance, XrSystemId system, XrSession session,
                      const XrGraphicsBindingVulkanKHR &binding, bool live, bool curved) noexcept {
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    if (!xrOk(api.systemProperties(instance, system, &props), "xrGetSystemProperties"))
        return;
    cylinderAvailable_ = live && curved;
    managed_ = live && std::getenv("EDMO_DISPLAY_FILE");
    maxWidth_ = props.graphicsProperties.maxSwapchainImageWidth;
    maxHeight_ = props.graphicsProperties.maxSwapchainImageHeight;
    w_ = live ? liveWidth : width;
    h_ = live ? liveHeight : height;
    const char* resolution = std::getenv("EDMO_RESOLUTION");
    if (live && resolution) {
        if (!std::strcmp(resolution, "640x480")) { w_=640; h_=480; }
        else if (!std::strcmp(resolution, "960x720")) { w_=960; h_=720; }
        else if (!std::strcmp(resolution, "1920x1440")) { w_=1920; h_=1440; }
    }
    if (w_ > props.graphicsProperties.maxSwapchainImageWidth ||
        h_ > props.graphicsProperties.maxSwapchainImageHeight)
        return;
    limit_ = props.graphicsProperties.maxLayerCount;
    session_ = session;
    binding_ = binding;
    binding_.next = nullptr;
    api_ = api;
    if (live) {
        live_.reset(new (std::nothrow) Live);
        if (!live_) return;
        try { live_->pixels.resize(size_t(w_)*h_); } catch (const std::bad_alloc&) { return; }
        const char* anchor = std::getenv("EDMO_ANCHOR");
        const char* shape = std::getenv("EDMO_SHAPE");
        display_ = {w_, h_, true, anchor && !std::strcmp(anchor,"room"), shape && !std::strcmp(shape,"curved"),
                    setting("EDMO_WIDTH",1.5f,.1f,5), setting("EDMO_DISTANCE",1.3f,.1f,10),
                    setting("EDMO_ARC",70,5,180),setting("EDMO_X",0,-5,5),setting("EDMO_Y",0,-5,5)};
        if (!live_->reader.start(display_))
            return;
    }
    state_ = State::fresh;
}

bool Panel::prepare() noexcept {
    uint32_t count{};
    if (!xrOk(api_.formats(session_, 0, &count, nullptr), "xrEnumerateSwapchainFormats") || !count)
        return false;
    auto formats = std::unique_ptr<int64_t[]>(new (std::nothrow) int64_t[count]);
    if (!formats ||
        !xrOk(api_.formats(session_, count, &count, formats.get()), "xrEnumerateSwapchainFormats"))
        return false;
    for (auto candidate : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_FORMAT_B8G8R8A8_UNORM}) {
        if (std::find(formats.get(), formats.get() + count, candidate) != formats.get() + count) {
            format_ = candidate;
            break;
        }
    }
    if (format_ == VK_FORMAT_UNDEFINED) {
        log("event=panel_error reason=no_rgba_format");
        return false;
    }
    XrReferenceSpaceCreateInfo view{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    view.referenceSpaceType = !managed_ && live_ && display_.room ? XR_REFERENCE_SPACE_TYPE_LOCAL : XR_REFERENCE_SPACE_TYPE_VIEW;
    view.poseInReferenceSpace.orientation.w = 1.f;
    if (!xrOk(api_.createSpace(session_, &view, &space_), "xrCreateReferenceSpace"))
        return false;
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.createFlags = live_ ? 0 : XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sc.format = format_;
    sc.width = w_;
    sc.height = h_;
    sc.arraySize = sc.faceCount = sc.sampleCount = sc.mipCount = 1;
    if (!xrOk(api_.createSwapchain(session_, &sc, &swapchain_), "xrCreateSwapchain"))
        return false;
    if (!xrOk(api_.images(swapchain_, 0, &imageCount_, nullptr), "xrEnumerateSwapchainImages") ||
        !imageCount_)
        return false;
    images_.reset(new (std::nothrow) XrSwapchainImageVulkanKHR[imageCount_]);
    if (!images_)
        return false;
    for (uint32_t i = 0; i < imageCount_; ++i)
        images_[i] = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if (!xrOk(api_.images(swapchain_, imageCount_, &count,
                          reinterpret_cast<XrSwapchainImageBaseHeader *>(images_.get())),
              "xrEnumerateSwapchainImages") ||
        count != imageCount_)
        return false;
    quad_.layerFlags =
        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    quad_.space = space_;
    quad_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_.subImage.swapchain = swapchain_;
    quad_.subImage.imageRect.extent = {int32_t(w_), int32_t(h_)};
    quad_.pose.orientation.w = 1.f;
    quad_.pose.position = {0.f, -0.16f, -1.f};
    layout();
    return true;
}

void Panel::layout() noexcept {
    room_ = live_ && display_.room;
    curved_ = cylinderAvailable_ && display_.curved;
    float size = live_ ? display_.size : .54f;
    quad_.size = {size, size * h_ / w_};
    if (live_) {
        quad_.pose.position = {display_.x, display_.y, -display_.distance};
        if (curved_) {
            cylinder_.space = space_;
            cylinder_.layerFlags = quad_.layerFlags;
            cylinder_.eyeVisibility = quad_.eyeVisibility;
            cylinder_.subImage = quad_.subImage;
            cylinder_.pose = quad_.pose;
            cylinder_.centralAngle = display_.arc * .01745329252f;
            cylinder_.radius = size / cylinder_.centralAngle;
            cylinder_.pose.position.z += cylinder_.radius;
            cylinder_.aspectRatio = float(w_) / h_;
        }
        log("event=display anchor=%s shape=%s size=%ux%u", room_ ? "room" : "head",
            curved_ ? "cylinder" : "flat", w_, h_);
    }
}

bool Panel::acquireImage() noexcept {
    uint32_t index{};
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!xrOk(api_.acquire(swapchain_, &acquire, &index), "xrAcquireSwapchainImage") || index >= imageCount_)
        return false;
    image_ = images_[index].image;
    return true;
}

bool Panel::allocateUpload() noexcept {
    const auto device = binding_.device;

    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = VkDeviceSize(w_) * h_ * 4;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!vkOk(vkCreateBuffer(device, &buffer, nullptr, &buffer_), "vkCreateBuffer"))
        return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer_, &req);
    if (!req.memoryTypeBits)
        return false;
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(binding_.physicalDevice, &props);
    const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = 0;
    while (type < props.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
           (props.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
    if (type == props.memoryTypeCount) return false;
    memory.memoryTypeIndex = type;
    memory.allocationSize = req.size;
    if (!vkOk(vkAllocateMemory(device, &memory, nullptr, &memory_), "vkAllocateMemory") ||
        !vkOk(vkBindBufferMemory(device, buffer_, memory_, 0), "vkBindBufferMemory"))
        return false;

    if (!vkOk(vkMapMemory(device, memory_, 0, buffer.size, 0, &mapped_), "vkMapMemory")) return false;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = binding_.queueFamilyIndex;
    if (!vkOk(vkCreateCommandPool(device, &pool, nullptr, &pool_), "vkCreateCommandPool"))
        return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_;
    alloc.commandBufferCount = 1;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    if (!vkOk(vkAllocateCommandBuffers(device, &alloc, &cmd_), "vkAllocateCommandBuffers"))
        return false;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vkOk(vkCreateFence(device, &fence, nullptr, &fence_), "vkCreateFence");
}

bool Panel::upload(const uint32_t *pixels, bool swizzle) noexcept {
    const auto device = binding_.device;
    if (!pool_ && !allocateUpload())
        return false;
    if (!vkOk(vkResetCommandPool(device, pool_, 0), "vkResetCommandPool") ||
        !vkOk(vkResetFences(device, 1, &fence_), "vkResetFences"))
        return false;
    auto cmd = cmd_;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vkOk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer"))
        return false;
    const size_t count = size_t(w_) * h_;
    if (swizzle) {
        auto* dst = static_cast<uint32_t*>(mapped_);
        for (size_t i = 0; i < count; ++i) {
            auto p = pixels[i];
            dst[i] = (p & 0xff00ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
        }
    } else std::memcpy(mapped_, pixels, count * 4);
    VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.image = image_;
    image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Discard all previous contents.
    image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &image);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w_, h_, 1};
    vkCmdCopyBufferToImage(cmd, buffer_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &image);
    if (!vkOk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"))
        return false;
    VkQueue queue{};
    vkGetDeviceQueue(device, binding_.queueFamilyIndex, binding_.queueIndex, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    inFlight_ = vkOk(vkQueueSubmit(queue, 1, &submit, fence_), "vkQueueSubmit");
    return inFlight_;
}

const XrCompositionLayerQuad *Panel::frame(uint32_t layerCount) noexcept {
    if (state_ == State::off || layerCount >= limit_)
        return nullptr;
    if (live_ && (state_ == State::fresh || state_ == State::ready)) {
        DisplaySettings next = display_;
        live_->reader.latest(live_->latest, live_->pixels, next);
        if (!(next == display_)) {
            display_ = next;
            if (next.w != w_ || next.h != h_) {
                releaseGraphics();
                state_ = State::fresh;
                w_ = next.w; h_ = next.h;
                live_->released = {};
            } else if (space_) layout();
        }
        if (!display_.enabled || w_ > maxWidth_ || h_ > maxHeight_) return nullptr;
    }
    // The compositor can keep showing the last released image during the next upload.
    const auto *previous =
        live_ && live_->released.sequence && live_->released.generation == live_->latest.generation ? &quad_
                                                                                                    : nullptr;
    if (state_ == State::fresh || state_ == State::ready) {
        if (state_ == State::ready && !live_)
            return &quad_;
        if (live_ && (!live_->latest.sequence || live_->latest == live_->released))
            return previous;
        if ((state_ == State::fresh && !prepare()) || !acquireImage()) {
            reset();
            return nullptr;
        }
        state_ = State::acquired;
    }
    if (state_ == State::acquired) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 0;
        const auto result = api_.wait(swapchain_, &wait);
        if (result == XR_TIMEOUT_EXPIRED)
            return previous;
        const bool bgra = format_ == VK_FORMAT_B8G8R8A8_SRGB || format_ == VK_FORMAT_B8G8R8A8_UNORM;
        if (live_) live_->uploading = live_->latest;
        if (!xrOk(result, "xrWaitSwapchainImage") ||
            !(live_ ? upload(live_->pixels.data(), bgra) : upload(testTexture(bgra).data(), false))) {
            reset();
            return nullptr;
        }
        state_ = State::submitted;
        return previous;
    }
    const auto result = vkGetFenceStatus(binding_.device, fence_);
    if (result == VK_NOT_READY)
        return previous;
    if (!vkOk(result, "vkGetFenceStatus")) {
        state_ = State::off;
        return nullptr;
    }
    inFlight_ = false;
    if (!live_)
        discardUpload();
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!xrOk(api_.release(swapchain_, &release), "xrReleaseSwapchainImage")) {
        reset();
        return nullptr;
    }
    state_ = State::ready;
    if (live_) {
        live_->released = live_->uploading;
        if (!live_->released.sequence || live_->released.generation != live_->latest.generation)
            return nullptr;
    }
    return &quad_;
}

void Panel::discardUpload() noexcept {
    const auto device = binding_.device;
    if (fence_)
        vkDestroyFence(device, fence_, nullptr);
    if (pool_)
        vkDestroyCommandPool(device, pool_, nullptr);
    if (buffer_)
        vkDestroyBuffer(device, buffer_, nullptr);
    if (mapped_) vkUnmapMemory(device, memory_);
    mapped_ = nullptr;
    if (memory_)
        vkFreeMemory(device, memory_, nullptr);
    fence_ = {};
    pool_ = {};
    buffer_ = {};
    memory_ = {};
}

void Panel::reset() noexcept {
    state_ = State::off;
    live_.reset();
    releaseGraphics();
}

void Panel::releaseGraphics() noexcept {
    images_.reset();
    format_ = VK_FORMAT_UNDEFINED;
    if (inFlight_) {
        // Only at teardown, for this panel's outstanding one-shot upload.
        vkWaitForFences(binding_.device, 1, &fence_, VK_TRUE, UINT64_MAX);
        inFlight_ = false;
    }
    discardUpload();
    if (swapchain_)
        api_.destroySwapchain(swapchain_);
    if (space_)
        api_.destroySpace(space_);
    swapchain_ = {};
    space_ = {};
}
} // namespace edmo
