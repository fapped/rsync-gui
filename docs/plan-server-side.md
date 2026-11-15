# Plan: running rsync on the server

All steps are done (2026-11-15); this file stays as the record of the design. Differences from the plan:
the shares of steps 3 and 4 share one dialog and one settings format (sshfs is opt-in too, as an sftp-only
account can be mounted with sshfs but cannot run rsync); the ownership check shows Samba's settings in the
share dialog instead of a warning per copy; `--chmod` from the masks is left in TODO.

The TODO item "Run rsync on the server when the source and the destination are on the same host", split into
steps that can each be committed, tested and used on their own. The order puts first what gives the most with
the least risk: `sftp://` on both sides needs no mount detection and no settings.

## Where the code is today

- `Location` is either local or `user@host:port:path`; `originKey()` identifies a remote origin.
- `RsyncJob::plan()` groups the sources into `Transfer`s (one rsync process each) and rejects a remote source
  with a remote destination ("rsync cannot copy between two remote locations").
- `RsyncJob::startProcess()` runs `program` + `arguments(port)` + `-- sources destination` locally; Copy, Verify
  and Remove are three phases of the same transfer. Cancelling = `SIGTERM`, `SIGKILL` after 3 s.
- Local-only work around the transfer: the existence of the sources and "destination inside a source"
  (`plan()`), creating the destination and the FAT `--modify-window` probe (`MainWindow::startCopy`), the total
  size for the graph (`measureSources`), dropping the page cache before verification and removing emptied source
  directories after a move (`RsyncJob`).
- `shellQuote()` writes `$'...\xff...'` for display only - it is bash syntax, not usable for a remote `sh`.
- `tools/ssh-test` runs the tests against an Alpine container with sshd + rsync (busybox `sh`, `find`, `du`).

## Step 1 - the core: a transfer that runs on a host (done 2026-11-15)

1. `Transfer` gets `QString runOn` (empty = local; else the ssh target, the `originKey` of the host) and the
   ssh port. Sources and the destination are then the paths **on that host** (no `host:` prefix).
2. `RemoteCommand` (new, in `rsynccore`):
   - `quote(arg)` for a POSIX shell: `'...'` with `'\''` for quotes; the bytes of names that are not valid UTF-8
     stay raw bytes inside the quotes (they go through `setRawArguments` like today) - works in dash, busybox ash,
     bash. The login shell of the ssh user parses the command, so wrapping it in `sh -c` would not help with a
     shell that is not POSIX (fish, csh): such a shell is detected by the probe (step 2) and refused with a clear
     message.
   - `sshArguments(target, port, shell)`: `ssh [-p N] [-o ControlPath=...] user@host --` + the quoted remote
     command; honours the custom remote shell option (`-e`), split like today.
3. `RsyncJob::startProcess()`: for `runOn` transfers the local program is `ssh` and the remote command is
   `rsync <same arguments> -- <sources> <destination>`. The output comes back over ssh unchanged, so progress,
   the file counter, the verification parser and exit codes work as they are (ssh returns rsync's code; 255 =
   ssh's own error, already described).
4. Cancelling: killing the local ssh does not stop the remote rsync at once (no pty; it dies on SIGPIPE only when it
   next writes, which with no progress output may be never). The remote command starts as
   `echo "RSYNC-GUI-PID $$"; exec rsync ...` - the job reads the PID from the first line (not logged) and
   `cancel()` runs `ssh host kill -TERM <pid>` before terminating the local ssh. Rejected: `ssh -tt` (a pty
   mangles non-UTF-8 output, needs `\r\n` handling and merges password prompts into the output).
5. `plan()`: a remote source with a remote destination **on the same origin** becomes one `runOn` transfer
   (when the option from step 4 is on); different hosts stay an error (host to host would need ssh keys between
   the servers - out of scope).
6. Command preview and the confirmation dialog show the full `ssh host 'rsync ...'`.
7. Tests (unit): quoting round trip through `sh -c 'printf %s "$1"'` locally (dash and busybox if installed,
   names with `'`, spaces, newlines, non-UTF-8 bytes); `plan()` for same host / different hosts / option off.
   Tests (container, `tools/ssh-test`): copy within the server, verify, move + verified move, non-UTF-8 names,
   cancel (afterwards no rsync process left in the container: `docker exec ... pgrep rsync`).

Result: two `sftp://` locations of one server (Dolphin) work, and the data stays on the server.

## Step 2 - checks over ssh (the probe) (done 2026-11-15)

Before the job starts, one ssh call per host runs a small `sh` script (no bash, no GNU-only options - busybox
must do) and prints machine-readable lines:

- the account runs commands at all: accounts for sftp only (`ForceCommand internal-sftp`, "This service allows
  sftp connections only.") cannot run rsync - such a host is used through the mount only (steps 3-5 fall back
  to the mount for it, with a note instead of an error); since step 1 the failed run already says so;
- the login shell is POSIX (`set -o` / `$((1+1))` works), `rsync --version` (missing rsync = clear error);
- for every source: exists, is a directory, `realpath` (for "destination inside a source");
- the destination: exists / is a directory / its nearest existing parent, writable; `df -Pk` free space;
- total size for the progress graph: `du -sk` (busybox has no `-b`; KiB precision is enough for a graph).

The probe is asynchronous (a `QProcess`, the window shows "Checking the server..."), cancellable, with a
timeout. Its results feed the same dialogs as the local checks: missing sources, create the destination
(`mkdir -p` over ssh, after asking), destination inside a source, not enough space (warning).

After the job, for a move: removing emptied source directories over ssh with the same rules as
`removeEmptyDirectories` (only `rmdir`, deepest first, no symlinks, not across file systems):
`find SRC -xdev -depth -type d -exec rmdir {} \; 2>/dev/null` + listing what is left.

Not possible on the server: dropping the page cache before verification (needs root) - the log says the
verification may read from the server's cache; the FAT `--modify-window` probe - done in the probe script with
`touch -d` on a temporary file when the destination is on a vfat mount (`stat -f -c %T`), else skipped.

**Connection sharing**: the probe, the transfers (Copy, Verify, Remove) and the kill would each ask for a
password. All ssh calls of a job use `-o ControlMaster=auto -o ControlPath=$XDG_RUNTIME_DIR/rsync-gui/%C
-o ControlPersist=60` - one login per job. Only added when the remote shell is plain `ssh` (a custom `-e`
command is left as it is).

## Step 3 - mounted shares: sshfs (done 2026-11-15)

`MountTable` (new, in `rsynccore`) parses `/proc/self/mountinfo` (octal escapes `\040` etc., the mount root
field for mounts of a subdirectory) and maps a local path to `(mount, path inside the mount)`.

- `fuse.sshfs`: the source field is `[user@]host:/path` - the server path is that + the path inside the mount.
  The port is not in mountinfo: taken from `~/.ssh/config` by ssh itself, or set in the settings (step 4).
- A local path on such a mount becomes a remote `Location` for the planning only; what the user typed is kept
  for the GUI, the log and "Open destination".
- Tests: mountinfo fixtures (unit). A real sshfs mount of the test container works without root
  (`fusermount3`) - added to `tools/ssh-test/run.sh` when `sshfs` is installed, skipped otherwise.

## Step 4 - mounted shares: CIFS (done 2026-11-15)

- `cifs` mounts give `//server/share` (+ a `prefixpath` for a subdirectory). The ssh target and the path of the
  share on the server are not in it:
  - the ssh target: `server` by default (as in the mount), editable;
  - the path: read once with `ssh server testparm -s --section-name=SHARE --parameter-name=path` (Samba);
    when that fails (no testparm, no access) the user types it.
- Remembered per share in QSettings (`servers/<//server/share>`: ssh target, port, path, "use the server").
  A small *Servers…* dialog (File menu) lists the detected shares and lets the user edit or turn them off.
- **Opt-in per share**: a share is used over ssh only after the user has confirmed its values once (they decide
  where the files go and who owns them). Until then everything goes through the mount as today - nothing changes
  for anyone without them knowing. The first time a detected share is used, the window offers to set it up
  (*Set up…* / *Not now* / *Never for this share*).
- Ownership: files are created by the ssh user, not by Samba. The probe reads `force user` / `force group` /
  `create mask` / `directory mask` (testparm) and warns when they would give a different owner or mode; an
  option adds `--chmod` from the masks. The ssh user needs a shell (some NAS accounts do not have one - the
  probe reports it).
- Tests: testparm parsing (unit), a Samba share in the test container (`testparm` query only; mounting CIFS
  needs root - checked by hand with the user's NAS).

## Step 5 - choosing the transfer, the option, warnings (done 2026-11-15)

Pure function `chooseRoute(source, destination, settings)` with unit tests for every combination:

| source | destination | route |
|---|---|---|
| host A (sftp / sshfs / CIFS mapped) | host A | rsync on A, A's paths |
| local | mount of host A (confirmed share) | rsync over ssh to A instead of through the mount (delta transfer, the file list and the verification checksums computed on A) |
| mount of host A (confirmed share) | local | rsync over ssh from A |
| share not confirmed / option off | anything | through the mount as today; when both sides are network mounts: a warning that the data crosses the network twice, with a link to set the share up |
| host A | host B | error, as today |

- Option *Run on the server when possible* (default on; in the options, remembered, in presets/JSON like the
  others). It covers `sftp://` locations and confirmed shares; there is no separate switch for "local ↔ share":
  the confirmation of a share is that switch.
- The source list tooltip and the info line say where each source really is ("on server X, /volume1/...").

## Step 6 - documentation, screenshots (done 2026-11-15)

README (a section on running on the server: requirements - ssh login with a POSIX shell and rsync on the server,
what is checked, ownership), NOTES (decisions above), TODO ticked; screenshots only if the window changes.

## Decisions (answered by the user, 2026-11-15)

1. Real device for steps 3-5: a NAS with ssh (key login), bash and rsync, three CIFS shares mounted from fstab
   (only the mounted ones appear in mountinfo). `testparm` there is not known yet - the probe falls back to
   asking for the path. The device's address and share names stay out of the repository (tests use the
   container; the NAS is checked by hand).
2. Authentication: keys preferred, but the tool is general - a password is asked for when needed (ksshaskpass
   via `SSH_ASKPASS`, as today), once per job thanks to the shared connection (`ControlMaster`).
3. "Local ↔ mounted share over ssh": opt-in per share (the confirmation in step 4), plus the global option
   *Run on the server when possible* to turn the whole feature off (e.g. in a preset).
