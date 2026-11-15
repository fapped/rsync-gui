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
- [x] Progress in the Plasma task manager and as a job in the notification area (D-Bus, no KF6),
      speed graph in the notification (checked on screen with the user, Plasma 6.6.6)
- [x] *Cancel* in the Plasma job notification cancels the transfer (checked by the user, 2026-10-28)
- [x] *Move…* Dolphin action and preset (`--remove-source-files` + removing emptied source directories)
- [x] *Move to FAT/NTFS/NAS (-rlt)…* preset and Dolphin action
- [x] *Move and verify (-ac)…*: the sources are removed only after a successful verification
- [x] Preset labels with the flags that decide what is copied, e.g. *Copy to FAT/NTFS/NAS (-rlt)*
- [x] File names that are not valid UTF-8: kept exactly (command line, URLs, drag & drop, clipboard),
      passed to rsync as bytes, shown with `\xNN` (Dolphin converts such names, see below)
- [x] Recently used settings files (*File → Recent settings*)
- [x] "Close when finished" checkbox (remembered window preference, only after a full success)
- [x] Consistent preset names: the same name in *Load preset*, the options title and the Dolphin menu
- [x] Remote tests against an ssh server in a container (`tools/ssh-test/run.sh`): both directions,
      non-UTF-8 names, verified move, remote source move, cancelling, password and host key via `SSH_ASKPASS`
- [x] Verification reads the disk: before it, `syncfs` + `POSIX_FADV_DONTNEED` for the copies (local destinations),
      checked with a FAT image changed underneath the mounted file system
- [x] `--modify-window=1` added automatically for destinations that keep times only to 2 s (FAT),
      detected by setting a time with an odd second on a temporary file (checked on FAT32/exFAT images);
      an option turns it off
- [x] Unknown ssh host keys: ssh asks through `SSH_ASKPASS` (Plasma: ksshaskpass with `SSH_ASKPASS_REQUIRE=prefer`),
      the question about the host key and the password checked by the user in the GUI (2026-10-28)
- [x] Speed graph in the application window: behind the progress bar (time on the x axis, the scale in
      the corner), `SpeedHistory` in the core library with tests
- [x] Built-in file dialog for names that are not valid UTF-8 (KDE's dialog shows them with U+FFFD and
      then reports that the file does not exist): the ⋮ menu next to *Add directory…*, optionally for
      all file dialogs of the sources and the destination; `FileListing` in the core library with tests
- [x] Dolphin (KIO) passes names that are not valid UTF-8 with U+FFFD in place of the bytes (context menu,
      drag & drop, clipboard; checked with *Copy* in Dolphin: `caf%EF%BF%BD`): such a path that does not exist
      is matched against the directory listing (each U+FFFD stands for one or more bytes) - one match: the
      exact name, several: only they are listed in the built-in dialog to choose from;
      `FileListing::replacedBytesMatches` with tests
- [x] Task manager progress: left to Plasma (the average of the jobs of the application)
- [x] Current file cut at the start with "…", the status line at the end - they do not widen the window
- [x] *Save the log to a file* under the log (an option like the others, on by default) - `RunLog`,
      one file per run in `~/.local/state/rsync-gui/logs`
- [x] *Quit* button next to *Copy* and *Cancel*
- [x] Checks on the server before running there (RemoteProbe: commands allowed, rsync, sources, destination,
      free space, real paths), `mkdir -p` of the destination, removing emptied source directories there, `du`
      over ssh for the speed graph, one ssh login per job (`ControlMaster`)
- [x] Network shares: CIFS/sshfs mounts from `/proc/self/mountinfo`, per-share settings (*File → Network
      shares…*, offered on first use), copies routed over ssh / on the server (`ShareRouting`)
- [x] rsync on the server for sources and a destination on the same remote host (`sftp://`, `host:path`):
      `ssh host 'rsync ...'`, arguments quoted for a POSIX shell (also names that are not valid UTF-8),
      cancelling by the PID of the remote rsync, option *Run on the server* (step 1 of docs/plan-server-side.md)

## To do

- [ ] Application icon (currently the `folder-sync` theme icon)
- [ ] Translations (`tr()` is used in the code, there are no `.ts` files yet)
- [ ] Nautilus / Nemo integration (scripts or an extension)
- [ ] `.deb` package / AppImage, CI (Gitea Actions: build + tests)
- [ ] Run rsync on the server / use ssh for network shares (docs/plan-server-side.md, steps 1-6 done 2026-11-15):
  - [ ] Checked by hand with the user's NAS: a CIFS share set up with *Detect* (testparm), a copy within a share
        on the server, local -> share over ssh, the ownership note (`force user` / masks)
  - [x] `--chmod` from Samba's masks and forced modes for copies over ssh to a share (*Modes* in the share dialog)
  - [x] Tests: a Samba configuration in the `tools/ssh-test` container for *Detect* (testparm; mounting CIFS needs
        root - checked by hand)
  - [ ] Tests with real sftp servers (by hand, with the user):
    - [ ] an account for sftp only (`ForceCommand internal-sftp`): the clear "did not run the command" error
          (checked once on 2026-11-15), and from step 2 on: detected by the probe before anything starts
    - [ ] an account restricted to rsync with `rrsync` and a key of its own (`command=` in authorized_keys):
          copies between the server and this computer work, copying on the server is refused with a clear message
    - [ ] a separate key for rsync in *Remote shell* (`ssh -i ...`) while Dolphin uses another one for sftp
    - [ ] `sftp://` locations with `~` (Dolphin's home-relative paths), a port and a user, on the server side
    - [ ] cancelling on a real server (the remote rsync is gone afterwards), with a password login (ksshaskpass)
- [ ] Very low priority: start the copy right after choosing the destination from Dolphin (`--start`).
      To consider instead: copy & paste - *Copy* in Dolphin (the URLs in the clipboard), then a service menu
      entry on the destination directory, e.g. *Rsync → Paste here*, that takes the sources from the clipboard
      (on Wayland the clipboard can be read only by a focused window - the window would have to show first)
