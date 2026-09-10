"""End-to-end control-flow tests: HA service/button -> HA integration
(coordinator, entity platforms) -> real GroheDialApiClient -> real HTTP
-> a fake, stateful firmware double -> entity state back in HA.

This is the layer test_api.py (client vs. a fake HTTP server, no HA) and
test_init.py (entity/device registry shape, one static snapshot) don't
cover on their own: whether a live dispense/stop actually propagates
through the *real* coordinator and *real* entities as the dial's own
state changes over several polls. Never touches real hardware -- the
"dial" here is FakeDial, an in-process aiohttp app simulating exactly
the state machine documented in dial_controller.cpp/dial_state.hpp
(DISPENSING -> delivered_ml counting up -> FINISHED -> auto-revert to
IDLE; DISPENSING -> STOPPING -> IDLE), driven by real polls, not by the
test asserting internal fields directly.

Needs a real localhost socket (a genuine aiohttp TestServer), same as
test_api.py, so opts back into pytest-socket the same way.
"""

from __future__ import annotations

from unittest.mock import patch

import pytest
import voluptuous as vol
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer
from homeassistant.const import STATE_UNAVAILABLE
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.grohe_dial.api import GroheDialConnectionError
from custom_components.grohe_dial.const import CONF_API_TOKEN, DOMAIN

pytestmark = [pytest.mark.asyncio, pytest.mark.enable_socket]

_DIAL_ID = "10.0.0.42"


class FakeDial:
    """Mirrors dial_controller.cpp's own state machine closely enough for
    these tests: DISPENSING advances delivered_ml by a fixed step each
    poll until it reaches amount_ml, then FINISHED, then auto-reverts to
    IDLE on the next poll (Tick()'s own behavior); STOPPING reverts to
    IDLE on the next poll (HandleCommandOutcome()'s own behavior). Each
    GET reports the state *as of before* this poll's own advance -- so a
    transition is only ever observed once it has actually happened
    server-side, never anticipated.
    """

    STEP_ML = 250

    def __init__(self) -> None:
        self.status = "IDLE"
        self.amount_ml = 500
        self.water_type = "SPARKLING"
        self.delivered_ml = 0
        self.dispense_calls = 0
        self.stop_calls = 0

    def _advance(self) -> None:
        if self.status == "DISPENSING":
            self.delivered_ml = min(self.delivered_ml + self.STEP_ML, self.amount_ml)
            if self.delivered_ml >= self.amount_ml:
                self.status = "FINISHED"
        elif self.status == "STOPPING":
            self.status = "IDLE"
        elif self.status == "FINISHED":
            self.status = "IDLE"
            self.delivered_ml = 0

    def snapshot(self) -> dict:
        return {
            "connection_status": "READY",
            "time_status": "AVAILABLE",
            "dispense_status": self.status,
            "water_type": self.water_type,
            "amount_ml": self.amount_ml,
            "active_dispense_amount_ml": self.amount_ml if self.status != "IDLE" else 0,
            "delivered_ml": self.delivered_ml,
            "appliance_response": {"received": True, "success": True, "code": 0},
        }


TOKEN = "test-token"


def _make_app(fake: FakeDial) -> web.Application:
    def _authorized(request: web.Request) -> bool:
        return request.headers.get("X-Api-Token") == TOKEN

    async def handle_status(request: web.Request) -> web.Response:
        if not _authorized(request):
            return web.json_response({}, status=401)
        body = fake.snapshot()
        fake._advance()
        return web.json_response(body)

    async def handle_config_get(request: web.Request) -> web.Response:
        if not _authorized(request):
            return web.json_response({}, status=401)
        return web.json_response(
            {"default_amount_ml": 500, "amount_step_ml": 100, "default_water_type": "SPARKLING"}
        )

    async def handle_dispense(request: web.Request) -> web.Response:
        if not _authorized(request):
            return web.json_response({}, status=401)
        fake.dispense_calls += 1
        body = await request.json()
        amount_ml = body.get("amount_ml")
        water_type = body.get("water_type")
        if not isinstance(amount_ml, int) or not (100 <= amount_ml <= 2000):
            return web.json_response({"status": "rejected", "reason": "invalid_amount"}, status=422)
        if water_type not in ("STILL", "MEDIUM", "SPARKLING"):
            return web.json_response({"status": "rejected", "reason": "invalid_water_type"}, status=422)
        if fake.status != "IDLE":
            return web.json_response({"status": "rejected", "reason": "not_available"}, status=409)
        fake.status = "DISPENSING"
        fake.amount_ml = amount_ml
        fake.water_type = water_type
        fake.delivered_ml = 0
        return web.json_response({"status": "accepted"})

    async def handle_stop(request: web.Request) -> web.Response:
        if not _authorized(request):
            return web.json_response({}, status=401)
        fake.stop_calls += 1
        if fake.status != "DISPENSING":
            return web.json_response({"status": "rejected", "reason": "not_available"}, status=409)
        fake.status = "STOPPING"
        return web.json_response({"status": "accepted"})

    app = web.Application()
    app.router.add_get("/api/status", handle_status)
    app.router.add_get("/api/config", handle_config_get)
    app.router.add_post("/api/dispense", handle_dispense)
    app.router.add_post("/api/stop", handle_stop)
    return app


@pytest.fixture
async def dial_setup(hass: HomeAssistant, enable_custom_integrations):
    """Sets up a real config entry against a real local TestServer
    running FakeDial -- patches only where HA obtains its aiohttp
    session (async_get_clientsession), so __init__.py, the coordinator,
    every entity platform, and GroheDialApiClient itself all run exactly
    as they would against a real dial.
    """
    fake = FakeDial()
    server = TestServer(_make_app(fake))
    test_client = TestClient(server)
    await test_client.start_server()

    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=_DIAL_ID,
        data={"host": test_client.host, "port": test_client.port, CONF_API_TOKEN: TOKEN},
    )
    entry.add_to_hass(hass)

    with patch(
        "custom_components.grohe_dial.async_get_clientsession", return_value=test_client.session
    ):
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
        try:
            yield hass, entry, fake
        finally:
            await hass.config_entries.async_unload(entry.entry_id)
            await hass.async_block_till_done()
    await test_client.close()


def _entity_id(hass: HomeAssistant, domain: str, suffix: str) -> str:
    entity_reg = er.async_get(hass)
    entity_id = entity_reg.async_get_entity_id(domain, DOMAIN, f"{_DIAL_ID}_{suffix}")
    assert entity_id is not None, f"no {domain} entity registered for suffix {suffix!r}"
    return entity_id


async def _press(hass: HomeAssistant, entity_id: str) -> None:
    await hass.services.async_call("button", "press", {"entity_id": entity_id}, blocking=True)
    await hass.async_block_till_done()


async def test_dispense_flow_updates_entities_through_ha(dial_setup) -> None:
    hass, entry, fake = dial_setup
    status_id = _entity_id(hass, "sensor", "dispense_status")
    delivered_id = _entity_id(hass, "sensor", "delivered_ml")
    dispense_button_id = _entity_id(hass, "button", "dispense")

    assert hass.states.get(status_id).state == "idle"

    # Press dispense (uses the coordinator's last-polled amount_ml/water_type,
    # 500ml SPARKLING from the fake's initial snapshot) -> POST /api/dispense
    # -> fake transitions to DISPENSING -> button.py's own refresh (one GET)
    # observes it, delivered_ml still 0 this poll.
    await _press(hass, dispense_button_id)
    assert fake.dispense_calls == 1
    assert hass.states.get(status_id).state == "dispensing"
    assert hass.states.get(delivered_id).state == "0"

    # Next poll: delivered_ml has advanced one step (250/500).
    coordinator = entry.runtime_data
    await coordinator.async_refresh()
    assert hass.states.get(status_id).state == "dispensing"
    assert hass.states.get(delivered_id).state == "250"

    # Next poll: delivered_ml reaches amount_ml -> FINISHED.
    await coordinator.async_refresh()
    assert hass.states.get(status_id).state == "finished"
    assert hass.states.get(delivered_id).state == "500"

    # Next poll: auto-reverts to IDLE (Tick()'s own behavior), delivered_ml
    # resets.
    await coordinator.async_refresh()
    assert hass.states.get(status_id).state == "idle"
    assert hass.states.get(delivered_id).state == "0"


async def test_stop_flow_updates_entities_through_ha(dial_setup) -> None:
    hass, entry, fake = dial_setup
    coordinator = entry.runtime_data
    status_id = _entity_id(hass, "sensor", "dispense_status")
    dispense_button_id = _entity_id(hass, "button", "dispense")
    stop_button_id = _entity_id(hass, "button", "stop")

    await _press(hass, dispense_button_id)
    assert hass.states.get(status_id).state == "dispensing"

    await _press(hass, stop_button_id)
    assert fake.stop_calls == 1
    # button.py's own post-press refresh goes through
    # DataUpdateCoordinator.async_request_refresh(), which is debounced
    # (a short real-time cooldown) -- since it follows the dispense
    # press's own refresh moments earlier in the same test, it may not
    # have actually run yet by this point. Force an immediate, undebounced
    # poll to observe the state deterministically -- exactly what the
    # next scheduled 10s poll would surface in production regardless.
    await coordinator.async_refresh()
    assert hass.states.get(status_id).state == "stopping"

    await coordinator.async_refresh()
    assert hass.states.get(status_id).state == "idle"


async def test_stop_while_idle_is_rejected_without_crashing(dial_setup) -> None:
    hass, entry, fake = dial_setup
    status_id = _entity_id(hass, "sensor", "dispense_status")
    stop_button_id = _entity_id(hass, "button", "stop")

    assert hass.states.get(status_id).state == "idle"
    with pytest.raises(Exception):  # noqa: B017 - see button.py's own comment: not wrapped in HomeAssistantError today
        await _press(hass, stop_button_id)
    assert fake.stop_calls == 1

    # HA itself must survive an uncaught GroheDialCommandRejected from an
    # entity action -- state stays exactly what the dial actually reported,
    # no crash, no stuck/duplicated entity.
    coordinator = entry.runtime_data
    await coordinator.async_refresh()
    assert hass.states.get(status_id).state == "idle"


async def test_dial_unavailable_marks_entities_unavailable_then_recovers(dial_setup) -> None:
    hass, entry, fake = dial_setup
    status_id = _entity_id(hass, "sensor", "dispense_status")
    assert hass.states.get(status_id).state == "idle"

    coordinator = entry.runtime_data
    with patch.object(
        coordinator.client, "get_status", side_effect=GroheDialConnectionError("simulated: dial unreachable")
    ):
        await coordinator.async_refresh()
    assert coordinator.last_update_success is False
    assert hass.states.get(status_id).state == STATE_UNAVAILABLE

    # Recovery: the very next successful poll (dial back on the network)
    # clears the unavailable state without any special-case code -- plain
    # DataUpdateCoordinator/CoordinatorEntity behavior.
    await coordinator.async_refresh()
    assert coordinator.last_update_success is True
    assert hass.states.get(status_id).state == "idle"


async def test_dispense_service_rejects_invalid_amount_before_any_http_call(dial_setup) -> None:
    hass, entry, fake = dial_setup
    dispense_button_id = _entity_id(hass, "button", "dispense")
    # cv.make_entity_service_schema's own vol.Range validation runs before
    # the service handler ever touches the network -- confirmed here by
    # asserting the fake never saw a request at all.
    with pytest.raises(vol.Invalid):
        await hass.services.async_call(
            DOMAIN,
            "dispense",
            {"entity_id": dispense_button_id, "amount_ml": 5, "water_type": "still"},
            blocking=True,
        )
    assert fake.dispense_calls == 0


async def test_dispense_service_rejects_invalid_water_type_before_any_http_call(dial_setup) -> None:
    hass, entry, fake = dial_setup
    dispense_button_id = _entity_id(hass, "button", "dispense")
    with pytest.raises(vol.Invalid):
        await hass.services.async_call(
            DOMAIN,
            "dispense",
            {"entity_id": dispense_button_id, "amount_ml": 500, "water_type": "carbonated"},
            blocking=True,
        )
    assert fake.dispense_calls == 0
