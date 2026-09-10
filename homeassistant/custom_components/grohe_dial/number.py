"""number.default_amount_ml / number.amount_step_ml -- write through
POST /api/config, mirroring the pre-M15 MQTT Discovery entities of the
same purpose exactly. See select.py's own comment for why these track
their own value locally instead of reading the coordinator's polled
data (config isn't part of GET /api/status).
"""

from __future__ import annotations

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.const import UnitOfVolume
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import GroheDialConfigEntry
from .const import MAX_AMOUNT_ML, MIN_AMOUNT_ML
from .coordinator import GroheDialCoordinator
from .entity import GroheDialEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: GroheDialConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities([DefaultAmountNumber(coordinator), AmountStepNumber(coordinator)])


class _ConfigNumberEntity(GroheDialEntity, NumberEntity):
    _attr_entity_category = EntityCategory.CONFIG
    _attr_mode = NumberMode.BOX
    _attr_native_unit_of_measurement = UnitOfVolume.MILLILITERS

    def __init__(self, coordinator: GroheDialCoordinator, suffix: str) -> None:
        super().__init__(coordinator, suffix)
        self._current: float | None = None

    @property
    def native_value(self) -> float | None:
        return self._current

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        await self._async_refresh()
        self.async_write_ha_state()


class DefaultAmountNumber(_ConfigNumberEntity):
    _attr_translation_key = "default_amount_ml"
    _attr_native_min_value = MIN_AMOUNT_ML
    _attr_native_max_value = MAX_AMOUNT_ML
    _attr_native_step = 10

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "default_amount_ml")

    async def _async_refresh(self) -> None:
        config = await self.coordinator.client.get_config()
        self._current = config.default_amount_ml

    async def async_set_native_value(self, value: float) -> None:
        await self.coordinator.client.set_config(default_amount_ml=int(value))
        self._current = value
        self.async_write_ha_state()


class AmountStepNumber(_ConfigNumberEntity):
    _attr_translation_key = "amount_step_ml"
    _attr_native_min_value = 10
    _attr_native_max_value = MAX_AMOUNT_ML
    _attr_native_step = 10

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "amount_step_ml")

    async def _async_refresh(self) -> None:
        config = await self.coordinator.client.get_config()
        self._current = config.amount_step_ml

    async def async_set_native_value(self, value: float) -> None:
        await self.coordinator.client.set_config(amount_step_ml=int(value))
        self._current = value
        self.async_write_ha_state()
