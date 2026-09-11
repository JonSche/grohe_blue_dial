"""Shared base entity -- device_info construction lives in exactly one
place so every platform's entities agree on it byte-for-byte (a config
entry's own unique_id -- M15.1: the dial's stable, MAC-based identity
where the firmware reports one, config_flow.py's host-based fallback
otherwise -- see __init__.py's _async_migrate_to_stable_unique_id() for
how an existing entry moves from the fallback to the stable form in
place, without changing what dial_id resolves to here breaking anything
already in the device/entity registries).

M15.3: links this device as a child of a matching ha-grohe_smarthome
device in the Device Registry, when GroheDialCoordinator resolved one
(that integration installed and owning a device for the same Cloud
appliance_id this dial was provisioned against -- see cloud.py/
config_flow.py's GroheDialOptionsFlow). Both coordinator.via_device_id/
via_device_identifier are None together whenever it wasn't resolved (no
appliance_id known for this dial, no matching device, or
ha-grohe_smarthome not installed) -- the dial is a fully standalone
device either way; this is a soft, optional enhancement only.

Which DeviceInfo key actually gets used (`via_device_id`, an internal
Device Registry id, vs. the older `via_device`, a (domain, identifier)
tuple) is decided once, at import time, by checking which one this
specific, running Home Assistant version's own DeviceInfo TypedDict
actually declares -- found the hard way, deploying to a real Home
Assistant instance: `via_device_id` (this integration's own first
attempt, matching a newer, separately-installed HA test dependency)
does not exist at all on HA 2026.4.1, which still only has `via_device`
-- and a future HA version could just as easily drop `via_device`
entirely the way it already dropped it from some newer test
environments. Never assume one specific HA version's own DeviceInfo
shape; check what's actually there.
"""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import GroheDialCoordinator

_SUPPORTS_VIA_DEVICE_ID = "via_device_id" in DeviceInfo.__annotations__


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
        if _SUPPORTS_VIA_DEVICE_ID:
            if coordinator.via_device_id is not None:
                device_info["via_device_id"] = coordinator.via_device_id
        elif coordinator.via_device_identifier is not None:
            device_info["via_device"] = coordinator.via_device_identifier
        self._attr_device_info = device_info
