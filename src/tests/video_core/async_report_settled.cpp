// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

TEST_CASE("Async report slots settle when every issued report completed", "[nx2][dma]") {
    const u64 address = 0x12000;
    const u64 slot = Tegra::Nx2AsyncReportSlot(address);
    const u32 issued = Tegra::g_nx2_async_reports_issued[slot].load();
    const u32 completed = Tegra::g_nx2_async_reports_completed[slot].load();
    REQUIRE(Tegra::Nx2AsyncReportsSettled(address, 16));

    Tegra::g_nx2_async_reports_issued[slot].fetch_add(1);
    CHECK_FALSE(Tegra::Nx2AsyncReportsSettled(address, 16));
    // A neighbouring 16-byte slot is a different report.
    CHECK(Tegra::Nx2AsyncReportsSettled(address + 16, 16));

    Tegra::g_nx2_async_reports_completed[slot].fetch_add(1);
    CHECK(Tegra::Nx2AsyncReportsSettled(address, 16));

    Tegra::g_nx2_async_reports_issued[slot].store(issued);
    Tegra::g_nx2_async_reports_completed[slot].store(completed);
}

TEST_CASE("An empty async-report range is settled", "[nx2][dma]") {
    CHECK(Tegra::Nx2AsyncReportsSettled(0x34000, 0));
}
