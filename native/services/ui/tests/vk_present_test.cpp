// SPDX-License-Identifier: MIT
// vk_present.h on the host: which present queue the overlay draws on, the queues a tool hands out while the game's
// device is made, and the semaphores of one present through Kiln's hook (inbox 80: OptiScaler's frame generation
// presented from its own queue, Kiln's submit there failed and the game died).
#include "../vk_present.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace kui::vk_present;

namespace {
unsigned checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        assert(false);
    }
}

// Binary semaphores and one queue, as far as one present through Kiln's hook goes: a wait needs a signalled
// semaphore and unsignals it; a signal needs an unsignalled one.
struct Model {
    std::map<int, bool> signalled;
    std::vector<std::string> errors;
    void wait(int s) {
        if (!signalled[s]) errors.push_back("waits on unsignalled " + std::to_string(s));
        signalled[s] = false;
    }
    void signal(int s) {
        if (signalled[s]) errors.push_back("signals signalled " + std::to_string(s));
        signalled[s] = true;
    }
};

enum class Submit { none, ok, failed };

// The game's frame signalled `game`; Kiln may submit (waiting on them, signalling `kiln`); then the present waits on what
// present_waits says. Returns the model after the present.
Model one_present(const std::vector<int>& game, int kiln, Submit submit) {
    Model m;
    for (int s : game) m.signal(s);  // the game's rendering
    if (submit == Submit::ok) {
        for (int s : game) m.wait(s);
        m.signal(kiln);
    }
    // Submit::failed: Vulkan leaves the semaphores of a failed vkQueueSubmit as they were (or the device is lost)
    const auto w = present_waits(submit == Submit::ok, game.data(), static_cast<uint32_t>(game.size()), &kiln);
    for (uint32_t i = 0; i < w.count; ++i) m.wait(w.semaphores[i]);
    return m;
}

bool all_unsignalled(const Model& m) {
    for (const auto& [s, on] : m.signalled)
        if (on) return false;
    return true;
}
}  // namespace

int main() {
    // queues: the game's graphics queue (family 0), frame generation's present queue (family 2: compute only)
    const std::vector<uint32_t> nvidia = {0x1 | 0x2 | 0x4 | 0x8, 0x4, 0x2 | 0x4, 0x20};  // graphics+compute+transfer, transfer, compute, video
    std::map<int, uint32_t> queues = {{10, 0}};
    const std::vector<int> none;
    uint32_t family = 99;
    check(classify(queues, nvidia, none, 10, &family) == QueueUse::drawable && family == 0, "the game's own graphics queue is drawn on");
    check(classify(queues, nvidia, none, 20, &family) == QueueUse::unknown, "a queue never seen handed out is not drawn on");
    queues[20] = 2;
    check(classify(queues, nvidia, none, 20, &family) == QueueUse::no_graphics && family == 2, "a compute queue cannot draw");
    queues[21] = 1;
    check(classify(queues, nvidia, none, 21, &family) == QueueUse::no_graphics && family == 1, "a transfer queue cannot draw");
    check(classify(queues, {}, none, 20, &family) == QueueUse::drawable, "flags that could not be read: drawn as before");
    queues[22] = 7;
    check(classify(queues, nvidia, none, 22, &family) == QueueUse::drawable, "a family past the list read: drawn as before");

    // queues a tool hands out while the device is being made (FSR frame generation's present queue on a compute
    // family, Streamline's graphics queue for DLSS frame generation): the device's own go to it, another device's do not;
    // none is drawn on, whatever its family, and the game's own queue still is
    std::vector<Handed<int, int>> handed = {{1, 2, 20}, {1, 0, 23}, {2, 0, 30}, {1, 0, 0}, {1, 2, 20}};
    std::map<int, uint32_t> device_queues = {{10, 0}};
    std::vector<int> tool;
    take_handed(handed, 1, device_queues, tool);
    check(device_queues.size() == 3 && device_queues[20] == 2 && device_queues[23] == 0, "the made device's queues, with their families");
    check(tool.size() == 2, "each tool queue once, none of another device");
    check(classify(device_queues, nvidia, tool, 20, &family) == QueueUse::tool && family == 2, "FSR frame generation's present queue: not drawn on");
    check(classify(device_queues, nvidia, tool, 23, &family) == QueueUse::tool && family == 0,
          "Streamline's own graphics queue: not drawn on either");
    check(classify(device_queues, nvidia, tool, 10, &family) == QueueUse::drawable, "the game's queue next to them: drawn on");
    // FFX also takes the game's own queue while the device is made; the game asks for it right after: drawn on again
    std::vector<Handed<int, int>> with_game = {{1, 0, 10}, {1, 2, 20}};
    std::map<int, uint32_t> q2;
    std::vector<int> t2;
    take_handed(with_game, 1, q2, t2);
    check(classify(q2, nvidia, t2, 10, &family) == QueueUse::tool, "before the game asks, the game's queue counts as the tool's");
    game_got(10, 0u, q2, t2);
    check(classify(q2, nvidia, t2, 10, &family) == QueueUse::drawable && t2.size() == 1, "the game asked for it: its own again");
    check(classify(q2, nvidia, t2, 20, &family) == QueueUse::tool, "the tool's present queue stays the tool's");

    // The lookup among the queues the game asked for (present_queue: a present from a queue Kiln did not see the game
    // get, or saw only during the device's creation). The device: family 0 (graphics) with 2 queues, family 2 (compute)
    // with 1, a protected family 0 entry; a tool under Kiln appended family 2 index 1 (OptiScaler raises the family's
    // count) and family 1 index 0 for itself.
    {
        struct Info {
            uint32_t queueFamilyIndex, queueCount, flags;
        };
        const Info infos[] = {{0, 2, 0}, {2, 1, 0}, {0, 1, 1}, {3, 0, 0}};
        const auto requested = requested_queues(infos, 4);
        check(requested.size() == 2 && requested[0].family == 0 && requested[0].count == 2 && requested[1].family == 2,
              "the create info's entries without flags and with queues");
        check(requested_queues<Info>(nullptr, 3).empty(), "no create info: nothing to look up");
        // the driver's queues: (family, index) -> handle
        const std::map<std::pair<uint32_t, uint32_t>, int> driver = {{{0, 0}, 10}, {{0, 1}, 11}, {{2, 0}, 40}, {{2, 1}, 41}, {{1, 0}, 50}};
        std::vector<std::pair<uint32_t, uint32_t>> asked;
        auto get = [&](uint32_t f, uint32_t i, int* q) {
            asked.push_back({f, i});
            const auto it = driver.find({f, i});
            *q = it == driver.end() ? 0 : it->second;
            return true;
        };
        // a normal game whose vkGetDeviceQueue went past Kiln's hook: Kiln saw no queue at all; its present queue is its own
        std::map<int, uint32_t> seen;
        std::vector<int> tools;
        check(classify(seen, nvidia, tools, 10, &family) == QueueUse::unknown, "before the lookup: not known");
        look_up_requested(requested, get, seen, tools);
        check(asked.size() == 3 && asked[2] == std::make_pair(2u, 0u), "only the families and indices the game asked for");
        check(classify(seen, nvidia, tools, 10, &family) == QueueUse::drawable && family == 0, "the game's queue: drawn on, family 0");
        check(classify(seen, nvidia, tools, 11, &family) == QueueUse::drawable && family == 0, "its second queue of family 0 too");
        check(classify(seen, nvidia, tools, 40, &family) == QueueUse::no_graphics && family == 2, "its compute queue: known, cannot draw");
        check(classify(seen, nvidia, tools, 41, &family) == QueueUse::unknown, "the tool's appended index: still not known");
        check(classify(seen, nvidia, tools, 50, &family) == QueueUse::unknown, "the tool's own family: still not known");
        // the tool under Kiln got the game's queue and its own while the device was made (FFX's game queue and present
        // queue); the game's own get went past Kiln: the lookup gives the game its queue back, the tool keeps its own
        std::map<int, uint32_t> made;
        std::vector<int> made_tools;
        std::vector<Handed<int, int>> during = {{1, 0, 10}, {1, 2, 41}, {1, 1, 50}};
        take_handed(during, 1, made, made_tools);
        check(classify(made, nvidia, made_tools, 10, &family) == QueueUse::tool, "before the lookup: the tool's");
        look_up_requested(requested, get, made, made_tools);
        check(classify(made, nvidia, made_tools, 10, &family) == QueueUse::drawable && family == 0, "the game's queue: its own again");
        check(classify(made, nvidia, made_tools, 41, &family) == QueueUse::tool && family == 2, "the tool's present queue: the tool's");
        check(classify(made, nvidia, made_tools, 50, &family) == QueueUse::tool && family == 1, "the tool's queue of its own family: the tool's");
        // a lookup that faulted or got nothing changes nothing; a family's count is bounded
        std::map<int, uint32_t> none_seen;
        std::vector<int> none_tools;
        look_up_requested(requested, [](uint32_t, uint32_t, int* q) { *q = 0; return true; }, none_seen, none_tools);
        look_up_requested(requested, [](uint32_t, uint32_t, int* q) { *q = 99; return false; }, none_seen, none_tools);
        check(none_seen.empty(), "a null queue or a faulted call is not a queue");
        unsigned calls = 0;
        look_up_requested(std::vector<Requested>{{0, 1000}}, [&](uint32_t, uint32_t, int* q) { ++calls; *q = 0; return true; },
                          none_seen, none_tools);
        check(calls == kMaxLookUpPerFamily, "at most kMaxLookUpPerFamily queues of one family");
    }

    // the semaphores of one present: each signalled once and waited on once, nothing left signalled
    for (const auto& game : std::vector<std::vector<int>>{{1}, {1, 2}, {}}) {
        for (Submit s : {Submit::none, Submit::ok, Submit::failed}) {
            const Model m = one_present(game, 100, s);
            check(m.errors.empty(), "no wait on an unsignalled semaphore, no second signal");
            check(all_unsignalled(m), "nothing left signalled for a later frame");
        }
    }
    // the model catches both wrong hand-offs: Kiln's semaphore after a failed submit (never signalled), the game's after
    // Kiln's submit went in (already waited on)
    {
        Model failed;
        failed.signal(1);
        failed.wait(100);
        check(!failed.errors.empty(), "a present on Kiln's semaphore after a failed submit waits on one never signalled");
        Model twice;
        twice.signal(1);
        twice.wait(1);
        twice.signal(100);
        twice.wait(1);
        check(!twice.errors.empty() && twice.signalled[100], "a present on the game's semaphore after Kiln's submit waits twice");
    }
    std::printf("vk_present: %u checks passed\n", checks);
}
