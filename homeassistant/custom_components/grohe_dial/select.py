"""select.default_water_type -- writes through POST /api/config, mirroring
the pre-M15 MQTT Discovery entity of the same purpose exactly (a fixed
small enum the user picks from -- correct `select` semantics, see
docs/m15_ha_integration.md §5.2).

Config (default_amount_ml/amount_step_ml/default_water_type) is not part
of the coordinator's own polled GET /api/status -- it doesn't change on
its own, only in response to this integration's own writes -- so this
entity fetches it once on setup and otherwise tracks its own last-known
value locally, updated optimistically on every successful
async_select_option() (the firmware's own POST /api/config is
synchronous and would have already failed loudly, raising, if the write
didn't actually take -- see api.py's GroheDialCommandRejected).
"""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import GroheDialConfigEntry
from .const import WATER_TYPES
from .coordinator import GroheDialCoordinator
from .entity import GroheDialEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: GroheDialConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities([DefaultWaterTypeSelect(entry.runtime_data)])


class DefaultWaterTypeSelect(GroheDialEntity, SelectEntity):
    _attr_translation_key = "default_water_type"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = [w.lower() for w in WATER_TYPES]

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "default_water_type")
        self._current: str | None = None

    @property
    def current_option(self) -> str | None:
        return self._current

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        config = await self.coordinator.client.get_config()
        self._current = config.default_water_type.lower()
        self.async_write_ha_state()

    async def async_select_option(self, option: str) -> None:
        await self.coordinator.client.set_config(default_water_type=option.upper())
        self._current = option
        self.async_write_ha_state()
