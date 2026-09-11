"""M16: thin async wrapper around the `grohe` PyPI package
(github.com/koproductions-code/grohe, MIT) -- the exact same package
scripts/grohe_cloud_bootstrap.py/grohe_cloud_refresh.py already use for
every real Grohe Cloud call (login, refresh, dashboard). No login/
refresh/dashboard logic is reimplemented here -- this module only
adapts that package's own GroheTokens/GroheError surface into a shape
the provisioning options flow (config_flow.py) can call and handle
HA-side, mirroring api.py's own role for the *local* dial API (that
module has no HA imports either, for the same reason -- independently
testable, and the one place either exception hierarchy gets translated
into something HA-shaped is the caller, not here).

API_URL/DASHBOARD_URL and the dashboard JSON shape
(locations[].rooms[].appliances[].presharedkey) are copied verbatim
from scripts/grohe_cloud_refresh.py, which documents in detail how they
were verified (grohe's own source plus the decompiled official Android
app's DTOs independently agreeing) -- see that module's own docstring
for the full story. user_id extraction (JWT `sub` claim from the access
token) mirrors that same module's own one-liner exactly, for the same
reason: no public accessor for it exists outside a full logged-in
GroheClient (which mandates a password at construction and has no way
to accept a bare refresh_token instead -- see that docstring).
"""

from __future__ import annotations

from dataclasses import dataclass

import httpx
import jwt
from grohe.tokens import GroheTokens

# grohe's own structured exception hierarchy (grohe.exceptions) was
# introduced in 0.3.0 -- older installs some real-world setups are
# genuinely still pinned to (e.g. a separately-installed, unrelated
# integration sharing this same Python environment that pins an older
# exact version -- found deploying to a real Home Assistant instance
# with exactly that conflict, not a hypothetical) predate it entirely
# and raise plain httpx exceptions / bare `Exception` from
# GroheTokens.get_tokens_from_credentials()/get_refresh_tokens() instead
# -- see _wrap()'s own comment for how both shapes are handled without
# ever assuming one specific `grohe` version is installed, and without
# forcing an upgrade that could break whatever else in the same
# environment pins an older one.
try:
    from grohe.exceptions import GroheError, GroheNetworkError, GroheUnauthorizedError
except ImportError:  # grohe < 0.3.0
    # Empty tuples, not None: `except GroheError:`/`isinstance(err,
    # GroheUnauthorizedError)` both stay syntactically valid this way
    # (`except ():`/`isinstance(x, ())` are legal Python, and simply
    # never match) -- see _wrap()'s own comment for the httpx-based
    # classification that covers this case instead.
    GroheError = GroheNetworkError = GroheUnauthorizedError = ()  # type: ignore[assignment,misc]

API_URL = "https://idp2-apigw.cloud.grohe.com/v3/iot"
DASHBOARD_URL = f"{API_URL}/dashboard"

# Matches scripts/grohe_cloud_refresh.py's own REQUEST_TIMEOUT_SECONDS.
_REQUEST_TIMEOUT_SECONDS = 15.0

__all__ = [
    "GroheCloudError",
    "GroheCloudAuthError",
    "GroheCloudConnectionError",
    "GroheCloudTokens",
    "GroheApplianceCandidate",
    "login_with_credentials",
    "refresh_tokens",
    "user_id_from_access_token",
    "list_appliances",
]


class GroheCloudError(Exception):
    """Base class for every error this module raises."""


class GroheCloudAuthError(GroheCloudError):
    """Invalid email/password, or an expired/revoked refresh token."""


class GroheCloudConnectionError(GroheCloudError):
    """The Grohe Cloud could not be reached at all, or returned something
    other than an auth failure this module doesn't know how to handle."""


@dataclass(frozen=True, slots=True)
class GroheCloudTokens:
    """Mirrors grohe.dto.grohe_dto.GroheTokensDTO's two fields this
    integration actually needs -- access_token (short-lived, used once
    per operation) and refresh_token (the only one ever persisted, in
    HA's own config entry storage, never written to the dial -- see
    config_flow.py's own comment on why)."""

    access_token: str
    refresh_token: str


@dataclass(frozen=True, slots=True)
class GroheApplianceCandidate:
    """One appliance from the Cloud dashboard that carries a preshared
    key -- i.e. a Grohe Blue Home/Professional this dial could be
    provisioned for. Mirrors grohe_cloud_refresh.py's own
    _find_presharedkey() filter exactly, except every match is kept (for
    a picker when there's more than one) instead of erroring out past
    the first."""

    name: str
    appliance_id: str
    preshared_key_base64: str


def _wrap(err: Exception) -> GroheCloudError:
    if GroheUnauthorizedError and isinstance(err, GroheUnauthorizedError):
        return GroheCloudAuthError(str(err))
    if GroheNetworkError and isinstance(err, GroheNetworkError):
        return GroheCloudConnectionError(str(err))
    if GroheError and isinstance(err, GroheError):
        return GroheCloudError(str(err))
    # grohe < 0.3.0 (this module's own header comment) has no structured
    # exception hierarchy at all -- GroheTokens.get_tokens_from_credentials()/
    # get_refresh_tokens() raise httpx's own exceptions directly instead
    # (verified by reading that version's real, installed source, not
    # assumed), classified here the same way list_appliances() below
    # already classifies its own direct httpx calls. A bare Exception
    # that isn't an httpx one either (e.g. 0.2.4's own generic "Invalid
    # username/password..." raise) falls through to GroheCloudError
    # (surfaced as "unknown", not "invalid_cloud_auth") -- an accepted,
    # honest precision loss against an unsupported-by-upstream version,
    # not a silently wrong classification.
    if isinstance(err, httpx.HTTPStatusError):
        if err.response.status_code in (401, 403):
            return GroheCloudAuthError(str(err))
        return GroheCloudConnectionError(str(err))
    if isinstance(err, httpx.HTTPError):
        return GroheCloudConnectionError(str(err))
    return GroheCloudError(str(err))


async def login_with_credentials(email: str, password: str) -> GroheCloudTokens:
    """One-time, password-based login -- mirrors
    scripts/grohe_cloud_bootstrap.py's own _login() exactly. `password`
    is only ever held by this function's own local scope and the
    library call it makes; never logged, never returned, never stored."""
    async with httpx.AsyncClient() as client:
        token_handler = GroheTokens(client, API_URL)
        try:
            tokens = await token_handler.get_tokens_from_credentials(email, password)
        except Exception as err:  # noqa: BLE001 - _wrap() classifies every real shape (see its own comment); anything truly unexpected still becomes a typed GroheCloudError, never escapes as-is.
            raise _wrap(err) from err
    return GroheCloudTokens(access_token=tokens.access_token, refresh_token=tokens.refresh_token)


async def refresh_tokens(refresh_token: str) -> GroheCloudTokens:
    """No password needed -- mirrors scripts/grohe_cloud_refresh.py's own
    get_provisioning_credentials() token-refresh step exactly."""
    async with httpx.AsyncClient() as client:
        token_handler = GroheTokens(client, API_URL)
        try:
            tokens = await token_handler.get_refresh_tokens(refresh_token)
        except Exception as err:  # noqa: BLE001 - see login_with_credentials()'s own comment.
            raise _wrap(err) from err
    return GroheCloudTokens(access_token=tokens.access_token, refresh_token=tokens.refresh_token)


def user_id_from_access_token(access_token: str) -> str:
    """Mirrors scripts/grohe_cloud_refresh.py's own jwt.decode() one-liner
    exactly -- see that module's own docstring for why this, not a
    public library accessor."""
    try:
        claims = jwt.decode(access_token, options={"verify_signature": False})
    except Exception as err:
        raise GroheCloudError(f"could not decode the access token's JWT payload: {err}") from err
    user_id = claims.get("sub")
    if not user_id:
        raise GroheCloudError("access token JWT payload has no 'sub' claim")
    return user_id


async def list_appliances(access_token: str) -> list[GroheApplianceCandidate]:
    """Mirrors scripts/grohe_cloud_refresh.py's own _fetch_dashboard() +
    _find_presharedkey() exactly, except every appliance with a
    preshared key is returned (for a picker), not just the first with an
    error if there's more than one -- see GroheApplianceCandidate's own
    comment for why that's a deliberate, minimal improvement over the
    CLI script's own hard-fail, not scope creep: HA has a UI to pick
    from, a shell script doesn't."""
    async with httpx.AsyncClient() as client:
        try:
            response = await client.get(
                DASHBOARD_URL,
                headers={"Authorization": f"Bearer {access_token}"},
                timeout=_REQUEST_TIMEOUT_SECONDS,
            )
            response.raise_for_status()
        except httpx.HTTPStatusError as err:
            if err.response.status_code in (401, 403):
                raise GroheCloudAuthError("Grohe Cloud rejected the access token") from err
            raise GroheCloudConnectionError(
                f"Grohe Cloud dashboard returned HTTP {err.response.status_code}"
            ) from err
        except httpx.HTTPError as err:
            raise GroheCloudConnectionError(f"could not reach the Grohe Cloud dashboard: {err}") from err
        dashboard = response.json()

    candidates: list[GroheApplianceCandidate] = []
    for location in dashboard.get("locations", []):
        for room in location.get("rooms", []):
            for appliance in room.get("appliances", []):
                key = appliance.get("presharedkey")
                if key:
                    candidates.append(
                        GroheApplianceCandidate(
                            name=appliance.get("name") or appliance.get("appliance_id", "Grohe Blue"),
                            appliance_id=appliance.get("appliance_id", ""),
                            preshared_key_base64=key,
                        )
                    )
    return candidates
