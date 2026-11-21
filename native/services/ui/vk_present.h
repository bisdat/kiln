// SPDX-License-Identifier: MIT
// kiln.ui: what the overlay may do in one of the game's presents (vk.cpp draw, run_pre_overlay, hk_present). Plain logic
// without Vulkan's headers: host test tests/vk_present_test.cpp.
//
// The queue. A present names the queue it is made on, and Kiln's frame is submitted on that same queue, in the same
// call, so that it waits on the game's frame and the present waits on Kiln's. A command buffer can only be submitted to a
// queue of the family its pool was made for, and a render pass needs a family with graphics. Kiln draws only on a queue
// the game got (vkGetDeviceQueue / vkGetDeviceQueue2 after its vkCreateDevice returned) whose family has graphics.
// Frame generation presents from queues of its own: OptiScaler's (OptiScaler master, framegen/IFGFeature_Vk.cpp
// PrepareDevice) adds them to the game's device while it is made and gets them inside that call: FSR frame generation a
// present queue in the family with the fewest abilities that can present (a compute or transfer family on most GPUs),
// DLSS frame generation (Streamline) a graphics queue of its own. Its thread presents the game's frames and the
// generated ones on them, through the present function Kiln handed out. A queue handed out while the device was being
// made is a tool's (the game cannot ask before vkCreateDevice returns) unless it is asked for again after vkCreateDevice
// returned (the game asks for its own; FFX takes the game's own graphics queue during the creation as well; a tool
// asking again later is taken for the game, as before 0.2.0-dev.29.18): Kiln does not draw on a tool's queue, whatever
// its family, as it does not on a queue it never saw. Until 0.2.0-dev.29.18 Kiln did not know such a queue and drew for the
// first family the game asked for: its submit (and ImGui's texture upload) went to a queue of another family and failed
// with VK_ERROR_UNKNOWN (inbox 80).
// A queue Kiln did not see the game get, or saw only while the device was being made, is looked up once per device
// among the queues the game asked for in the VkDeviceCreateInfo Kiln's vkCreateDevice hook saw (look_up_requested: the
// real vkGetDeviceQueue for each family and index there). Those are the game's own, whoever asked for them first, and
// keep their families, so a game whose vkGetDeviceQueue went past Kiln's hook (a tool's or loader's patch on top of it,
// vkGetDeviceQueue2 where Kiln could not hook it) is drawn on as before 0.2.0-dev.29.18, without a guess. A queue a tool
// added to the device under Kiln is not among them (OptiScaler appends create infos or raises a family's count:
// IFGFeature_Vk.cpp PrepareDevice) and stays unknown or a tool's. A device Kiln did not see being made (adopt_device)
// has no create info to look in: only the queues seen handed out, as before.
//
// The semaphores. With Kiln's frame submitted, the submit waits on the present's semaphores (the game's, or a
// pre-overlay subscriber's when one submitted first) and signals Kiln's own, and the present waits on Kiln's instead:
// each is signalled once and waited on once. When Kiln does not submit, or its submit fails, the present goes on with the
// semaphores it came with, untouched: Vulkan says a vkQueueSubmit that fails with an out-of-memory error leaves the
// semaphores it names as they were, and one that cannot promise that reports the device lost (and then nothing waits
// any more). Kiln's own semaphore was not signalled then, and nothing waits on it. VK_ERROR_UNKNOWN (invalid input or
// a failure in the driver) comes with no such promise: that is why Kiln checks the queue before it submits, and draws
// no more after any failure.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace kui::vk_present {

inline constexpr uint32_t kGraphicsBit = 0x1;  // VK_QUEUE_GRAPHICS_BIT

enum class QueueUse {
    drawable,     // the game's, its family has graphics (or its family's abilities could not be read: as before)
    unknown,      // never seen handed out: its family is not known
    tool,         // handed out while the device was being made: a tool's own (frame generation's present queue)
    no_graphics,  // its family cannot draw
};

// `queues`: the queues seen handed out on the device, with their families; `families`: each family's queue flags
// (vkGetPhysicalDeviceQueueFamilyProperties), empty when they could not be read; `tool`: the ones handed out while the
// device was being made (take_handed).
template <class Queue>
QueueUse classify(const std::map<Queue, uint32_t>& queues, const std::vector<uint32_t>& families, const std::vector<Queue>& tool,
                  Queue queue, uint32_t* family) {
    const auto q = queues.find(queue);
    if (q == queues.end()) return QueueUse::unknown;
    if (family) *family = q->second;
    for (const Queue& t : tool)
        if (t == queue) return QueueUse::tool;
    if (q->second < families.size() && !(families[q->second] & kGraphicsBit)) return QueueUse::no_graphics;
    return QueueUse::drawable;
}

// The semaphores the present waits on.
template <class Semaphore>
struct Waits {
    const Semaphore* semaphores;
    uint32_t count;
};
template <class Semaphore>
Waits<Semaphore> present_waits(bool submitted, const Semaphore* game, uint32_t game_count, const Semaphore* kiln) {
    if (submitted) return {kiln, 1};
    return {game, game_count};
}

// Queues handed out while a device was being made (inside vkCreateDevice: a tool under Kiln that adds queues to the
// device, as frame generation does), before Kiln has the device on its list: the ones of `device` go to its queues, with
// their families, and to its tool queues.
template <class Device, class Queue>
struct Handed {
    Device device;
    uint32_t family;
    Queue queue;
};
// The game (or anyone after the device was made) got `queue` of `family`: known, and not a tool's own any more.
template <class Queue>
void game_got(Queue queue, uint32_t family, std::map<Queue, uint32_t>& queues, std::vector<Queue>& tool) {
    queues[queue] = family;
    std::erase(tool, queue);
}

template <class Device, class Queue>
void take_handed(const std::vector<Handed<Device, Queue>>& handed, Device device, std::map<Queue, uint32_t>& queues,
                 std::vector<Queue>& tool) {
    for (const auto& h : handed)
        if (h.device == device && h.queue) {
            queues[h.queue] = h.family;
            bool known = false;
            for (const Queue& t : tool) known = known || t == h.queue;
            if (!known) tool.push_back(h.queue);
        }
}

// The queues the game asked for when it made its device, as Kiln's vkCreateDevice hook saw the create info: a family and
// how many of its queues. Entries with flags (protected queues) are left out: vkGetDeviceQueue cannot get those.
struct Requested {
    uint32_t family;
    uint32_t count;
};
inline constexpr uint32_t kMaxLookUpPerFamily = 64;  // more than any GPU has in a family

// `Info`: VkDeviceQueueCreateInfo (queueFamilyIndex, queueCount, flags).
template <class Info>
std::vector<Requested> requested_queues(const Info* infos, uint32_t count) {
    std::vector<Requested> out;
    for (uint32_t i = 0; infos && i < count; ++i)
        if (infos[i].flags == 0 && infos[i].queueCount > 0) out.push_back({infos[i].queueFamilyIndex, infos[i].queueCount});
    return out;
}

// The game's own queues, looked up (`get(family, index, &queue)`: the real vkGetDeviceQueue; false when it faulted):
// each becomes known with its family and is not a tool's any more. A queue a tool added under Kiln is not among them.
template <class Queue, class Get>
void look_up_requested(const std::vector<Requested>& requested, Get&& get, std::map<Queue, uint32_t>& queues,
                       std::vector<Queue>& tool) {
    for (const Requested& r : requested)
        for (uint32_t i = 0; i < r.count && i < kMaxLookUpPerFamily; ++i) {
            Queue q{};
            if (get(r.family, i, &q) && q) game_got(q, r.family, queues, tool);
        }
}

}  // namespace kui::vk_present
