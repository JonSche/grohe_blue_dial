"""Tests for GroheDialApiClient against a minimal fake of the firmware's
own local HTTP API (a real aiohttp server, not a mock of aiohttp itself
-- exercises the real request/response/JSON-parsing path). Does not
depend on Home Assistant at all -- api.py itself has no HA import, only
aiohttp -- so these run with a plain aiohttp + pytest-asyncio
environment, unlike config_flow.py's own tests (see test_config_flow.py's
own header comment for why those need the full HA test harness this
environment does not have installed).
"""

from __future__ import annotations

import pytest
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer

# pytest-homeassistant-custom-component's own plugin (pulled in by
# test_config_flow.py's needs) blocks real socket use test-session-wide by
# default (pytest-socket) -- these tests need a real localhost socket
# (a genuine aiohttp TestServer, not a mock of aiohttp itself), so every
# test in this module opts back in explicitly.
pytestmark = pytest.mark.enable_socket

# conftest.py loads grohe_dial.api/grohe_dial.const without going through
# grohe_dial/__init__.py (which needs Home Assistant, unavailable here).
from grohe_dial.api import (
    DialConfig,
    DialStatus,
    GroheDialApiClient,
    GroheDialApiError,
    GroheDialAuthError,
    GroheDialCommandRejected,
    GroheDialConnectionError,
)

TOKEN = "test-token"
WRONG_TOKEN = "wrong-token"


def _require_auth(request: web.Request) -> web.Response | None:
    if request.headers.get("X-Api-Token") != TOKEN:
        return web.json_response({"error": "unauthorized"}, status=401)
    return None


async def _handle_status(request: web.Request) -> web.Response:
    if (unauthorized := _require_auth(request)) is not None:
        return unauthorized
    return web.json_response(
        {
            "connection_status": "READY",
            "time_status": "AVAILABLE",
            "dispense_status": "IDLE",
            "water_type": "SPARKLING",
            "amount_ml": 500,
            "active_dispense_amount_ml": 0,
            "delivered_ml": 0,
            "appliance_response": {"received": True, "success": True, "code": 0},
        }
    )


async def _handle_config_get(request: web.Request) -> web.Response:
    if (unauthorized := _require_auth(request)) is not None:
        return unauthorized
    return web.json_response({"default_amount_ml": 500, "amount_step_ml": 100, "default_water_type": "SPARKLING"})


async def _handle_config_post(request: web.Request) -> web.Response:
    if (unauthorized := _require_auth(request)) is not None:
        return unauthorized
    body = await request.json()
    if "amount_step_ml" in body and body["amount_step_ml"] <= 0:
        return web.json_response({"status": "rejected", "reason": "invalid_amount_step_ml"}, status=422)
    return web.json_response({"status": "ok"})


async def _handle_dispense(request: web.Request) -> web.Response:
    if (unauthorized := _require_auth(request)) is not None:
        return unauthorized
    body = await request.json()
    if not (100 <= body.get("amount_ml", 0) <= 2000):
        return web.json_response({"status": "rejected", "reason": "invalid_amount"}, status=422)
    if body.get("water_type") not in ("STILL", "MEDIUM", "SPARKLING"):
        return web.json_response({"status": "rejected", "reason": "invalid_water_type"}, status=422)
    return web.json_response({"status": "accepted"})


async def _handle_stop(request: web.Request) -> web.Response:
    if (unauthorized := _require_auth(request)) is not None:
        return unauthorized
    return web.json_response({"status": "rejected", "reason": "not_available"}, status=409)


def _make_app() -> web.Application:
    app = web.Application()
    app.router.add_get("/api/status", _handle_status)
    app.router.add_get("/api/config", _handle_config_get)
    app.router.add_post("/api/config", _handle_config_post)
    app.router.add_post("/api/dispense", _handle_dispense)
    app.router.add_post("/api/stop", _handle_stop)
    return app


@pytest.fixture
async def client():
    server = TestServer(_make_app())
    test_client = TestClient(server)
    await test_client.start_server()
    api = GroheDialApiClient(test_client.session, test_client.host, test_client.port, TOKEN)
    yield api
    await test_client.close()


@pytest.mark.asyncio
async def test_get_status(client: GroheDialApiClient) -> None:
    status = await client.get_status()
    assert status == DialStatus(
        connection_status="READY",
        time_status="AVAILABLE",
        dispense_status="IDLE",
        water_type="SPARKLING",
        amount_ml=500,
        active_dispense_amount_ml=0,
        delivered_ml=0,
        appliance_response_received=True,
        appliance_response_success=True,
        appliance_response_code=0,
    )


@pytest.mark.asyncio
async def test_get_config(client: GroheDialApiClient) -> None:
    config = await client.get_config()
    assert config == DialConfig(default_amount_ml=500, amount_step_ml=100, default_water_type="SPARKLING")


@pytest.mark.asyncio
async def test_set_config_success(client: GroheDialApiClient) -> None:
    await client.set_config(amount_step_ml=50)  # must not raise


@pytest.mark.asyncio
async def test_set_config_rejected(client: GroheDialApiClient) -> None:
    with pytest.raises(GroheDialCommandRejected) as exc_info:
        await client.set_config(amount_step_ml=-1)
    assert exc_info.value.reason == "invalid_amount_step_ml"
    assert exc_info.value.status_code == 422


@pytest.mark.asyncio
async def test_dispense_accepted(client: GroheDialApiClient) -> None:
    await client.dispense(500, "SPARKLING")  # must not raise


@pytest.mark.asyncio
async def test_dispense_invalid_amount(client: GroheDialApiClient) -> None:
    with pytest.raises(GroheDialCommandRejected) as exc_info:
        await client.dispense(99999, "SPARKLING")
    assert exc_info.value.reason == "invalid_amount"


@pytest.mark.asyncio
async def test_dispense_invalid_water_type(client: GroheDialApiClient) -> None:
    with pytest.raises(GroheDialCommandRejected) as exc_info:
        await client.dispense(500, "CARBONATED")
    assert exc_info.value.reason == "invalid_water_type"


@pytest.mark.asyncio
async def test_stop_not_available(client: GroheDialApiClient) -> None:
    with pytest.raises(GroheDialCommandRejected) as exc_info:
        await client.stop()
    assert exc_info.value.reason == "not_available"
    assert exc_info.value.status_code == 409


@pytest.mark.asyncio
async def test_invalid_token_raises_auth_error() -> None:
    server = TestServer(_make_app())
    test_client = TestClient(server)
    await test_client.start_server()
    try:
        api = GroheDialApiClient(test_client.session, test_client.host, test_client.port, WRONG_TOKEN)
        with pytest.raises(GroheDialAuthError):
            await api.get_status()
    finally:
        await test_client.close()


async def _handle_status_truncated(request: web.Request) -> web.Response:
    # HTTP 200, valid JSON, but missing every field DialStatus.from_json()
    # requires -- simulates a truncated/malformed-but-still-200 response.
    # Regression test for a real bug found during the M15 final review:
    # DialStatus.from_json()'s direct dict indexing let a raw KeyError
    # escape GroheDialApiClient entirely (uncaught by
    # GroheDialCoordinator's except clauses) instead of the typed
    # GroheDialApiError every other malformed-response path already
    # raises. Fixed in api.py's get_status()/get_config().
    return web.json_response({"connection_status": "READY"})


@pytest.mark.asyncio
async def test_get_status_malformed_body_raises_typed_error() -> None:
    app = web.Application()
    app.router.add_get("/api/status", _handle_status_truncated)
    server = TestServer(app)
    test_client = TestClient(server)
    await test_client.start_server()
    try:
        api = GroheDialApiClient(test_client.session, test_client.host, test_client.port, TOKEN)
        with pytest.raises(GroheDialApiError):
            await api.get_status()
    finally:
        await test_client.close()


async def _handle_dispense_app_task_timeout(request: web.Request) -> web.Response:
    # Contract test: mirrors provisioning_server.cpp's SendRequestResult()
    # exactly for dial_api::RequestResult::kTimeout -- "500 Internal
    # Server Error", body {"status":"error","reason":"timeout"}. This is
    # the one documented, non-command-rejection reason a POST can return
    # 500 at all (see dial_api.hpp's RequestResult::kTimeout comment) --
    # confirms the client maps it the same way as every other rejection,
    # not as an opaque/uncaught HTTP error.
    return web.json_response({"status": "error", "reason": "timeout"}, status=500)


@pytest.mark.asyncio
async def test_dispense_http_500_app_task_timeout_raises_command_rejected() -> None:
    app = web.Application()
    app.router.add_post("/api/dispense", _handle_dispense_app_task_timeout)
    server = TestServer(app)
    test_client = TestClient(server)
    await test_client.start_server()
    try:
        api = GroheDialApiClient(test_client.session, test_client.host, test_client.port, TOKEN)
        with pytest.raises(GroheDialCommandRejected) as exc_info:
            await api.dispense(500, "SPARKLING")
        assert exc_info.value.reason == "timeout"
        assert exc_info.value.status_code == 500
    finally:
        await test_client.close()


@pytest.mark.asyncio
async def test_request_timeout_raises_connection_error(client: GroheDialApiClient) -> None:
    # A genuinely slow/unreachable dial (BLE stack wedged, Wi-Fi dropped
    # mid-request, ...) -- exercised by making the underlying aiohttp
    # session raise the same TimeoutError a real 10s socket timeout would
    # produce, rather than actually waiting 10s in the test suite.
    from unittest.mock import patch

    with patch.object(client._session, "request", side_effect=TimeoutError("timed out")):
        with pytest.raises(GroheDialConnectionError):
            await client.get_status()
