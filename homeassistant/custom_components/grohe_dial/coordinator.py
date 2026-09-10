"""Polling coordinator for a single Grohe Dial.

Standard HA DataUpdateCoordinator pattern (the same shape
ha-grohe_smarthome's own coordinators use, per
docs/m15_ha_integration_analysis.md's verified source read) -- polls
GET /api/status on a fixed interval; every entity in this integration
reads from the coordinator's own .data, never calls the API client
directly for state (only for commands -- see button.py/services.py).
"""

from __future__ import annotations

import logging
from datetime import timedelta

from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import DialStatus, GroheDialApiClient, GroheDialApiError, GroheDialAuthError
from .const import DEFAULT_SCAN_INTERVAL_SECONDS, DOMAIN

_LOGGER = logging.getLogger(__name__)


class GroheDialCoordinator(DataUpdateCoordinator[DialStatus]):
    def __init__(self, hass: HomeAssistant, client: GroheDialApiClient) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=DOMAIN,
            update_interval=timedelta(seconds=DEFAULT_SCAN_INTERVAL_SECONDS),
        )
        self.client = client

    async def _async_update_data(self) -> DialStatus:
        try:
            return await self.client.get_status()
        except GroheDialAuthError as err:
            # ConfigEntryAuthFailed triggers HA's own reauth flow
            # (config_flow.py's async_step_reauth) -- imported locally to
            # avoid a config_entries<->coordinator import cycle, matching
            # HA's own documented pattern for this exact exception.
            from homeassistant.exceptions import ConfigEntryAuthFailed

            raise ConfigEntryAuthFailed(str(err)) from err
        except GroheDialApiError as err:
            raise UpdateFailed(str(err)) from err
