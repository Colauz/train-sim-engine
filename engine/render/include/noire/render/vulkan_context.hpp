#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <vulkan/vulkan.h>

// Forward-declare l'allocateur VMA pour ne pas tirer tout <vk_mem_alloc.h> ici.
// (Typedef identique à celui de VMA : redéfinition sans danger en C++.)
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
};

}  // namespace noire::render
