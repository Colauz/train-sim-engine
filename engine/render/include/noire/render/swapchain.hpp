#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace noire::render {

class VulkanContext;

// Encapsule la swapchain et ses vues d'image. Construite via vk-bootstrap ;
// sait se reconstruire (redimensionnement de fenêtre).
class Swapchain {
public:
    Swapchain() = default;
    ~Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    bool initialize(VulkanContext& ctx, VkExtent2D desired_extent);
    void recreate(VkExtent2D desired_extent);
    void shutdown();

    [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
    [[nodiscard]] VkFormat image_format() const { return image_format_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] const std::vector<VkImageView>& image_views() const { return image_views_; }
    [[nodiscard]] std::uint32_t image_count() const {
        return static_cast<std::uint32_t>(image_views_.size());
    }

private:
    bool build(VkExtent2D extent, VkSwapchainKHR old_swapchain);
    void destroy_views();

    VulkanContext* ctx_ = nullptr;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat image_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
};

}  // namespace noire::render
