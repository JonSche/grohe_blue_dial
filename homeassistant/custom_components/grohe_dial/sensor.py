"""Diagnostic/state sensors -- dispense_status as an enum sensor
(state, not an action -- correctly modeled as a sensor per
docs/m15_ha_integration.md §5.2), the detailed BLE connection_status
string (diagnostic, complementing binary_sensor.py's plain on/off), and
delivered_ml (live count-up during a dispense, mirrors the physical
dial's own UI exactly -- see dial_state.hpp's own comment on that
field).
"""

from __future__ import annotations

from homeassistant.components.sensor import SensorEntity
from homeassistant.const import UnitOfVolume
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import GroheDialConfigEntry
from .coordinator import GroheDialCoordinator
from .entity import GroheDialEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: GroheDialConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(
        [
            DispenseStatusSensor(coordinator),
            ConnectionStatusSensor(coordinator),
            DeliveredAmountSensor(coordinator),
        ]
    )


class DispenseStatusSensor(GroheDialEntity, SensorEntity):
    _attr_translation_key = "dispense_status"
    _attr_device_class = "enum"
    _attr_options = ["idle", "dispensing", "stopping", "finished", "failed"]

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "dispense_status")

    @property
    def native_value(self) -> str:
        return self.coordinator.data.dispense_status.lower()


class ConnectionStatusSensor(GroheDialEntity, SensorEntity):
    _attr_translation_key = "connection_status"
    _attr_device_class = "enum"
    _attr_options = ["connecting", "ready", "connection_lost"]
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "connection_status")

    @property
    def native_value(self) -> str:
        return self.coordinator.data.connection_status.lower()


class DeliveredAmountSensor(GroheDialEntity, SensorEntity):
    # Deliberately no device_class/state_class: this resets to 0 at the
    # start of every dispense (dial_state.hpp's own comment on
    # delivered_ml) -- a live per-pour progress counter, not a
    # monotonically increasing meter reading. HA's "volume" device_class
    # only accepts state_class total/total_increasing (validated at
    # entity-add time -- caught by this integration's own test suite,
    # tests/test_init.py), neither of which describes a value that
    # resets; a plain numeric sensor with a unit is the honest fit.
    _attr_translation_key = "delivered_ml"
    _attr_native_unit_of_measurement = UnitOfVolume.MILLILITERS

    def __init__(self, coordinator: GroheDialCoordinator) -> None:
        super().__init__(coordinator, "delivered_ml")

    @property
    def native_value(self) -> int:
        return self.coordinator.data.delivered_ml
