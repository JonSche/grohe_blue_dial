#!/usr/bin/env bash
# Provisions this dial's Grohe BLE credentials (USER_ID / PRESHARED_KEY) onto
# the device's local-network provisioning endpoint (components/provisioning/)
# -- one command instead of manually copying values out of .env by hand:
#
#   ./scripts/provision.sh <device-ip>
#
# Usage:
#   ./scripts/provision.sh <device-ip>
#   ./scripts/provision.sh --host 192.168.1.42
#   GROHE_DIAL_HOST=192.168.1.42 ./scripts/provision.sh
#   ./scripts/provision.sh <device-ip> --from-cloud
#
# Credentials source -- two modes:
#
#   default: ../grohe_blue_ble/.env next to this repo (override with
#   GROHE_BLUE_BLE_ENV=/path/to/.env) -- the same .env grohe_blue_ble's own
#   examples already read USER_ID/PRESHARED_KEY from. Just forwards
#   whatever is already in that file, unmodified -- no Grohe Cloud call.
#
#   --from-cloud: fetches fresh credentials straight from the Grohe Cloud
#   instead (scripts/grohe_cloud_refresh.py, which reuses
#   github.com/koproductions-code/grohe for the actual login/refresh/
#   dashboard calls -- see that script's own docstring) -- no grohe_blue_ble
#   checkout needed at all. One-time setup:
#     python3 -m pip install -r scripts/requirements.txt
#     python3 scripts/grohe_cloud_bootstrap.py
#   See docs/ARCHITECTURE.md's Provisioning section for the full story.
#
# Provisioning token: read from the same gitignored
# components/provisioning/include/provisioning/provisioning_secret_local.hpp
# the firmware itself reads (one file, one place to edit) -- override with
# GROHE_DIAL_PROVISION_TOKEN if you'd rather not have this script read that
# file.
#
# Credential values (USER_ID, PRESHARED_KEY, the provisioning token, and in
# --from-cloud mode the refresh/access token) are never printed, logged, or
# passed as a command-line argument to this script -- only held in shell
# variables for as long as needed and sent straight to curl. See
# SECURITY.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PARENT_DIR="$(cd "$REPO_ROOT/.." && pwd)"
ENV_FILE="${GROHE_BLUE_BLE_ENV:-$PARENT_DIR/grohe_blue_ble/.env}"
SECRET_FILE="$REPO_ROOT/components/provisioning/include/provisioning/provisioning_secret_local.hpp"
CLOUD_REFRESH_SCRIPT="$SCRIPT_DIR/grohe_cloud_refresh.py"
PROVISION_PORT=8080

# --- 1. Device host and credential source -----------------------------------
HOST="${GROHE_DIAL_HOST:-}"
FROM_CLOUD=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      HOST="$2"
      shift 2
      ;;
    --from-cloud)
      FROM_CLOUD=1
      shift
      ;;
    -h|--help)
      sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      HOST="$1"
      shift
      ;;
  esac
done

if [[ -z "$HOST" ]]; then
  echo "error: no device IP given -- pass one (./scripts/provision.sh 192.168.178.64)," >&2
  echo "       or set GROHE_DIAL_HOST." >&2
  exit 1
fi

echo "=== Grohe Dial Provisioning ==="
echo "Device: $HOST"
if [[ "$FROM_CLOUD" -eq 1 ]]; then
  echo "Credentials: Grohe Cloud (scripts/grohe_cloud_refresh.py)"
else
  echo "Credentials: $ENV_FILE"
fi
echo

# --- 2. Credentials -----------------------------------------------------
# Values are held only in these two shell variables for the rest of this
# script -- never echoed, never written anywhere else, never passed as a
# curl command-line argument (see the request below).
echo "Reading Grohe credentials..."

if [[ "$FROM_CLOUD" -eq 1 ]]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 not found on PATH -- required for --from-cloud." >&2
    exit 1
  fi
  if ! python3 -c "import grohe" >/dev/null 2>&1; then
    echo "error: the 'grohe' package is not importable for $(command -v python3)." >&2
    echo "       Run: python3 -m pip install -r scripts/requirements.txt" >&2
    exit 1
  fi

  # grohe_cloud_refresh.py writes progress/errors to its own stderr (passed
  # through to this terminal untouched, not captured below) and, only on
  # full success, exactly two "KEY=VALUE" lines to its stdout -- captured
  # here via command substitution, never a CLI argument, never a file. See
  # that script's own docstring for the full contract.
  set +e
  CLOUD_OUTPUT="$(python3 "$CLOUD_REFRESH_SCRIPT")"
  CLOUD_EXIT=$?
  set -e

  if [[ $CLOUD_EXIT -ne 0 ]]; then
    echo "error: fetching credentials from the Grohe Cloud failed (see above)." >&2
    exit 1
  fi

  USER_ID="$(printf '%s\n' "$CLOUD_OUTPUT" | sed -n 's/^USER_ID=//p')"
  PRESHARED_KEY="$(printf '%s\n' "$CLOUD_OUTPUT" | sed -n 's/^PRESHARED_KEY_BASE64=//p')"
  unset CLOUD_OUTPUT

  if [[ -z "$USER_ID" || -z "$PRESHARED_KEY" ]]; then
    echo "error: grohe_cloud_refresh.py exited 0 but its output didn't parse --" >&2
    echo "       this is a bug, please report it (nothing sent to the device)." >&2
    exit 1
  fi
else
  if [[ ! -f "$ENV_FILE" ]]; then
    echo "error: $ENV_FILE not found." >&2
    echo "       Set GROHE_BLUE_BLE_ENV to point at your grohe_blue_ble" >&2
    echo "       checkout's .env, or check that repo out at ../grohe_blue_ble" >&2
    echo "       next to this one (see its README for how USER_ID/" >&2
    echo "       PRESHARED_KEY get populated in the first place) -- or use" >&2
    echo "       --from-cloud instead (see docs/ARCHITECTURE.md)." >&2
    exit 1
  fi

  USER_ID="$(sed -n 's/^USER_ID=//p' "$ENV_FILE" | tail -n1)"
  PRESHARED_KEY="$(sed -n 's/^PRESHARED_KEY=//p' "$ENV_FILE" | tail -n1)"

  if [[ -z "$USER_ID" ]]; then
    echo "error: USER_ID is missing or empty in $ENV_FILE." >&2
    exit 1
  fi
  if [[ -z "$PRESHARED_KEY" ]]; then
    echo "error: PRESHARED_KEY is missing or empty in $ENV_FILE." >&2
    exit 1
  fi
fi

# --- 3. Provisioning token ---------------------------------------------------
TOKEN="${GROHE_DIAL_PROVISION_TOKEN:-}"
if [[ -z "$TOKEN" ]]; then
  if [[ ! -f "$SECRET_FILE" ]]; then
    echo "error: $SECRET_FILE not found." >&2
    echo "       Copy provisioning_secret_local.hpp.example to" >&2
    echo "       provisioning_secret_local.hpp and fill in a secret (the" >&2
    echo "       same file the firmware itself reads), or set" >&2
    echo "       GROHE_DIAL_PROVISION_TOKEN." >&2
    exit 1
  fi
  TOKEN="$(sed -n 's/.*kLocalProvisioningSecret\[\] *= *"\(.*\)".*/\1/p' "$SECRET_FILE")"
fi

if [[ -z "$TOKEN" ]]; then
  echo "error: provisioning token is empty ($SECRET_FILE) -- fill it in on" >&2
  echo "       both the device and here; the device refuses to even start" >&2
  echo "       the /provision endpoint with an empty secret (see" >&2
  echo "       docs/ARCHITECTURE.md's Provisioning section)." >&2
  exit 1
fi

# --- 4. Request --------------------------------------------------------------
echo "Provisioning device..."

# Minimal JSON-string escaping (backslash, double quote) -- user_id and the
# Base64 preshared key are not expected to need more than this, but it's
# cheap correctness rather than assuming "no special characters".
json_escape() {
  printf '%s' "$1" | sed 's/[\\"]/\\&/g'
}

BODY="$(printf '{"user_id":"%s","preshared_key_base64":"%s"}' \
  "$(json_escape "$USER_ID")" "$(json_escape "$PRESHARED_KEY")")"

# Body goes over stdin (--data @-), not a curl argument, so it never shows
# up in `ps` output for the process's lifetime. The provisioning token is
# still passed as a -H argument, exactly like scripts/ota.sh's own X-OTA-Token
# handling -- this project's existing, accepted posture for a local-secret
# header (see docs/ARCHITECTURE.md's Provisioning section and SECURITY.md:
# plain HTTP already puts both on the wire in the clear on this network).
set +e
RAW="$(printf '%s' "$BODY" | curl -sS \
  -X POST \
  -H "X-Provision-Token: $TOKEN" \
  -H "Content-Type: application/json" \
  --data @- \
  --connect-timeout 5 \
  --max-time 15 \
  -w $'\n%{http_code}' \
  "http://$HOST:$PROVISION_PORT/provision")"
CURL_EXIT=$?
set -e

if [[ $CURL_EXIT -ne 0 ]]; then
  echo "error: could not reach $HOST:$PROVISION_PORT (curl exit $CURL_EXIT)" >&2
  exit 1
fi

HTTP_CODE="${RAW##*$'\n'}"
RESPONSE_BODY="${RAW%$'\n'*}"

if [[ "$HTTP_CODE" != "200" ]]; then
  echo "error: device rejected the request (HTTP $HTTP_CODE)" >&2
  echo "$RESPONSE_BODY" >&2
  exit 1
fi

echo
echo "Provisioning successful."
echo "Credentials are active immediately."
echo
echo "$RESPONSE_BODY"
