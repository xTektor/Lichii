// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

class ContentProvider;
class NupContainer;
struct ContentProviderEntry;

// How Dump RomFS chooses a source. Installed content wins when any readable
// entry exists. A loose NUP is used only when the content provider has none.
enum class RomFSDumpRoute {
    Installed,
    LooseNup,
    Unavailable,
};

[[nodiscard]] inline RomFSDumpRoute RouteRomFSDump(bool has_installed_target,
                                                   bool loose_nup_recognized) {
    if (has_installed_target) {
        return RomFSDumpRoute::Installed;
    }
    if (loose_nup_recognized) {
        return RomFSDumpRoute::LooseNup;
    }
    return RomFSDumpRoute::Unavailable;
}

[[nodiscard]] inline const char* RomFSDumpRouteName(RomFSDumpRoute route) {
    switch (route) {
    case RomFSDumpRoute::Installed:
        return "installed";
    case RomFSDumpRoute::LooseNup:
        return "loose-nup";
    case RomFSDumpRoute::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

struct InstalledRomFSDumpTarget {
    u64 title_id{};
    TitleType title_type{};
    ContentRecordType record_type{};
};

// Installed Program, HTML, legal-info and DLC entries whose base title matches
// `program_id` and whose `readable` callback accepts them. Order matches the
// historical title-id, title-type, record-type sort.
[[nodiscard]] std::vector<InstalledRomFSDumpTarget> ListInstalledRomFSDumpTargets(
    const ContentProvider& installed, u64 program_id,
    const std::function<bool(const ContentProviderEntry&)>& readable);

// The program RomFS a loose NUP already exposes: base directory with the
// newest update overlaid (update paths win). Older updates are not merged.
// Add-on packages are not included. `container` keeps the member files alive.
struct LooseNupRomFS {
    LooseNupRomFS();
    ~LooseNupRomFS();
    LooseNupRomFS(LooseNupRomFS&&) noexcept;
    LooseNupRomFS& operator=(LooseNupRomFS&&) noexcept;
    LooseNupRomFS(const LooseNupRomFS&) = delete;
    LooseNupRomFS& operator=(const LooseNupRomFS&) = delete;

    bool recognized{};
    bool ok{};
    std::string detail;
    u64 title_id{};
    bool update_layered{};
    u32 update_version{};
    VirtualDir root;
    std::unique_ptr<NupContainer> container;
};

[[nodiscard]] LooseNupRomFS OpenLooseNupProgramRomFS(const VirtualFile& file);

// Sum of file sizes in a directory tree. A null directory is zero.
[[nodiscard]] u64 RomFSTreeSize(const VirtualDir& root);

enum class RomFSExportCode {
    Success,
    Cancelled,
    UnsafePath,
    ProtectedPath,
    IoError,
    ShortRead,
    NothingToExport,
};

struct RomFSExportOptions {
    std::filesystem::path destination;
    bool full{true};
    // Host path of the source container. Export refuses to write this file.
    std::filesystem::path protect_source;
    // Export refuses a destination inside any of these directories.
    std::vector<std::filesystem::path> protected_directories;
};

struct RomFSExportResult {
    RomFSExportCode code{RomFSExportCode::NothingToExport};
    std::string detail;
    u64 files_written{};
    u64 directories_written{};
    u64 bytes_written{};
};

// Copy `root` into `options.destination`. Return false from `progress` to cancel.
// Progress is called before writing and after each chunk; `done == total` means finished.
// Names that could escape the destination are rejected. On failure or cancel, only
// files and directories this call created are removed. The source file is never opened
// for writing.
[[nodiscard]] RomFSExportResult ExportRomFSTree(
    const VirtualDir& root, const RomFSExportOptions& options,
    const std::function<bool(u64 done, u64 total)>& progress = {});

} // namespace FileSys
