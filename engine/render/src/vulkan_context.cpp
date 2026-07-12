#include "noire/render/vulkan_context.hpp"

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include "noire/core/log.hpp"

namespace noire::render {

VulkanContext::~VulkanContext() { shutdown(); }

bool VulkanContext::initialize(const ContextCreateInfo& info) {
    // --- Instance -------------------------------------------------------------
    vkb::InstanceBuilder builder;
    builder.set_app_name(info.app_name).require_api_version(1, 2, 0);
    if (info.enable_validation) {
        builder.request_validation_layers(true).use_default_debug_messenger();
    }
    for (const char* ext : info.instance_extensions) {
        builder.enable_extension(ext);
    }

    auto inst_ret = builder.build();
    if (!inst_ret) {
        log::error("Vulkan : création de l'instance échouée : {}", inst_ret.error().message());
        return false;
    }
    vkb::Instance vkb_inst = inst_ret.value();
    instance_ = vkb_inst.instance;
    debug_messenger_ = vkb_inst.debug_messenger;

    // --- Surface (fournie par l'appelant, sans couplage à GLFW) --------------
    surface_ = info.make_surface(instance_);
    if (surface_ == VK_NULL_HANDLE) {
        log::error("Vulkan : surface nulle");
        return false;
    }

    // --- Sélection du GPU + device logique -----------------------------------
    vkb::PhysicalDeviceSelector selector{vkb_inst};
    auto phys_ret = selector.set_surface(surface_).set_minimum_version(1, 2).select();
    if (!phys_ret) {
        log::error("Vulkan : aucun GPU compatible : {}", phys_ret.error().message());
        return false;
    }
    vkb::PhysicalDevice vkb_phys = phys_ret.value();
    physical_device_ = vkb_phys.physical_device;
    log::info("GPU sélectionné : {}", vkb_phys.name);

    vkb::DeviceBuilder device_builder{vkb_phys};
    auto dev_ret = device_builder.build();
    if (!dev_ret) {
        log::error("Vulkan : création du device échouée : {}", dev_ret.error().message());
        return false;
    }
    vkb::Device vkb_dev = dev_ret.value();
    device_ = vkb_dev.device;

    auto gq = vkb_dev.get_queue(vkb::QueueType::graphics);
    auto gqi = vkb_dev.get_queue_index(vkb::QueueType::graphics);
    auto pq = vkb_dev.get_queue(vkb::QueueType::present);
    if (!gq || !gqi || !pq) {
        log::error("Vulkan : récupération des files d'attente échouée");
        return false;
    }
    graphics_queue_ = gq.value();
    graphics_queue_family_ = gqi.value();
    present_queue_ = pq.value();

    // --- VMA : allocateur mémoire (indispensable pour le futur streaming) -----
    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.physicalDevice = physical_device_;
    alloc_info.device = device_;
    alloc_info.instance = instance_;
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_2;
    if (vmaCreateAllocator(&alloc_info, &allocator_) != VK_SUCCESS) {
        log::error("VMA : création de l'allocateur échouée");
        return false;
    }

    log::info("Contexte Vulkan initialisé (VMA prêt)");
    return true;
}

void VulkanContext::shutdown() {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debug_messenger_ != VK_NULL_HANDLE) {
        vkb::destroy_debug_utils_messenger(instance_, debug_messenger_);
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

}  // namespace noire::render
