#!/usr/bin/env bash
# Upload build/grohe_dial.bin to the Grohe Blue Dial's local-network OTA
# endpoint (components/ota/) and wait for it to reboot into the new
# image. See docs/ARCHITECTURE.md's "OTA" section for the endpoint's own
# design (plain HTTP, no TLS, shared-secret auth -- see that section for
# why this is a trusted-local-network mechanism, not an Internet-facing
# one).
#
# Usage:
#   idf.py build
#   ./scripts/ota.sh <device-ip>
#   ./scripts/ota.sh --host 192.168.1.42
#   GROHE_DIAL_HOST=192.168.1.42 ./scripts/ota.sh
#
# Find the device's IP in its serial log ("Wi-Fi connected ... ip=...")
# after boot -- this script does not attempt Wi-Fi discovery.
#
# The OTA shared secret is read from the same gitignored
# components/ota/include/ota/ota_secret_local.hpp the firmware itself
# reads (one file, one place to edit) -- override with
# GROHE_DIAL_OTA_TOKEN if you'd rather not have this script read that file.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_PATH="$REPO_ROOT/build/grohe_dial.bin"
SECRET_FILE="$REPO_ROOT/components/ota/include/ota/ota_secret_local.hpp"

HOST="${GROHE_DIAL_HOST:-}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      HOST="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      HOST="$1"
      shift
      ;;
  esac
done

if [[ -z "$HOST" ]]; then
  echo "error: no device IP given -- pass one (./scripts/ota.sh 192.168.1.42)," >&2
  echo "       or set GROHE_DIAL_HOST. Find it in the device's serial log" >&2
  echo "       (\"Wi-Fi connected ... ip=...\") after boot." >&2
  exit 1
fi

if [[ ! -f "$BIN_PATH" ]]; then
  echo "error: $BIN_PATH not found -- run 'idf.py build' first." >&2
  exit 1
fi

TOKEN="${GROHE_DIAL_OTA_TOKEN:-}"
if [[ -z "$TOKEN" ]]; then
  if [[ ! -f "$SECRET_FILE" ]]; then
    echo "error: $SECRET_FILE not found." >&2
    echo "       Copy ota_secret_local.hpp.example to ota_secret_local.hpp and fill in" >&2
    echo "       a secret (the same file the firmware itself reads), or set" >&2
    echo "       GROHE_DIAL_OTA_TOKEN." >&2
    exit 1
  fi
  TOKEN="$(sed -n 's/.*kLocalOtaSecret\[\] *= *"\(.*\)".*/\1/p' "$SECRET_FILE")"
fi

if [[ -z "$TOKEN" ]]; then
  echo "error: OTA secret is empty ($SECRET_FILE) -- fill it in on both the device" >&2
  echo "       and here; the device won't accept this upload otherwise." >&2
  exit 1
fi

BIN_SIZE="$(du -h "$BIN_PATH" | cut -f1)"
echo "Device:   $HOST"
echo "Firmware: $BIN_PATH ($BIN_SIZE)"
echo "Uploading..."

RESPONSE_FILE="$(mktemp)"
trap 'rm -f "$RESPONSE_FILE"' EXIT

set +e
HTTP_CODE="$(curl -sS -# \
  -X POST \
  -H "X-OTA-Token: $TOKEN" \
  --data-binary "@$BIN_PATH" \
  -o "$RESPONSE_FILE" \
  -w '%{http_code}' \
  "http://$HOST/ota")"
CURL_EXIT=$?
set -e

if [[ $CURL_EXIT -ne 0 ]]; then
  echo "error: could not reach $HOST (curl exit $CURL_EXIT)" >&2
  exit 1
fi
if [[ "$HTTP_CODE" != "200" ]]; then
  echo "error: device rejected the upload (HTTP $HTTP_CODE)" >&2
  cat "$RESPONSE_FILE" >&2
  exit 1
fi

echo "Upload accepted (HTTP $HTTP_CODE) -- device is rebooting."
echo -n "Waiting for it to come back..."

for _ in $(seq 1 30); do
  sleep 1
  if curl -sS -m 2 "http://$HOST/version" >"$RESPONSE_FILE" 2>/dev/null; then
    echo " up."
    echo "New version:"
    cat "$RESPONSE_FILE"
    exit 0
  fi
  echo -n "."
done

echo
echo "warning: device did not come back within 30s -- check it manually" >&2
echo "         (serial log, or USB recovery if it's stuck)." >&2
exit 0
