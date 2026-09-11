"""M15.1: the config entry's stable, MAC-based unique_id -- both the
"brand new entry" path (config_flow.py's async_step_user(), using the
firmware-reported device_id directly when available) and the "existing
entry, upgrading in place" path
(__init__.py's _async_migrate_to_stable_unique_id(), which must rename
the *same* device/entity registry rows rather than let new ones be
created alongside orphaned old ones -- see that function's own
docstring for the full reasoning this test suite verifies).
"""

from __future__ import annotations

from unittest.mock import AsyncMock, patch

import pytest
from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers import device_registry as dr, entity_registry as er
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.grohe_dial.api import DialStatus
from custom_components.grohe_dial.const import CONF_API_TOKEN, DOMAIN

pytestmark = pytest.mark.asyncio

_CONFIG = {"default_amount_ml": 500, "amount_step_ml": 100, "default_water_type": "SPARKLING"}
_HOST = "192.168.1.60"
_DEVICE_ID = "3c:71:bf:12:34:56"


def _status(device_id: str | None) -> DialStatus:
    return DialStatus(
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
        device_id=device_id,
    )


def _patched(status: DialStatus):
    return (
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_status",
            new=AsyncMock(return_value=status),
        ),
        patch(
            "custom_components.grohe_dial.api.GroheDialApiClient.get_config",
            new=AsyncMock(return_value=type("C", (), _CONFIG)()),
        ),
    )


async def test_new_entry_uses_stable_id_when_firmware_reports_one(hass: HomeAssistant) -> None:
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    p1, p2 = _patched(_status(_DEVICE_ID))
    with p1, p2:
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], {"host": _HOST, "port": 8080, CONF_API_TOKEN: "sometoken"}
        )
        await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY

    entries = hass.config_entries.async_entries(DOMAIN)
    assert len(entries) == 1
    assert entries[0].unique_id == f"mac-{_DEVICE_ID}"


async def test_new_entry_falls_back_to_host_without_device_id(hass: HomeAssistant) -> None:
    """Old firmware, predating the device_id field -- unchanged M15
    behavior, not a regression."""
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    p1, p2 = _patched(_status(None))
    with p1, p2:
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], {"host": _HOST, "port": 8080, CONF_API_TOKEN: "sometoken"}
        )
        await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY

    entries = hass.config_entries.async_entries(DOMAIN)
    assert entries[0].unique_id == _HOST


async def test_migration_renames_existing_device_and_entities_in_place(
    hass: HomeAssistant, enable_custom_integrations
) -> None:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=_HOST,
        data={"host": _HOST, "port": 8080, CONF_API_TOKEN: "sometoken"},
    )
    entry.add_to_hass(hass)

    # First setup: old firmware (no device_id yet) -- establishes the
    # device/entity registry rows under the legacy host-based identity,
    # exactly like every real installation that predates M15.1.
    p1, p2 = _patched(_status(None))
    with p1, p2:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    device_registry = dr.async_get(hass)
    entity_registry = er.async_get(hass)
    old_device = device_registry.async_get_device_by_identifier((DOMAIN, _HOST), entry.entry_id)
    assert old_device is not None
    old_device_id = old_device.id
    old_entities = {e.entity_id: e.unique_id for e in er.async_entries_for_device(entity_registry, old_device_id)}
    assert old_entities  # sanity: the fixture really did create entities
    assert all(uid.startswith(f"{_HOST}_") for uid in old_entities.values())

    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()

    # Second setup, same entry: firmware now reports a device_id (e.g.
    # the dial was reflashed to M15.1+) -- must rename in place, not
    # create a parallel device/entities.
    p1, p2 = _patched(_status(_DEVICE_ID))
    with p1, p2:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    assert entry.unique_id == f"mac-{_DEVICE_ID}"

    new_device = device_registry.async_get_device_by_identifier(
        (DOMAIN, f"mac-{_DEVICE_ID}"), entry.entry_id
    )
    assert new_device is not None
    assert new_device.id == old_device_id  # same device, renamed -- not a new one

    # The *old* identifier must no longer resolve to anything.
    assert device_registry.async_get_device_by_identifier((DOMAIN, _HOST), entry.entry_id) is None

    new_entities = {e.entity_id: e.unique_id for e in er.async_entries_for_device(entity_registry, old_device_id)}
    # Same entity_ids as before (nothing orphaned, nothing duplicated) --
    # only their own unique_id moved to the new prefix.
    assert set(new_entities.keys()) == set(old_entities.keys())
    for entity_id, old_unique_id in old_entities.items():
        suffix = old_unique_id[len(_HOST) :]
        assert new_entities[entity_id] == f"mac-{_DEVICE_ID}{suffix}"


async def test_migration_noop_without_device_id(hass: HomeAssistant, enable_custom_integrations) -> None:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=_HOST,
        data={"host": _HOST, "port": 8080, CONF_API_TOKEN: "sometoken"},
    )
    entry.add_to_hass(hass)

    p1, p2 = _patched(_status(None))
    with p1, p2:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    assert entry.unique_id == _HOST


async def test_migration_idempotent_once_already_migrated(
    hass: HomeAssistant, enable_custom_integrations
) -> None:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=f"mac-{_DEVICE_ID}",
        data={"host": _HOST, "port": 8080, CONF_API_TOKEN: "sometoken"},
    )
    entry.add_to_hass(hass)

    p1, p2 = _patched(_status(_DEVICE_ID))
    with p1, p2:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
    assert entry.unique_id == f"mac-{_DEVICE_ID}"

    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()

    # Setting up again (e.g. an HA restart) must not error or change
    # anything further -- the prefix check alone already guards this.
    p3, p4 = _patched(_status(_DEVICE_ID))
    with p3, p4:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
    assert entry.unique_id == f"mac-{_DEVICE_ID}"


async def test_migration_self_heals_a_stale_device_identifier(
    hass: HomeAssistant, enable_custom_integrations
) -> None:
    """Regression test for a real bug found deploying to a real, live
    HA 2026.4.1 installation: a first migration correctly updated
    entry.unique_id and every entity's own unique_id, but -- on that HA
    version specifically, where DeviceRegistry has no async_get_devices()
    at all -- silently failed to find/rename the device registry row
    itself, leaving it permanently stuck on the legacy identifier (the
    original guard only checked entry.unique_id, so a device-rename
    gated the exact same way would never get a second chance). The fix
    decouples the device-identifier check from that guard and re-runs
    it, cheaply, on every setup -- this simulates exactly that stuck
    state (unique_id and entities already correct, device identifier
    deliberately left stale) and confirms a further setup corrects it.
    """
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=_HOST,
        data={"host": _HOST, "port": 8080, CONF_API_TOKEN: "sometoken"},
    )
    entry.add_to_hass(hass)

    p1, p2 = _patched(_status(None))
    with p1, p2:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    device_registry = dr.async_get(hass)
    old_device = device_registry.async_get_device_by_identifier((DOMAIN, _HOST), entry.entry_id)
    assert old_device is not None
    device_id = old_device.id

    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()

    # Simulate the real, partially-migrated state directly: unique_id
    # and entity unique_ids already correct, but the device's own
    # identifiers manually left on the legacy value -- exactly what was
    # found on the real installation, not artificially constructed from
    # nothing.
    hass.config_entries.async_update_entry(entry, unique_id=f"mac-{_DEVICE_ID}")
    entity_registry = er.async_get(hass)
    for entity_entry in er.async_entries_for_config_entry(entity_registry, entry.entry_id):
        assert entity_entry.unique_id.startswith(f"{_HOST}_")
        suffix = entity_entry.unique_id[len(_HOST) :]
        entity_registry.async_update_entity(
            entity_entry.entity_id, new_unique_id=f"mac-{_DEVICE_ID}{suffix}"
        )
    assert device_registry.async_get_device_by_identifier((DOMAIN, _HOST), entry.entry_id) is not None
    assert (
        device_registry.async_get_device_by_identifier((DOMAIN, f"mac-{_DEVICE_ID}"), entry.entry_id)
        is None
    )

    # A further setup, now against firmware that *does* report a
    # device_id, must find and fix the still-stale device identifier
    # even though entry.unique_id already looks fully migrated.
    p3, p4 = _patched(_status(_DEVICE_ID))
    with p3, p4:
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()

    fixed_device = device_registry.async_get_device_by_identifier(
        (DOMAIN, f"mac-{_DEVICE_ID}"), entry.entry_id
    )
    assert fixed_device is not None
    assert fixed_device.id == device_id  # same device, not a new one
    assert device_registry.async_get_device_by_identifier((DOMAIN, _HOST), entry.entry_id) is None
