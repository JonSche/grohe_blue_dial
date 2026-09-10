"""Binary sensors: exactly the "is it working" signal an automation would
actually gate on -- the detailed connection_status string lives in
sensor.py instead (diagnostic, not automation material). See
docs/m15_ha_integration.md §5.2 for the platform-choice reasoning.
"""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import GroheDialConfigEntry
from .entity import GroheDialEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: GroheDialConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities([GroheDialConnectivitySensor(entry.runtime_data)])


class GroheDialConnectivitySensor(GroheDialEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_translation_key = "ble_connected"

    def __init__(self, coordinator) -> None:  # noqa: ANN001 - see entity.py's own type
        super().__init__(coordinator, "ble_connected")

    @property
    def is_on(self) -> bool:
        return self.coordinator.data.connection_status == "READY"
