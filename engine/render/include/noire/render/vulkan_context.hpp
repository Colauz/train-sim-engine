#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <vulkan/vulkan.h>

#include "noire/render/gpu_buffer.hpp"

// Forward-declare les handles VMA (typedefs identiques à ceux de VMA).
typedef struct VmaAllocator_T* VmaAllocator;

namespace noire::render {

// Paramètres de création : la couche appelante fournit les extensions requises
// par la fenêtre + une fabrique de surface. Ainsi render NE connaît PAS GLFW.
struct ContextCreateInfo {
    std::vector<const char*> instance_extensions;         // ex. issues de la fenêtre
    std::function<VkSurfaceKHR(VkInstance)> make_surface;  // création de la surface
    bool enable_validation = true;
    const char* app_name = "Noire Engine";
};

// Possède l'instance, le device, les files d'attente et l'allocateur VMA.
// Fournit aussi les helpers d'allocation (buffers, images) au-dessus de VMA.
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool initialize(const ContextCreateInfo& info);
    void shutdown();

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_device_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] VmaAllocator allocator() const { return allocator_; }

    [[nodiscard]] VkQueue graphics_queue() const { return graphics_queue_; }
    [[nodiscard]] VkQueue present_queue() const { return present_queue_; }
    [[nodiscard]] std::uint32_t graphics_queue_family() const { return graphics_queue_family_; }

    // Anisotropie d'échantillonnage : activée seulement si le GPU la supporte.
    [[nodiscard]] bool sampler_anisotropy() const { return sampler_anisotropy_; }
    [[nodiscard]] float max_sampler_anisotropy() const { return max_sampler_anisotropy_; }

    // --- Helpers d'allocation VMA --------------------------------------------
    // `host_visible` => tampon mappé en permanence (GpuBuffer::mapped valide).
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible,
                       GpuBuffer& out) const;
    void destroy_buffer(GpuBuffer& buffer) const;

    // Les 3 derniers paramètres n'ont de valeur non triviale que pour la cubemap
    // d'environnement (M8 étape 6a) : 6 layers + CUBE_COMPATIBLE + chaîne de mips.
    // Les défauts reproduisent exactement le comportement d'avant (2D, 1 mip, 1 layer).
    bool create_image(std::uint32_t width, std::uint32_t height, VkFormat format,
                      VkImageUsageFlags usage, VkImage& out_image, VmaAllocation& out_allocation,
                      std::uint32_t mip_levels = 1, std::uint32_t array_layers = 1,
                      VkImageCreateFlags flags = 0) const;
    void destroy_image(VkImage image, VmaAllocation allocation) const;

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    std::uint32_t graphics_queue_family_ = 0;
    bool sampler_anisotropy_ = false;
    float max_sampler_anisotropy_ = 1.0f;
};

}  // namespace noire::render
