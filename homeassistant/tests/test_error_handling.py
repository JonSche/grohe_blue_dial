"""M16.2: button/service actions must never let a raw GroheDialApiError
escape as HA's generic "Unknown error" -- see errors.py's own header
comment for the real bug this fixes (found on real hardware during the
M15 acceptance test: a rejected duplicate dispense showed as an
unhelpful "Unknown error" in the UI).

Patches GroheDialApiClient.dispense()/stop() directly (unlike
test_integration_flow.py's FakeDial-over-real-HTTP approach) -- this
file is specifically about the *translation* logic in errors.py, not
the request/response cycle, which is already covered elsewhere.
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.grohe_dial.api import (
    DialStatus,
    GroheDialApiError,
    GroheDialCommandRejected,
    GroheDialConnectionError,
)
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


@pytest.fixture
async def setup_entry(hass: HomeAssistant, enable_custom_integrations):
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="192.168.1.60",
        data={"host": "192.168.1.60", "port": 8080, CONF_API_TOKEN: "sometoken"},
    )
    entry.add_to_hass(hass)
    # get_status/get_config must stay patched for the *whole* test, not
    # just setup -- a successful button press/service call ends with
    # coordinator.async_request_refresh(), which calls get_status()
    # again as its own side effect. Patching only around
    # async_setup_entry() (as this fixture originally did) left that
    # later call unmocked -- a real, doomed connection attempt to
    # 192.168.1.60, blocked by pytest-socket. yield stays inside the
    # `with` block so the patches cover the entire test body.
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
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
        yield hass, entry


async def _press(hass: HomeAssistant, entity_id: str) -> None:
    await hass.services.async_call("button", "press", {"entity_id": entity_id}, blocking=True)


def _dispense_button_id(hass: HomeAssistant) -> str:
    from homeassistant.helpers import entity_registry as er

    entity_reg = er.async_get(hass)
    entity_id = entity_reg.async_get_entity_id("button", DOMAIN, "192.168.1.60_dispense")
    assert entity_id is not None
    return entity_id


def _stop_button_id(hass: HomeAssistant) -> str:
    from homeassistant.helpers import entity_registry as er

    entity_reg = er.async_get(hass)
    entity_id = entity_reg.async_get_entity_id("button", DOMAIN, "192.168.1.60_stop")
    assert entity_id is not None
    return entity_id


async def test_dispense_success_raises_nothing(setup_entry) -> None:
    hass, entry = setup_entry
    with patch(
        "custom_components.grohe_dial.api.GroheDialApiClient.dispense",
        new=AsyncMock(return_value=None),
    ):
        await _press(hass, _dispense_button_id(hass))  # must not raise


async def test_dispense_connection_error_gives_actionable_message(setup_entry) -> None:
    hass, entry = setup_entry
    with patch(
        "custom_components.grohe_dial.api.GroheDialApiClient.dispense",
        new=AsyncMock(side_effect=GroheDialConnectionError("refused")),
    ):
        with pytest.raises(HomeAssistantError) as exc_info:
            await _press(hass, _dispense_button_id(hass))
    assert exc_info.value.translation_key == "dial_unreachable"
    assert exc_info.value.translation_domain == DOMAIN
    # The original error must survive as the cause, not be swallowed.
    assert isinstance(exc_info.value.__cause__, GroheDialConnectionError)


async def test_dispense_command_rejected_gives_actionable_message(setup_entry) -> None:
    hass, entry = setup_entry
    with patch(
        "custom_components.grohe_dial.api.GroheDialApiClient.dispense",
        new=AsyncMock(side_effect=GroheDialCommandRejected("not_available", 409)),
    ):
        with pytest.raises(HomeAssistantError) as exc_info:
            await _press(hass, _dispense_button_id(hass))
    assert exc_info.value.translation_key == "command_rejected"
    assert exc_info.value.translation_placeholders == {"reason": "not_available"}


async def test_stop_failure_gives_actionable_message(setup_entry) -> None:
    hass, entry = setup_entry
    with patch(
        "custom_components.grohe_dial.api.GroheDialApiClient.stop",
        new=AsyncMock(side_effect=GroheDialCommandRejected("not_available", 409)),
    ):
        with pytest.raises(HomeAssistantError) as exc_info:
            await _press(hass, _stop_button_id(hass))
    assert exc_info.value.translation_key == "command_rejected"


async def test_unexpected_api_error_still_gets_a_specific_message(setup_entry) -> None:
    hass, entry = setup_entry
    with patch(
        "custom_components.grohe_dial.api.GroheDialApiClient.dispense",
        new=AsyncMock(side_effect=GroheDialApiError("something unexpected")),
    ):
        with pytest.raises(HomeAssistantError) as exc_info:
            await _press(hass, _dispense_button_id(hass))
    assert exc_info.value.translation_key == "unexpected_dial_error"
    assert exc_info.value.translation_placeholders == {"error": "something unexpected"}


async def test_dispense_service_connection_error_gives_actionable_message(setup_entry) -> None:
    hass, entry = setup_entry
    with patch(
        "custom_components.grohe_dial.api.GroheDialApiClient.dispense",
        new=AsyncMock(side_effect=GroheDialConnectionError("refused")),
    ):
        with pytest.raises(HomeAssistantError) as exc_info:
            await hass.services.async_call(
                DOMAIN,
                "dispense",
                {"entity_id": _dispense_button_id(hass), "amount_ml": 100, "water_type": "still"},
                blocking=True,
            )
    assert exc_info.value.translation_key == "dial_unreachable"
