#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include "noire/render/swapchain.hpp"

#include <VkBootstrap.h>

#include "noire/core/log.hpp"
#include "noire/render/vulkan_context.hpp"

namespace noire::render {

bool Swapchain::build(VkExtent2D extent, VkSwapchainKHR old_swapchain) {
    vkb::SwapchainBuilder builder{ctx_->physical_device(), ctx_->device(), ctx_->surface()};

    // Journalise les modes RÉELLEMENT supportés. Utile parce que `set_desired_present_mode`
    // est un voeu : un mode non supporté est silencieusement remplacé par FIFO, et rien ne
    // le signale — sinon des FPS suspectement ronds, qu'on met un moment à ne plus croire.
    std::uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physical_device(), ctx_->surface(),
                                              &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physical_device(), ctx_->surface(),
                                              &mode_count, modes.data());
    std::string list;
    for (VkPresentModeKHR m : modes) {
        list += (m == VK_PRESENT_MODE_IMMEDIATE_KHR)      ? "IMMEDIATE "
                : (m == VK_PRESENT_MODE_MAILBOX_KHR)      ? "MAILBOX "
                : (m == VK_PRESENT_MODE_FIFO_KHR)         ? "FIFO "
                : (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? "FIFO_RELAXED "
                                                          : "? ";
    }
    log::info("Swapchain : modes supportés = {}", list);

    VkPresentModeKHR desired_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::getenv("NOIRE_NO_VSYNC") != nullptr) {
        const auto has = [&](VkPresentModeKHR m) {
            return std::find(modes.begin(), modes.end(), m) != modes.end();
        };
        desired_mode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                       : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                          : VK_PRESENT_MODE_FIFO_KHR;
        if (desired_mode == VK_PRESENT_MODE_FIFO_KHR) {
            log::warn("NOIRE_NO_VSYNC demandé mais AUCUN mode libre : mesure impossible");
        }
    }
    auto ret =
        builder
            .set_desired_format(VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB,
                                                   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
            // FIFO (VSync) par défaut : simple et sans tearing. NOIRE_NO_VSYNC bascule en
            // IMMEDIATE pour le profilage : sous FIFO, le GPU chôme la moitié de la frame
            // et un iGPU se met alors à baisser ses fréquences — les mesures deviennent
            // ininterprétables, et les FPS ne disent plus que « 120 ».
            .set_desired_present_mode(desired_mode)
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
