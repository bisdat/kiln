// SPDX-License-Identifier: MIT
// input_attach configures the repeat window on the render thread while the window thread handles keys.
// Run with ThreadSanitizer to check that sharing; held() is read from other threads too.
#include "../key_hold.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    kui::hold::Keys keys;
    std::atomic<bool> ready{false};
    std::thread render([&] {
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 100000; ++i) {
            keys.set_window(i % 2 ? 1500 : 1750);
            (void)keys.held('J');
        }
    });
    ready.store(true, std::memory_order_release);
    for (uint64_t i = 0; i < 100000; ++i) {
        const auto window = keys.window();
        assert(window == 1500 || window == 1750);
        assert(!keys.down('J', kui::hold::Prev::unknown, i * 2 + 1).repeat);
        keys.up('J', i * 2 + 2);
    }
    render.join();
    std::puts("PASS repeat-window configuration and key events on separate threads");
}
