"""Shared base entity -- device_info construction lives in exactly one
place so every platform's entities agree on it byte-for-byte (a config
entry's own unique_id -- M15.1: the dial's stable, MAC-based identity
where the firmware reports one, config_flow.py's host-based fallback
otherwise -- see __init__.py's _async_migrate_to_stable_unique_id() for
how an existing entry moves from the fallback to the stable form in
place, without changing what dial_id resolves to here breaking anything
already in the device/entity registries).

M15.3: via_device_id, when GroheDialCoordinator resolved one (an
installed ha-grohe_smarthome integration owning the same Cloud
appliance_id this dial was provisioned against -- see cloud.py/
config_flow.py's GroheDialOptionsFlow), links this device as a child of
that one in the Device Registry. None whenever it wasn't resolved (no
appliance_id known for this dial, no matching device, or
ha-grohe_smarthome not installed) -- the dial is a fully standalone
device either way; this is a soft, optional enhancement only.
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
        device_info = DeviceInfo(
            identifiers={(DOMAIN, dial_id)},
            name=coordinator.config_entry.title,
            manufacturer="Grohe Dial (community project)",
            model="ESP32-C3 Grohe Dial",
        )
        if coordinator.via_device_id is not None:
            device_info["via_device_id"] = coordinator.via_device_id
        self._attr_device_info = device_info
