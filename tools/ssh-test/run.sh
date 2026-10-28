#!/bin/bash
# Runs the tests (including the remote ones, which are skipped otherwise) against a throwaway ssh
# server with rsync in a Docker container, listening on 127.0.0.1 only. ssh uses a key and a
# known_hosts file of its own in a temporary directory - the user's ~/.ssh is neither used nor changed.
#
#   tools/ssh-test/run.sh [build directory]        (default: build)
#
# The container and the temporary directory are removed at the end.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "${1:-$repo/build}" && pwd)
port=${RSYNC_GUI_SSH_TEST_PORT:-2222}
image=rsync-gui-ssh-test
name=rsync-gui-ssh-test-$$
work=$(mktemp -d)

cleanup() {
    docker rm -f "$name" > /dev/null 2>&1 || true
    rm -rf -- "$work"
}
trap cleanup EXIT

docker build -q -t "$image" "$repo/tools/ssh-test" > /dev/null
ssh-keygen -q -t ed25519 -N '' -C rsync-gui-ssh-test -f "$work/key"
docker run -d --rm --name "$name" -p "127.0.0.1:$port:22" "$image" > /dev/null
docker exec -i "$name" sh -c 'mkdir -p /home/tester/.ssh && cat > /home/tester/.ssh/authorized_keys &&
    chown -R tester:tester /home/tester/.ssh && chmod 700 /home/tester/.ssh && chmod 600 /home/tester/.ssh/authorized_keys' \
    < "$work/key.pub"
for _ in $(seq 50); do
    ssh-keyscan -p "$port" 127.0.0.1 > /dev/null 2>&1 && break
    sleep 0.2
done

export RSYNC_GUI_TEST_SSH="sftp://tester@127.0.0.1:$port/home/tester"
export RSYNC_GUI_TEST_SSH_SHELL="ssh -F /dev/null -i $work/key -o IdentitiesOnly=yes -o BatchMode=yes \
-o UserKnownHostsFile=$work/known_hosts -o StrictHostKeyChecking=accept-new"
# The password of the test user (see the Dockerfile), for the test of password logins through SSH_ASKPASS.
export RSYNC_GUI_TEST_SSH_PASSWORD=tester-password

"$build/tst_rsynccore" "${@:2}"
