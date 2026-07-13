#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <vulkan/vulkan.h>

#include "noire/render/gpu_buffer.hpp"

namespace noire::render {

class VulkanContext;

// Téléversements GPU asynchrones via staging buffers + fences POLLÉES.
//
// Cycle d'un transfert (thread principal uniquement — pool/queue non thread-safe) :
//   1. begin()       -> réserve un slot, renvoie un command buffer déjà ouvert ;
//   2. l'appelant y enregistre ses vkCmdCopy* / barrières ;
//   3. stage(...)     -> confie les staging buffers (libérés à la complétion) ;
//   4. on_complete(..)-> callback exécuté à la complétion (depuis poll()) ;
//   5. submit()       -> ferme + soumet à la file graphique avec la fence du slot.
//
// poll() (1x/frame) teste les fences avec vkGetFenceStatus — JAMAIS vkWaitForFences —,
// libère les staging terminés et exécute les callbacks : la boucle n'est jamais
// bloquée. Sous saturation (tous les slots occupés), begin() attend le transfert le
// plus ancien : cas rare et borné, réservé à l'init ; en régime, le budget d'upload
// par frame (côté appelant) empêche la saturation.
class TransferManager {
public:
    TransferManager() = default;
    ~TransferManager();
    TransferManager(const TransferManager&) = delete;
    TransferManager& operator=(const TransferManager&) = delete;

    bool initialize(VulkanContext* context, std::uint32_t max_in_flight = 4);
    void shutdown();

    // Poignée sur un transfert en cours d'enregistrement.
    struct Transfer {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        std::uint32_t slot = 0;
    };

    [[nodiscard]] Transfer begin();
    void stage(const Transfer& t, const GpuBuffer& staging);
    void on_complete(const Transfer& t, std::function<void()> callback);
    void submit(const Transfer& t);

    // Thread principal, 1x/frame : récupère les transferts terminés (NON bloquant).
    void poll();

    [[nodiscard]] std::uint32_t in_flight() const;

private:
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool busy = false;
        std::vector<GpuBuffer> staging;
        std::vector<std::function<void()>> callbacks;
    };

    void reclaim(Slot& slot);  // libère staging + exécute callbacks d'un slot terminé

    VulkanContext* context_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<Slot> slots_;
};

}  // namespace noire::render
