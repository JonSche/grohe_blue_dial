"""Config flow tests, using pytest-homeassistant-custom-component's real
HA test harness (unlike test_api.py, this module needs actual Home
Assistant core -- config_entries.ConfigFlow, hass fixtures, etc.).
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import pytest
from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType

from custom_components.grohe_dial.api import DialStatus, GroheDialAuthError, GroheDialConnectionError
from custom_components.grohe_dial.const import CONF_API_TOKEN, DOMAIN

pytestmark = pytest.mark.asyncio

_STATUS = DialStatus(
    connection_status="READY",
    time_status="AVAILABLE",
    dispense_status="IDLE",
    water_type="SPARKLING",
    amount_ml=500,
    active_dispense_amount_ml=0,
    delivered_ml=0,
    appliance_response_received=False,
    appliance_response_success=False,
    appliance_response_code=0,
)
_CONFIG = {"default_amount_ml": 500, "amount_step_ml": 100, "default_water_type": "SPARKLING"}


async def test_user_flow_success(hass: HomeAssistant) -> None:
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert result["type"] is FlowResultType.FORM

    # Both get_status (config_flow.py's own validation call, and the
    # coordinator's first refresh) and get_config (number.py/select.py's
    # own setup, both reached once CREATE_ENTRY below auto-triggers a
    # real, unpatched async_setup_entry() as a background task) must
    # stay mocked until that background task actually runs -- see the
    # explicit async_block_till_done() below, still inside this
    # patch context, not after it exits.
    with (
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_status",
            new=AsyncMock(return_value=_STATUS),
        ),
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_config",
            new=AsyncMock(return_value=type("C", (), _CONFIG)()),
        ),
    ):
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"],
            {"host": "192.168.1.50", "port": 8080, CONF_API_TOKEN: "sometoken"},
        )
        await hass.async_block_till_done()

    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["title"] == "Grohe Dial (192.168.1.50)"
    assert result["data"] == {"host": "192.168.1.50", "port": 8080, CONF_API_TOKEN: "sometoken"}


async def test_user_flow_invalid_auth(hass: HomeAssistant) -> None:
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})

    with patch(
        "custom_components.grohe_dial.config_flow.GroheDialApiClient.get_status",
        new=AsyncMock(side_effect=GroheDialAuthError("bad token")),
    ):
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"],
            {"host": "192.168.1.50", "port": 8080, CONF_API_TOKEN: "wrong"},
        )

    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "invalid_auth"}


async def test_user_flow_cannot_connect(hass: HomeAssistant) -> None:
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})

    with patch(
        "custom_components.grohe_dial.config_flow.GroheDialApiClient.get_status",
        new=AsyncMock(side_effect=GroheDialConnectionError("unreachable")),
    ):
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"],
            {"host": "10.0.0.99", "port": 8080, CONF_API_TOKEN: "sometoken"},
        )

    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "cannot_connect"}


async def test_user_flow_normalizes_float_port_to_int(hass: HomeAssistant) -> None:
    # Regression test for a real bug found on real hardware:
    # selector.NumberSelector (DATA_SCHEMA's own port field) always
    # yields a float, regardless of what was typed in the UI -- unlike
    # test_user_flow_success above, which passes a plain Python int and
    # therefore never exercised this path. A previous version of
    # async_step_user() validated this float successfully (via
    # _async_validate()'s own local int() cast) but then persisted the
    # *original* float via async_create_entry(data=user_input) -- the
    # stored config entry ended up with port=8080.0, which
    # GroheDialApiClient turned into the URL "http://<host>:8080.0",
    # permanently failing to connect (ConfigEntryNotReady, retried
    # forever, since the stored value never changed).
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})

    with (
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_status",
            new=AsyncMock(return_value=_STATUS),
        ),
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_config",
            new=AsyncMock(return_value=type("C", (), _CONFIG)()),
        ),
    ):
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"],
            {"host": "192.168.1.50", "port": 8080.0, CONF_API_TOKEN: "sometoken"},
        )
        await hass.async_block_till_done()

    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"]["port"] == 8080
    assert isinstance(result["data"]["port"], int), (
        f"port must be stored as int, not {type(result['data']['port'])} "
        f"-- a float here reproduces the exact bug this test guards against"
    )


async def test_duplicate_host_aborts(hass: HomeAssistant) -> None:
    with (
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_status",
            new=AsyncMock(return_value=_STATUS),
        ),
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_config",
            new=AsyncMock(return_value=type("C", (), _CONFIG)()),
        ),
    ):
        result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
        await hass.config_entries.flow.async_configure(
            result["flow_id"], {"host": "192.168.1.50", "port": 8080, CONF_API_TOKEN: "sometoken"}
        )
        await hass.async_block_till_done()

        result2 = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
        result2 = await hass.config_entries.flow.async_configure(
            result2["flow_id"], {"host": "192.168.1.50", "port": 8080, CONF_API_TOKEN: "sometoken"}
        )

    assert result2["type"] is FlowResultType.ABORT
    assert result2["reason"] == "already_configured"
