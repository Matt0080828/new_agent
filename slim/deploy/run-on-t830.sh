#!/bin/bash
# Push the deploy bundle to the T830 over adb and run the on-device smoke test.
#
# adb runs inside a privileged container on purpose: the host's USB node
# (/dev/bus/usb/001/*) is root-only, and a container sidesteps adding a udev rule. The
# container is reused, so only the first run pays for installing adb.
#
# Everything stays in /tmp on the device, and the smoke test never deletes anything: it
# writes into a fresh /tmp/slim-smoke/data-$$ directory.
#
# Usage: ./run-on-t830.sh [--no-smoke] [--binary PATH]
#
# Run it from a clone (the artifact is looked up in this directory first, then in
# ../cpp, so `make -C slim/cpp t830-static` beforehand is enough) or from the deploy
# bundle. --binary overrides the lookup.
set -u

NAME=adb-t830-run
DEPLOY=$(cd "$(dirname "$0")" && pwd)
REMOTE=/tmp/slim-smoke
RUN_SMOKE=1
BINARY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-smoke) RUN_SMOKE=0 ;;
    --binary) shift; BINARY=${1:-} ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '  %s\n' "$*"; }
die() { printf '  FATAL: %s\n' "$*" >&2; exit 1; }

# 1. container with adb
if docker inspect "$NAME" >/dev/null 2>&1; then
  say "reusing container $NAME"
else
  say "creating container $NAME"
  docker run -d --name "$NAME" --privileged -v /dev/bus/usb:/dev/bus/usb \
    debian:bookworm-slim sleep infinity >/dev/null || die "docker run failed"
fi
if ! docker exec "$NAME" bash -c 'command -v adb >/dev/null 2>&1'; then
  say "installing adb in the container (first run only)"
  docker exec "$NAME" bash -c 'apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq adb >/dev/null 2>&1' \
    || die "cannot install adb"
fi

# 2. is the device there and authorised?
DEVLINE=$(docker exec "$NAME" bash -c 'adb start-server >/dev/null 2>&1; adb devices' | sed -n '2p')
case "$DEVLINE" in
  *"	device"*) say "device: $DEVLINE" ;;
  *unauthorized*) die "device is unauthorized: accept the adb prompt on the CPE, or enable adb in its UI" ;;
  *) die "no device attached (adb devices: '$DEVLINE')" ;;
esac

# 3. push, then prove the bytes arrived intact
# 0. find the artifact and the skills directory
BIN_LOCAL=$BINARY
if [ -z "$BIN_LOCAL" ]; then
  for c in "$DEPLOY/slim-agent-t830-static" "$DEPLOY/../cpp/slim-agent-t830-static"; do
    if [ -f "$c" ]; then
      BIN_LOCAL=$(cd "$(dirname "$c")" && pwd)/$(basename "$c")
      break
    fi
  done
fi
[ -n "$BIN_LOCAL" ] || die "no slim-agent-t830-static found - build it (make -C slim/cpp t830-static) or pass --binary PATH"
[ -f "$BIN_LOCAL" ] || die "not a file: $BIN_LOCAL"
SKILLS_SRC="$DEPLOY/skills"
if [ ! -d "$SKILLS_SRC" ]; then
  SKILLS_SRC=$(cd "$DEPLOY/../skills" 2>/dev/null && pwd) || SKILLS_SRC=""
fi
say "artifact: $BIN_LOCAL ($(stat -c%s "$BIN_LOCAL") bytes, sha256 $(sha256sum "$BIN_LOCAL" | cut -c1-16)...)"

say "pushing the bundle to $REMOTE"
docker exec "$NAME" adb shell "mkdir -p $REMOTE" >/dev/null
# adb runs INSIDE the container and cannot read host paths: stage with docker cp,
# then push from there. A direct host-path push fails with a bare "push failed".
docker cp "$BIN_LOCAL" "$NAME:/tmp/stage-binary" >/dev/null || die "docker cp of the artifact failed"
docker exec "$NAME" adb push /tmp/stage-binary "$REMOTE/slim-agent-t830-static" >/dev/null || die "push failed"
docker cp "$DEPLOY/on-device-smoke.sh" "$NAME:/tmp/stage-smoke" >/dev/null || die "docker cp of the smoke test failed"
docker exec "$NAME" adb push /tmp/stage-smoke "$REMOTE/on-device-smoke.sh" >/dev/null || die "push failed"
if [ -n "$SKILLS_SRC" ]; then
  docker exec "$NAME" adb shell "rm -rf $REMOTE/skills" >/dev/null 2>&1
  if docker cp "$SKILLS_SRC" "$NAME:/tmp/stage-skills" >/dev/null 2>&1 && \
     docker exec "$NAME" adb push /tmp/stage-skills "$REMOTE/skills" >/dev/null 2>&1; then
    say "skills pushed from $SKILLS_SRC"
  else
    say "note: skills push failed (optional)"
  fi
fi
docker exec "$NAME" adb shell "chmod +x $REMOTE/slim-agent-t830-static $REMOTE/on-device-smoke.sh"

LOCAL_SUM=$(sha256sum "$BIN_LOCAL" | awk '{print $1}')
REMOTE_SUM=$(docker exec "$NAME" adb shell "sha256sum $REMOTE/slim-agent-t830-static 2>/dev/null || md5sum $REMOTE/slim-agent-t830-static" | awk '{print $1}')
if [ "$LOCAL_SUM" = "$REMOTE_SUM" ]; then
  say "sha256 matches: $LOCAL_SUM"
else
  die "checksum mismatch (local $LOCAL_SUM vs device $REMOTE_SUM)"
fi

# 4. run it
if [ "$RUN_SMOKE" = "1" ]; then
  say "running the on-device smoke test"
  echo
  docker exec "$NAME" adb shell "cd $REMOTE && sh on-device-smoke.sh $REMOTE" 2>&1
else
  say "skipping the smoke test (--no-smoke)"
fi
