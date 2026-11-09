// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_perf.h"

namespace VideoCommon {

class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
public:
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
        full_pages.resize((num_pages + 63) / 64, 0ULL);
    }

    void Reset() noexcept {
        if (!tracked) {
            return;
        }
        tracked = false;
        std::ranges::fill(pages, 0ULL);
        std::ranges::fill(full_pages, 0ULL);
        recent.fill({~u64{0}, 0});
        full_begin = 0;
        full_end = 0;
    }

    void Track(u64 offset, u64 size) noexcept {
        // Tracking is idempotent until Reset: skip ranges tracked just before. Draws bind the
        // same (often multi-megabyte) storage buffers again and again, and each Track wrote a
        // word per 4 KiB of the range.
        for (const auto& [recent_offset, recent_size] : recent) {
            if (recent_offset == offset && recent_size == size) {
                return;
            }
        }
        tracked = true;
        recent[recent_cursor++ % recent.size()] = {offset, size};
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        FillPages(page + 1, page_end);
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
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

    [[nodiscard]] std::span<const u64> Pages() const noexcept {
        return pages;
    }

private:
    void FillPages(size_t begin, size_t end) noexcept {
        if (begin >= end) {
            return;
        }
        // Pages in [full_begin, full_end) are all ones already (only Reset clears a page): fill
        // around that run and grow it. Keeps tracking a multi-megabyte binding O(1) once filled.
        if (end < full_begin || begin > full_end) {
            FillPageBits(begin, end);
            if (end - begin > full_end - full_begin) {
                full_begin = begin;
                full_end = end;
            }
            return;
        }
        if (Nx2Perf::Verify()) {
            static std::atomic<u32> logged{};
            const size_t check_begin = (std::max)(begin, full_begin);
            const size_t check_end = (std::min)(end, full_end);
            if (!std::all_of(pages.begin() + check_begin, pages.begin() + check_end,
                             [](u64 page) { return page == ~u64{0}; }) &&
                logged.fetch_add(1, std::memory_order_relaxed) < 64) {
                LOG_ERROR(HW_GPU, "Nx2Perf draw-binds: usage pages {}..{} not all tracked",
                          check_begin, check_end);
            }
            std::fill(pages.begin() + begin, pages.begin() + end, ~u64{0});
            MarkFull(begin, end);
        } else {
            if (begin < full_begin) {
                FillPageBits(begin, full_begin);
            }
            if (end > full_end) {
                FillPageBits(full_end, end);
            }
        }
        full_begin = (std::min)(begin, full_begin);
        full_end = (std::max)(end, full_end);
    }

    /// EDEN_NX2_USAGE_TRACKER_SKIP_FULL=0 writes every page of a range again, as before.
    static bool SkipFullPages() noexcept {
        static const bool skip = [] {
            const char* value = std::getenv("EDEN_NX2_USAGE_TRACKER_SKIP_FULL");
            return !(value && value[0] == '0');
        }();
        return skip;
    }

    /// Marks pages [begin, end) fully used. Draws bind the same multi-megabyte storage buffers
    /// many times between resets, often several disjoint ones, so besides the run above one bit
    /// per page remembers pages already full and 64 of them at a time are skipped.
    void FillPageBits(size_t begin, size_t end) noexcept {
        if (!SkipFullPages()) {
            std::fill(pages.begin() + begin, pages.begin() + end, ~u64{0});
            MarkFull(begin, end);
            return;
        }
        for (size_t i = begin; i < end;) {
            const size_t word = i >> 6;
            const size_t bit = i & 63;
            const size_t count = (std::min<size_t>)(64 - bit, end - i);
            const u64 mask = (count == 64 ? ~u64{0} : (u64{1} << count) - 1) << bit;
            if ((full_pages[word] & mask) != mask) {
                std::fill_n(pages.begin() + i, count, ~u64{0});
                full_pages[word] |= mask;
            }
            i += count;
        }
    }

    void MarkFull(size_t begin, size_t end) noexcept {
        for (size_t i = begin; i < end;) {
            const size_t word = i >> 6;
            const size_t bit = i & 63;
            const size_t count = (std::min<size_t>)(64 - bit, end - i);
            full_pages[word] |= (count == 64 ? ~u64{0} : (u64{1} << count) - 1) << bit;
            i += count;
        }
    }

    void TrackPage(u64 page, u64 offset, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t bytes = std::min<size_t>(size, BUFFER_PAGE_BYTES - offset_in_page);
        const size_t last_bit = (offset_in_page + bytes - 1) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = last_bit - first_bit + 1;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        pages[page] |= (~u64{0} & mask) << first_bit;
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const noexcept {
        if (size == 0) {
            return false;
        }
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t bytes = std::min<size_t>(size, BUFFER_PAGE_BYTES - offset_in_page);
        const size_t last_bit = (offset_in_page + bytes - 1) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = last_bit - first_bit + 1;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        const size_t mask2 = (~u64{0} & mask) << first_bit;
        return (pages[page] & mask2) != 0;
    }

private:
    std::vector<u64> pages;
    /// One bit per page: set only when that page is all ones in pages.
    std::vector<u64> full_pages;
    /// Ranges tracked last, (offset, size).
    std::array<std::pair<u64, u64>, 4> recent{{{~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}}};
    size_t recent_cursor{};
    /// Pages known to be all ones (FillPages).
    size_t full_begin{};
    size_t full_end{};
    bool tracked{};
};

} // namespace VideoCommon
