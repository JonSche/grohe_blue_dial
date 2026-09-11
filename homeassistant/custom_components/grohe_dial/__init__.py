"""The Grohe Dial integration.

Architecture (see docs/m15_ha_integration.md for the full picture):

    Home Assistant
          | native integration, local HTTP
          v
      Grohe Dial (this repo's own firmware)
          | BLE
          v
      Grohe Blue Home

Not MQTT Discovery -- a real Config Entry, device registry entry, and
entity registry entries, all owned by this integration.
"""

from __future__ import annotations

import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT, Platform
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import DialStatus, GroheDialApiClient, GroheDialApiError
from .const import (
    CONF_API_TOKEN,
    CONF_GROHE_APPLIANCE_ID,
    DOMAIN,
    GROHE_SMARTHOME_DOMAIN,
    STABLE_UNIQUE_ID_PREFIX,
)
from .coordinator import GroheDialCoordinator
from .services import async_register_services

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [
    Platform.BINARY_SENSOR,
    Platform.BUTTON,
    Platform.NUMBER,
    Platform.SELECT,
    Platform.SENSOR,
]

type GroheDialConfigEntry = ConfigEntry[GroheDialCoordinator]


async def _async_migrate_to_stable_unique_id(
    hass: HomeAssistant, entry: GroheDialConfigEntry, status: DialStatus
) -> None:
    """M15.1: moves this entry from its original host/IP-based unique_id
    (config_flow.py's own comment on why that was used for the M15
    vertical slice) to a stable one derived from the dial's own Wi-Fi MAC
    -- the whole reason `unique_id` existed as a *separate* field from
    `entry.data` in the first place, so it could later be upgraded like
    this without touching connection details at all.

    Deliberately NOT done via HA's formal async_migrate_entry/VERSION
    mechanism: that runs *before* this integration's own
    ConfigEntryNotReady retry logic even exists for this entry, so a
    dial that's merely offline at HA startup would strand the entry in
    MIGRATION_ERROR instead of the graceful, automatically-retried state
    ConfigEntryNotReady already provides. Running this here instead --
    after a real, successful status fetch -- means it only ever runs
    when the dial is definitely reachable, and is a no-op (guarded by
    the prefix check below) on every call after the first.

    A no-op entirely if status.device_id is None (talking to firmware
    older than this field) -- self-heals automatically the next time
    this entry is set up against firmware that has it, never blocks
    startup on it.

    The actual rename does NOT touch entry.data (host/port/api_token are
    untouched) and, critically, does NOT let the device/entity registry
    simply create new rows under the new identity -- entity.py's own
    dial_id computation feeds *both* the device registry identifier and
    every entity's own unique_id (an entity's is f"{dial_id}_{suffix}"),
    so naively changing entry.unique_id alone would silently orphan
    every existing entity/device (new entity_ids, broken automations/
    dashboards/history) for every current installation. Instead: the
    *existing* device registry row is renamed in place
    (device_registry.async_update_device(new_identifiers=...) -- same
    internal device.id, so area/name_by_user/labels all survive), every
    existing entity registry row is renamed in place
    (entity_registry.async_migrate_entries() -- same internal entity
    UUID, same entity_id), and only then is entry.unique_id itself
    updated -- by which point coordinator.config_entry (the same object
    reference) already reflects the new value for entity.py's own
    dial_id computation when async_forward_entry_setups() runs right
    after this returns.
    """
    if status.device_id is None:
        return
    new_unique_id = f"{STABLE_UNIQUE_ID_PREFIX}{status.device_id}"
    old_unique_id = entry.unique_id
    if old_unique_id is not None and old_unique_id.startswith(STABLE_UNIQUE_ID_PREFIX):
        return  # Already migrated by an earlier setup.

    # config_flow.py always sets a unique_id (the dial's host) before
    # creating an entry, so old_unique_id is None only in a defensive,
    # not actually expected, case -- entity.py's own dial_id computation
    # falls back to entry.entry_id whenever unique_id is None, so that's
    # the identifier every existing device/entity row would actually be
    # using here, and what must be searched for and renamed.
    old_dial_id = old_unique_id if old_unique_id is not None else entry.entry_id

    device_registry = dr.async_get(hass)
    old_devices = device_registry.async_get_devices(
        identifiers={(DOMAIN, old_dial_id)}, config_entry_id=entry.entry_id
    )
    if old_devices:
        device_registry.async_update_device(
            old_devices[0].id, new_identifiers={(DOMAIN, new_unique_id)}
        )

    entity_registry = er.async_get(hass)
    old_prefix = f"{old_dial_id}_"

    def _migrate_entity_unique_id(entity_entry: er.RegistryEntry) -> dict[str, str] | None:
        if not entity_entry.unique_id.startswith(old_prefix):
            return None
        suffix = entity_entry.unique_id[len(old_dial_id) :]  # keeps the leading "_"
        return {"new_unique_id": f"{new_unique_id}{suffix}"}

    await er.async_migrate_entries(hass, entry.entry_id, _migrate_entity_unique_id)

    hass.config_entries.async_update_entry(entry, unique_id=new_unique_id)
    _LOGGER.info(
        "Grohe Dial %s: migrated to a stable, MAC-based identity (was host/IP-based)",
        entry.title,
    )


def _resolve_via_device_id(hass: HomeAssistant, entry: GroheDialConfigEntry) -> str | None:
    """M15.3: finds the ha-grohe_smarthome device (if that integration is
    installed and owns a device for the same Cloud appliance_id this
    dial was provisioned against -- see config_flow.py's
    GroheDialOptionsFlow, which is the only place CONF_GROHE_APPLIANCE_ID
    ever gets set) and returns its Device Registry id for entity.py's own
    DeviceInfo(via_device_id=...).

    Every failure mode returns None, never raises -- a dial provisioned
    outside this integration's Options Flow (scripts/provision.sh, or
    simply not re-run since upgrading to M15.3), ha-grohe_smarthome not
    installed, or no matching device all mean exactly the same thing to
    this dial: stay a fully standalone device, no different from before
    M15.3 existed.

    Uses device_registry.async_get_devices() (searches across every
    config entry, unlike the deprecated single-entry async_get_device()
    or the config-entry-scoped async_get_device_by_identifier()) since
    the ha-grohe_smarthome device this looks for belongs to a
    *different* integration's config entry, never this one's own.
    """
    appliance_id = entry.data.get(CONF_GROHE_APPLIANCE_ID)
    if appliance_id is None:
        return None
    device_registry = dr.async_get(hass)
    matches = device_registry.async_get_devices(
        identifiers={(GROHE_SMARTHOME_DOMAIN, appliance_id)}
    )
    return matches[0].id if matches else None


async def async_setup_entry(hass: HomeAssistant, entry: GroheDialConfigEntry) -> bool:
    session = async_get_clientsession(hass)
    # int(...): defensive re-cast, not just belt-and-braces -- entry.data
    # is whatever config_flow.py's DATA_SCHEMA last stored, and
    # selector.NumberSelector always yields a float regardless of what
    # was typed. config_flow.py itself now normalizes to int before ever
    # persisting (see its own comment), but this also self-heals any
    # config entry that was already created before that fix -- without
    # this, entry.data[CONF_PORT] being e.g. 8080.0 would build the
    # client's base URL as "http://<host>:8080.0", a real hardware
    # acceptance-test failure this fixes (GET /api/status failing
    # forever via ConfigEntryNotReady, never actually reaching the dial).
    client = GroheDialApiClient(
        session, entry.data[CONF_HOST], int(entry.data[CONF_PORT]), entry.data[CONF_API_TOKEN]
    )

    coordinator = GroheDialCoordinator(hass, client)
    try:
        await coordinator.async_config_entry_first_refresh()
    except GroheDialApiError as err:
        raise ConfigEntryNotReady(f"Could not reach the dial: {err}") from err

    # M15.1: only reachable once the dial has genuinely answered above --
    # see this function's own docstring for why that, not HA's formal
    # async_migrate_entry/VERSION mechanism, is what gates this.
    await _async_migrate_to_stable_unique_id(hass, entry, coordinator.data)

    # M15.3: resolved once, here -- if ha-grohe_smarthome's own config
    # entry hasn't finished setting up yet on this same HA restart, its
    # device won't be found this time and the link simply waits for the
    # next reload; see _resolve_via_device_id()'s own comment for why
    # that's an accepted, documented limitation rather than something
    # this integration retries on its own.
    coordinator.via_device_id = _resolve_via_device_id(hass, entry)

    entry.runtime_data = coordinator

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    async_register_services(hass)

    return True


async def async_unload_entry(hass: HomeAssistant, entry: GroheDialConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
