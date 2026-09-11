"""M16.9: cloud.py -- the thin wrapper around the `grohe` PyPI package.
Mocks grohe.tokens.GroheTokens's own methods (the actual login/refresh/
network calls) -- never makes a real Grohe Cloud request, never uses
real credentials, per this milestone's own explicit security
requirement. Runs against the real, installed `grohe` package (not a
stand-in) for its dataclasses/exceptions, matching this project's own
established "genuinely run, not just written" testing discipline.
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import httpx
import pytest
from grohe.dto.grohe_dto import GroheTokensDTO
from grohe.exceptions import GroheNetworkError, GroheUnauthorizedError

from custom_components.grohe_dial.cloud import (
    GroheCloudAuthError,
    GroheCloudConnectionError,
    GroheCloudError,
    list_appliances,
    login_with_credentials,
    refresh_tokens,
    user_id_from_access_token,
)

# No module-level `pytestmark = pytest.mark.asyncio` -- this file mixes
# async and plain sync tests (user_id_from_access_token() is
# synchronous), and pytest.ini's own asyncio_mode = auto already picks
# up every `async def test_*` without it. A blanket mark here would
# incorrectly tag the sync tests too.

_FAKE_TOKENS = GroheTokensDTO(
    access_token="fake.access.token",
    expires_in=3600,
    refresh_expires_in=86400,
    refresh_token="fake-refresh-token",
    token_type="Bearer",
    id_token="fake-id-token",
    session_state="fake-session",
    scope="openid",
    not_before_policy=0,
)


async def test_login_with_valid_credentials_returns_tokens() -> None:
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_tokens_from_credentials",
        new=AsyncMock(return_value=_FAKE_TOKENS),
    ):
        tokens = await login_with_credentials("user@example.com", "not-a-real-password")
    assert tokens.access_token == "fake.access.token"
    assert tokens.refresh_token == "fake-refresh-token"


async def test_login_with_invalid_credentials_raises_auth_error() -> None:
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_tokens_from_credentials",
        new=AsyncMock(side_effect=GroheUnauthorizedError("Invalid Grohe username or password")),
    ):
        with pytest.raises(GroheCloudAuthError):
            await login_with_credentials("user@example.com", "wrong")


async def test_login_when_cloud_unavailable_raises_connection_error() -> None:
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_tokens_from_credentials",
        new=AsyncMock(side_effect=GroheNetworkError("Could not load Grohe login page: timeout")),
    ):
        with pytest.raises(GroheCloudConnectionError):
            await login_with_credentials("user@example.com", "irrelevant")


# grohe < 0.3.0 (a real, currently-deployed dependency conflict found
# deploying this integration to a real Home Assistant instance sharing
# its Python environment with another, unrelated integration pinned to
# grohe==0.2.4 -- see cloud.py's own header/import comment) has no
# grohe.exceptions module at all; GroheTokens.get_tokens_from_credentials()/
# get_refresh_tokens() raise httpx's own exceptions, or a bare Exception,
# directly instead. These three tests exercise _wrap()'s own fallback
# classification for exactly that shape -- deliberately using real
# httpx/Exception instances, not grohe.exceptions ones, regardless of
# which grohe version this suite's own pinned test dependency actually
# has (the fallback branches only run if the *structured* isinstance
# checks above them didn't already match, which a plain httpx/Exception
# instance never does either way).
async def test_login_with_httpx_401_raises_auth_error() -> None:
    request = httpx.Request("POST", "https://idp2-apigw.cloud.grohe.com/v3/iot/oidc/login")
    response = httpx.Response(401, request=request)
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_tokens_from_credentials",
        new=AsyncMock(
            side_effect=httpx.HTTPStatusError("401", request=request, response=response)
        ),
    ):
        with pytest.raises(GroheCloudAuthError):
            await login_with_credentials("user@example.com", "wrong")


async def test_login_with_httpx_connection_error_raises_connection_error() -> None:
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_tokens_from_credentials",
        new=AsyncMock(side_effect=httpx.ConnectError("connection refused")),
    ):
        with pytest.raises(GroheCloudConnectionError):
            await login_with_credentials("user@example.com", "irrelevant")


async def test_login_with_unstructured_exception_raises_generic_cloud_error() -> None:
    """grohe==0.2.4's own real, installed source raises exactly this
    shape (a bare Exception, not an httpx one) for invalid credentials
    specifically -- not distinguishable from a handful of other
    malformed-response cases at that version, so this is deliberately
    generic (GroheCloudError / "unknown"), not misclassified as
    GroheCloudAuthError by guessing at the message text."""
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_tokens_from_credentials",
        new=AsyncMock(
            side_effect=Exception("Invalid username/password or unexpected response from Grohe service")
        ),
    ):
        with pytest.raises(GroheCloudError) as exc_info:
            await login_with_credentials("user@example.com", "wrong")
        assert not isinstance(exc_info.value, (GroheCloudAuthError, GroheCloudConnectionError))


async def test_refresh_tokens_success() -> None:
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_refresh_tokens",
        new=AsyncMock(return_value=_FAKE_TOKENS),
    ):
        tokens = await refresh_tokens("some-refresh-token")
    assert tokens.access_token == "fake.access.token"


async def test_refresh_tokens_invalid_raises_auth_error() -> None:
    with patch(
        "custom_components.grohe_dial.cloud.GroheTokens.get_refresh_tokens",
        new=AsyncMock(side_effect=GroheUnauthorizedError("Grohe refresh token is no longer valid")),
    ):
        with pytest.raises(GroheCloudAuthError):
            await refresh_tokens("expired-token")


# A real, structurally valid JWT whose payload is {"sub": "abc-123-user-id"}
# and an empty signature -- jwt.decode(..., verify_signature=False) only
# parses the payload, it does not need a valid signature for this call.
_FAKE_JWT_WITH_SUB = (
    "eyJhbGciOiJIUzI1NiJ9."
    "eyJzdWIiOiJhYmMtMTIzLXVzZXItaWQifQ."
    "signature-is-never-checked"
)


def test_user_id_from_access_token_extracts_sub_claim() -> None:
    assert user_id_from_access_token(_FAKE_JWT_WITH_SUB) == "abc-123-user-id"


def test_user_id_from_access_token_without_sub_raises() -> None:
    # {"foo": "bar"} -- valid JWT shape, no "sub" claim.
    token = "eyJhbGciOiJIUzI1NiJ9.eyJmb28iOiJiYXIifQ.sig"
    with pytest.raises(Exception):  # GroheCloudError
        user_id_from_access_token(token)


class _FakeResponse:
    def __init__(self, status_code: int, json_data: dict) -> None:
        self.status_code = status_code
        self._json_data = json_data

    def raise_for_status(self) -> None:
        if self.status_code >= 400:
            import httpx

            raise httpx.HTTPStatusError("error", request=None, response=self)  # type: ignore[arg-type]

    def json(self) -> dict:
        return self._json_data


async def test_list_appliances_finds_single_candidate() -> None:
    dashboard = {
        "locations": [
            {
                "rooms": [
                    {
                        "appliances": [
                            {"appliance_id": "appl-1", "name": "Kitchen Blue Home", "presharedkey": "cHJlc2hhcmVkS2V5"}
                        ]
                    }
                ]
            }
        ]
    }
    with patch(
        "custom_components.grohe_dial.cloud.httpx.AsyncClient.get",
        new=AsyncMock(return_value=_FakeResponse(200, dashboard)),
    ):
        candidates = await list_appliances("fake-access-token")
    assert len(candidates) == 1
    assert candidates[0].appliance_id == "appl-1"
    assert candidates[0].name == "Kitchen Blue Home"
    assert candidates[0].preshared_key_base64 == "cHJlc2hhcmVkS2V5"


async def test_list_appliances_finds_multiple_candidates() -> None:
    dashboard = {
        "locations": [
            {
                "rooms": [
                    {
                        "appliances": [
                            {"appliance_id": "appl-1", "name": "Kitchen", "presharedkey": "key1"},
                            {"appliance_id": "appl-2", "name": "Office", "presharedkey": "key2"},
                        ]
                    }
                ]
            }
        ]
    }
    with patch(
        "custom_components.grohe_dial.cloud.httpx.AsyncClient.get",
        new=AsyncMock(return_value=_FakeResponse(200, dashboard)),
    ):
        candidates = await list_appliances("fake-access-token")
    assert len(candidates) == 2
    assert {c.appliance_id for c in candidates} == {"appl-1", "appl-2"}


async def test_list_appliances_ignores_appliances_without_presharedkey() -> None:
    # e.g. a Grohe Sense leak sensor -- no BLE preshared key, not a
    # candidate for this dial regardless of account contents.
    dashboard = {
        "locations": [{"rooms": [{"appliances": [{"appliance_id": "sense-1", "name": "Sense"}]}]}]
    }
    with patch(
        "custom_components.grohe_dial.cloud.httpx.AsyncClient.get",
        new=AsyncMock(return_value=_FakeResponse(200, dashboard)),
    ):
        candidates = await list_appliances("fake-access-token")
    assert candidates == []


async def test_list_appliances_empty_account_returns_empty_list() -> None:
    with patch(
        "custom_components.grohe_dial.cloud.httpx.AsyncClient.get",
        new=AsyncMock(return_value=_FakeResponse(200, {"locations": []})),
    ):
        candidates = await list_appliances("fake-access-token")
    assert candidates == []
