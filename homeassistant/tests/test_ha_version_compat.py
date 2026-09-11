"""Regression test for a real bug found deploying this integration to a
real, currently-used Home Assistant instance (2026.4.1) -- much older
than the pinned test dependency this suite otherwise runs against.
`DeviceRegistry.async_get_devices()`, which __init__.py's own
_resolve_via_device_id()/_async_migrate_to_stable_unique_id() were
originally written against, does not exist at all on that version --
only the older `async_get_device()` (no config_entry_id scoping).
`_find_device_by_identifier()`'s own hasattr()-based branch is what
this file exercises directly, against plain fakes (not a real
DeviceRegistry) so both HA API shapes can be simulated regardless of
which one this suite's own pinned HA test dependency actually has.
"""

from __future__ import annotations

import pytest

from custom_components.grohe_dial import _find_device_by_identifier


class _FakeDeviceEntry:
    def __init__(self, device_id: str) -> None:
        self.id = device_id


class _NewStyleRegistry:
    """Mirrors a current HA DeviceRegistry: async_get_devices() exists,
    supports config_entry_id scoping."""

    def __init__(self, devices: dict[tuple[str, tuple[str, str]], _FakeDeviceEntry]) -> None:
        self._devices = devices

    def async_get_devices(self, *, identifiers=None, connections=None, config_entry_id=None):
        (identifier,) = identifiers
        entry = self._devices.get((config_entry_id, identifier))
        return [entry] if entry is not None else []


class _OldStyleRegistry:
    """Mirrors HA 2026.4.1's DeviceRegistry: only async_get_device()
    exists -- no async_get_devices, no config_entry_id scoping at all."""

    def __init__(self, devices: dict[tuple[str, str], _FakeDeviceEntry]) -> None:
        self._devices = devices

    def async_get_device(self, identifiers=None, connections=None):
        (identifier,) = identifiers
        return self._devices.get(identifier)


def test_new_style_registry_finds_scoped_match() -> None:
    device = _FakeDeviceEntry("device-1")
    registry = _NewStyleRegistry({("entry-1", ("domain", "id-1")): device})
    assert _find_device_by_identifier(registry, ("domain", "id-1"), config_entry_id="entry-1") is device


def test_new_style_registry_returns_none_without_match() -> None:
    registry = _NewStyleRegistry({})
    assert _find_device_by_identifier(registry, ("domain", "id-1"), config_entry_id="entry-1") is None


def test_old_style_registry_finds_match_without_async_get_devices() -> None:
    """The exact real-world case that broke on HA 2026.4.1: a registry
    object with no async_get_devices attribute at all must still work,
    via the async_get_device() fallback."""
    assert not hasattr(_OldStyleRegistry, "async_get_devices")
    device = _FakeDeviceEntry("device-1")
    registry = _OldStyleRegistry({("domain", "id-1"): device})
    assert _find_device_by_identifier(registry, ("domain", "id-1"), config_entry_id="entry-1") is device


def test_old_style_registry_returns_none_without_match() -> None:
    registry = _OldStyleRegistry({})
    assert _find_device_by_identifier(registry, ("domain", "id-1")) is None
