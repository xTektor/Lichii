// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/romfs_export.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <system_error>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/registered_cache.h"
#include "core/loader/loader.h"

namespace FileSys {
namespace {

constexpr int MaxDirectoryDepth = 128;
constexpr std::size_t CopyChunk = 1024 * 1024;

namespace fs = std::filesystem;

bool IsReservedWindowsName(std::string_view name) {
    const auto dot = name.find('.');
    const auto stem = name.substr(0, dot == std::string_view::npos ? name.size() : dot);
    if (stem.empty()) {
        return false;
    }
    std::string upper(stem);
    for (char& character : upper) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    static constexpr std::array<const char*, 22> Reserved{{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    }};
    return std::any_of(Reserved.begin(), Reserved.end(), [&](const char* reserved) {
        return upper == reserved;
    });
}

bool IsSafeComponent(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    if (name.back() == ' ' || name.back() == '.') {
        return false;
    }
    for (const unsigned char character : name) {
        if (character < 0x20 || character == '/' || character == '\\' || character == ':' ||
            character == '*' || character == '?' || character == '"' || character == '<' ||
            character == '>' || character == '|') {
            return false;
        }
    }
    return !IsReservedWindowsName(name);
}

fs::path HostIdentity(const fs::path& path) {
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    const auto base = (error ? path : absolute).lexically_normal();
    const auto canonical = fs::weakly_canonical(base, error);
    return (error ? base : canonical).lexically_normal();
}

std::string ToUtf8(const fs::path& path) {
    return Common::FS::PathToUTF8String(path);
}

fs::path ComponentPath(std::string_view name) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(name.data()), name.size()}};
}

bool IsSameOrInside(const fs::path& root, const fs::path& path) {
    if (root.empty()) {
        return false;
    }
    const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty()) {
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string JoinRelative(const std::string& prefix, std::string_view name) {
    if (prefix.empty()) {
        return std::string{name};
    }
    return prefix + "/" + std::string{name};
}

struct TreeStats {
    u64 bytes{};
    u64 files{};
};

std::optional<std::string> ValidateTree(const VirtualDir& dir, const std::string& prefix, int depth,
                                        TreeStats& stats) {
    if (dir == nullptr) {
        return "RomFS directory is missing.";
    }
    if (depth > MaxDirectoryDepth) {
        return "RomFS directory is nested more than 128 levels at " +
               (prefix.empty() ? std::string{"/"} : prefix);
    }
    if (!dir->IsReadable()) {
        return "RomFS directory is not readable: " + (prefix.empty() ? std::string{"/"} : prefix);
    }
    for (const auto& file : dir->GetFiles()) {
        if (file == nullptr) {
            return "RomFS directory contains a missing file at " +
                   (prefix.empty() ? std::string{"/"} : prefix);
        }
        const auto name = file->GetName();
        if (!IsSafeComponent(name)) {
            return "RomFS file name cannot be exported safely: " + JoinRelative(prefix, name);
        }
        if (!file->IsReadable()) {
            return "RomFS file is not readable: " + JoinRelative(prefix, name);
        }
        stats.bytes += file->GetSize();
        ++stats.files;
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub == nullptr) {
            return "RomFS directory contains a missing subdirectory at " +
                   (prefix.empty() ? std::string{"/"} : prefix);
        }
        const auto name = sub->GetName();
        if (!IsSafeComponent(name)) {
            return "RomFS directory name cannot be exported safely: " + JoinRelative(prefix, name);
        }
        if (const auto error = ValidateTree(sub, JoinRelative(prefix, name), depth + 1, stats)) {
            return error;
        }
    }
    return std::nullopt;
}

u64 TreeSize(const VirtualDir& dir) {
    if (dir == nullptr) {
        return 0;
    }
    u64 bytes = 0;
    for (const auto& file : dir->GetFiles()) {
        if (file != nullptr) {
            bytes += file->GetSize();
        }
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        bytes += TreeSize(sub);
    }
    return bytes;
}

struct CopyState {
    const RomFSExportOptions* options{};
    const std::function<bool(u64, u64)>* progress{};
    fs::path destination;
    fs::path source;
    u64 total{};
    RomFSExportResult result;
    std::vector<fs::path> created_files;
    std::vector<fs::path> created_dirs;
};

bool Cancelled(CopyState& state) {
    return state.progress != nullptr && *state.progress &&
           !(*state.progress)(state.result.bytes_written, state.total);
}

void Rollback(CopyState& state) {
    std::error_code error;
    for (auto it = state.created_files.rbegin(); it != state.created_files.rend(); ++it) {
        if (IsSameOrInside(state.destination, HostIdentity(*it)) &&
            (state.source.empty() || HostIdentity(*it) != state.source)) {
            fs::remove(*it, error);
        }
    }
    for (auto it = state.created_dirs.rbegin(); it != state.created_dirs.rend(); ++it) {
        const auto identity = HostIdentity(*it);
        // Ancestors were created so the destination could exist. Remove those too,
        // deepest first. remove() leaves a directory that still has something in it.
        if (*it == state.destination || IsSameOrInside(state.destination, identity) ||
            IsSameOrInside(identity, state.destination)) {
            fs::remove(*it, error);
        }
    }
    if (error) {
        state.result.detail += " Cleanup reported: " + error.message() + ".";
    }
}

bool EnsureDirectory(CopyState& state, const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (!error && fs::is_symlink(status)) {
        state.result.code = RomFSExportCode::UnsafePath;
        state.result.detail = "Refusing to follow a symlink while exporting: " + ToUtf8(path);
        return false;
    }
    if (fs::exists(path, error)) {
        if (!fs::is_directory(path, error)) {
            state.result.code = RomFSExportCode::IoError;
            state.result.detail = "Export path exists and is not a directory: " + ToUtf8(path);
            return false;
        }
        return true;
    }
    if (!fs::create_directory(path, error)) {
        state.result.code = RomFSExportCode::IoError;
        state.result.detail = "Could not create directory " + ToUtf8(path) + ": " + error.message();
        return false;
    }
    state.created_dirs.push_back(path);
    ++state.result.directories_written;
    return true;
}

bool CreateChain(CopyState& state) {
    std::vector<fs::path> missing;
    std::error_code error;
    fs::path cursor = state.destination;
    while (!cursor.empty() && !fs::exists(cursor, error)) {
        missing.push_back(cursor);
        const auto parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (!EnsureDirectory(state, *it)) {
            return false;
        }
    }
    return true;
}

bool WriteFile(CopyState& state, const VirtualFile& file, const fs::path& path) {
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(path, error))) {
        state.result.code = RomFSExportCode::UnsafePath;
        state.result.detail = "Refusing to follow a symlink while exporting: " + ToUtf8(path);
        return false;
    }
    if (!state.source.empty() && HostIdentity(path) == state.source) {
        state.result.code = RomFSExportCode::ProtectedPath;
        state.result.detail = "Refusing to overwrite the source file: " + ToUtf8(path);
        return false;
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        state.result.code = RomFSExportCode::IoError;
        state.result.detail = "Could not create " + ToUtf8(path);
        return false;
    }
    state.created_files.push_back(path);

    const auto size = file->GetSize();
    std::vector<u8> buffer(CopyChunk);
    u64 offset = 0;
    while (offset < size) {
        if (Cancelled(state)) {
            state.result.code = RomFSExportCode::Cancelled;
            state.result.detail = "RomFS export was cancelled.";
            return false;
        }
        const auto want = static_cast<std::size_t>(std::min<u64>(buffer.size(), size - offset));
        const auto got = file->Read(buffer.data(), want, static_cast<std::size_t>(offset));
        if (got == 0 || got > want) {
            state.result.code = RomFSExportCode::ShortRead;
            state.result.detail = fmt::format(
                "Short read of {} at byte {} (file is {} bytes).", ToUtf8(path), offset, size);
            return false;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(got));
        if (!out) {
            state.result.code = RomFSExportCode::IoError;
            state.result.detail = "Write failed for " + ToUtf8(path);
            return false;
        }
        offset += got;
        state.result.bytes_written += got;
    }
    out.close();
    if (!out) {
        state.result.code = RomFSExportCode::IoError;
        state.result.detail = "Could not finish writing " + ToUtf8(path);
        return false;
    }
    ++state.result.files_written;
    return true;
}

bool CopyDirectory(CopyState& state, const VirtualDir& dir, const fs::path& path) {
    if (!EnsureDirectory(state, path)) {
        return false;
    }
    if (state.options->full) {
        for (const auto& file : dir->GetFiles()) {
            if (Cancelled(state)) {
                state.result.code = RomFSExportCode::Cancelled;
                state.result.detail = "RomFS export was cancelled.";
                return false;
            }
            if (!WriteFile(state, file, path / ComponentPath(file->GetName()))) {
                return false;
            }
        }
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (!CopyDirectory(state, sub, path / ComponentPath(sub->GetName()))) {
            return false;
        }
    }
    return true;
}

u64 ProgramTitleId(NupContainer& nup) {
    const auto& packages = nup.GetPackages();
    const auto base = nup.FindBasePackage();
    const auto update = nup.FindBestUpdatePackage();
    if (base && nup.GetExeFSDir(*base) != nullptr) {
        return packages[*base].title_id;
    }
    if (update && nup.GetExeFSDir(*update) != nullptr) {
        return packages[*update].title_id;
    }
    if (base) {
        return packages[*base].title_id;
    }
    if (update) {
        return packages[*update].title_id;
    }
    return 0;
}

} // namespace

LooseNupRomFS::LooseNupRomFS() = default;
LooseNupRomFS::~LooseNupRomFS() = default;
LooseNupRomFS::LooseNupRomFS(LooseNupRomFS&&) noexcept = default;
LooseNupRomFS& LooseNupRomFS::operator=(LooseNupRomFS&&) noexcept = default;

std::vector<InstalledRomFSDumpTarget> ListInstalledRomFSDumpTargets(
    const ContentProvider& installed, u64 program_id,
    const std::function<bool(const ContentProviderEntry&)>& readable) {
    std::vector<InstalledRomFSDumpTarget> targets;
    const auto collect = [&](TitleType title_type, ContentRecordType record_type) {
        for (const auto& entry : installed.ListEntriesFilter(title_type, record_type)) {
            if (GetBaseTitleID(entry.title_id) != program_id) {
                continue;
            }
            if (readable && !readable(entry)) {
                continue;
            }
            targets.push_back(InstalledRomFSDumpTarget{
                .title_id = entry.title_id,
                .title_type = title_type,
                .record_type = record_type,
            });
        }
    };
    collect(TitleType::Application, ContentRecordType::Program);
    collect(TitleType::Application, ContentRecordType::HtmlDocument);
    collect(TitleType::Application, ContentRecordType::LegalInformation);
    collect(TitleType::AOC, ContentRecordType::Data);

    const auto sort_key = [](const InstalledRomFSDumpTarget& item) {
        return std::tuple{item.title_id, static_cast<u8>(item.title_type),
                          static_cast<u8>(item.record_type)};
    };
    std::ranges::sort(targets, [&](const InstalledRomFSDumpTarget& left,
                                  const InstalledRomFSDumpTarget& right) {
        return sort_key(left) < sort_key(right);
    });
    targets.erase(std::unique(targets.begin(), targets.end(),
                              [](const InstalledRomFSDumpTarget& left,
                                 const InstalledRomFSDumpTarget& right) {
                                  return left.title_id == right.title_id &&
                                         left.title_type == right.title_type &&
                                         left.record_type == right.record_type;
                              }),
                  targets.end());
    return targets;
}

LooseNupRomFS OpenLooseNupProgramRomFS(const VirtualFile& file) {
    LooseNupRomFS out;
    if (file == nullptr) {
        out.detail = "No file was opened.";
        LOG_ERROR(Loader, "Loose NUP RomFS: {}", out.detail);
        return out;
    }
    if (!NupContainer::IsNupFile(file)) {
        out.detail = "This file is not a NUP container (the NUP\\x01 header is missing). "
                     "Dump RomFS can read a loose NUP, or content that is already installed.";
        LOG_INFO(Loader, "Loose NUP RomFS: {}", out.detail);
        return out;
    }
    out.recognized = true;
    out.container = std::make_unique<NupContainer>(file);
    if (out.container->GetStatus() != Loader::ResultStatus::Success) {
        out.detail = fmt::format("NUP container could not be read: {}.",
                                 Loader::GetResultStatusString(out.container->GetStatus()));
        LOG_ERROR(Loader, "Loose NUP RomFS: {}", out.detail);
        return out;
    }

    const auto base_index = out.container->FindBasePackage();
    const auto update_index = out.container->FindBestUpdatePackage();
    const auto base_romfs =
        base_index ? out.container->GetRomFSDir(*base_index) : VirtualDir{};
    const auto update_romfs =
        update_index ? out.container->GetRomFSDir(*update_index) : VirtualDir{};
    out.root = NupContainer::MergeShadowDirs(base_romfs, update_romfs);
    out.title_id = ProgramTitleId(*out.container);
    out.update_layered = update_romfs != nullptr;
    if (update_index) {
        out.update_version = out.container->GetPackages()[*update_index].version;
    }
    if (out.root == nullptr) {
        out.detail = fmt::format(
            "NUP container for title {:016X} has no RomFS in the base package or the newest update.",
            out.title_id);
        LOG_ERROR(Loader, "Loose NUP RomFS: {}", out.detail);
        return out;
    }

    out.ok = true;
    if (out.update_layered) {
        out.detail = fmt::format(
            "Loose NUP program RomFS for title {:016X}. Update version {} is overlaid on the base; "
            "paths present in that update replace the base, and older updates are not merged.",
            out.title_id, out.update_version);
    } else if (update_index) {
        out.detail = fmt::format(
            "Loose NUP program RomFS for title {:016X}. The newest update (version {}) has no RomFS, "
            "so the tree is the base RomFS.",
            out.title_id, out.update_version);
    } else if (!base_index) {
        out.detail = fmt::format(
            "Loose NUP program RomFS for title {:016X}. The container has no base package; "
            "the tree is the newest update RomFS.",
            out.title_id);
    } else {
        out.detail = fmt::format("Loose NUP program RomFS for title {:016X}. Base RomFS only.",
                                 out.title_id);
    }
    out.detail += " Add-on packages are not mounted by the loader and are not exported. "
                  "LayeredFS overrides are not applied to this tree.";
    LOG_INFO(Loader, "{}", out.detail);
    return out;
}

u64 RomFSTreeSize(const VirtualDir& root) {
    return TreeSize(root);
}

RomFSExportResult ExportRomFSTree(const VirtualDir& root, const RomFSExportOptions& options,
                                  const std::function<bool(u64, u64)>& progress) {
    RomFSExportResult result;
    if (root == nullptr) {
        result.code = RomFSExportCode::NothingToExport;
        result.detail = "There is no RomFS directory to export.";
        LOG_ERROR(Loader, "RomFS export: {}", result.detail);
        return result;
    }
    if (options.destination.empty()) {
        result.code = RomFSExportCode::IoError;
        result.detail = "No destination directory was given.";
        LOG_ERROR(Loader, "RomFS export: {}", result.detail);
        return result;
    }

    CopyState state;
    state.options = &options;
    state.progress = &progress;
    state.destination = HostIdentity(options.destination);
    state.source = options.protect_source.empty() ? fs::path{} : HostIdentity(options.protect_source);

    if (!state.source.empty() &&
        (state.destination == state.source || IsSameOrInside(state.destination, state.source))) {
        result.code = RomFSExportCode::ProtectedPath;
        result.detail =
            "Refusing to export into the source file or a directory that contains it (" +
            ToUtf8(state.source) + ").";
        LOG_ERROR(Loader, "RomFS export: {}", result.detail);
        return result;
    }
    for (const auto& protected_dir : options.protected_directories) {
        if (protected_dir.empty()) {
            continue;
        }
        const auto identity = HostIdentity(protected_dir);
        if (IsSameOrInside(identity, state.destination)) {
            result.code = RomFSExportCode::ProtectedPath;
            result.detail = "Refusing to export into protected directory " + ToUtf8(identity) +
                            ". Save data was not modified.";
            LOG_ERROR(Loader, "RomFS export: {}", result.detail);
            return result;
        }
    }

    TreeStats stats;
    if (const auto error = ValidateTree(root, {}, 0, stats)) {
        result.code = error->find("safely") != std::string::npos ||
                              error->find("nested") != std::string::npos
                          ? RomFSExportCode::UnsafePath
                          : RomFSExportCode::IoError;
        result.detail = *error;
        LOG_ERROR(Loader, "RomFS export: {}", result.detail);
        return result;
    }

    state.total = options.full ? stats.bytes : 0;
    state.result.code = RomFSExportCode::Success;
    if (Cancelled(state)) {
        result.code = RomFSExportCode::Cancelled;
        result.detail = "RomFS export was cancelled before any file was written.";
        LOG_INFO(Loader, "RomFS export: {}", result.detail);
        return result;
    }

    if (!CreateChain(state) || !CopyDirectory(state, root, state.destination)) {
        const auto code = state.result.code;
        Rollback(state);
        state.result.files_written = 0;
        state.result.directories_written = 0;
        state.result.bytes_written = 0;
        state.result.code = code;
        if (code == RomFSExportCode::Cancelled) {
            LOG_INFO(Loader, "RomFS export: {}", state.result.detail);
        } else {
            LOG_ERROR(Loader, "RomFS export: {}", state.result.detail);
        }
        return state.result;
    }
    if (progress && !progress(state.result.bytes_written, state.total)) {
        state.result.code = RomFSExportCode::Cancelled;
        state.result.detail = "RomFS export was cancelled.";
        Rollback(state);
        state.result.files_written = 0;
        state.result.directories_written = 0;
        state.result.bytes_written = 0;
        LOG_INFO(Loader, "RomFS export: {}", state.result.detail);
        return state.result;
    }

    state.result.detail = fmt::format("Exported {} file(s), {} director{}, {} byte(s) to {}.",
                                      state.result.files_written, state.result.directories_written,
                                      state.result.directories_written == 1 ? "y" : "ies",
                                      state.result.bytes_written, ToUtf8(state.destination));
    LOG_INFO(Loader, "RomFS export: {}", state.result.detail);
    return state.result;
}

} // namespace FileSys
