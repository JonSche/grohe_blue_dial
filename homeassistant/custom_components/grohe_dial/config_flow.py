"""Config flow for the Grohe Dial integration.

The main (config entry creation) flow stays deliberately local-only:
host/port + API token, validated against the dial's own GET /api/status
(proves both reachability and that the token is accepted) -- no Grohe
Cloud login here. An already-provisioned dial must stay addable this
way with no forced detour through a Cloud login.

M16 adds a *separate* Options Flow (GroheDialOptionsFlow, below) for
provisioning Grohe Blue Home credentials onto a dial that doesn't have
them yet -- reachable from the device's own "Configure" action once
it's already added. Reuses the `grohe` PyPI package (cloud.py, no
Grohe Cloud logic reimplemented -- see that module's own header
comment) and the dial's existing POST /provision endpoint
(components/provisioning/provisioning_server.cpp, M13.2, completely
unchanged by M16 -- no firmware code needed for provisioning at all).
"""

from __future__ import annotations

from typing import Any

import voluptuous as vol
from homeassistant import config_entries
from homeassistant.const import CONF_EMAIL, CONF_HOST, CONF_PASSWORD, CONF_PORT
from homeassistant.helpers import selector
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from . import cloud
from .api import (
    GroheDialApiError,
    GroheDialApiClient,
    GroheDialAuthError,
    GroheDialConnectionError,
    provision_dial,
)
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

CONF_PROVISION_TOKEN = "provision_token"


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
            # selector.NumberSelector always yields a float, regardless
            # of what was typed -- normalized to int here, before
            # anything persists it, not just for _async_validate()'s own
            # call below. A previous version only cast locally inside
            # _async_validate(), which validated successfully and then
            # went on to store the *original* float via
            # async_create_entry(data=user_input) further down -- a real
            # bug found on real hardware: the client's base URL ended up
            # as "http://<host>:8080.0", a connection failure that
            # retried forever since the stored value never changed. See
            # __init__.py's own defensive re-cast for entries already
            # created before this fix.
            user_input[CONF_PORT] = int(user_input[CONF_PORT])
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
            # Same normalization as async_step_user() above -- reauth_entry.data
            # may itself still carry a float port from an entry created
            # before this fix; this also self-heals it going forward.
            merged[CONF_PORT] = int(merged[CONF_PORT])
            error = await self._async_validate(merged)
            if error is None:
                return self.async_update_reload_and_abort(reauth_entry, data=merged)
            errors["base"] = error

        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({vol.Required(CONF_API_TOKEN): str}),
            errors=errors,
        )

    @staticmethod
    @config_entries.callback
    def async_get_options_flow(config_entry: config_entries.ConfigEntry) -> GroheDialOptionsFlow:
        return GroheDialOptionsFlow()


class GroheDialOptionsFlow(config_entries.OptionsFlow):
    """M16: provisions Grohe Blue Home credentials onto this dial via its
    existing POST /provision endpoint -- see this module's own header
    comment. Not a settings form (no config entry data is actually
    changed by this flow -- async_create_entry(data={}) at the end is
    HA's own documented way for an options flow to just "do a thing"
    without persisting new options): every step's own state
    (self._tokens/_candidates/_selected) lives only for the duration of
    one flow run, never persisted beyond what's needed to reach the
    final POST /provision call.
    """

    def __init__(self) -> None:
        self._tokens: cloud.GroheCloudTokens | None = None
        self._candidates: list[cloud.GroheApplianceCandidate] = []
        self._selected: cloud.GroheApplianceCandidate | None = None

    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> config_entries.ConfigFlowResult:
        return await self.async_step_cloud_login()

    async def async_step_cloud_login(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            # `password` lives only in this local variable and the one
            # cloud.login_with_credentials() call below -- never logged,
            # never stored (see cloud.py's own comment on
            # login_with_credentials()). Only the resulting refresh_token
            # (self._tokens.refresh_token) is held, and only in memory
            # for the rest of this one flow run -- this flow doesn't
            # persist it anywhere either; a future flow run logs in
            # again. Minimal secret surface, matching this milestone's
            # own explicit security requirements.
            try:
                self._tokens = await cloud.login_with_credentials(
                    user_input[CONF_EMAIL], user_input[CONF_PASSWORD]
                )
            except cloud.GroheCloudAuthError:
                errors["base"] = "invalid_cloud_auth"
            except cloud.GroheCloudConnectionError:
                errors["base"] = "cloud_unavailable"
            except cloud.GroheCloudError:
                errors["base"] = "unknown"
            else:
                return await self.async_step_select_appliance()

        return self.async_show_form(
            step_id="cloud_login",
            data_schema=vol.Schema({vol.Required(CONF_EMAIL): str, vol.Required(CONF_PASSWORD): str}),
            errors=errors,
        )

    async def async_step_select_appliance(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        assert self._tokens is not None  # only reachable after cloud_login succeeds

        if not self._candidates:
            try:
                self._candidates = await cloud.list_appliances(self._tokens.access_token)
            except cloud.GroheCloudAuthError:
                return self.async_abort(reason="invalid_cloud_auth")
            except cloud.GroheCloudConnectionError:
                return self.async_abort(reason="cloud_unavailable")
            except cloud.GroheCloudError:
                return self.async_abort(reason="unknown")

            if not self._candidates:
                return self.async_abort(reason="no_appliances_found")
            if len(self._candidates) == 1:
                # Exactly one Grohe Blue in this account -- matches
                # scripts/grohe_cloud_refresh.py's own single-appliance
                # assumption, just auto-selected instead of erroring;
                # see GroheApplianceCandidate's own comment on why a
                # picker (below) exists at all for the >1 case, which
                # that CLI script can't offer.
                self._selected = self._candidates[0]
                return await self.async_step_provision_token()

        if user_input is not None:
            selected_id = user_input["appliance"]
            self._selected = next(c for c in self._candidates if c.appliance_id == selected_id)
            return await self.async_step_provision_token()

        options = {c.appliance_id: c.name for c in self._candidates}
        return self.async_show_form(
            step_id="select_appliance",
            data_schema=vol.Schema({vol.Required("appliance"): vol.In(options)}),
        )

    async def async_step_provision_token(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        assert self._tokens is not None
        assert self._selected is not None
        errors: dict[str, str] = {}

        if user_input is not None:
            try:
                user_id = cloud.user_id_from_access_token(self._tokens.access_token)
            except cloud.GroheCloudError:
                return self.async_abort(reason="unknown")

            session = async_get_clientsession(self.hass)
            try:
                await provision_dial(
                    session,
                    self.config_entry.data[CONF_HOST],
                    int(self.config_entry.data[CONF_PORT]),
                    user_input[CONF_PROVISION_TOKEN],
                    user_id,
                    self._selected.preshared_key_base64,
                )
            except GroheDialAuthError:
                errors["base"] = "invalid_provision_token"
            except GroheDialConnectionError:
                errors["base"] = "dial_unreachable"
            except GroheDialApiError:
                errors["base"] = "provisioning_failed"
            else:
                # No config entry *data* actually changes -- provisioning
                # writes to the dial's own NVS, not to anything HA
                # stores. title="" is HA's own documented convention for
                # an options flow that performs an action rather than
                # editing settings.
                return self.async_create_entry(title="", data={})

        return self.async_show_form(
            step_id="provision_token",
            data_schema=vol.Schema({vol.Required(CONF_PROVISION_TOKEN): str}),
            errors=errors,
        )
