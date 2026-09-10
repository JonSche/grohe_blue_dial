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
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import GroheDialApiClient, GroheDialApiError
from .const import CONF_API_TOKEN, DOMAIN
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

    entry.runtime_data = coordinator

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    async_register_services(hass)

    return True


async def async_unload_entry(hass: HomeAssistant, entry: GroheDialConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
