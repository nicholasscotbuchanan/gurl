#!/bin/sh
# Manage the local Samba (SMB 3.1.1) container used to test curl's
# libsmb2-backed smb:// handler.
#
#   ./smb3-server.sh start     build (if needed) and run the server
#   ./smb3-server.sh stop      remove the container
#   ./smb3-server.sh status    show container + live session state
#   ./smb3-server.sh encrypt   require SMB3 encryption (AES-CCM/GCM) and reload
#   ./smb3-server.sh logs      tail smbd output
#
# Server exposes //127.0.0.1/share as smbuser/smbpass on port 445.
set -eu

IMAGE=curl-smb3-test
NAME=curl-smb3
DIR=$(cd "$(dirname "$0")" && pwd)

# podman on macOS, docker elsewhere - use whichever is present.
if command -v podman >/dev/null 2>&1; then RT=podman
elif command -v docker >/dev/null 2>&1; then RT=docker
else echo "need podman or docker" >&2; exit 1
fi

case "${1:-start}" in
  start)
    "$RT" image exists "$IMAGE" 2>/dev/null || "$RT" build -t "$IMAGE" "$DIR"
    "$RT" rm -f "$NAME" >/dev/null 2>&1 || true
    "$RT" run -d --name "$NAME" -p 445:445 "$IMAGE" >/dev/null
    sleep 2
    echo "SMB 3.1.1 server up: smb://127.0.0.1/share  (smbuser/smbpass)"
    ;;
  stop)
    "$RT" rm -f "$NAME" >/dev/null 2>&1 || true
    echo "stopped"
    ;;
  status)
    "$RT" ps --filter "name=$NAME" --format '{{.Names}} {{.Status}} {{.Ports}}'
    echo "--- live session (dialect / encryption / signing) ---"
    # Loopback sessions are very short-lived, so poll for one rather than
    # taking a single sample that almost always misses it.
    i=0
    while [ "$i" -lt 100 ]; do
      line=$("$RT" exec "$NAME" smbstatus 2>/dev/null | grep -E 'SMB[0-9]' || true)
      if [ -n "$line" ]; then echo "$line"; break; fi
      i=$((i+1))
    done
    [ -n "${line:-}" ] || echo "(no active session - run a transfer while this polls)"
    ;;
  encrypt)
    "$RT" exec "$NAME" sh -c \
      "sed -i 's/^   smb encrypt = .*/   smb encrypt = required/' /etc/samba/smb.conf && smbcontrol smbd reload-config"
    echo "SMB3 encryption now REQUIRED"
    ;;
  logs)
    "$RT" logs "$NAME"
    ;;
  *)
    echo "usage: $0 {start|stop|status|encrypt|logs}" >&2; exit 2
    ;;
esac
