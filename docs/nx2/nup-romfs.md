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

`Load` calls `ApplyRomFSOverrides` before the process RomFS is published.
`RomFSFactory::OpenCurrentProcess` then returns that image. The NUP stays
non-updatable: an installed NCA update is not applied on top of the container. The
override sits on the merged base and newest-update tree. Add-on packages stay
unmounted. Dump RomFS does not call `ApplyRomFSOverrides`, so a dump is the packed
tree without mods.

Only these directory names are consulted, without case: `romfs`, `romfslite`, and
`romfs_ext`. `manual_html` is used for an HTML document. A UI replacement belongs
under `romfs`. Its path is the path under the dumped `romfs` directory.

Load directory. Each immediate child of the title folder is one named mod:

```text
<eden>/load/<title id>/<mod name>/romfs/<virtual path>
<eden>/load/<title id>/<mod name>/romfslite/<virtual path>
<eden>/load/<title id>/<mod name>/romfs_ext/<virtual path>
```

SD card Atmosphere layout. There is no extra mod-name directory. The title folder
itself is the layer, and its directory name is the title id:

```text
<eden>/sdmc/atmosphere/contents/<title id>/romfs/<virtual path>
<eden>/sdmc/atmosphere/contents/<title id>/romfslite/<virtual path>
<eden>/sdmc/atmosphere/contents/<title id>/romfs_ext/<virtual path>
```

`<title id>` is 16 uppercase hex digits of the id the NUP loader boots. For a base
plus update container that is the base application id, not the update id ending in
`800`. Title id 0 and ids ending in `800` have no mod root. For `0400486024DEC000`
the load folder is `<eden>/load/0400486024DEC000/` and the SD folder is
`<eden>/sdmc/atmosphere/contents/0400486024DEC000/`.

On Windows, `<eden>` is `<folder of lichii.exe>\user` when that `user` directory
exists, otherwise `%APPDATA%\eden`. Give a test build its own empty `user` directory
beside `lichii.exe` before the first launch so it does not open an existing profile.

Precedence:

- The merged NUP image is the lowest layer. A path with no override stays as the
  newest update left it, or the base file when the update has no such path.
- Named load mods and the Atmosphere title folder are sorted by directory name in
  byte order. The earlier name wins when both contain the same path.
- The Atmosphere folder's name is the title id. A title id that starts with a digit
  sorts before a mod folder named with a letter, so that Atmosphere file wins over
  that named mod. Use one location for a one-file test.
- Disabling a mod uses its folder name. Disabling the name `SDMC` skips the
  Atmosphere layer. The next launch reads the packed file again.
- `..` in a host path is collapsed and rejected when it would leave the directory
  being opened. A symlink whose target resolves outside that directory is omitted.
  This path does not open the NUP or a save for writing.

Synthetic fixtures prove this through `RomFSFactory::OpenCurrentProcess`. A retail
game boot has not.

An example, not a path taken from a game: a dumped file
`dump/0400486024DEC000/romfs/UI/A.bntx` is overridden by one of

```text
<eden>/load/0400486024DEC000/IconTest/romfs/UI/A.bntx
<eden>/sdmc/atmosphere/contents/0400486024DEC000/romfs/UI/A.bntx
```

`Dump RomFS to SDMC` writes the unmodified packed tree into the Atmosphere directory.
Move that copy aside before testing one replacement, or the title-id folder and a
named mod compete by the sort above.
