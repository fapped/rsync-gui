# TODO

## Done

- [x] Git repository (user `fapped`), MIT license
- [x] CMake + Qt 6 Widgets, `rsynccore` library, QtTest tests (`ctest`)
- [x] Source list: local paths, `file://`, `sftp://` (with user and port), `user@host:path`;
      add/paste/drop/command line
- [x] Destination with recent destinations, created if missing, remote destinations
- [x] Supported subset of rsync options + additional arguments field
- [x] Grouping of sources into rsync runs (local / per remote host)
- [x] Verification pass (checksum dry run with itemized differences)
- [x] Command preview, progress (per file / overall), speed and ETA, log, cancellation
- [x] Remembered options, JSON export/import (+ `--config`), presets `rsync-copy` / `rsync-copy-verify`
- [x] Dolphin context menu (*Rsync* submenu: Copy to…, Copy and verify to…), installed by CMake

## To do

- [ ] Check the Dolphin menu after installing on a live system (so far only installed into a test prefix)
- [ ] Remove the older service menu after installation (user's decision)
- [ ] Test remote sources/destinations against a real ssh host (only parsing and command building are tested)
- [ ] "Move to…" Dolphin action (`--remove-source-files` + removing emptied source directories)
- [ ] Optionally start the copy right after choosing the destination from Dolphin (`--start`)
- [ ] Ask for unknown ssh host keys in the GUI (ssh has no terminal; passwords work via `SSH_ASKPASS` only)
- [ ] Verification that bypasses the page cache (e.g. drop caches for the destination, or `--checksum` with `O_DIRECT`)
- [ ] Recently used settings files (*File → Recent settings*)
- [ ] Application icon (currently the `folder-sync` theme icon)
- [ ] Translations (`tr()` is used in the code, there are no `.ts` files yet)
- [ ] File names that are not valid UTF-8 (the `QString` ↔ bytes conversion may change them)
- [ ] Nautilus / Nemo integration (scripts or an extension)
- [ ] `.deb` package / AppImage, CI (GitHub Actions: build + tests)
