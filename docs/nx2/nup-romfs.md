# Loose NUP RomFS export and overrides

This describes what the code does. It is not a claim that a retail game dump or an icon
override has been run.

## Why Dump RomFS failed on a loose NUP

`Dump RomFS` used to list only installed content (NAND and SD `Contents/registered`).
A loose NUP is not installed there. With an empty provider the selection returned
before creating a directory, showed the generic "error copying the RomFS files or the
user cancelled" dialog, and wrote no log line.

A loose NUP is now exported when that installed list is empty. Installed entries are
still preferred when any of them is readable. The dialog and the log say which route
ran and why a route failed.

## What a loose NUP export contains

The export is the program RomFS `AppLoader_NUP::ReadRomFS` already builds:

- The base package's RomFS directory.
- The single newest update package's RomFS overlaid on it. A path that exists in that
  update replaces the base file. Directories are merged. Older updates are not consulted.
- If the newest update has no RomFS, the export is the base RomFS.
- Add-on packages are not included. The loader does not mount them.
- LayeredFS files are not applied to this tree.

The output directory is:

- Dump RomFS: `<Dump Root>/<title id>/romfs/`
- Dump RomFS to SDMC: `<SDMC>/atmosphere/contents/<title id>/romfs/`

`<title id>` is 16 uppercase hex digits, the same id the NUP loader boots (the base
package when it has an ExeFS, otherwise the update package). Default Dump Root is
`<eden>/dump` and default SDMC is `<eden>/sdmc`. On Windows, `<eden>` is
`<folder of lichii.exe>\user` when that `user` directory exists, otherwise
`%APPDATA%\eden`. Paths changed under Emulation > Configure > System > Filesystem
replace those defaults.

The virtual path of an exported file is its path under that `romfs` directory. A full
export copies file bytes. A skeleton export creates directories only. The source NUP
is opened read-only. Export refuses to write the source file itself or anything under
`<NAND>/user/save` and `<NAND>/system/save`. Cancelling, a short read, or an unsafe
name removes only the files and directories that export created.

Names that would leave the destination are rejected. That includes empty names, `.`,
`..`, names containing `/ \ : * ? " < > |` or a control character, names ending in a
space or dot, and Windows device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`,
`LPT1`–`LPT9`, including with an extension).

## RomFS overrides and NUP

A running NUP does not apply RomFS overrides. `AppLoader_NUP::IsRomFSUpdatable()`
returns false, and `RomFSFactory::OpenCurrentProcess` then returns the packed RomFS
from `ReadRomFS` without calling `PatchManager::ApplyLayeredFS`. Placing a file in
the folders below does not change what the NUP title opens.

Upstream pull request 366 (`file_sys: apply LayeredFS to non-updatable RomFS (NUP)`,
https://github.com/Djihads80/Eglantine/pull/366) is open and not merged. It is not
part of this change. This tree does not claim that an icon override is visible in a
game.

The folders the other loaders consult, which are the ones a later LayeredFS change
would use, are:

Load directory, one mod folder per override (the folder name is the mod name):

```text
<eden>/load/<title id>/<mod name>/romfs/<virtual path>
<eden>/load/<title id>/<mod name>/romfslite/<virtual path>
<eden>/load/<title id>/<mod name>/romfs_ext/<virtual path>
```

SD card Atmosphere layout. There is no extra mod-name directory; `romfs` sits directly
under the title id:

```text
<eden>/sdmc/atmosphere/contents/<title id>/romfs/<virtual path>
<eden>/sdmc/atmosphere/contents/<title id>/romfslite/<virtual path>
<eden>/sdmc/atmosphere/contents/<title id>/romfs_ext/<virtual path>
```

`<title id>` is 16 uppercase hex digits. For title `0400486024DEC000` the load folder
is `<eden>/load/0400486024DEC000/` and the SD folder is
`<eden>/sdmc/atmosphere/contents/0400486024DEC000/`.

`romfs`, `romfslite`, and `romfs_ext` are matched without case. `<virtual path>` is
the path under the exported `romfs` directory. An example, not a path taken from a
game: a dumped file `dump/0400486024DEC000/romfs/UI/A.bntx` would be overridden by
`<eden>/load/0400486024DEC000/xbox-icons/romfs/UI/A.bntx` or by
`<eden>/sdmc/atmosphere/contents/0400486024DEC000/romfs/UI/A.bntx` only once LayeredFS
is applied to NUP.

When several of those directories contain the same file, `PatchManager::ApplyLayeredFS`
sorts the mod folders by name and uses the first match. The original file is used only
when no layer has that path. A mod whose folder name is listed in that title's disabled
add-ons is skipped. The disabled name `SDMC` skips the SD card layer. `romfs_ext` is
packed as the RomFS extension section, separate from `romfs`.

`Dump RomFS to SDMC` writes into `<SDMC>/atmosphere/contents/<title id>/romfs/`, which
is the SD override directory above. That dump is a copy of the exported tree. It does
not modify the NUP, and a running NUP still does not read it.
