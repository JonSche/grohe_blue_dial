"""M16.9: the provisioning Options Flow (config_flow.py's own
GroheDialOptionsFlow) end to end, against a MockConfigEntry -- mocks
cloud.py's own functions and api.provision_dial() (never a real Grohe
Cloud call, never real credentials, per this milestone's own explicit
rule) but drives the real HA options-flow machinery
(hass.config_entries.options.async_init/async_configure), the same
level test_config_flow.py already uses for the main config flow.
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.grohe_dial import cloud
from custom_components.grohe_dial.api import (
    GroheDialAuthError,
    GroheDialCommandRejected,
    GroheDialConnectionError,
)
from custom_components.grohe_dial.const import CONF_API_TOKEN, CONF_GROHE_APPLIANCE_ID, DOMAIN

pytestmark = pytest.mark.asyncio

# A structurally valid (unsigned) JWT whose payload is {"sub": "abc-123-user-id"} --
# matches test_cloud.py's own _FAKE_JWT_WITH_SUB; needed here too since
# async_step_provision_token() really calls cloud.user_id_from_access_token()
# (not mocked -- it's pure/local, no network) on _TOKENS.access_token.
_FAKE_JWT_WITH_SUB = (
    "eyJhbGciOiJIUzI1NiJ9."
    "eyJzdWIiOiJhYmMtMTIzLXVzZXItaWQifQ."
    "signature-is-never-checked"
)
_TOKENS = cloud.GroheCloudTokens(access_token=_FAKE_JWT_WITH_SUB, refresh_token="fake-refresh")
_ONE_CANDIDATE = [
    cloud.GroheApplianceCandidate(name="Kitchen Blue Home", appliance_id="appl-1", preshared_key_base64="key1")
]
_TWO_CANDIDATES = _ONE_CANDIDATE + [
    cloud.GroheApplianceCandidate(name="Office Blue Home", appliance_id="appl-2", preshared_key_base64="key2")
]


@pytest.fixture
def entry(hass: HomeAssistant, enable_custom_integrations) -> MockConfigEntry:
    e = MockConfigEntry(
        domain=DOMAIN,
        unique_id="192.168.1.70",
        data={"host": "192.168.1.70", "port": 8080, CONF_API_TOKEN: "sometoken"},
    )
    e.add_to_hass(hass)
    return e


async def test_successful_provisioning_single_appliance(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with (
        patch("custom_components.grohe_dial.config_flow.cloud.login_with_credentials", new=AsyncMock(return_value=_TOKENS)),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=_ONE_CANDIDATE)),
        patch("custom_components.grohe_dial.config_flow.provision_dial", new=AsyncMock(return_value=None)) as mock_provision,
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        assert result["step_id"] == "cloud_login"

        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "irrelevant-in-test"}
        )
        # Single appliance -- auto-selected, skips straight to the token step.
        assert result["step_id"] == "provision_token"

        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"provision_token": "the-dials-provision-token"}
        )
        assert result["type"] is FlowResultType.CREATE_ENTRY

    mock_provision.assert_called_once()
    call_kwargs = mock_provision.call_args
    # user_id (from the JWT "sub" claim of _TOKENS' access_token in a
    # real run) and the appliance's own preshared key must have reached
    # provision_dial() -- confirms the whole chain actually wired
    # together, not just that each step didn't crash.
    assert call_kwargs.args[5] == "key1"  # preshared_key_base64 (positional arg 5: session,host,port,provision_token,user_id,preshared_key_base64)
    # M15.3: the Cloud appliance_id must be persisted into entry.data so
    # __init__.py's own via_device_id resolution can find it later.
    assert entry.data[CONF_GROHE_APPLIANCE_ID] == "appl-1"


async def test_multiple_appliances_shows_picker(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with (
        patch("custom_components.grohe_dial.config_flow.cloud.login_with_credentials", new=AsyncMock(return_value=_TOKENS)),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=_TWO_CANDIDATES)),
        patch("custom_components.grohe_dial.config_flow.provision_dial", new=AsyncMock(return_value=None)) as mock_provision,
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "irrelevant"}
        )
        assert result["step_id"] == "select_appliance"

        result = await hass.config_entries.options.async_configure(result["flow_id"], {"appliance": "appl-2"})
        assert result["step_id"] == "provision_token"

        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"provision_token": "tok"}
        )
        assert result["type"] is FlowResultType.CREATE_ENTRY

    assert mock_provision.call_args.args[5] == "key2"  # the *selected* (Office) appliance's key, not Kitchen's (index 5, not 4 -- see above)
    assert entry.data[CONF_GROHE_APPLIANCE_ID] == "appl-2"  # the *selected* appliance's id, not the first candidate's


async def test_invalid_cloud_credentials(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with patch(
        "custom_components.grohe_dial.config_flow.cloud.login_with_credentials",
        new=AsyncMock(side_effect=cloud.GroheCloudAuthError("bad credentials")),
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "wrong"}
        )
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "invalid_cloud_auth"}


async def test_cloud_unavailable(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with patch(
        "custom_components.grohe_dial.config_flow.cloud.login_with_credentials",
        new=AsyncMock(side_effect=cloud.GroheCloudConnectionError("network down")),
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "irrelevant"}
        )
    assert result["errors"] == {"base": "cloud_unavailable"}


async def test_no_appliances_found_aborts(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with (
        patch("custom_components.grohe_dial.config_flow.cloud.login_with_credentials", new=AsyncMock(return_value=_TOKENS)),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=[])),
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "irrelevant"}
        )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "no_appliances_found"


async def test_dial_unreachable_during_provisioning(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with (
        patch("custom_components.grohe_dial.config_flow.cloud.login_with_credentials", new=AsyncMock(return_value=_TOKENS)),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=_ONE_CANDIDATE)),
        patch(
            "custom_components.grohe_dial.config_flow.provision_dial",
            new=AsyncMock(side_effect=GroheDialConnectionError("refused")),
        ),
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "irrelevant"}
        )
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"provision_token": "tok"}
        )
    assert result["errors"] == {"base": "dial_unreachable"}


async def test_invalid_provision_token(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with (
        patch("custom_components.grohe_dial.config_flow.cloud.login_with_credentials", new=AsyncMock(return_value=_TOKENS)),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=_ONE_CANDIDATE)),
        patch(
            "custom_components.grohe_dial.config_flow.provision_dial",
            new=AsyncMock(side_effect=GroheDialAuthError("bad provision token")),
        ),
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "irrelevant"}
        )
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"provision_token": "wrong-token"}
        )
    assert result["errors"] == {"base": "invalid_provision_token"}


async def test_provisioning_rejected_by_dial(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    with (
        patch("custom_components.grohe_dial.config_flow.cloud.login_with_credentials", new=AsyncMock(return_value=_TOKENS)),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=_ONE_CANDIDATE)),
        patch(
            "custom_components.grohe_dial.config_flow.provision_dial",
            new=AsyncMock(side_effect=GroheDialCommandRejected("malformed body", 400)),
        ),
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": "irrelevant"}
        )
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"provision_token": "tok"}
        )
    assert result["errors"] == {"base": "provisioning_failed"}


async def test_repeated_provisioning_is_idempotent(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    """Running the whole flow twice in a row (e.g. rotating the Grohe
    Blue's credentials later) must succeed both times -- POST /provision
    itself is already idempotent (M13.2, a single atomic NVS overwrite),
    this just confirms the HA-side flow doesn't accumulate state across
    runs that would break a second attempt."""
    with (
        patch("custom_components.grohe_dial.config_flow.cloud.login_with_credentials", new=AsyncMock(return_value=_TOKENS)),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=_ONE_CANDIDATE)),
        patch("custom_components.grohe_dial.config_flow.provision_dial", new=AsyncMock(return_value=None)) as mock_provision,
    ):
        for _ in range(2):
            result = await hass.config_entries.options.async_init(entry.entry_id)
            result = await hass.config_entries.options.async_configure(
                result["flow_id"], {"email": "user@example.com", "password": "irrelevant"}
            )
            result = await hass.config_entries.options.async_configure(
                result["flow_id"], {"provision_token": "tok"}
            )
            assert result["type"] is FlowResultType.CREATE_ENTRY

    assert mock_provision.call_count == 2


async def test_no_secrets_in_logs(hass: HomeAssistant, entry: MockConfigEntry, caplog: pytest.LogCaptureFixture) -> None:
    """Neither the Grohe account password, the resulting refresh_token,
    nor the dial's provisioning token may ever appear in HA's logs --
    this milestone's own explicit security requirement."""
    secret_password = "correct-horse-battery-staple"  # noqa: S105 - test fixture, not a real secret
    secret_provision_token = "super-secret-provision-token"
    with (
        patch(
            "custom_components.grohe_dial.config_flow.cloud.login_with_credentials",
            new=AsyncMock(return_value=_TOKENS),
        ),
        patch("custom_components.grohe_dial.config_flow.cloud.list_appliances", new=AsyncMock(return_value=_ONE_CANDIDATE)),
        patch("custom_components.grohe_dial.config_flow.provision_dial", new=AsyncMock(return_value=None)),
    ):
        result = await hass.config_entries.options.async_init(entry.entry_id)
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"email": "user@example.com", "password": secret_password}
        )
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"provision_token": secret_provision_token}
        )
    assert result["type"] is FlowResultType.CREATE_ENTRY

    log_text = caplog.text
    assert secret_password not in log_text
    assert secret_provision_token not in log_text
    assert _TOKENS.refresh_token not in log_text
