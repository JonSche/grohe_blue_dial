#!/usr/bin/env python3
"""Fetches fresh Grohe BLE credentials (USER_ID / PRESHARED_KEY) from the
Grohe Cloud for scripts/provision.sh --from-cloud, using an already-bootstrapped
refresh_token (see scripts/grohe_cloud_bootstrap.py -- that's the one-time,
interactive, password-touching step; this script never sees a password).

Reuses github.com/koproductions-code/grohe (MIT, `pip install grohe`, see
scripts/requirements.txt) for every actual Grohe Cloud auth call -- this
script does not reimplement OIDC/login/refresh, it calls the library's own
`GroheTokens.get_refresh_tokens()` directly. Two things are still done here
rather than via the library, both deliberate, both explained below:

* `GroheClient` (the library's main, documented entry point) mandates an
  email+password at construction time and only ever populates its token
  state via `login()` or its own private `__refresh_tokens()` -- there is no
  public way to hand it an existing refresh_token instead. `GroheTokens`
  (also public, `grohe/tokens.py`) has no such restriction: its
  `get_refresh_tokens(refresh_token)` is exactly the one call this script
  needs, usable standalone. Confirmed by reading grohe==0.3.1's source
  (github.com/koproductions-code/grohe) directly, not assumed.
* `GroheClient.get_dashboard()` is a single authenticated GET this module
  replicates directly (see _fetch_dashboard below) rather than constructing
  a full GroheClient just to reach it -- the alternative would be reaching
  into GroheClient's own name-mangled private attributes from outside,
  which is worse practice, not better reuse. This is a plain, already-
  documented REST call (not auth/token logic), using the exact access token
  GroheTokens already handed us.
* The `sub` claim (-> user_id) is decoded with the same one-liner
  `GroheClient.__set_tokens()` itself uses internally (`jwt.decode(...,
  options={'verify_signature': False})['sub']`) -- PyJWT is already a
  runtime need of this library (see requirements.txt's own note on its
  incomplete upstream dependency metadata), so this isn't a new dependency,
  just the same call the library makes, since there's no public accessor
  for it outside of a full logged-in GroheClient.

Dashboard JSON shape (`locations[].rooms[].appliances[]`, `presharedkey` on
the individual appliance) confirmed two independent ways: (1) reading
grohe/dto/grohe_device.py's own `GroheDevice.get_devices()`, which walks
that exact structure; (2) reading the decompiled official Android app's
own DTOs (GroheWatersystems/sources/com/grohe/smarthome/core/network/
dashboard/model/*.java) -- both agree, so this isn't guessed.

Never prints USER_ID, PRESHARED_KEY, the refresh/access token, or the
Grohe account password (this script never even receives one) -- not to
stdout, not to stderr, not in an exception message or traceback. On
success, the two values this script exists to produce go to stdout as a
small `KEY=VALUE` block, meant to be captured straight into shell variables
by scripts/provision.sh (command substitution, never a CLI argument, never
a file) -- everything else (progress, errors) goes to stderr. See
SECURITY.md.
"""

from __future__ import annotations

import asyncio
import os
import stat
import sys
from pathlib import Path
from typing import Any

import httpx
import jwt

from grohe.exceptions import GroheError
from grohe.tokens import GroheTokens

BASE_URL = "https://idp2-apigw.cloud.grohe.com"
API_URL = f"{BASE_URL}/v3/iot"
DASHBOARD_URL = f"{API_URL}/dashboard"

# Matches scripts/provision.sh's own curl --connect-timeout/--max-time.
REQUEST_TIMEOUT_SECONDS = 15.0

DEFAULT_TOKEN_FILE = Path(__file__).resolve().parent / ".grohe_cloud_refresh_token"


class ProvisioningCredentialsError(Exception):
    """Raised for any failure fetching credentials -- caught once in
    main() and printed without ever including a token/credential value.
    Distinct from grohe's own GroheError (also caught, also safe to print
    per that library's own exceptions.py -- see its docstrings) so callers
    can tell "the library rejected the request" from "something around it,
    e.g. a missing/empty local file, went wrong" if they ever care to."""


def _resolve_refresh_token() -> tuple[str, Path | None]:
    """Returns (refresh_token, path_to_write_a_rotated_token_back_to).

    The second element is None when the token came from
    GROHE_CLOUD_REFRESH_TOKEN (an env var has nowhere to persist a rotated
    token back to -- the caller's problem, same tradeoff
    GROHE_DIAL_PROVISION_TOKEN already accepts in provision.sh).
    """
    env_token = os.environ.get("GROHE_CLOUD_REFRESH_TOKEN", "").strip()
    if env_token:
        return env_token, None

    token_file = Path(os.environ.get("GROHE_CLOUD_REFRESH_TOKEN_FILE", str(DEFAULT_TOKEN_FILE)))
    if not token_file.is_file():
        raise ProvisioningCredentialsError(
            f"refresh token file not found: {token_file}\n"
            "       Run scripts/grohe_cloud_bootstrap.py once to create it (the\n"
            "       one-time, interactive, password-touching step -- see\n"
            "       docs/ARCHITECTURE.md's Provisioning section), or set\n"
            "       GROHE_CLOUD_REFRESH_TOKEN / GROHE_CLOUD_REFRESH_TOKEN_FILE."
        )
    token = token_file.read_text(encoding="utf-8").strip()
    if not token:
        raise ProvisioningCredentialsError(f"refresh token file is empty: {token_file}")
    return token, token_file


def _write_token_file(path: Path, refresh_token: str) -> None:
    path.write_text(refresh_token + "\n", encoding="utf-8")
    try:
        path.chmod(stat.S_IRUSR | stat.S_IWUSR)  # 0600 -- owner read/write only
    except OSError:
        pass  # best-effort (e.g. unsupported on the local filesystem); not fatal


async def _fetch_dashboard(client: httpx.AsyncClient, access_token: str) -> dict[str, Any]:
    """Mirrors GroheClient.get_dashboard()'s single GET exactly -- see this
    module's own docstring for why it's not called through GroheClient
    itself."""
    try:
        response = await client.get(
            DASHBOARD_URL,
            headers={"Authorization": f"Bearer {access_token}"},
            timeout=REQUEST_TIMEOUT_SECONDS,
        )
        response.raise_for_status()
    except httpx.HTTPStatusError as e:
        raise ProvisioningCredentialsError(
            f"fetching the Grohe Cloud dashboard failed: HTTP {e.response.status_code}"
        ) from None
    except httpx.HTTPError as e:
        raise ProvisioningCredentialsError(
            f"could not reach the Grohe Cloud dashboard endpoint ({type(e).__name__})"
        ) from None
    return response.json()


def _find_presharedkey(dashboard: dict[str, Any]) -> str:
    """Walks locations[].rooms[].appliances[] looking for the one appliance
    carrying a "presharedkey" field -- see this module's own docstring for
    why the real shape is this nested tree, not the flat
    {"appliance": {"presharedkey": ...}} grohe_blue_ble/docs/EVIDENCE.md
    shows as a simplified example. Not filtered by numeric appliance
    `type` (confirmed identical field name across every appliance variant
    in the decompiled official app's own DTOs), so this doesn't need a
    type list that could silently go stale."""
    found: list[str] = []
    for location in dashboard.get("locations", []):
        for room in location.get("rooms", []):
            for appliance in room.get("appliances", []):
                key = appliance.get("presharedkey")
                if key:
                    found.append(key)

    if not found:
        raise ProvisioningCredentialsError(
            "no appliance with a preshared key found in this Grohe account's "
            "dashboard -- is the Blue Home appliance set up in the Ondus app?"
        )
    if len(found) > 1:
        raise ProvisioningCredentialsError(
            f"found {len(found)} appliances with a preshared key in this Grohe "
            "account -- this script (and the dial's own /provision endpoint) "
            "only supports a single appliance; not guessing which one to use."
        )
    return found[0]


async def get_provisioning_credentials(refresh_token: str) -> tuple[str, str, str | None]:
    """Returns (user_id, preshared_key_base64, rotated_refresh_token).
    rotated_refresh_token is None unless the cloud handed back a new one
    alongside the access token (a normal, expected OAuth refresh-token
    rotation, not an error)."""
    async with httpx.AsyncClient() as client:
        token_handler = GroheTokens(client, API_URL)
        try:
            tokens = await token_handler.get_refresh_tokens(refresh_token)
        except GroheError as e:
            # grohe's own exceptions are already designed not to embed the
            # token itself (see grohe/exceptions.py's own docstrings) --
            # str(e) is safe to surface as-is.
            raise ProvisioningCredentialsError(str(e)) from None

        rotated = tokens.refresh_token if tokens.refresh_token != refresh_token else None

        try:
            claims = jwt.decode(tokens.access_token, options={"verify_signature": False})
        except Exception as e:
            raise ProvisioningCredentialsError(
                f"could not decode the access token's JWT payload ({type(e).__name__})"
            ) from None
        user_id = claims.get("sub")
        if not user_id:
            raise ProvisioningCredentialsError("access token JWT payload has no 'sub' claim")

        dashboard = await _fetch_dashboard(client, tokens.access_token)
        preshared_key = _find_presharedkey(dashboard)
        return user_id, preshared_key, rotated


def main() -> int:
    try:
        refresh_token, token_file = _resolve_refresh_token()
        print("Fetching Grohe Cloud credentials...", file=sys.stderr)
        user_id, preshared_key, rotated = asyncio.run(get_provisioning_credentials(refresh_token))
        if rotated is not None and token_file is not None:
            _write_token_file(token_file, rotated)
            print("Refresh token rotated -- updated local copy.", file=sys.stderr)
    except (ProvisioningCredentialsError, GroheError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except Exception as e:  # noqa: BLE001 -- defense in depth, see module docstring
        # Deliberately prints only the exception *type*, never str(e): an
        # unexpected failure here must never risk echoing a token/secret
        # that happened to be embedded in some library's own error message.
        print(f"error: unexpected failure fetching Grohe Cloud credentials ({type(e).__name__})", file=sys.stderr)
        return 1

    sys.stdout.write(f"USER_ID={user_id}\nPRESHARED_KEY_BASE64={preshared_key}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
