#pragma once

#include <vulkan/vulkan.h>

// Forward-declare le handle d'allocation VMA (typedef identique à celui de VMA).
typedef struct VmaAllocation_T* VmaAllocation;

namespace noire::render {

// Tampon GPU alloué via VMA. `mapped` est non-nul pour les tampons host-visible
// à mapping persistant (mise à jour CPU directe, ex. UBO / vertex buffers du M2).
struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

}  // namespace noire::render
