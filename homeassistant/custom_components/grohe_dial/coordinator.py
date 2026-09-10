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

from .api import DialStatus, GroheDialApiClient, GroheDialApiError, GroheDialAuthError, GroheDialConnectionError
from .const import DEFAULT_SCAN_INTERVAL_SECONDS, DOMAIN

_LOGGER = logging.getLogger(__name__)

# M16.1: bounded, escalating retry delay for *transient* connection
# failures only (GroheDialConnectionError -- timeout/DNS/refused, not a
# protocol-level problem) -- mirrors ble_manager.cpp's own
# kBackoffDelaysMs[] scheme (M11.1) for the same reason: fast enough to
# recover quickly from a real blip, capped so a genuinely offline dial
# is never hammered faster than a sensible minimum. Fed to HA's own
# native UpdateFailed(retry_after=...) mechanism (see
# _async_update_data() below) -- no custom timer/scheduler of our own.
_CONNECTION_RETRY_BACKOFF_SECONDS = (5, 10, 20, 30)


class GroheDialCoordinator(DataUpdateCoordinator[DialStatus]):
    def __init__(self, hass: HomeAssistant, client: GroheDialApiClient) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=DOMAIN,
            update_interval=timedelta(seconds=DEFAULT_SCAN_INTERVAL_SECONDS),
        )
        self.client = client
        self._consecutive_connection_failures = 0

    async def _async_update_data(self) -> DialStatus:
        try:
            status = await self.client.get_status()
        except GroheDialAuthError as err:
            # ConfigEntryAuthFailed triggers HA's own reauth flow
            # (config_flow.py's async_step_reauth) -- imported locally to
            # avoid a config_entries<->coordinator import cycle, matching
            # HA's own documented pattern for this exact exception.
            from homeassistant.exceptions import ConfigEntryAuthFailed

            raise ConfigEntryAuthFailed(str(err)) from err
        except GroheDialConnectionError as err:
            # Must be caught before the generic GroheDialApiError below --
            # it's a subclass, and Python tries except clauses in order.
            index = min(
                self._consecutive_connection_failures,
                len(_CONNECTION_RETRY_BACKOFF_SECONDS) - 1,
            )
            retry_after = _CONNECTION_RETRY_BACKOFF_SECONDS[index]
            self._consecutive_connection_failures += 1
            raise UpdateFailed(str(err), retry_after=retry_after) from err
        except GroheDialApiError as err:
            # Anything else (e.g. a malformed response) is left at HA's
            # own default polling cadence -- unlike a connection error,
            # this suggests a genuine protocol/data problem, not a
            # network blip a faster retry would actually help with.
            raise UpdateFailed(str(err)) from err
        else:
            self._consecutive_connection_failures = 0
            return status
