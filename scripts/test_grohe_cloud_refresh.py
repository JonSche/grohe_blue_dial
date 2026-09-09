#!/usr/bin/env python3
"""Tests for grohe_cloud_refresh.py -- mocks grohe.tokens.GroheTokens (the
library we deliberately reuse instead of reimplementing), no real network
access, no real credentials anywhere in this file. Every token/id/key used
below is an obviously-fake fixture string chosen to be unmistakable if it
ever leaked into output by accident.

Requires the packages in scripts/requirements.txt to be installed (this
module imports the real `grohe` package, since it's a thin wrapper around
it -- there is deliberately nothing left to test with the dependency
mocked out too).

Run with:
    python3 scripts/test_grohe_cloud_refresh.py
    python3 -m unittest scripts.test_grohe_cloud_refresh   (from repo root)
"""

from __future__ import annotations

import io
import sys
import unittest
from dataclasses import dataclass
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import jwt

import grohe_cloud_refresh as gcr  # noqa: E402
from grohe.exceptions import GroheNetworkError, GroheUnauthorizedError  # noqa: E402

FAKE_REFRESH_TOKEN = "fake-refresh-token-do-not-use"  # noqa: S105 -- test fixture, not a secret
FAKE_USER_ID = "fake-sub-claim-user-id"
FAKE_PRESHARED_KEY = "ZmFrZS1wcmVzaGFyZWQta2V5"  # base64("fake-preshared-key")


def _fake_jwt(claims: dict) -> str:
    """A syntactically valid (unsigned) JWT -- PyJWT is happy to both
    produce and, with verify_signature=False, consume one without a real
    signing key, matching _decode's own "never verifies" behaviour."""
    return jwt.encode(claims, key="unused-test-key", algorithm="HS256")


@dataclass
class _FakeTokensDTO:
    """Stand-in for grohe.dto.grohe_dto.GroheTokensDTO -- only the two
    fields grohe_cloud_refresh.py actually reads."""

    access_token: str
    refresh_token: str


DASHBOARD_ONE_APPLIANCE = {
    "locations": [
        {
            "rooms": [
                {
                    "appliances": [
                        {"type": 104, "appliance_id": "appliance-1", "presharedkey": FAKE_PRESHARED_KEY},
                        {"type": 999, "appliance_id": "not-an-iot-device"},  # no presharedkey key at all
                    ]
                }
            ]
        }
    ]
}

DASHBOARD_NO_APPLIANCE = {"locations": [{"rooms": [{"appliances": []}]}]}

DASHBOARD_TWO_APPLIANCES = {
    "locations": [
        {
            "rooms": [
                {
                    "appliances": [
                        {"type": 104, "appliance_id": "a1", "presharedkey": "key-one"},
                        {"type": 104, "appliance_id": "a2", "presharedkey": "key-two"},
                    ]
                }
            ]
        }
    ]
}


class FindPresharedkeyTest(unittest.TestCase):
    def test_single_appliance_found(self):
        self.assertEqual(gcr._find_presharedkey(DASHBOARD_ONE_APPLIANCE), FAKE_PRESHARED_KEY)

    def test_no_appliance_raises(self):
        with self.assertRaises(gcr.ProvisioningCredentialsError):
            gcr._find_presharedkey(DASHBOARD_NO_APPLIANCE)

    def test_empty_locations_raises(self):
        with self.assertRaises(gcr.ProvisioningCredentialsError):
            gcr._find_presharedkey({"locations": []})

    def test_multiple_appliances_raises_not_guessing(self):
        with self.assertRaises(gcr.ProvisioningCredentialsError):
            gcr._find_presharedkey(DASHBOARD_TWO_APPLIANCES)


class GetProvisioningCredentialsTest(unittest.IsolatedAsyncioTestCase):
    """Patches GroheTokens.get_refresh_tokens (the one library call this
    module delegates to) and httpx.AsyncClient.get (the one plain REST call
    this module does itself, see grohe_cloud_refresh.py's own docstring)."""

    async def test_full_flow_no_rotation(self):
        tokens = _FakeTokensDTO(access_token=_fake_jwt({"sub": FAKE_USER_ID}), refresh_token=FAKE_REFRESH_TOKEN)
        with (
            mock.patch("grohe.tokens.GroheTokens.get_refresh_tokens", return_value=tokens),
            mock.patch("httpx.AsyncClient.get") as fake_get,
        ):
            fake_get.return_value = httpx_response(DASHBOARD_ONE_APPLIANCE)
            user_id, preshared_key, rotated = await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)
        self.assertEqual(user_id, FAKE_USER_ID)
        self.assertEqual(preshared_key, FAKE_PRESHARED_KEY)
        self.assertIsNone(rotated)

    async def test_full_flow_with_rotation(self):
        new_refresh_token = "fake-rotated-refresh-token"  # noqa: S105
        tokens = _FakeTokensDTO(access_token=_fake_jwt({"sub": FAKE_USER_ID}), refresh_token=new_refresh_token)
        with (
            mock.patch("grohe.tokens.GroheTokens.get_refresh_tokens", return_value=tokens),
            mock.patch("httpx.AsyncClient.get", return_value=httpx_response(DASHBOARD_ONE_APPLIANCE)),
        ):
            _, _, rotated = await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)
        self.assertEqual(rotated, new_refresh_token)

    async def test_unchanged_refresh_token_is_not_reported_as_rotated(self):
        tokens = _FakeTokensDTO(access_token=_fake_jwt({"sub": FAKE_USER_ID}), refresh_token=FAKE_REFRESH_TOKEN)
        with (
            mock.patch("grohe.tokens.GroheTokens.get_refresh_tokens", return_value=tokens),
            mock.patch("httpx.AsyncClient.get", return_value=httpx_response(DASHBOARD_ONE_APPLIANCE)),
        ):
            _, _, rotated = await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)
        self.assertIsNone(rotated)

    async def test_invalid_or_expired_refresh_token_raises_without_leaking_it(self):
        with mock.patch(
            "grohe.tokens.GroheTokens.get_refresh_tokens",
            side_effect=GroheUnauthorizedError("Grohe refresh token is no longer valid"),
        ):
            with self.assertRaises(gcr.ProvisioningCredentialsError) as ctx:
                await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)
        self.assertNotIn(FAKE_REFRESH_TOKEN, str(ctx.exception))

    async def test_network_error_during_refresh_raises(self):
        with mock.patch(
            "grohe.tokens.GroheTokens.get_refresh_tokens",
            side_effect=GroheNetworkError("Could not refresh Grohe tokens: connection failed"),
        ):
            with self.assertRaises(gcr.ProvisioningCredentialsError):
                await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)

    async def test_missing_presharedkey_field_raises(self):
        tokens = _FakeTokensDTO(access_token=_fake_jwt({"sub": FAKE_USER_ID}), refresh_token=FAKE_REFRESH_TOKEN)
        with (
            mock.patch("grohe.tokens.GroheTokens.get_refresh_tokens", return_value=tokens),
            mock.patch("httpx.AsyncClient.get", return_value=httpx_response(DASHBOARD_NO_APPLIANCE)),
        ):
            with self.assertRaises(gcr.ProvisioningCredentialsError):
                await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)

    async def test_dashboard_http_error_raises(self):
        import httpx

        tokens = _FakeTokensDTO(access_token=_fake_jwt({"sub": FAKE_USER_ID}), refresh_token=FAKE_REFRESH_TOKEN)

        async def raise_http_error(*_args, **_kwargs):
            request = httpx.Request("GET", gcr.DASHBOARD_URL)
            response = httpx.Response(401, request=request)
            raise httpx.HTTPStatusError("unauthorized", request=request, response=response)

        with (
            mock.patch("grohe.tokens.GroheTokens.get_refresh_tokens", return_value=tokens),
            mock.patch("httpx.AsyncClient.get", side_effect=raise_http_error),
        ):
            with self.assertRaises(gcr.ProvisioningCredentialsError):
                await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)

    async def test_missing_sub_claim_raises(self):
        tokens = _FakeTokensDTO(access_token=_fake_jwt({"not_sub": "x"}), refresh_token=FAKE_REFRESH_TOKEN)
        with mock.patch("grohe.tokens.GroheTokens.get_refresh_tokens", return_value=tokens):
            with self.assertRaises(gcr.ProvisioningCredentialsError):
                await gcr.get_provisioning_credentials(FAKE_REFRESH_TOKEN)


def httpx_response(body: dict):
    import httpx

    request = httpx.Request("GET", gcr.DASHBOARD_URL)
    return httpx.Response(200, json=body, request=request)


class ResolveRefreshTokenTest(unittest.TestCase):
    def test_env_var_takes_priority_no_file_path_returned(self):
        with mock.patch.dict("os.environ", {"GROHE_CLOUD_REFRESH_TOKEN": FAKE_REFRESH_TOKEN}, clear=False):
            token, path = gcr._resolve_refresh_token()
        self.assertEqual(token, FAKE_REFRESH_TOKEN)
        self.assertIsNone(path)

    def test_missing_file_raises(self):
        with mock.patch.dict(
            "os.environ",
            {"GROHE_CLOUD_REFRESH_TOKEN_FILE": "/nonexistent/path/token", "GROHE_CLOUD_REFRESH_TOKEN": ""},
            clear=False,
        ):
            with self.assertRaises(gcr.ProvisioningCredentialsError):
                gcr._resolve_refresh_token()

    def test_empty_file_raises(self):
        import tempfile

        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write("   \n")
            path = f.name
        try:
            with mock.patch.dict(
                "os.environ", {"GROHE_CLOUD_REFRESH_TOKEN_FILE": path, "GROHE_CLOUD_REFRESH_TOKEN": ""}, clear=False
            ):
                with self.assertRaises(gcr.ProvisioningCredentialsError):
                    gcr._resolve_refresh_token()
        finally:
            Path(path).unlink()

    def test_file_with_token_is_read_and_stripped(self):
        import tempfile

        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write(f"  {FAKE_REFRESH_TOKEN}  \n")
            path = f.name
        try:
            with mock.patch.dict(
                "os.environ", {"GROHE_CLOUD_REFRESH_TOKEN_FILE": path, "GROHE_CLOUD_REFRESH_TOKEN": ""}, clear=False
            ):
                token, returned_path = gcr._resolve_refresh_token()
            self.assertEqual(token, FAKE_REFRESH_TOKEN)
            self.assertEqual(returned_path, Path(path))
        finally:
            Path(path).unlink()


class MainTest(unittest.TestCase):
    def test_success_prints_only_key_value_lines_to_stdout(self):
        tokens = _FakeTokensDTO(access_token=_fake_jwt({"sub": FAKE_USER_ID}), refresh_token=FAKE_REFRESH_TOKEN)
        with (
            mock.patch.dict("os.environ", {"GROHE_CLOUD_REFRESH_TOKEN": FAKE_REFRESH_TOKEN}, clear=False),
            mock.patch("grohe.tokens.GroheTokens.get_refresh_tokens", return_value=tokens),
            mock.patch("httpx.AsyncClient.get", return_value=httpx_response(DASHBOARD_ONE_APPLIANCE)),
            mock.patch("sys.stdout", new_callable=io.StringIO) as fake_stdout,
            mock.patch("sys.stderr", new_callable=io.StringIO) as fake_stderr,
        ):
            exit_code = gcr.main()

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            fake_stdout.getvalue(),
            f"USER_ID={FAKE_USER_ID}\nPRESHARED_KEY_BASE64={FAKE_PRESHARED_KEY}\n",
        )
        self.assertNotIn(FAKE_REFRESH_TOKEN, fake_stderr.getvalue())

    def test_failure_prints_nothing_to_stdout_and_exits_nonzero(self):
        with (
            mock.patch.dict("os.environ", {"GROHE_CLOUD_REFRESH_TOKEN": FAKE_REFRESH_TOKEN}, clear=False),
            mock.patch(
                "grohe.tokens.GroheTokens.get_refresh_tokens",
                side_effect=GroheUnauthorizedError("Grohe refresh token is no longer valid"),
            ),
            mock.patch("sys.stdout", new_callable=io.StringIO) as fake_stdout,
            mock.patch("sys.stderr", new_callable=io.StringIO) as fake_stderr,
        ):
            exit_code = gcr.main()

        self.assertNotEqual(exit_code, 0)
        self.assertEqual(fake_stdout.getvalue(), "")
        self.assertNotIn(FAKE_REFRESH_TOKEN, fake_stderr.getvalue())

    def test_unexpected_exception_never_leaks_its_message(self):
        with (
            mock.patch.dict("os.environ", {"GROHE_CLOUD_REFRESH_TOKEN": FAKE_REFRESH_TOKEN}, clear=False),
            mock.patch(
                "grohe_cloud_refresh.get_provisioning_credentials",
                side_effect=RuntimeError(f"leaked-secret-{FAKE_REFRESH_TOKEN}"),
            ),
            mock.patch("sys.stdout", new_callable=io.StringIO) as fake_stdout,
            mock.patch("sys.stderr", new_callable=io.StringIO) as fake_stderr,
        ):
            exit_code = gcr.main()

        self.assertNotEqual(exit_code, 0)
        self.assertEqual(fake_stdout.getvalue(), "")
        self.assertNotIn(FAKE_REFRESH_TOKEN, fake_stderr.getvalue())
        self.assertIn("RuntimeError", fake_stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
