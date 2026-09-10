"""M16.1: GroheDialCoordinator's connection-failure backoff.

Tests directly against _async_update_data() -- HA's own scheduler uses
real wall-clock timers (loop.call_at), so a test that actually waited
out the delays would be slow and inherently timing-flaky. What we
actually own and need to verify deterministically is the *value* of
retry_after our code computes for the Nth consecutive
GroheDialConnectionError -- HA's own update_coordinator.py (verified by
reading the real installed source, not from memory) is what turns that
into an actual delayed re-poll; that machinery itself is not ours to
re-test.
"""

from __future__ import annotations

from unittest.mock import AsyncMock

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import UpdateFailed

from custom_components.grohe_dial.api import DialStatus, GroheDialApiError, GroheDialConnectionError
from custom_components.grohe_dial.coordinator import GroheDialCoordinator

pytestmark = pytest.mark.asyncio

_STATUS = DialStatus(
    connection_status="READY",
    time_status="AVAILABLE",
    dispense_status="IDLE",
    water_type="SPARKLING",
    amount_ml=500,
    active_dispense_amount_ml=0,
    delivered_ml=0,
    appliance_response_received=False,
    appliance_response_success=False,
    appliance_response_code=0,
)


def _coordinator(hass: HomeAssistant, *, get_status_side_effect) -> GroheDialCoordinator:
    client = AsyncMock()
    client.get_status = AsyncMock(side_effect=get_status_side_effect)
    return GroheDialCoordinator(hass, client)


async def test_first_failure_retries_after_5s(hass: HomeAssistant, enable_custom_integrations) -> None:
    coordinator = _coordinator(hass, get_status_side_effect=[GroheDialConnectionError("refused")])
    with pytest.raises(UpdateFailed) as exc_info:
        await coordinator._async_update_data()
    assert exc_info.value.retry_after == 5


async def test_second_consecutive_failure_retries_after_10s(hass: HomeAssistant, enable_custom_integrations) -> None:
    coordinator = _coordinator(
        hass, get_status_side_effect=[GroheDialConnectionError("a"), GroheDialConnectionError("b")]
    )
    for _ in range(2):
        with pytest.raises(UpdateFailed) as exc_info:
            await coordinator._async_update_data()
    assert exc_info.value.retry_after == 10


async def test_third_consecutive_failure_retries_after_20s(hass: HomeAssistant, enable_custom_integrations) -> None:
    coordinator = _coordinator(hass, get_status_side_effect=[GroheDialConnectionError("x")] * 3)
    for _ in range(3):
        with pytest.raises(UpdateFailed) as exc_info:
            await coordinator._async_update_data()
    assert exc_info.value.retry_after == 20


async def test_fourth_and_further_failures_cap_at_30s(hass: HomeAssistant, enable_custom_integrations) -> None:
    # 6 consecutive failures -- the 4th, 5th, and 6th must all read 30,
    # proving the cap holds rather than growing (or resetting) further.
    coordinator = _coordinator(hass, get_status_side_effect=[GroheDialConnectionError("x")] * 6)
    retry_afters = []
    for _ in range(6):
        with pytest.raises(UpdateFailed) as exc_info:
            await coordinator._async_update_data()
        retry_afters.append(exc_info.value.retry_after)
    assert retry_afters == [5, 10, 20, 30, 30, 30]


async def test_success_resets_failure_counter(hass: HomeAssistant, enable_custom_integrations) -> None:
    coordinator = _coordinator(
        hass,
        get_status_side_effect=[
            GroheDialConnectionError("a"),
            GroheDialConnectionError("b"),
            _STATUS,  # recovers
        ],
    )
    with pytest.raises(UpdateFailed) as exc_info:
        await coordinator._async_update_data()
    assert exc_info.value.retry_after == 5
    with pytest.raises(UpdateFailed) as exc_info:
        await coordinator._async_update_data()
    assert exc_info.value.retry_after == 10

    status = await coordinator._async_update_data()  # success
    assert status == _STATUS
    assert coordinator._consecutive_connection_failures == 0


async def test_failure_after_recovery_starts_backoff_over(hass: HomeAssistant, enable_custom_integrations) -> None:
    coordinator = _coordinator(
        hass,
        get_status_side_effect=[
            GroheDialConnectionError("a"),
            GroheDialConnectionError("b"),
            GroheDialConnectionError("c"),
            _STATUS,  # recovers -- counter resets
            GroheDialConnectionError("d"),  # must be back at 5s, not 30s
        ],
    )
    for _ in range(3):
        with pytest.raises(UpdateFailed):
            await coordinator._async_update_data()
    await coordinator._async_update_data()  # success, resets counter

    with pytest.raises(UpdateFailed) as exc_info:
        await coordinator._async_update_data()
    assert exc_info.value.retry_after == 5


async def test_non_connection_api_error_keeps_default_cadence(hass: HomeAssistant, enable_custom_integrations) -> None:
    # A malformed response or other non-connection GroheDialApiError is
    # deliberately NOT given a faster retry -- see coordinator.py's own
    # comment for why (it suggests a protocol/data problem, not a
    # network blip).
    coordinator = _coordinator(hass, get_status_side_effect=[GroheDialApiError("malformed")])
    with pytest.raises(UpdateFailed) as exc_info:
        await coordinator._async_update_data()
    assert exc_info.value.retry_after is None
