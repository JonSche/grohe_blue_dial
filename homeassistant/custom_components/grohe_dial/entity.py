"""Shared base entity -- device_info construction lives in exactly one
place so every platform's entities agree on it byte-for-byte (a config
entry's own unique_id, i.e. the dial's host -- see config_flow.py's own
comment on why that, not the firmware's true MAC, for this vertical
slice).
"""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import GroheDialCoordinator


class GroheDialEntity(CoordinatorEntity[GroheDialCoordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: GroheDialCoordinator, unique_id_suffix: str) -> None:
        super().__init__(coordinator)
        dial_id = coordinator.config_entry.unique_id or coordinator.config_entry.entry_id
        self._attr_unique_id = f"{dial_id}_{unique_id_suffix}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, dial_id)},
            name=coordinator.config_entry.title,
            manufacturer="Grohe Dial (community project)",
            model="ESP32-C3 Grohe Dial",
        )
