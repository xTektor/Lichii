// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

/// The host memory the transcode cache may use: a sixteenth of the machine's (a thirty-second on
/// Android, where the GPU shares it), from 256 MiB to 2 GiB.
[[nodiscard]] constexpr u64 TranscodeCacheBudget(u64 total_physical_memory, bool android = false) {
    constexpr u64 MiB = u64{1} << 20;
    const u64 share = total_physical_memory / (android ? 32 : 16);
    return share < 256 * MiB ? 256 * MiB : (share > 2048 * MiB ? 2048 * MiB : share);
}

/// Host memory copies of textures the CPU transcoded (ASTC to BC1/BC3 or RGBA8), by the hash of
/// their guest data and layout. On a card whose memory cannot hold a title's working set the
/// collector keeps evicting the same textures, and each came back as a new decode: in Ocarina of
/// Time on a 4 GB card nearly every decode while walking repeated content decoded before, and the
/// transcode threads took a third of the process's CPU time. A hit costs a copy instead.
/// Least recently used entries go first once the budget is reached. Thread safe.
class TranscodeCache {
public:
    using Data = std::shared_ptr<const std::vector<u8>>;

    explicit TranscodeCache(u64 budget_bytes_ = 0) : budget_bytes{budget_bytes_} {}

    void SetBudget(u64 bytes) {
        std::scoped_lock lock{mutex};
        budget_bytes = bytes;
        Trim();
    }

    [[nodiscard]] u64 Budget() const {
        std::scoped_lock lock{mutex};
        return budget_bytes;
    }

    /// The data stored for key, if it is there and has the expected size.
    [[nodiscard]] Data Find(u64 key, size_t size) {
        std::scoped_lock lock{mutex};
        const auto it = entries.find(key);
        if (it == entries.end() || it->second.data->size() != size) {
            ++misses;
            return nullptr;
        }
        order.splice(order.begin(), order, it->second.position);
        ++hits;
        return it->second.data;
    }

    /// Stores data for key, replacing what was there, unless it alone exceeds the budget.
    void Insert(u64 key, std::vector<u8> data) {
        std::scoped_lock lock{mutex};
        if (data.size() > budget_bytes) {
            return;
        }
        if (const auto it = entries.find(key); it != entries.end()) {
            used_bytes -= it->second.data->size();
            order.erase(it->second.position);
            entries.erase(it);
        }
        used_bytes += data.size();
        order.push_front(key);
        entries.emplace(key, Entry{std::make_shared<const std::vector<u8>>(std::move(data)),
                                   order.begin()});
        Trim();
    }

    [[nodiscard]] u64 UsedBytes() const {
        std::scoped_lock lock{mutex};
        return used_bytes;
    }

    [[nodiscard]] size_t Size() const {
        std::scoped_lock lock{mutex};
        return entries.size();
    }

    /// Hits and misses since the last call.
    [[nodiscard]] std::pair<u64, u64> TakeCounts() {
        std::scoped_lock lock{mutex};
        return {std::exchange(hits, 0), std::exchange(misses, 0)};
    }

private:
    struct Entry {
        Data data;
        std::list<u64>::iterator position;
    };

    void Trim() {
        while (used_bytes > budget_bytes && !order.empty()) {
            const auto it = entries.find(order.back());
            used_bytes -= it->second.data->size();
            entries.erase(it);
            order.pop_back();
        }
    }

    mutable std::mutex mutex;
    std::unordered_map<u64, Entry> entries;
    std::list<u64> order; ///< Most recently used first
    u64 budget_bytes;
    u64 used_bytes = 0;
    u64 hits = 0;
    u64 misses = 0;
};

} // namespace VideoCommon
