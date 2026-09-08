#!/usr/bin/env bash
# Full Wi-Fi OTA workflow for the Grohe Blue Dial: builds the firmware,
# shows exactly what's about to be flashed, uploads it to the device's
# local-network OTA endpoint (components/ota/), waits for it to reboot,
# and confirms what actually ended up running -- all in one command:
#
#   ./scripts/ota.sh <device-ip>
#
# Usage:
#   ./scripts/ota.sh <device-ip>
#   ./scripts/ota.sh --host 192.168.1.42
#   GROHE_DIAL_HOST=192.168.1.42 ./scripts/ota.sh
#
# Steps: (1) make sure idf.py is available -- uses it straight off PATH if
# already sourced, otherwise sources $IDF_PATH/export.sh itself, otherwise
# aborts with a clear message (never assumes a local setup that isn't
# already there); (2) idf.py build, aborting immediately on any build
# error -- never uploads a stale/missing binary; (3) prints the
# version/commit/branch that was just built, read from the actual build
# output (esptool.py image_info + the generated firmware_info_git.h), not
# blindly from version.txt; (4) uploads build/grohe_dial.bin exactly as
# before (X-OTA-Token from ota_secret_local.hpp, POST /ota, HTTP status
# checked, upload progress shown -- no protocol changes, no TLS); (5) polls
# GET /version until the device comes back; (6) prints what's actually
# running now; (7) compares the two and reports OTA SUCCESS, or a clear
# mismatch warning/error with a non-zero exit code.
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
GIT_HEADER="$REPO_ROOT/build/esp-idf/firmware_info/firmware_info_git.h"

HOST="${GROHE_DIAL_HOST:-}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      HOST="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'
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

# Prints a bordered "key: value" block, its border width matched to the
# heading -- used for both the pre-upload (just-built) and post-upload
# (actually running) version summaries, so the two are visually
# unmistakably parallel.
print_version_block() {
  local heading="$1" version="$2" commit="$3" branch="$4"
  echo "=== $heading ==="
  echo "Version: $version"
  echo "Commit:  $commit"
  echo "Branch:  $branch"
  printf '=%.0s' $(seq 1 $((${#heading} + 8)))
  echo
}

# --- 1. ESP-IDF environment -------------------------------------------
# Never assumes a local setup that doesn't already exist: uses idf.py
# straight off PATH if the caller already sourced export.sh themselves
# (the common case, matching README.md's own Quick start), otherwise
# tries $IDF_PATH/export.sh (ESP-IDF's own standard env var -- not
# something this script invents), otherwise aborts with exactly what's
# missing rather than guessing a path.
if ! command -v idf.py >/dev/null 2>&1; then
  if [[ -z "${IDF_PATH:-}" ]]; then
    echo "error: idf.py not found on PATH, and \$IDF_PATH is not set." >&2
    echo "       Run '. \$IDF_PATH/export.sh' in this shell first (see" >&2
    echo "       README.md's Quick start), or set IDF_PATH to your ESP-IDF" >&2
    echo "       checkout and re-run." >&2
    exit 1
  fi
  if [[ ! -f "$IDF_PATH/export.sh" ]]; then
    echo "error: \$IDF_PATH is set ($IDF_PATH) but $IDF_PATH/export.sh" >&2
    echo "       doesn't exist -- check that IDF_PATH actually points at" >&2
    echo "       an ESP-IDF checkout." >&2
    exit 1
  fi
  echo "idf.py not on PATH -- sourcing \$IDF_PATH/export.sh ..."
  set +e
  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh" >/dev/null
  export_status=$?
  set -e
  if [[ $export_status -ne 0 ]] || ! command -v idf.py >/dev/null 2>&1; then
    echo "error: sourcing \$IDF_PATH/export.sh did not make idf.py available." >&2
    exit 1
  fi
fi

# --- 2. Build ------------------------------------------------------------
# -C, not a `cd`, so this works regardless of the caller's own working
# directory. set -e (above) already aborts this whole script immediately
# on any build failure -- no OTA attempt with a stale/missing binary.
echo "=== Building firmware ==="
idf.py -C "$REPO_ROOT" build

if [[ ! -f "$BIN_PATH" ]]; then
  echo "error: $BIN_PATH not found after a successful build -- unexpected" >&2
  echo "       app/project name? Check idf.py build's own output above." >&2
  exit 1
fi

# --- 3. Version to be flashed, read from the build output itself -------
# Deliberately not version.txt: that file only records the human-chosen
# version string, and two different builds routinely share the same one
# (it isn't bumped per-commit) -- the actual identity of *this* build is
# the app version esptool.py reads back out of the binary that's about to
# be uploaded, plus the git commit CMake baked into it (firmware_info_git.h,
# regenerated by every build -- see components/firmware_info/CMakeLists.txt).
BUILT_VERSION="$(esptool.py image_info --version 2 "$BIN_PATH" 2>/dev/null | sed -n 's/^App version: //p')"
if [[ -z "$BUILT_VERSION" ]]; then
  echo "error: could not read the app version out of $BIN_PATH (esptool.py image_info)." >&2
  exit 1
fi

if [[ -f "$GIT_HEADER" ]]; then
  BUILT_COMMIT="$(sed -n 's/.*FIRMWARE_INFO_GIT_COMMIT "\(.*\)".*/\1/p' "$GIT_HEADER")"
  BUILT_BRANCH="$(sed -n 's/.*FIRMWARE_INFO_GIT_BRANCH "\(.*\)".*/\1/p' "$GIT_HEADER")"
  BUILT_DIRTY="$(sed -n 's/.*FIRMWARE_INFO_GIT_DIRTY \([01]\).*/\1/p' "$GIT_HEADER")"
else
  BUILT_COMMIT="unknown"
  BUILT_BRANCH="unknown"
  BUILT_DIRTY="0"
fi
# Dirty suffix goes on the branch, matching firmware_info::GitDirty()'s own
# display convention in the device's /version response exactly (see
# ota_server.cpp's HandleVersionGet(): "branch=%s%s") -- not on the commit,
# which must stay a raw, directly comparable hash on both sides of the
# built-vs-running check below.
[[ "$BUILT_DIRTY" == "1" ]] && BUILT_BRANCH="$BUILT_BRANCH (dirty)"

echo
print_version_block "Firmware to flash" "$BUILT_VERSION" "$BUILT_COMMIT" "$BUILT_BRANCH"
echo

# --- 4. Secret -----------------------------------------------------------
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

# --- 5. Upload (unchanged OTA protocol) -----------------------------------
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

# --- 6. Wait for reboot (unchanged /version polling) ----------------------
echo -n "Waiting for it to come back..."

DEVICE_BACK_UP=0
for _ in $(seq 1 30); do
  sleep 1
  if curl -sS -m 2 "http://$HOST/version" >"$RESPONSE_FILE" 2>/dev/null; then
    DEVICE_BACK_UP=1
    break
  fi
  echo -n "."
done
echo

if [[ "$DEVICE_BACK_UP" -ne 1 ]]; then
  echo "error: device did not come back within 30s -- check it manually" >&2
  echo "       (serial log, or USB recovery if it's stuck)." >&2
  exit 1
fi

echo "Device is back up."
echo

# --- 7. Version actually running, and the verdict -------------------------
RUNNING_VERSION="$(sed -n 's/^version=//p' "$RESPONSE_FILE")"
RUNNING_COMMIT="$(sed -n 's/^commit=//p' "$RESPONSE_FILE")"
RUNNING_BRANCH="$(sed -n 's/^branch=//p' "$RESPONSE_FILE")"

if [[ -z "$RUNNING_VERSION" || -z "$RUNNING_COMMIT" ]]; then
  echo "error: could not parse a version/commit out of the device's /version response:" >&2
  cat "$RESPONSE_FILE" >&2
  exit 1
fi

print_version_block "Firmware running on device" "$RUNNING_VERSION" "$RUNNING_COMMIT" "$RUNNING_BRANCH"
echo

if [[ "$RUNNING_VERSION" == "$BUILT_VERSION" && "$RUNNING_COMMIT" == "$BUILT_COMMIT" ]]; then
  echo "OTA SUCCESS -- device is running the firmware just built and flashed."
  exit 0
fi

echo "OTA WARNING: device is reachable, but is not running the firmware just flashed." >&2
echo "  built:   version=$BUILT_VERSION commit=$BUILT_COMMIT" >&2
echo "  running: version=$RUNNING_VERSION commit=$RUNNING_COMMIT" >&2
exit 1
