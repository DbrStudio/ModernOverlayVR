#pragma once
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>
#include "frames.hpp"
#include <memory>

namespace edmo {
constexpr char layerName[] = "XR_APILAYER_EDMO_overlay";
void log(const char *format, ...) noexcept __attribute__((format(printf, 1, 2)));

struct PanelApi {
    PFN_xrGetSystemProperties systemProperties{};
    PFN_xrCreateReferenceSpace createSpace{};
    PFN_xrDestroySpace destroySpace{};
    PFN_xrEnumerateSwapchainFormats formats{};
    PFN_xrCreateSwapchain createSwapchain{};
    PFN_xrDestroySwapchain destroySwapchain{};
    PFN_xrEnumerateSwapchainImages images{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
    bool load(XrInstance instance, PFN_xrGetInstanceProcAddr get) noexcept;
};

// Owned by one XrSession. Frame calls and destruction are externally synchronized by OpenXR.
class Panel {
  public:
    Panel() = default;
    Panel(const Panel &) = delete;
    Panel &operator=(const Panel &) = delete;
    ~Panel() { reset(); }
    void configure(const PanelApi &api, XrInstance instance, XrSystemId system, XrSession session,
                   const XrGraphicsBindingVulkanKHR &binding, bool live, bool curved) noexcept;
    const XrCompositionLayerQuad *frame(uint32_t layerCount) noexcept;
    const XrCompositionLayerBaseHeader* composition(XrSpace projectionSpace) noexcept {
        // OpenComposite replaces the seated space on ResetSeatedZeroPose.
        // Borrow the current projection space for this submission; only space_ is owned.
        XrSpace anchor = room_ && projectionSpace ? projectionSpace : space_;
        if (room_ && quad_.space != anchor)
            log("event=projection_anchor space=%p", static_cast<void*>(anchor));
        quad_.space = cylinder_.space = anchor;
        if (curved_) return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder_);
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad_);
    }
    void reset() noexcept;

  private:
    enum class State { off, fresh, acquired, submitted, ready };
    void releaseGraphics() noexcept;
    void layout() noexcept;
    bool prepare() noexcept;
    bool acquireImage() noexcept;
    bool allocateUpload() noexcept;
    bool upload(const uint32_t *pixels, bool swizzle) noexcept;
    void discardUpload() noexcept;
    State state_{State::off};
    PanelApi api_{};
    XrSession session_{};
    XrGraphicsBindingVulkanKHR binding_{};
    XrSpace space_{};
    XrSwapchain swapchain_{};
    XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    uint32_t limit_{};
    uint32_t w_{}, h_{};
    bool curved_{}, room_{}, cylinderAvailable_{}, managed_{};
    DisplaySettings display_{};
    uint32_t maxWidth_{}, maxHeight_{};
    void *mapped_{};
    XrCompositionLayerCylinderKHR cylinder_{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    VkFormat format_{};
    struct Live {
        FrameReader reader;
        LivePixels pixels{};
        FrameId latest{}, uploading{}, released{};
    };
    std::unique_ptr<Live> live_;
    std::unique_ptr<XrSwapchainImageVulkanKHR[]> images_;
    uint32_t imageCount_{};
    VkImage image_{};
    VkCommandBuffer cmd_{};
    VkBuffer buffer_{};
    VkDeviceMemory memory_{};
    VkCommandPool pool_{};
    VkFence fence_{};
    bool inFlight_{};
};
} // namespace edmo
