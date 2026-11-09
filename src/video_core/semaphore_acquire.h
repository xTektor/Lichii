// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCommon {

/// How the split's front may read a semaphore word.
/// A word that already matches is not enough: a queued GPU release can still be the one the
/// acquire is ordered behind, and a `>=` compare is true of the previous payload.
enum class SemaphoreRead : u32 {
    /// No GPU release is queued, its host write has landed, and the buffer cache is clean.
    Memory,
    /// A release is queued at `wait_position`. Wait for that, then decide again.
    WaitForRelease,
    /// The word may be in the buffer cache or an unfinished host query. Drain and flush.
    Drain,
};

struct SemaphoreReadPlan {
    SemaphoreRead action{SemaphoreRead::Drain};
    u64 wait_position{};
};

/// `consumed` is how far the back has executed. `fence_position` / `async_position` are the newest
/// noted GPU releases of this word (0 if none). `reports_settled` is false while a host query
/// write of the word has been issued and not completed. `buffer_dirty` is a GPU-modified cache.
constexpr SemaphoreReadPlan PlanSemaphoreRead(u64 consumed, u64 fence_position, u64 async_position,
                                              bool reports_settled, bool buffer_dirty) {
    const u64 queued = fence_position > async_position ? fence_position : async_position;
    if (queued > consumed) {
        return {SemaphoreRead::WaitForRelease, queued};
    }
    if (!reports_settled || buffer_dirty) {
        return {SemaphoreRead::Drain, 0};
    }
    return {SemaphoreRead::Memory, 0};
}

} // namespace VideoCommon
