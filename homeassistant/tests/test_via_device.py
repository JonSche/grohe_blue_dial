"""M15.3: via_device_id linking this dial's own Device Registry entry to
a matching ha-grohe_smarthome device, when one is installed and owns a
device for the same Cloud appliance_id this dial was provisioned
against (config_flow.py's GroheDialOptionsFlow persists that
appliance_id -- see const.py's own CONF_GROHE_APPLIANCE_ID comment).
Every scenario is driven through the real __init__.py's
async_setup_entry() and inspected via the real Device Registry, matching
this suite's own established style (test_init.py, test_stable_identity.py)
rather than reaching into __init__.py's private _resolve_via_device()
directly.
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.grohe_dial import entity as entity_module
from custom_components.grohe_dial.api import DialStatus
from custom_components.grohe_dial.const import CONF_API_TOKEN, CONF_GROHE_APPLIANCE_ID, DOMAIN, GROHE_SMARTHOME_DOMAIN

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
    device_id="3c:71:bf:12:34:56",
)
_CONFIG = {"default_amount_ml": 500, "amount_step_ml": 100, "default_water_type": "SPARKLING"}
_HOST = "192.168.1.70"
_APPLIANCE_ID = "cloud-appliance-uuid-1234"


def _patched():
    return (
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_status",
            new=AsyncMock(return_value=_STATUS),
        ),
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_config",
            new=AsyncMock(return_value=type("C", (), _CONFIG)()),
        ),
    )


async def _setup_dial_entry(hass: HomeAssistant, *, with_appliance_id: bool) -> MockConfigEntry:
    data = {"host": _HOST, "port": 8080, CONF_API_TOKEN: "sometoken"}
    if with_appliance_id:
        data[CONF_GROHE_APPLIANCE_ID] = _APPLIANCE_ID
    entry = MockConfigEntry(domain=DOMAIN, unique_id=_HOST, data=data)
    entry.add_to_hass(hass)
    p1, p2 = _patched()
    with p1, p2:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
    return entry


def _dial_device(hass: HomeAssistant, entry: MockConfigEntry):
    device_registry = dr.async_get(hass)
    return device_registry.async_get_device_by_identifier((DOMAIN, entry.unique_id), entry.entry_id)


async def test_via_device_id_resolved_when_matching_device_exists(hass: HomeAssistant) -> None:
    smarthome_entry = MockConfigEntry(domain=GROHE_SMARTHOME_DOMAIN)
    smarthome_entry.add_to_hass(hass)
    device_registry = dr.async_get(hass)
    smarthome_device = device_registry.async_get_or_create(
        config_entry_id=smarthome_entry.entry_id,
        identifiers={(GROHE_SMARTHOME_DOMAIN, _APPLIANCE_ID)},
        name="My GROHE Blue Home",
    )

    entry = await _setup_dial_entry(hass, with_appliance_id=True)

    dial_device = _dial_device(hass, entry)
    assert dial_device is not None
    assert dial_device.via_device_id == smarthome_device.id


async def test_via_device_id_none_without_appliance_id(hass: HomeAssistant) -> None:
    """Dial provisioned outside this integration's Options Flow (e.g.
    scripts/provision.sh) -- no known Cloud appliance_id at all."""
    entry = await _setup_dial_entry(hass, with_appliance_id=False)

    dial_device = _dial_device(hass, entry)
    assert dial_device is not None
    assert dial_device.via_device_id is None


async def test_via_device_id_none_without_matching_device(hass: HomeAssistant) -> None:
    """appliance_id known, but ha-grohe_smarthome isn't installed (or
    hasn't created a device for it) -- no crash, just no link."""
    entry = await _setup_dial_entry(hass, with_appliance_id=True)

    dial_device = _dial_device(hass, entry)
    assert dial_device is not None
    assert dial_device.via_device_id is None


async def test_via_device_tuple_used_on_ha_versions_without_via_device_id(
    hass: HomeAssistant,
) -> None:
    """Real bug, found deploying to a real HA 2026.4.1 instance: that
    version's own DeviceInfo TypedDict has no `via_device_id` key at all
    (only the older `via_device` (domain, identifier) tuple form) --
    entity.py's own _SUPPORTS_VIA_DEVICE_ID check picks between the two
    at import time, but this suite's own pinned test dependency is newer
    and always takes the via_device_id branch on its own. Monkeypatching
    the flag directly is the only way to exercise the older branch here,
    without pinning this whole suite to an older HA release just for this
    one path -- entity.py's own DeviceInfo construction is what's under
    test, not which branch a given HA version happens to select.
    """
    smarthome_entry = MockConfigEntry(domain=GROHE_SMARTHOME_DOMAIN)
    smarthome_entry.add_to_hass(hass)
    device_registry = dr.async_get(hass)
    device_registry.async_get_or_create(
        config_entry_id=smarthome_entry.entry_id,
        identifiers={(GROHE_SMARTHOME_DOMAIN, _APPLIANCE_ID)},
        name="My GROHE Blue Home",
    )

    with patch.object(entity_module, "_SUPPORTS_VIA_DEVICE_ID", False):
        entry = await _setup_dial_entry(hass, with_appliance_id=True)

    dial_device = _dial_device(hass, entry)
    assert dial_device is not None
    # HA's own DeviceRegistry resolves a stored `via_device` tuple back to
    # the same internal device.id regardless of which DeviceInfo key was
    # used to set it -- via_device_id is the one stable, version-
    # independent thing to assert on here.
    assert dial_device.via_device_id is not None
