#include "test_gpu.hpp"
#include "../layer/texture.hpp"
#include <cstring>
#include <vector>

bool TestGpu::init() {
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (vkCreateInstance(&info, nullptr, &binding.instance) != VK_SUCCESS)
        return false;
    uint32_t n{};
    if (vkEnumeratePhysicalDevices(binding.instance, &n, nullptr) != VK_SUCCESS || !n)
        return false;
    std::vector<VkPhysicalDevice> devices(n);
    CHECK(vkEnumeratePhysicalDevices(binding.instance, &n, devices.data()) == VK_SUCCESS);
    for (auto dev : devices) {
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, families.data());
        for (uint32_t i = 0; i < n; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            binding.queueFamilyIndex = i;
            binding.physicalDevice = dev;
            float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = i;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            CHECK(vkCreateDevice(dev, &device, nullptr, &binding.device) == VK_SUCCESS);
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            std::fprintf(stderr, "Vulkan device: %s\n", props.deviceName);
            return true;
        }
    }
    return false;
}

VkDeviceMemory TestGpu::allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(binding.physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits & (1u << i)) || (props.memoryTypes[i].propertyFlags & flags) != flags)
            continue;
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = req.size;
        info.memoryTypeIndex = i;
        VkDeviceMemory memory{};
        CHECK(vkAllocateMemory(binding.device, &info, nullptr, &memory) == VK_SUCCESS);
        return memory;
    }
    CHECK(false);
}

void TestGpu::makeImage(const XrSwapchainCreateInfo& sc) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    format = info.format = VkFormat(sc.format);
    w = sc.width; h = sc.height;
    info.extent = {w, h, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    CHECK(vkCreateImage(binding.device, &info, nullptr, &image) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(binding.device, image, &req);
    imageMemory = allocate(req, 0);
    CHECK(vkBindImageMemory(binding.device, image, imageMemory, 0) == VK_SUCCESS);
}

std::vector<uint32_t> TestGpu::readPixels() {
    // Read back from the runtime image, not from the layer's CPU-side staging data.
    const auto device = binding.device;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size_t(w) * h * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    auto memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = binding.queueFamilyIndex;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &alloc, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                         nullptr, 0, nullptr);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkQueue queue;
    vkGetDeviceQueue(device, binding.queueFamilyIndex, 0, &queue);
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS); // Test readback only.
    void* mapped{};
    CHECK(vkMapMemory(device, memory, 0, size_t(w) * h * 4, 0, &mapped) == VK_SUCCESS);
    std::vector<uint32_t> pixels(size_t(w) * h);
    std::memcpy(pixels.data(), mapped, pixels.size() * 4);
    vkUnmapMemory(device, memory);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    return pixels;
}

void TestGpu::checkPixels() {
    const auto pixels = readPixels();
    const auto expected = edmo::testTexture(format == VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(std::equal(pixels.begin(), pixels.end(), expected.begin(), expected.end()));
    const auto* px = pixels.data();
    CHECK((px[0] >> 24) == 0 && (px[6 * edmo::width + 6] >> 24) == 120);
    CHECK((px[9 * edmo::width + 13] >> 24) == 255);
}

void TestGpu::freeImage() {
    vkDestroyImage(binding.device, image, nullptr);
    vkFreeMemory(binding.device, imageMemory, nullptr);
    image = {};
    imageMemory = {};
}
TestGpu::~TestGpu() {
    if (binding.device)
        vkDestroyDevice(binding.device, nullptr);
    if (binding.instance)
        vkDestroyInstance(binding.instance, nullptr);
}
