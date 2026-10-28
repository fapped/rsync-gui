# TODO

## Done

- [x] Git repository (user `fapped`), MIT license
- [x] CMake + Qt 6 Widgets, `rsynccore` library, QtTest tests (`ctest`)
- [x] Source list: local paths, `file://`, `sftp://` (with user and port), `user@host:path`;
      add/paste/drop/command line
- [x] Destination with recent destinations, created if missing, remote destinations
- [x] Supported subset of rsync options + additional arguments field
- [x] Parts of `-a` (`-rlptgoD`) as separate checkboxes, preset `rsync-copy-portable` (`-rltvhP`)
      and the *Copy to FAT/NTFS/NAS…* Dolphin action
- [x] Grouping of sources into rsync runs (local / per remote host)
- [x] Verification pass (checksum dry run with itemized differences)
- [x] Command preview, progress (per file / overall), speed and ETA, log, cancellation
- [x] Remembered options, JSON export/import (+ `--config`), presets `rsync-copy` / `rsync-copy-verify`
- [x] Dolphin context menu (*Rsync* submenu: Copy…, Copy and verify…), installed by CMake
      with the absolute program path
- [x] Installed to `~/.local`; the older service menu removed
- [x] Dolphin menu checked on the live system by the user (2026-10-28)
- [x] Progress in the Plasma task manager and as a job in the notification area (D-Bus, no KF6)
- [x] *Move…* Dolphin action and preset (`--remove-source-files` + removing emptied source directories)
- [x] "Close when finished" checkbox (remembered window preference, only after a full success)
- [x] Consistent preset names: the same name in *Load preset*, the options title and the Dolphin menu

## To do

- [ ] Check the cancel button of the Plasma job notification by hand
- [ ] Test remote sources/destinations against a real ssh host (only parsing and command building are tested)
- [ ] Optionally start the copy right after choosing the destination from Dolphin (`--start`)
- [ ] Ask for unknown ssh host keys in the GUI (ssh has no terminal; passwords work via `SSH_ASKPASS` only)
- [ ] Verification that bypasses the page cache (e.g. drop caches for the destination, or `--checksum` with `O_DIRECT`)
- [ ] `--modify-window=1` as a checkbox (or automatically for FAT destinations, detected with `QStorageInfo`)
- [ ] Recently used settings files (*File → Recent settings*)
- [ ] Application icon (currently the `folder-sync` theme icon)
- [ ] Translations (`tr()` is used in the code, there are no `.ts` files yet)
- [ ] File names that are not valid UTF-8 (the `QString` ↔ bytes conversion may change them)
- [ ] Nautilus / Nemo integration (scripts or an extension)
- [ ] `.deb` package / AppImage, CI (GitHub Actions: build + tests)
