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

    // Anisotropie « si possible » : activée uniquement si le GPU la supporte (doit
    // précéder la construction du device, qui reprend les features de vkb_phys).
    VkPhysicalDeviceFeatures wanted{};
    wanted.samplerAnisotropy = VK_TRUE;
    sampler_anisotropy_ = vkb_phys.enable_features_if_present(wanted);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    max_sampler_anisotropy_ = props.limits.maxSamplerAnisotropy;
    log::info("Anisotropie : {}", sampler_anisotropy_ ? "activée" : "non supportée");

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

bool VulkanContext::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible,
                                  GpuBuffer& out) const {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_create{};
    alloc_create.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) {
        // Mapping persistant + écriture séquentielle CPU (UBO, vertex buffers du M2).
        alloc_create.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VmaAllocationInfo alloc_info{};
    if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_create, &out.buffer, &out.allocation,
                        &alloc_info) != VK_SUCCESS) {
        log::error("VMA : création d'un tampon échouée ({} octets)", size);
        return false;
    }
    out.mapped = alloc_info.pMappedData;  // non-nul si host-visible + MAPPED
    out.size = size;
    return true;
}

void VulkanContext::destroy_buffer(GpuBuffer& buffer) const {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
    }
    buffer = GpuBuffer{};
}

bool VulkanContext::create_image(std::uint32_t width, std::uint32_t height, VkFormat format,
                                 VkImageUsageFlags usage, VkImage& out_image,
                                 VmaAllocation& out_allocation) const {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_create{};
    alloc_create.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_create.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // image lourde => dédiée

    if (vmaCreateImage(allocator_, &image_info, &alloc_create, &out_image, &out_allocation,
                       nullptr) != VK_SUCCESS) {
        log::error("VMA : création d'une image échouée ({}x{})", width, height);
        return false;
    }
    return true;
}

void VulkanContext::destroy_image(VkImage image, VmaAllocation allocation) const {
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image, allocation);
    }
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
