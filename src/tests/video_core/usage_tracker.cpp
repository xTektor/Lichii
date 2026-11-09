// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"

TEST_CASE("Usage tracker resets clear pages and recently tracked ranges",
          "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{16384};
    constexpr std::array<u64, 5> offsets{64, 256, 4160, 8256, 12352};

    for (u32 cycle = 0; cycle < 8; ++cycle) {
        tracker.Reset();
        tracker.Reset();
        for (const u64 offset : offsets) {
            REQUIRE_FALSE(tracker.IsUsed(offset, 64));
        }
        for (const u64 offset : offsets) {
            tracker.Track(offset, 64);
            tracker.Track(offset, 64);
            REQUIRE(tracker.IsUsed(offset, 64));
        }
    }

    tracker.Reset();
    tracker.Track(4032, 128);
    REQUIRE(tracker.IsUsed(4032, 128));
    REQUIRE(tracker.IsUsed(4096, 64));
    tracker.Reset();
    tracker.Reset();
    REQUIRE_FALSE(tracker.IsUsed(4032, 128));
    REQUIRE_FALSE(tracker.IsUsed(4096, 64));
    tracker.Track(4032, 128);
    REQUIRE(tracker.IsUsed(4096, 64));
}

TEST_CASE("Usage tracker reset and reuse match fresh tracker range semantics",
          "[video_core][usage_tracker]") {
    constexpr u64 PageBytes = 4096;
    constexpr u64 BlockBytes = 64;
    constexpr u64 BlocksPerPage = PageBytes / BlockBytes;
    constexpr u64 PageCount = 4;
    VideoCommon::UsageTracker tracker{PageCount * PageBytes};
    VideoCommon::UsageTracker reference{PageCount * PageBytes};
    std::mt19937 random{0x5e1e};
    const auto range = [&] {
        const u64 first = random() % (PageCount * BlocksPerPage);
        const u64 count = random() % (PageCount * BlocksPerPage - first + 1);
        return std::pair{first, count};
    };

    for (u32 step = 0; step < 4096; ++step) {
        if (random() % 5 == 0) {
            tracker.Reset();
            tracker.Reset();
            reference = VideoCommon::UsageTracker{PageCount * PageBytes};
        } else {
            const auto [first, count] = range();
            tracker.Track(first * BlockBytes, count * BlockBytes);
            reference.Track(first * BlockBytes, count * BlockBytes);
        }
        for (u32 probe = 0; probe < 8; ++probe) {
            const auto [first, count] = range();
            CAPTURE(step, first, count);
            REQUIRE(tracker.IsUsed(first * BlockBytes, count * BlockBytes) ==
                    reference.IsUsed(first * BlockBytes, count * BlockBytes));
        }
    }
}

TEST_CASE("Usage tracker handles empty ranges and page-aligned ends",
          "[video_core][usage_tracker]") {
    constexpr u64 Size = 16384;
    constexpr std::array<std::pair<u64, u64>, 8> ranges{
        {{0, 0}, {0, 64}, {0, 4096}, {4032, 64}, {4032, 128},
         {4096, 4096}, {64, 8192}, {0, Size}}};
    VideoCommon::UsageTracker tracker{Size};
    for (const auto& [offset, size] : ranges) {
        CAPTURE(offset, size);
        tracker.Reset();
        REQUIRE_FALSE(tracker.IsUsed(offset, size));
        tracker.Track(offset, size);
        REQUIRE(tracker.IsUsed(offset, size) == (size != 0));
        for (u64 block = offset; block < offset + size; block += 64) {
            CAPTURE(block);
            REQUIRE(tracker.IsUsed(block, 64));
        }
        tracker.Reset();
        tracker.Reset();
        REQUIRE_FALSE(tracker.IsUsed(offset, size));
        REQUIRE_FALSE(tracker.IsUsed(offset, 0));
        for (u64 block = 0; block < Size; block += 64) {
            CAPTURE(block);
            REQUIRE_FALSE(tracker.IsUsed(block, 64));
        }
        tracker.Track(offset, size);
        REQUIRE(tracker.IsUsed(offset, size) == (size != 0));
    }
}

TEST_CASE("Usage tracker retains sub-block writes and unaligned tails",
          "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{16384};
    constexpr std::array<u64, 10> offsets{0, 1, 63, 64, 100, 4033, 4095, 4096, 4097, 8191};
    for (const u64 offset : offsets) {
        for (u64 size = 1; size < 64; ++size) {
            CAPTURE(offset, size);
            tracker.Reset();
            tracker.Track(offset, 0);
            REQUIRE_FALSE(tracker.IsUsed(offset, 1));
            tracker.Track(offset, size);
            REQUIRE(tracker.IsUsed(offset, size));
            REQUIRE(tracker.IsUsed(offset, 1));
            REQUIRE(tracker.IsUsed(offset + size - 1, 1));
            tracker.Reset();
            REQUIRE_FALSE(tracker.IsUsed(offset, size));
        }
    }

    constexpr std::array<std::pair<u64, u64>, 10> ranges{
        {{63, 2}, {100, 64}, {100, 100}, {100, 4095}, {4090, 100},
         {4095, 1}, {4095, 2}, {4095, 4095}, {0, 4095}, {4096, 4095}}};
    for (const auto& [offset, size] : ranges) {
        CAPTURE(offset, size);
        tracker.Reset();
        tracker.Track(offset, size);
        for (u64 byte = offset; byte < offset + size; ++byte) {
            CAPTURE(byte);
            REQUIRE(tracker.IsUsed(byte, 1));
        }
    }
}

TEST_CASE("Usage tracker never misses an overlapping byte range",
          "[video_core][usage_tracker]") {
    constexpr u64 Size = 16384;
    VideoCommon::UsageTracker tracker{Size};
    std::array<bool, Size> used{};
    std::mt19937 random{0x1004095};
    const auto range = [&] {
        const u64 offset = random() % Size;
        const u64 size = random() % (std::min<u64>(Size - offset, 8192) + 1);
        return std::pair{offset, size};
    };

    for (u32 step = 0; step < 4096; ++step) {
        if (random() % 5 == 0) {
            tracker.Reset();
            tracker.Reset();
            used.fill(false);
            REQUIRE_FALSE(tracker.IsUsed(0, Size));
        } else {
            const auto [offset, size] = range();
            tracker.Track(offset, size);
            std::fill_n(used.begin() + offset, size, true);
            if (size != 0) {
                CAPTURE(step, offset, size);
                REQUIRE(tracker.IsUsed(offset, 1));
                REQUIRE(tracker.IsUsed(offset + size - 1, 1));
            }
        }
        for (u32 probe = 0; probe < 8; ++probe) {
            const auto [offset, size] = range();
            const bool overlaps = std::any_of(used.begin() + offset, used.begin() + offset + size,
                                              [](bool written) { return written; });
            CAPTURE(step, offset, size);
            REQUIRE((!overlaps || tracker.IsUsed(offset, size)));
        }
    }
}

namespace {

// UsageTracker as it was before full pages were skipped.
class ReferenceTracker {
    static constexpr size_t BUFFER_PAGE_SHIFT = 12;
    static constexpr size_t BUFFER_PAGE_BYTES = size_t{1} << BUFFER_PAGE_SHIFT;

public:
    explicit ReferenceTracker(size_t size) : pages((size >> BUFFER_PAGE_SHIFT) + 1, 0) {}

    void Track(u64 offset, u64 size) {
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            pages[i] = ~u64{0};
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    bool IsUsed(u64 offset, u64 size) const {
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return false;
        }
        if (IsPageUsed(page, offset, size)) {
            return true;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            if (pages[i] != 0) {
                return true;
            }
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        return IsPageUsed(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

private:
    static u64 Mask(u64 offset, u64 size) {
        const size_t first_bit = (offset % BUFFER_PAGE_BYTES) >> 6;
        const size_t num_bits = (std::min<size_t>)(size, BUFFER_PAGE_BYTES) >> 6;
        return (~u64{0} >> (64 - num_bits)) << first_bit;
    }

    void TrackPage(u64 page, u64 offset, u64 size) {
        pages[page] |= Mask(offset, size);
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const {
        return (pages[page] & Mask(offset, size)) != 0;
    }

    std::vector<u64> pages;
};

} // Anonymous namespace

TEST_CASE("UsageTracker skips full pages without changing what it tracks",
          "[video_core][usage_tracker]") {
    constexpr size_t buffer_size = 4 << 20;
    std::mt19937_64 rng{1234};
    for (int round = 0; round < 20; ++round) {
        VideoCommon::UsageTracker tracker{buffer_size};
        ReferenceTracker reference{buffer_size};
        if (round % 2 == 1) {
            tracker.Reset();
        }
        std::uniform_int_distribution<u64> offset_dist{0, buffer_size - 1};
        for (int i = 0; i < 300; ++i) {
            // Sizes that end less than 64 bytes into a page shift by 64 in TrackPage: skip them.
            const u64 offset = offset_dist(rng) & ~u64{63};
            const u64 size = std::uniform_int_distribution<u64>{64, 1 << 20}(rng) & ~u64{63};
            if (offset + size >= buffer_size || (offset + size) % 4096 < 64) {
                continue;
            }
            tracker.Track(offset, size);
            reference.Track(offset, size);
        }
        for (u64 page = 0; page < (buffer_size >> 12); ++page) {
            for (u64 chunk = 0; chunk < 63; chunk += 7) {
                const u64 offset = (page << 12) + (chunk << 6);
                REQUIRE(tracker.IsUsed(offset, 64) == reference.IsUsed(offset, 64));
            }
        }
    }
}

TEST_CASE("UsageTracker forgets full pages on reset", "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{1 << 20};
    tracker.Track(0, 1 << 19);
    REQUIRE(tracker.IsUsed(1 << 18, 64));
    tracker.Reset();
    REQUIRE_FALSE(tracker.IsUsed(1 << 18, 64));
    tracker.Track(0, 1 << 19);
    REQUIRE(tracker.IsUsed(1 << 18, 64));
}
