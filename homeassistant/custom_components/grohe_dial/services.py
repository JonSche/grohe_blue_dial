"""grohe_dial.dispense / grohe_dial.stop -- entity-targeted services (HA's
own standard pattern for "an action against one of possibly several
configured devices/entries" -- see cv.make_entity_service_schema()).
Target any entity belonging to the dial you mean (any of this
integration's own entities resolves to the same device); the target's
own config entry's coordinator/client is what the command is actually
sent through, exactly the same call button.py's DispenseButton makes,
just parameterized (amount_ml/water_type) instead of reusing the
dial's currently-dialed-in values -- see button.py's own comment for why
both exist.
"""

from __future__ import annotations

import voluptuous as vol
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import config_validation as cv, entity_registry as er

from .const import ATTR_AMOUNT_ML, ATTR_WATER_TYPE, DOMAIN, MAX_AMOUNT_ML, MIN_AMOUNT_ML, SERVICE_DISPENSE, SERVICE_STOP, WATER_TYPES

DISPENSE_SCHEMA = cv.make_entity_service_schema(
    {
        vol.Required(ATTR_AMOUNT_ML): vol.All(vol.Coerce(int), vol.Range(min=MIN_AMOUNT_ML, max=MAX_AMOUNT_ML)),
        vol.Required(ATTR_WATER_TYPE): vol.In([w.lower() for w in WATER_TYPES]),
    }
)
STOP_SCHEMA = cv.make_entity_service_schema({})


def _coordinator_for_entity(hass: HomeAssistant, entity_id: str):
    entity_reg = er.async_get(hass)
    entry = entity_reg.async_get(entity_id)
    if entry is None or entry.platform != DOMAIN or entry.config_entry_id is None:
        raise HomeAssistantError(f"{entity_id} is not a Grohe Dial entity")
    config_entry = hass.config_entries.async_get_entry(entry.config_entry_id)
    if config_entry is None:
        raise HomeAssistantError(f"{entity_id}'s config entry is no longer loaded")
    return config_entry.runtime_data


async def _async_handle_dispense(hass: HomeAssistant, call: ServiceCall) -> None:
    for entity_id in call.data[cv.ATTR_ENTITY_ID]:
        coordinator = _coordinator_for_entity(hass, entity_id)
        await coordinator.client.dispense(call.data[ATTR_AMOUNT_ML], call.data[ATTR_WATER_TYPE].upper())
        await coordinator.async_request_refresh()


async def _async_handle_stop(hass: HomeAssistant, call: ServiceCall) -> None:
    for entity_id in call.data[cv.ATTR_ENTITY_ID]:
        coordinator = _coordinator_for_entity(hass, entity_id)
        await coordinator.client.stop()
        await coordinator.async_request_refresh()


def async_register_services(hass: HomeAssistant) -> None:
    if hass.services.has_service(DOMAIN, SERVICE_DISPENSE):
        return  # Already registered by an earlier config entry's setup.

    hass.services.async_register(
        DOMAIN, SERVICE_DISPENSE, lambda call: _async_handle_dispense(hass, call), schema=DISPENSE_SCHEMA
    )
    hass.services.async_register(
        DOMAIN, SERVICE_STOP, lambda call: _async_handle_stop(hass, call), schema=STOP_SCHEMA
    )
