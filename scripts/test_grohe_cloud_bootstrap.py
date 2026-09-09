#!/usr/bin/env python3
"""Tests for grohe_cloud_bootstrap.py -- mocks input()/getpass.getpass() and
grohe.tokens.GroheTokens.get_tokens_from_credentials (the library call this
script delegates the actual login to). No real network access, no real
credentials. See test_grohe_cloud_refresh.py's own docstring for the same
fake-fixture convention used here.

Run with:
    python3 scripts/test_grohe_cloud_bootstrap.py
"""

from __future__ import annotations

import io
import sys
import unittest
from dataclasses import dataclass
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import grohe_cloud_bootstrap as bootstrap  # noqa: E402
from grohe.exceptions import GroheUnauthorizedError  # noqa: E402

FAKE_EMAIL = "fake-user@example.invalid"
FAKE_PASSWORD = "fake-password-do-not-use"  # noqa: S105 -- test fixture, not a secret
FAKE_REFRESH_TOKEN = "fake-refresh-token-from-bootstrap"  # noqa: S105


@dataclass
class _FakeTokensDTO:
    access_token: str
    refresh_token: str


class MainTest(unittest.TestCase):
    def _run_main(self, token_file: Path):
        return mock.patch.dict("os.environ", {"GROHE_CLOUD_REFRESH_TOKEN_FILE": str(token_file)}, clear=False)

    def test_successful_bootstrap_writes_only_the_refresh_token(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            token_file = Path(tmp) / "refresh_token"
            tokens = _FakeTokensDTO(access_token="unused", refresh_token=FAKE_REFRESH_TOKEN)
            with (
                self._run_main(token_file),
                mock.patch("builtins.input", return_value=FAKE_EMAIL),
                mock.patch("getpass.getpass", return_value=FAKE_PASSWORD),
                mock.patch("grohe.tokens.GroheTokens.get_tokens_from_credentials", return_value=tokens),
                mock.patch("sys.stdout", new_callable=io.StringIO) as fake_stdout,
                mock.patch("sys.stderr", new_callable=io.StringIO) as fake_stderr,
            ):
                exit_code = bootstrap.main()

            self.assertEqual(exit_code, 0)
            self.assertEqual(token_file.read_text().strip(), FAKE_REFRESH_TOKEN)
            combined_output = fake_stdout.getvalue() + fake_stderr.getvalue()
            self.assertNotIn(FAKE_PASSWORD, combined_output)
            self.assertNotIn(FAKE_REFRESH_TOKEN, combined_output)

    def test_empty_email_aborts_without_attempting_login(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            token_file = Path(tmp) / "refresh_token"
            with (
                self._run_main(token_file),
                mock.patch("builtins.input", return_value=""),
                mock.patch("getpass.getpass", return_value=FAKE_PASSWORD),
                mock.patch("grohe.tokens.GroheTokens.get_tokens_from_credentials") as fake_login,
                mock.patch("sys.stdout", new_callable=io.StringIO),
                mock.patch("sys.stderr", new_callable=io.StringIO),
            ):
                exit_code = bootstrap.main()

            self.assertNotEqual(exit_code, 0)
            fake_login.assert_not_called()
            self.assertFalse(token_file.exists())

    def test_empty_password_aborts_without_attempting_login(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            token_file = Path(tmp) / "refresh_token"
            with (
                self._run_main(token_file),
                mock.patch("builtins.input", return_value=FAKE_EMAIL),
                mock.patch("getpass.getpass", return_value=""),
                mock.patch("grohe.tokens.GroheTokens.get_tokens_from_credentials") as fake_login,
                mock.patch("sys.stdout", new_callable=io.StringIO),
                mock.patch("sys.stderr", new_callable=io.StringIO),
            ):
                exit_code = bootstrap.main()

            self.assertNotEqual(exit_code, 0)
            fake_login.assert_not_called()
            self.assertFalse(token_file.exists())

    def test_wrong_credentials_reports_error_without_writing_file(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            token_file = Path(tmp) / "refresh_token"
            with (
                self._run_main(token_file),
                mock.patch("builtins.input", return_value=FAKE_EMAIL),
                mock.patch("getpass.getpass", return_value=FAKE_PASSWORD),
                mock.patch(
                    "grohe.tokens.GroheTokens.get_tokens_from_credentials",
                    side_effect=GroheUnauthorizedError("Invalid Grohe username or password"),
                ),
                mock.patch("sys.stdout", new_callable=io.StringIO),
                mock.patch("sys.stderr", new_callable=io.StringIO) as fake_stderr,
            ):
                exit_code = bootstrap.main()

            self.assertNotEqual(exit_code, 0)
            self.assertFalse(token_file.exists())
            self.assertNotIn(FAKE_PASSWORD, fake_stderr.getvalue())

    def test_response_without_refresh_token_is_an_error(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            token_file = Path(tmp) / "refresh_token"
            tokens = _FakeTokensDTO(access_token="unused", refresh_token="")
            with (
                self._run_main(token_file),
                mock.patch("builtins.input", return_value=FAKE_EMAIL),
                mock.patch("getpass.getpass", return_value=FAKE_PASSWORD),
                mock.patch("grohe.tokens.GroheTokens.get_tokens_from_credentials", return_value=tokens),
                mock.patch("sys.stdout", new_callable=io.StringIO),
                mock.patch("sys.stderr", new_callable=io.StringIO),
            ):
                exit_code = bootstrap.main()

            self.assertNotEqual(exit_code, 0)
            self.assertFalse(token_file.exists())


if __name__ == "__main__":
    unittest.main()
