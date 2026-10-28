# Rsync GUI

> **Created with AI.** The source code, tests and documentation of this project were written by an
> AI model — Claude (Anthropic) in Claude Code — following the author's requirements and decisions.
> The author directed the work and tested the application; the AI wrote the code and ran the automated
> tests. Commits made this way carry a `Co-Authored-By: Claude` line. Review the code before relying
> on it, especially for moving files (*Move* removes the sources).

A Qt 6 graphical front-end for copying files with `rsync`, with an optional verification pass
and a Dolphin context menu. It covers the options needed for everyday copying; anything else can
be passed as additional arguments.

- **source list** (files and directories, local or remote): add with buttons, type/paste a path
  (also `file://...`, `sftp://[user@]host[:port]/path`, rsync's `[user@]host:path`, quoted, with `~`),
  paste many paths (`Ctrl+V`, including files copied in Dolphin), drag & drop, paths on the command line,
- **destination**: a local directory (created if missing) or a remote one, with a list of
  recently used destinations,
- the most useful **rsync options**: `-a` or its parts separately (`-r`, `-l`, `-p`, `-t`, `-g`, `-o`,
  `-D`, e.g. `-rlt`), `-H`, `-A`, `-X`, `-v`, `-h`, `--partial`,
  `--progress` / `--info=progress2`, `-c`, `-u`, `--ignore-existing`, `-z`, `--bwlimit`, `-e`,
  `--delete`, `--remove-source-files`, `-n`, copying directory contents (trailing `/`) and a field for
  **any other arguments** (e.g. `--exclude='*.tmp' --mkpath`),
- **verification after copying**: a second rsync pass that compares checksums of the source and the
  destination and lists every file that differs or is missing,
- preview of the generated command, progress bar (per file or overall), speed and ETA, log,
  cancellation, confirmation only for options that delete files, optional **closing the window
  when the transfer finishes** successfully,
- **progress outside the window** (KDE Plasma): on the task manager entry and as a job in the
  notification area (with speed, ETA, a cancel button and "Open destination" afterwards),
- options are remembered between runs, export/import to/from JSON files, **presets** and
  integration with the **Dolphin** context menu (*Rsync* submenu).

## Requirements

- Linux, Qt 6 (Core, Widgets; Test for the tests), CMake ≥ 3.19, a C++17 compiler
- `rsync` (tested with 3.4.1); `ssh` for remote locations

Ubuntu / Debian:

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev rsync
```

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

Running (optionally with sources, a destination, a preset or a settings file):

```bash
./build/rsync-gui ~/Photos ~/Documents/report.pdf --destination /media/backup
```

```bash
./build/rsync-gui --preset rsync-copy-verify --browse-destination ~/Photos
```

```bash
./build/rsync-gui --config ~/rsync-mirror.json sftp://nas/volume1/photos -d ~/Photos-copy
```

```bash
./build/rsync-gui --list-presets
```

Options loaded with `--preset` / `--config` (and from *File → Load preset*) apply **to the current
session only** — they do not overwrite the remembered options. The title of the options section
then shows "… (session only, not remembered)". Only *Import settings…* changes them permanently.

Tests:

```bash
ctest --test-dir build --output-on-failure
```

System-wide installation (executable, `.desktop` entry, presets, Dolphin menu):

```bash
sudo cmake --install build
```

Or for the current user only, without root (into `~/.local/bin` and `~/.local/share`).
The `.desktop` files get the absolute path of the installed program, so Dolphin finds it even
before `~/.local/bin` is in `PATH`:

```bash
cmake --install build --prefix ~/.local
```

The Dolphin menu can be skipped with `cmake -S . -B build -DRSYNC_GUI_INSTALL_SERVICEMENUS=OFF`.

## Presets

Installed into `<prefix>/share/rsync-gui/presets/`:

| Label (menus) | Preset (`--preset`) | Command | Effect |
|---|---|---|---|
| Copy (-a) | `rsync-copy` | `rsync -avhP` | archive mode, file list, progress, partial files kept |
| Copy and verify (-ac) | `rsync-copy-verify` | `rsync -avhPc` + verification | as above, files compared by checksum, then a verification pass |
| Copy to FAT/NTFS/NAS (-rlt) | `rsync-copy-portable` | `rsync -rltvhP` | recursive, symlinks and times only — for FAT/exFAT/NTFS drives or a NAS without Unix permissions and owners |
| Move (-a) | `rsync-move` | `rsync -avhP --remove-source-files` + removing emptied directories | moves files and directories (asks for confirmation) |
| Move and verify (-ac) | `rsync-move-verify` | `rsync -avhPc` + verification, then `--remove-source-files` | the sources are removed only after the verification found no differences |
| Move to FAT/NTFS/NAS (-rlt) | `rsync-move-portable` | `rsync -rltvhP --remove-source-files` + removing emptied directories | the same for drives and shares without Unix permissions and owners |

The label — the name (the `name` field of the file) followed by the flags that decide what is
copied — is shown in *File → Load preset* (sorted by name, the full command and the file in the
tooltip), in the title of the options section and, with "…", in the Dolphin menu, in the same order.
The flags are generated from the options (`-a` or its parts, `-H -A -X -c -u -z`, `--ignore-existing`,
`--delete`); output flags (`-v -h -P`) and the removal of sources (the name says *Move*) are left out.
Your own presets get the flags too. `--preset` uses the file names; `--list-presets` prints the file
name, the label and the path.

Presets set the copy options but not the program, the remote shell, the bandwidth limit or the
additional arguments — those stay as chosen by the user. They are searched for in this order:
`~/.local/share/rsync-gui/presets/`, the `$XDG_DATA_DIRS` directories,
`<program dir>/../share/rsync-gui/presets/` and `<program dir>/presets/` (build).
A custom `~/.local/share/rsync-gui/presets/rsync-copy.json` overrides the shipped one;
you can also add your own presets (any `*.json` name, format as below).

## Dolphin context menu

After installation Dolphin shows an **Rsync** submenu for files and directories:

- **Copy (-a)…** — `--preset rsync-copy` (formerly *Copy (rsync)*, `rsync -avhP`),
- **Copy and verify (-ac)…** — `--preset rsync-copy-verify` (formerly *Copy (rsync + verify)*, `rsync -avhPc`),
- **Copy to FAT/NTFS/NAS (-rlt)…** — `--preset rsync-copy-portable` (`rsync -rltvhP`),
- **Move (-a)…** — `--preset rsync-move` (`rsync -avhP --remove-source-files`, then the emptied source
  directories are removed; a confirmation is shown before it starts),
- **Move and verify (-ac)…** — `--preset rsync-move-verify`: copy, verify, and only then remove the
  sources (see below),
- **Move to FAT/NTFS/NAS (-rlt)…** — `--preset rsync-move-portable` (the same with `-rlt`).
  Files the drive cannot store — larger than 4 GB on FAT32, names with `:` `?` `"` etc., symlinks on
  FAT — fail and stay in the source (rsync removes only files it transferred successfully). After such a
  partial transfer no source directories are removed at all, not even emptied ones: an empty directory
  whose name the drive cannot store would not exist in the destination.

Clicking an entry opens Rsync GUI with the selected items as sources and the preset loaded, and
immediately asks for the destination directory (starting in the last used destination). The copy
starts after clicking *Copy* (`Enter`). Several selected items end up in one window. Local files and
`sftp://` locations (Dolphin browsing a server) are supported — remote sources are copied with
`rsync` over `ssh` (`sftp://user@host:port/path` → `-e "ssh -p port" user@host:/path`), so the host
must be reachable without typing in a terminal: ssh keys / an agent, or a password asked through
`SSH_ASKPASS` (e.g. `ksshaskpass` on KDE); the host key must already be known.

If you used your own rsync service menu before, remove it to avoid duplicated entries.
New entries usually appear immediately; if not, run `kbuildsycoca6`.

## Progress outside the window

While a transfer runs, its progress is also shown

- on the Rsync GUI entry in the task manager (the `com.canonical.Unity.LauncherEntry` D-Bus signal,
  supported by Plasma; the entry is matched by `rsync-gui.desktop`, so it works for the installed
  application),
- as a job in the Plasma notification area (`org.kde.JobViewServerV2`): title (Copying / Verifying /
  Moving), source, destination, percentage, size, speed and ETA, a button that cancels the transfer,
  and after it the result with a link to the destination.

On other desktops or without a session bus this does nothing.

## Close when finished

The *Close when finished* checkbox next to the *Copy* button quits Rsync GUI 1.5 s after a
successful transfer (unchecking it in the meantime keeps the window open). After errors,
verification differences, cancelling or a dry run the window always stays open with the log.
It is a window preference, remembered immediately — also in a window opened from Dolphin with a
preset — and not part of the settings files or presets.

## Archive mode and its parts

`-a` means `-rlptgoD`. With *Archive mode (-a)* on, its parts are shown checked and cannot be changed;
turn it off to choose them separately — the command is then built from the checked parts (e.g.
`-rltvhP`). Your choice of the parts is kept while archive mode is on and comes back when you turn
it off again. Without `-a` the parts default to all on, so turning it off alone changes nothing.

`-rlt` is the usual choice for drives and shares that cannot store Unix permissions and owners
(FAT/exFAT/NTFS, many NAS shares) — with `-a` rsync reports errors for every file there.
FAT stores modification times with a 2-second resolution: add `--modify-window=1` to the additional
arguments, otherwise repeated runs copy unchanged files again.

## What "verify" does

`-c` (`--checksum`) alone only changes how rsync decides **which files to transfer** (by checksum
instead of size and modification time). rsync always checks every transferred file with a
whole-file checksum, but files it decides to skip are not compared.

With *Verify after copying* enabled, after a successful transfer Rsync GUI runs rsync again as a
dry run that changes nothing:

```bash
rsync -nrlci '--out-format=%i %n%L' <additional arguments> -- <sources> <destination>/
```

Every file is read on both sides and its checksum compared; every entry that would be transferred
(different content, missing file, directory or symlink) is reported in the log and in a dialog. The
additional arguments (e.g. `--exclude`) are passed too, so excluded files are not reported. The
verification is skipped for a dry run.

**Moving with verification.** With both *Remove source files* and *Verify* on (the *Move and verify*
preset), the sources are not removed during the copy. After the verification has found no
differences, rsync runs a third time with `--remove-source-files`: everything is already up to date,
so it transfers nothing (a file changed in the meantime would be transferred again) and only removes
the source files; then the emptied directories are removed. If the verification finds a difference
or fails, nothing in the source is touched.

Note that files just written may still be in the page cache, so the destination is not necessarily
read back from the disk itself.

## Settings files (JSON)

Remembering the options between runs works independently (`~/.config/fapped/rsync-gui.conf`).
In addition, the current options can be exported to a JSON file and imported later. Options imported
with *Import settings…* become the new remembered options (unlike presets and `--config`).
The last 10 imported, exported or `--config` files are listed in *File → Recent settings* (with
their `name`); choosing one works like *Import settings…*. Files that no longer exist disappear from
the list, presets are not listed there (they have *Load preset*).

```json
{
    "format": "rsync-gui-settings",
    "version": 1,
    "name": "My settings",
    "options": {
        "program": "rsync",
        "bwLimit": "",
        "remoteShell": "",
        "extraArguments": "--exclude='*.tmp'",
        "archive": true,
        "recursive": true,
        "links": true,
        "perms": true,
        "times": true,
        "group": true,
        "owner": true,
        "devices": true,
        "hardLinks": false,
        "acls": false,
        "xattrs": false,
        "verbose": true,
        "human": true,
        "partial": true,
        "checksum": false,
        "compress": false,
        "contentsOnly": false,
        "deleteExtraneous": false,
        "removeSourceFiles": false,
        "removeSourceDirs": false,
        "dryRun": false,
        "verifyAfter": false,
        "useBwLimit": false,
        "useRemoteShell": false,
        "progress": "per-file",
        "existing": "replace"
    }
}
```

- `progress`: `none`, `per-file` (`--progress`, `-P` with `partial`), `overall` (`--info=progress2`),
- `existing`: `replace`, `skip-newer` (`-u`), `ignore-existing` (`--ignore-existing`),
- `recursive`, `links`, `perms`, `times`, `group`, `owner`, `devices` (`-rlptgoD`) are used only
  when `archive` is false (`-a` implies them all),
- `extraArguments` is split like a shell command line (quotes and backslashes),
- `name` (optional) — the name shown in the window, in *Load preset* and in the log,
- keys missing from `options` keep the current values from the window, unknown keys are ignored,
- a file with a value of the wrong type, an unknown `format` or a newer `version` is rejected
  as a whole (the options in the window stay unchanged).

## How sources are copied

- The sources are always copied **into** the destination directory (a trailing `/` is added to it).
  A source directory `dir` ends up as `<destination>/dir`, unless *Copy the contents of source
  directories* is enabled (`dir/` — only for local sources; remote paths are passed as typed).
- rsync can read from only one remote host at a time, so the sources are grouped: all local
  sources in one rsync run, and one run per remote host (and port). Copying between two remote
  locations is not possible (rsync does not support it).
- Missing local sources are reported and can be skipped; duplicates are skipped.
- **Moving**: `--remove-source-files` makes rsync remove each transferred file from the source, but
  it leaves all directories. With *Also remove source directories that are left empty*
  (`removeSourceDirs`, on in the *Move* preset) the emptied directories of local sources are removed
  after a successful transfer, deepest first — only with `rmdir`, so a directory that still contains
  anything (an excluded file, a file that failed) stays and is reported. Symlinks are not followed,
  other file systems are not entered, and with *Copy the contents of source directories* the source
  directory itself stays. Remote sources keep their directories.
- Copying a directory into itself (the destination inside a source directory) is refused.
- Exit code 24 (some source files vanished during the transfer) is reported as a warning,
  other non-zero exit codes as errors, with the meaning of the code.

## File names that are not valid UTF-8

Linux file names are bytes, and old files (e.g. from Windows/Latin-1 times or broken archives) may
contain bytes that are not valid UTF-8. Rsync GUI keeps such names exactly — from the command line
(Dolphin), `file://` URLs, drag & drop and the clipboard — and passes the exact bytes to rsync.
They are shown with `\xNN` escapes (e.g. `caf\xe9`), and the command preview uses bash's `$'...'`
quoting, so it can be pasted into a terminal. rsync itself shows them as `\#351` in its output.
Names inside source directories were never a problem (rsync reads them itself).

Only the file dialogs (*Add files…*, *Browse…*) cannot return such names — Qt converts them on the
way; use Dolphin, drag & drop or paste instead.

## AI disclosure

This project was created with AI assistance: all code, tests and documentation were generated by
Claude (Anthropic) via Claude Code, based on the author's instructions, and verified with the
automated tests in `tests/` and by manual checks. The notice is also shown in *Help → About*.

## License

MIT — see [LICENSE](LICENSE).
