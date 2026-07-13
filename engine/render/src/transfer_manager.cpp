#include "noire/render/transfer_manager.hpp"

#include <cstddef>

#include "noire/core/log.hpp"
#include "noire/render/vulkan_context.hpp"

namespace noire::render {

TransferManager::~TransferManager() { shutdown(); }

bool TransferManager::initialize(VulkanContext* context, std::uint32_t max_in_flight) {
    context_ = context;
    if (max_in_flight == 0) {
        max_in_flight = 1;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context_->graphics_queue_family();
    if (vkCreateCommandPool(context_->device(), &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        log::error("TransferManager : création du command pool échouée");
        return false;
    }

    slots_.resize(max_in_flight);

    std::vector<VkCommandBuffer> cmds(max_in_flight);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = max_in_flight;
    if (vkAllocateCommandBuffers(context_->device(), &alloc, cmds.data()) != VK_SUCCESS) {
        log::error("TransferManager : allocation des command buffers échouée");
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;  // non signalée (slots libres au départ)
    for (std::uint32_t i = 0; i < max_in_flight; ++i) {
        Slot& slot = slots_[i];
        slot.cmd = cmds[i];
        if (vkCreateFence(context_->device(), &fence_info, nullptr, &slot.fence) != VK_SUCCESS) {
            log::error("TransferManager : création d'une fence échouée");
            return false;
        }
    }

    log::info("TransferManager prêt ({} transferts simultanés max)", max_in_flight);
    return true;
}

void TransferManager::reclaim(Slot& slot) {
    for (GpuBuffer& staging : slot.staging) {
        context_->destroy_buffer(staging);
    }
    slot.staging.clear();
    for (std::function<void()>& callback : slot.callbacks) {
        if (callback) {
            callback();
        }
    }
    slot.callbacks.clear();
    slot.busy = false;
}

void TransferManager::poll() {
    if (context_ == nullptr) {
        return;
    }
    for (Slot& slot : slots_) {
        if (slot.busy && vkGetFenceStatus(context_->device(), slot.fence) == VK_SUCCESS) {
            reclaim(slot);
        }
    }
}

TransferManager::Transfer TransferManager::begin() {
    poll();  // récupère d'abord les slots dont le transfert est terminé.

    std::size_t index = slots_.size();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].busy) {
            index = i;
            break;
        }
    }
    if (index == slots_.size()) {
        // Saturation : on attend le plus ancien transfert (slot 0). Cas rare (init) —
        // en régime, le budget d'upload par frame empêche d'atteindre ce point.
        Slot& oldest = slots_[0];
        vkWaitForFences(context_->device(), 1, &oldest.fence, VK_TRUE, UINT64_MAX);
        reclaim(oldest);
        index = 0;
    }

    Slot& slot = slots_[index];
    slot.busy = true;
    vkResetFences(context_->device(), 1, &slot.fence);
    vkResetCommandBuffer(slot.cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.cmd, &begin);

    return Transfer{slot.cmd, static_cast<std::uint32_t>(index)};
}

void TransferManager::stage(const Transfer& t, const GpuBuffer& staging) {
    slots_[t.slot].staging.push_back(staging);
}

void TransferManager::on_complete(const Transfer& t, std::function<void()> callback) {
    slots_[t.slot].callbacks.push_back(std::move(callback));
}

void TransferManager::submit(const Transfer& t) {
    Slot& slot = slots_[t.slot];
    vkEndCommandBuffer(slot.cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.cmd;
    if (vkQueueSubmit(context_->graphics_queue(), 1, &submit, slot.fence) != VK_SUCCESS) {
        log::error("TransferManager : soumission d'un transfert échouée");
        // La fence ne se signalera jamais : on libère le slot de force pour ne pas
        // fuir les staging buffers ni bloquer les futurs begin().
        reclaim(slot);
    }
}

void TransferManager::shutdown() {
    if (context_ == nullptr) {
        return;
    }
    VkDevice device = context_->device();
    if (device != VK_NULL_HANDLE) {
        for (Slot& slot : slots_) {
            if (slot.busy) {
                vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
                reclaim(slot);
            }
            if (slot.fence != VK_NULL_HANDLE) {
                vkDestroyFence(device, slot.fence, nullptr);
            }
        }
        slots_.clear();
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
        }
    }
    context_ = nullptr;
}

std::uint32_t TransferManager::in_flight() const {
    std::uint32_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.busy) {
            ++count;
        }
    }
    return count;
}

}  // namespace noire::render
