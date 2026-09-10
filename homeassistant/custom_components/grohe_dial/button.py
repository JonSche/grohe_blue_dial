"""button.dispense / button.stop -- one-shot actions, deliberately not
switches (no persisted on/off state to represent -- see
docs/m15_ha_integration.md §5.2). Dispense fires with whatever amount_ml/
water_type the coordinator's last poll of GET /api/status reported --
exactly the "currently dialed in" values the physical encoder itself
would use, not the separate default_amount_ml/default_water_type config
entities (see number.py/select.py) -- pressing the physical dial and
pressing this button dispense the same amount for the same reason. For
scripted use with an explicit amount/water_type per call, see
services.py's grohe_dial.dispense service instead.
"""

from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import GroheDialConfigEntry
from .api import GroheDialApiError
from .coordinator import GroheDialCoordinator
from .entity import GroheDialEntity
from .errors import raise_as_home_assistant_error


async def async_setup_entry(
    hass: HomeAssistant, entry: GroheDialConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities([DispenseButton(coordinator), StopButton(coordinator)])


class DispenseButton(GroheDialEntity, ButtonEntity):
    _attr_translation_key = "dispense"

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "dispense")

    async def async_press(self) -> None:
        status = self.coordinator.data
        try:
            await self.coordinator.client.dispense(status.amount_ml, status.water_type)
        except GroheDialApiError as err:
            # M16.2: was an uncaught, generic "Unknown error" before --
            # see errors.py's own header comment.
            raise_as_home_assistant_error(err)
        await self.coordinator.async_request_refresh()


class StopButton(GroheDialEntity, ButtonEntity):
    _attr_translation_key = "stop"

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "stop")

    async def async_press(self) -> None:
        try:
            await self.coordinator.client.stop()
        except GroheDialApiError as err:
            raise_as_home_assistant_error(err)
        await self.coordinator.async_request_refresh()
