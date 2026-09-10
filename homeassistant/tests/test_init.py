"""Full config-entry setup: verifies the device and every entity this
integration is supposed to create actually appear in the registries --
not just that async_setup_entry() returns True.
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr, entity_registry as er
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.grohe_dial.api import DialStatus
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


async def test_setup_creates_device_and_entities(hass: HomeAssistant, enable_custom_integrations) -> None:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="192.168.1.50",
        data={"host": "192.168.1.50", "port": 8080, CONF_API_TOKEN: "sometoken"},
    )
    entry.add_to_hass(hass)

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

    assert entry.state.value == "loaded"

    device_reg = dr.async_get(hass)
    device = device_reg.async_get_device_by_identifier((DOMAIN, "192.168.1.50"), entry.entry_id)
    assert device is not None
    assert device.manufacturer == "Grohe Dial (community project)"

    entity_reg = er.async_get(hass)
    entities = er.async_entries_for_device(entity_reg, device.id)
    entity_domains = sorted(e.entity_id.split(".")[0] for e in entities)
    # Every entity every platform module registers: binary_sensor.py (1),
    # button.py (2: dispense, stop), number.py (2: default_amount_ml,
    # amount_step_ml), select.py (1: default_water_type), sensor.py (3:
    # dispense_status, connection_status, delivered_ml).
    assert entity_domains == sorted(
        ["binary_sensor"]
        + ["button", "button"]
        + ["number", "number"]
        + ["select"]
        + ["sensor", "sensor", "sensor"]
    )

    # Services registered globally, not per-entry.
    assert hass.services.has_service(DOMAIN, "dispense")
    assert hass.services.has_service(DOMAIN, "stop")

    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state.value == "not_loaded"
