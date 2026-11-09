// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/transcode_cache.h"

namespace {
using VideoCommon::TranscodeCache;
using VideoCommon::TranscodeCacheBudget;

std::vector<u8> Bytes(size_t size, u8 value) {
    return std::vector<u8>(size, value);
}
} // namespace

TEST_CASE("Transcode cache returns stored data of the expected size", "[video_core][transcode]") {
    TranscodeCache cache{1024};
    cache.Insert(1, Bytes(100, 7));
    const auto hit = cache.Find(1, 100);
    REQUIRE(hit != nullptr);
    REQUIRE(hit->size() == 100);
    REQUIRE((*hit)[99] == 7);
    // Another size under the same key is a miss, as is an unknown key.
    REQUIRE(cache.Find(1, 99) == nullptr);
    REQUIRE(cache.Find(2, 100) == nullptr);
    REQUIRE(cache.TakeCounts() == std::pair<u64, u64>{1, 2});
    REQUIRE(cache.TakeCounts() == std::pair<u64, u64>{0, 0});
}

TEST_CASE("Transcode cache drops the least recently used entries past its budget",
          "[video_core][transcode]") {
    TranscodeCache cache{300};
    cache.Insert(1, Bytes(100, 1));
    cache.Insert(2, Bytes(100, 2));
    cache.Insert(3, Bytes(100, 3));
    REQUIRE(cache.UsedBytes() == 300);
    // Using 1 makes 2 the oldest.
    REQUIRE(cache.Find(1, 100) != nullptr);
    cache.Insert(4, Bytes(100, 4));
    REQUIRE(cache.Find(2, 100) == nullptr);
    REQUIRE(cache.Find(1, 100) != nullptr);
    REQUIRE(cache.Find(3, 100) != nullptr);
    REQUIRE(cache.Find(4, 100) != nullptr);
    REQUIRE(cache.Size() == 3);
    // Data held by a reader stays valid after its entry goes.
    const auto held = cache.Find(3, 100);
    cache.SetBudget(100);
    REQUIRE(cache.UsedBytes() <= 100);
    REQUIRE((*held)[0] == 3);
}

TEST_CASE("Transcode cache replaces a key and skips data larger than its budget",
          "[video_core][transcode]") {
    TranscodeCache cache{200};
    cache.Insert(1, Bytes(100, 1));
    cache.Insert(1, Bytes(150, 2));
    REQUIRE(cache.Size() == 1);
    REQUIRE(cache.UsedBytes() == 150);
    REQUIRE(cache.Find(1, 150) != nullptr);
    cache.Insert(2, Bytes(201, 3));
    REQUIRE(cache.Find(2, 201) == nullptr);
    REQUIRE(cache.Find(1, 150) != nullptr);
}

TEST_CASE("Transcode cache budget follows the machine's memory", "[video_core][transcode]") {
    constexpr u64 GiB = u64{1} << 30;
    constexpr u64 MiB = u64{1} << 20;
    REQUIRE(TranscodeCacheBudget(2 * GiB) == 256 * MiB);
    REQUIRE(TranscodeCacheBudget(16 * GiB) == 1 * GiB);
    REQUIRE(TranscodeCacheBudget(32 * GiB) == 2 * GiB);
    REQUIRE(TranscodeCacheBudget(64 * GiB) == 2 * GiB);
    REQUIRE(TranscodeCacheBudget(16 * GiB, true) == 512 * MiB);
}
