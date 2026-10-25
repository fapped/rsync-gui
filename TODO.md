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
- [x] Dolphin context menu (*Rsync* submenu: Copy to…, Copy and verify to…), installed by CMake
      with the absolute program path
- [x] Installed to `~/.local`; the older service menu removed

## To do

- [ ] Click through the Dolphin menu on the live system (installed to `~/.local` on 2026-10-25,
      the installed Exec line was started manually and works)
- [ ] Test remote sources/destinations against a real ssh host (only parsing and command building are tested)
- [ ] "Move to…" Dolphin action (`--remove-source-files` + removing emptied source directories)
- [ ] Optionally start the copy right after choosing the destination from Dolphin (`--start`)
- [ ] Ask for unknown ssh host keys in the GUI (ssh has no terminal; passwords work via `SSH_ASKPASS` only)
- [ ] Verification that bypasses the page cache (e.g. drop caches for the destination, or `--checksum` with `O_DIRECT`)
- [ ] `--modify-window=1` as a checkbox (or automatically for FAT destinations, detected with `QStorageInfo`)
- [ ] Progress outside the window (asked 2026-10-25, for later):
      - taskbar (Plasma task manager): the `com.canonical.Unity.LauncherEntry` D-Bus signal `Update`
        with `application://rsync-gui.desktop` and `{progress: 0..1, progress-visible: bool}` —
        only Qt6::DBus needed, the desktop file name must match (`setDesktopFileName` is already set)
      - notification area (Plasma "jobs", like a Dolphin copy, with speed and a cancel button):
        `KUiServerV2JobTracker` from KF6 JobWidgets + a `KJob` wrapping `RsyncJob`, or `org.kde.JobViewServerV2`
        over D-Bus directly to avoid the KF6 dependency
      - both optional at runtime (no-op outside KDE / without the D-Bus services)
- [ ] Consistent preset names (asked 2026-10-25, for later): *File → Load preset* shows file names
      (`rsync-copy`, `rsync-copy-verify`, `rsync-copy-portable`), the presets' `name` fields say
      "Copy (rsync -avhP)" etc., and Dolphin says "Copy to…", "Copy and verify to…", "Copy to FAT/NTFS/NAS…".
      Show the `name` in the menu (file name as a tooltip, `--preset` keeps using file names) and make the
      three places use the same wording, e.g. "Copy", "Copy and verify", "Copy to FAT/NTFS/NAS"
- [ ] "Close when finished" checkbox (asked 2026-10-25, for later): quit Rsync GUI after the job ends.
      Only on full success (`Summary::ok()`) — after errors, verification differences or cancellation
      the window stays open with the log. To decide: a remembered window preference, or an option in
      the JSON settings so that presets (the Dolphin actions) can turn it on; possibly a desktop
      notification with the summary before closing (see the progress-outside-the-window item)
- [ ] Recently used settings files (*File → Recent settings*)
- [ ] Application icon (currently the `folder-sync` theme icon)
- [ ] Translations (`tr()` is used in the code, there are no `.ts` files yet)
- [ ] File names that are not valid UTF-8 (the `QString` ↔ bytes conversion may change them)
- [ ] Nautilus / Nemo integration (scripts or an extension)
- [ ] `.deb` package / AppImage, CI (GitHub Actions: build + tests)
