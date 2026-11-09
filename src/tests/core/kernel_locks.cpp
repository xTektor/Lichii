// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/kernel/k_spin_lock.h"
#include "core/hle/kernel/k_thread.h"

TEST_CASE("KThreadContextGuard admits one holder at a time", "[kernel]") {
    Kernel::KThreadContextGuard guard;
    REQUIRE(guard.try_lock());
    REQUIRE_FALSE(guard.try_lock());
    guard.unlock();
    REQUIRE(guard.try_lock());
    guard.unlock();
}

TEST_CASE("KSpinLock excludes contending threads past its spin budget", "[kernel]") {
    Kernel::KSpinLock lock;
    constexpr int Threads = 4;
    constexpr int Iterations = 20000;
    int counter = 0;
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < Iterations; ++i) {
                Kernel::KScopedSpinLock lk{lock};
                if (inside.fetch_add(1, std::memory_order_relaxed) != 0) {
                    overlap = true;
                }
                // Hold it long enough, now and then, that waiters run out of spins and sleep.
                if (i % 1000 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds{50});
                }
                ++counter;
                inside.fetch_sub(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE_FALSE(overlap);
    REQUIRE(counter == Threads * Iterations);
    REQUIRE(lock.TryLock());
    lock.Unlock();
}
