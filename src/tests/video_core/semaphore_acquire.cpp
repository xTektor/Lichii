// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/semaphore_acquire.h"

using VideoCommon::PlanSemaphoreRead;
using VideoCommon::SemaphoreRead;

TEST_CASE("A CPU semaphore with no queued GPU release is read from memory", "[gpu][acquire]") {
    const auto plan = PlanSemaphoreRead(100, 0, 0, true, false);
    CHECK(plan.action == SemaphoreRead::Memory);
}

TEST_CASE("A matching word still waits while a GPU release is queued", "[gpu][acquire]") {
    // The previous payload already satisfies >=. The acquire is ordered behind the queued one.
    const auto plan = PlanSemaphoreRead(100, 140, 0, true, false);
    CHECK(plan.action == SemaphoreRead::WaitForRelease);
    CHECK(plan.wait_position == 140);
    const auto after = PlanSemaphoreRead(140, 140, 0, true, false);
    CHECK(after.action == SemaphoreRead::Memory);
}

TEST_CASE("An unfinished host query or a dirty cache drains", "[gpu][acquire]") {
    CHECK(PlanSemaphoreRead(50, 40, 0, false, false).action == SemaphoreRead::Drain);
    CHECK(PlanSemaphoreRead(50, 40, 0, true, true).action == SemaphoreRead::Drain);
}

TEST_CASE("The newer of a fence release and an async release is the one waited for",
          "[gpu][acquire]") {
    const auto plan = PlanSemaphoreRead(10, 20, 30, true, false);
    CHECK(plan.action == SemaphoreRead::WaitForRelease);
    CHECK(plan.wait_position == 30);
}
