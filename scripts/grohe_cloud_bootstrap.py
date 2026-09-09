#!/usr/bin/env python3
"""One-time, interactive bootstrap for scripts/provision.sh --from-cloud:
logs in to the Grohe Cloud with your account email+password (typed once,
here, interactively) and saves only the resulting refresh_token to a local,
gitignored file. The password itself is never written anywhere -- not to
disk, not to a log, not to the refresh token file -- only held in memory
for the single login call below.

    python3 scripts/grohe_cloud_bootstrap.py

After this, scripts/provision.sh --from-cloud / scripts/grohe_cloud_refresh.py
need only that refresh_token (it refreshes itself indefinitely -- see that
script's own docstring) -- this bootstrap step does not need to be repeated
unless the refresh_token itself is later revoked/invalidated Grohe-side.

Reuses github.com/koproductions-code/grohe's own login implementation
(`GroheTokens.get_tokens_from_credentials()`, MIT) verbatim -- this script
does not implement its own username/password login. See
grohe_cloud_refresh.py's own docstring for why `GroheTokens` is used
directly rather than through `GroheClient`.
"""

from __future__ import annotations

import asyncio
import getpass
import os
import sys
from pathlib import Path

import httpx

from grohe.exceptions import GroheError
from grohe.tokens import GroheTokens

from grohe_cloud_refresh import API_URL, DEFAULT_TOKEN_FILE, _write_token_file


async def _login(email: str, password: str) -> str:
    async with httpx.AsyncClient() as client:
        token_handler = GroheTokens(client, API_URL)
        tokens = await token_handler.get_tokens_from_credentials(email, password)
    return tokens.refresh_token


def main() -> int:
    token_file = Path(os.environ.get("GROHE_CLOUD_REFRESH_TOKEN_FILE", str(DEFAULT_TOKEN_FILE)))

    print("=== Grohe Cloud Bootstrap ===")
    print(f"Refresh token will be saved to: {token_file}")
    print("Your password is used once, in memory only, for this login -- it is")
    print("never saved, logged, or written anywhere by this script.")
    print()

    email = input("Grohe account email: ").strip()
    password = getpass.getpass("Grohe account password: ")  # not echoed to the terminal

    if not email or not password:
        print("error: both email and password are required", file=sys.stderr)
        return 1

    print("Logging in...", file=sys.stderr)
    try:
        refresh_token = asyncio.run(_login(email, password))
    except GroheError as e:
        # grohe's own exceptions don't embed the password (see
        # grohe/tokens.py: GroheUnauthorizedError('Invalid Grohe username or
        # password'), no credential interpolation) -- safe to print as-is.
        print(f"error: login failed: {e}", file=sys.stderr)
        return 1
    except Exception as e:  # noqa: BLE001 -- defense in depth, see module docstring
        print(f"error: unexpected login failure ({type(e).__name__})", file=sys.stderr)
        return 1
    finally:
        # Best-effort only: Python strings are immutable, so this cannot
        # guarantee the password is scrubbed from process memory -- it just
        # drops this function's own reference to it as soon as possible.
        password = None  # noqa: F841

    if not refresh_token:
        print("error: login succeeded but the response had no refresh_token", file=sys.stderr)
        return 1

    _write_token_file(token_file, refresh_token)
    print()
    print(f"Saved. Refresh token stored at {token_file} (never printed).")
    print("You can now run: ./scripts/provision.sh <device-ip> --from-cloud")
    return 0


if __name__ == "__main__":
    sys.exit(main())
