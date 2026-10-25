# Rsync GUI

A Qt 6 graphical front-end for copying files with `rsync`, with an optional verification pass
and a Dolphin context menu. It covers the options needed for everyday copying; anything else can
be passed as additional arguments.

- **source list** (files and directories, local or remote): add with buttons, type/paste a path
  (also `file://...`, `sftp://[user@]host[:port]/path`, rsync's `[user@]host:path`, quoted, with `~`),
  paste many paths (`Ctrl+V`, including files copied in Dolphin), drag & drop, paths on the command line,
- **destination**: a local directory (created if missing) or a remote one, with a list of
  recently used destinations,
- the most useful **rsync options**: `-a`, `-r`, `-H`, `-A`, `-X`, `-v`, `-h`, `--partial`,
  `--progress` / `--info=progress2`, `-c`, `-u`, `--ignore-existing`, `-z`, `--bwlimit`, `-e`,
  `--delete`, `--remove-source-files`, `-n`, copying directory contents (trailing `/`) and a field for
  **any other arguments** (e.g. `--exclude='*.tmp' --mkpath`),
- **verification after copying**: a second rsync pass that compares checksums of the source and the
  destination and lists every file that differs or is missing,
- preview of the generated command, progress bar (per file or overall), speed and ETA, log,
  cancellation, confirmation only for options that delete files,
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

Or for the current user only, without root (into `~/.local/bin` and `~/.local/share`;
`~/.local/bin` has to be in `PATH` so that Dolphin can find `rsync-gui`):

```bash
cmake --install build --prefix ~/.local
```

The Dolphin menu can be skipped with `cmake -S . -B build -DRSYNC_GUI_INSTALL_SERVICEMENUS=OFF`.

## Presets

Installed into `<prefix>/share/rsync-gui/presets/`:

| Preset | Command | Effect |
|---|---|---|
| `rsync-copy` | `rsync -avhP` | archive mode, file list, progress, partial files kept |
| `rsync-copy-verify` | `rsync -avhPc` + verification | as above, files compared by checksum, then a verification pass |

Presets set the copy options but not the program, the remote shell, the bandwidth limit or the
additional arguments — those stay as chosen by the user. They are searched for in this order:
`~/.local/share/rsync-gui/presets/`, the `$XDG_DATA_DIRS` directories,
`<program dir>/../share/rsync-gui/presets/` and `<program dir>/presets/` (build).
A custom `~/.local/share/rsync-gui/presets/rsync-copy.json` overrides the shipped one;
you can also add your own presets (any `*.json` name, format as below).

## Dolphin context menu

After installation Dolphin shows an **Rsync** submenu for files and directories:

- **Copy to…** — `--preset rsync-copy` (formerly *Copy (rsync)*, `rsync -avhP`),
- **Copy and verify to…** — `--preset rsync-copy-verify` (formerly *Copy (rsync + verify)*, `rsync -avhPc`).

Clicking an entry opens Rsync GUI with the selected items as sources and the preset loaded, and
immediately asks for the destination directory (starting in the last used destination). The copy
starts after clicking *Copy* (`Enter`). Several selected items end up in one window. Local files and
`sftp://` locations (Dolphin browsing a server) are supported — remote sources are copied with
`rsync` over `ssh` (`sftp://user@host:port/path` → `-e "ssh -p port" user@host:/path`), so the host
must be reachable without typing in a terminal: ssh keys / an agent, or a password asked through
`SSH_ASKPASS` (e.g. `ksshaskpass` on KDE); the host key must already be known.

If you used your own rsync service menu before, remove it to avoid duplicated entries.
New entries usually appear immediately; if not, run `kbuildsycoca6`.

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
verification is skipped for a dry run and with `--remove-source-files` (the sources are gone).

Note that files just written may still be in the page cache, so the destination is not necessarily
read back from the disk itself.

## Settings files (JSON)

Remembering the options between runs works independently (`~/.config/fapped/rsync-gui.conf`).
In addition, the current options can be exported to a JSON file and imported later. Options imported
with *Import settings…* become the new remembered options (unlike presets and `--config`).

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
- `recursive` is used only when `archive` is false (`-a` implies `-r`),
- `extraArguments` is split like a shell command line (quotes and backslashes),
- `name` (optional) — the name shown in the window and in the log,
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
- Copying a directory into itself (the destination inside a source directory) is refused.
- Exit code 24 (some source files vanished during the transfer) is reported as a warning,
  other non-zero exit codes as errors, with the meaning of the code.

## License

MIT — see [LICENSE](LICENSE).
