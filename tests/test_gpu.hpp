#pragma once
#include "../layer/panel.hpp"
#include <cstdio>
#include <vector>
#include <cstdlib>

// Assertions also run in Release builds.
#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);                                  \
            std::abort();                                                                                    \
        }                                                                                                    \
    } while (false)

struct TestGpu {
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    VkImage image{};
    VkDeviceMemory imageMemory{};
    VkFormat format{};
    bool init();
    void makeImage(const XrSwapchainCreateInfo& sc);
    uint32_t w{}, h{};
    std::vector<uint32_t> readPixels();
    void checkPixels();
    void freeImage();
    ~TestGpu();

  private:
    VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags);
};
