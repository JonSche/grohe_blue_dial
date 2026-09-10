"""Config flow for the Grohe Dial integration.

Deliberately local-only for this vertical slice: host/port + API token,
validated against the dial's own GET /api/status (proves both
reachability and that the token is accepted) -- no Grohe Cloud login
step. See docs/m15_ha_integration.md §5.4/§9 for why Cloud-login reuse
(Option A, linking to an existing ha-grohe_smarthome config entry) is a
deferred enhancement, not part of this flow.
"""

from __future__ import annotations

from typing import Any

import voluptuous as vol
from homeassistant import config_entries
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.helpers import selector
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import GroheDialApiClient, GroheDialAuthError, GroheDialConnectionError
from .const import CONF_API_TOKEN, DEFAULT_PORT, DOMAIN

DATA_SCHEMA = vol.Schema(
    {
        vol.Required(CONF_HOST): str,
        vol.Required(CONF_PORT, default=DEFAULT_PORT): selector.NumberSelector(
            selector.NumberSelectorConfig(min=1, max=65535, mode=selector.NumberSelectorMode.BOX)
        ),
        vol.Required(CONF_API_TOKEN): str,
    }
)


class GroheDialConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def _async_validate(self, user_input: dict[str, Any]) -> str | None:
        """Returns an error code, or None on success -- same shape as
        every other HA config flow's own validation helper.
        """
        session = async_get_clientsession(self.hass)
        client = GroheDialApiClient(
            session, user_input[CONF_HOST], int(user_input[CONF_PORT]), user_input[CONF_API_TOKEN]
        )
        try:
            await client.get_status()
        except GroheDialAuthError:
            return "invalid_auth"
        except GroheDialConnectionError:
            return "cannot_connect"
        except Exception:  # noqa: BLE001 - anything else really is unexpected here
            return "unknown"
        return None

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> config_entries.ConfigFlowResult:
        errors: dict[str, str] = {}

        if user_input is not None:
            error = await self._async_validate(user_input)
            if error is None:
                # The dial's own stable Wi-Fi-MAC-based device ID isn't
                # exposed over this API yet (see docs/m15_ha_integration.md
                # §9) -- host is used as the unique_id for this vertical
                # slice instead. Known limitation, not an oversight: a
                # dial that later changes IP (DHCP lease change, no
                # reservation) would need re-adding, not silently
                # re-matched. Documented, not hidden.
                await self.async_set_unique_id(user_input[CONF_HOST])
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title=f"Grohe Dial ({user_input[CONF_HOST]})", data=user_input)
            errors["base"] = error

        return self.async_show_form(step_id="user", data_schema=DATA_SCHEMA, errors=errors)

    async def async_step_reauth(self, entry_data: dict[str, Any]) -> config_entries.ConfigFlowResult:
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        errors: dict[str, str] = {}
        reauth_entry = self._get_reauth_entry()

        if user_input is not None:
            merged = {**reauth_entry.data, CONF_API_TOKEN: user_input[CONF_API_TOKEN]}
            error = await self._async_validate(merged)
            if error is None:
                return self.async_update_reload_and_abort(reauth_entry, data=merged)
            errors["base"] = error

        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({vol.Required(CONF_API_TOKEN): str}),
            errors=errors,
        )
