#include "noire/render/swapchain.hpp"

#include <VkBootstrap.h>

#include "noire/core/log.hpp"
#include "noire/render/vulkan_context.hpp"

namespace noire::render {

bool Swapchain::build(VkExtent2D extent, VkSwapchainKHR old_swapchain) {
    vkb::SwapchainBuilder builder{ctx_->physical_device(), ctx_->device(), ctx_->surface()};
    auto ret =
        builder
            .set_desired_format(VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB,
                                                   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)  // VSync : simple et sans tearing
            .set_desired_extent(extent.width, extent.height)
            .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
            .set_old_swapchain(old_swapchain)
            .build();
    if (!ret) {
        log::error("Vulkan : création de la swapchain échouée : {}", ret.error().message());
        return false;
    }

    vkb::Swapchain sc = ret.value();
    swapchain_ = sc.swapchain;
    image_format_ = sc.image_format;
    extent_ = sc.extent;
    images_ = sc.get_images().value();
    image_views_ = sc.get_image_views().value();
    return true;
}

bool Swapchain::initialize(VulkanContext& ctx, VkExtent2D desired_extent) {
    ctx_ = &ctx;
    return build(desired_extent, VK_NULL_HANDLE);
}

void Swapchain::recreate(VkExtent2D desired_extent) {
    if (ctx_ == nullptr) {
        return;
    }
    VkSwapchainKHR old = swapchain_;
    destroy_views();
    build(desired_extent, old);
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx_->device(), old, nullptr);
    }
}

void Swapchain::destroy_views() {
    if (ctx_ == nullptr) {
        return;
    }
    for (VkImageView view : image_views_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx_->device(), view, nullptr);
        }
    }
    image_views_.clear();
    images_.clear();
}

void Swapchain::shutdown() {
    if (ctx_ == nullptr) {
        return;
    }
    destroy_views();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx_->device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

}  // namespace noire::render
