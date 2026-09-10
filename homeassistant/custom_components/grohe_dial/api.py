"""Async client for the Grohe Dial firmware's local HTTP API.

Talks to exactly the five endpoints documented in
docs/m15_ha_integration.md / implemented in
components/provisioning/provisioning_server.cpp: GET /api/status,
GET/POST /api/config, POST /api/dispense, POST /api/stop. Deliberately
thin -- no retry policy, no caching -- those are the coordinator's job
(coordinator.py), not this client's. Uses whatever aiohttp
ClientSession Home Assistant's own aiohttp_client helper provides (see
__init__.py), never opens its own.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import aiohttp

from .const import API_TOKEN_HEADER

__all__ = [
    "GroheDialApiClient",
    "GroheDialApiError",
    "GroheDialAuthError",
    "GroheDialConnectionError",
    "GroheDialCommandRejected",
    "DialStatus",
    "DialConfig",
]


class GroheDialApiError(Exception):
    """Base class for every error this client raises."""


class GroheDialAuthError(GroheDialApiError):
    """The configured API token was rejected (HTTP 401)."""


class GroheDialConnectionError(GroheDialApiError):
    """The dial could not be reached at all (timeout, DNS, refused, ...)."""


class GroheDialCommandRejected(GroheDialApiError):
    """A command was reachable and authenticated, but the dial rejected it.

    Mirrors dial_api::RequestResult's own non-accepted values (see
    dial_api.hpp) -- `reason` is the exact string the firmware's own
    JSON body already carries (e.g. "not_available", "ble_not_ready",
    "invalid_amount"), not re-interpreted here.
    """

    def __init__(self, reason: str, status_code: int) -> None:
        super().__init__(f"command rejected: {reason} (HTTP {status_code})")
        self.reason = reason
        self.status_code = status_code


@dataclass(frozen=True, slots=True)
class DialStatus:
    """Mirrors GET /api/status's JSON body exactly -- see
    provisioning_server.cpp's HandleApiStatusGet() for the authoritative
    field list. Field names intentionally match the wire format, not
    reformatted to Python convention, so there is exactly one place
    (from_json() below) that could ever drift from the firmware.
    """

    connection_status: str
    time_status: str
    dispense_status: str
    water_type: str
    amount_ml: int
    active_dispense_amount_ml: int
    delivered_ml: int
    appliance_response_received: bool
    appliance_response_success: bool
    appliance_response_code: int

    @classmethod
    def from_json(cls, data: dict[str, Any]) -> DialStatus:
        appliance_response = data.get("appliance_response", {})
        return cls(
            connection_status=data["connection_status"],
            time_status=data["time_status"],
            dispense_status=data["dispense_status"],
            water_type=data["water_type"],
            amount_ml=data["amount_ml"],
            active_dispense_amount_ml=data["active_dispense_amount_ml"],
            delivered_ml=data["delivered_ml"],
            appliance_response_received=appliance_response.get("received", False),
            appliance_response_success=appliance_response.get("success", False),
            appliance_response_code=appliance_response.get("code", 0),
        )


@dataclass(frozen=True, slots=True)
class DialConfig:
    """Mirrors GET/POST /api/config's JSON body exactly -- see
    provisioning_server.cpp's HandleApiConfigGet()/HandleApiConfigPost().
    """

    default_amount_ml: int
    amount_step_ml: int
    default_water_type: str

    @classmethod
    def from_json(cls, data: dict[str, Any]) -> DialConfig:
        return cls(
            default_amount_ml=data["default_amount_ml"],
            amount_step_ml=data["amount_step_ml"],
            default_water_type=data["default_water_type"],
        )


class GroheDialApiClient:
    """One instance per configured dial (per config entry)."""

    def __init__(self, session: aiohttp.ClientSession, host: str, port: int, api_token: str) -> None:
        self._session = session
        self._base_url = f"http://{host}:{port}"
        self._headers = {API_TOKEN_HEADER: api_token}

    async def _request(
        self, method: str, path: str, *, json: dict[str, Any] | None = None
    ) -> tuple[int, dict[str, Any]]:
        url = f"{self._base_url}{path}"
        try:
            async with self._session.request(
                method, url, headers=self._headers, json=json, timeout=aiohttp.ClientTimeout(total=10)
            ) as response:
                if response.status == 401:
                    raise GroheDialAuthError(f"{method} {path}: invalid API token")
                # Every response this API ever sends is JSON, success or
                # error alike (see provisioning_server.cpp's SendJsonStatus()/
                # SendRequestResult()) -- a 400 for genuinely malformed JSON
                # is the one exception, whose body is plain text, not JSON.
                try:
                    body = await response.json(content_type=None)
                except (aiohttp.ContentTypeError, ValueError):
                    body = {}
                return response.status, (body or {})
        except aiohttp.ClientError as err:
            raise GroheDialConnectionError(f"{method} {path}: {err}") from err
        except TimeoutError as err:
            raise GroheDialConnectionError(f"{method} {path}: timed out") from err

    async def get_status(self) -> DialStatus:
        status_code, body = await self._request("GET", "/api/status")
        if status_code != 200:
            raise GroheDialApiError(f"GET /api/status returned HTTP {status_code}")
        try:
            return DialStatus.from_json(body)
        except (KeyError, TypeError) as err:
            # A 200 whose body is valid JSON but missing/mistyped fields
            # (e.g. a truncated response) -- from_json()'s direct dict
            # indexing would otherwise let a raw KeyError/TypeError escape
            # this client entirely, which GroheDialCoordinator's own
            # except clauses (coordinator.py) don't catch, unlike every
            # other failure mode here. Re-raised as the same typed error
            # every other malformed-response case already produces.
            raise GroheDialApiError(f"GET /api/status returned malformed JSON: {err}") from err

    async def get_config(self) -> DialConfig:
        status_code, body = await self._request("GET", "/api/config")
        if status_code != 200:
            raise GroheDialApiError(f"GET /api/config returned HTTP {status_code}")
        try:
            return DialConfig.from_json(body)
        except (KeyError, TypeError) as err:
            # See get_status()'s own comment above -- identical reasoning.
            raise GroheDialApiError(f"GET /api/config returned malformed JSON: {err}") from err

    async def set_config(
        self,
        *,
        default_amount_ml: int | None = None,
        amount_step_ml: int | None = None,
        default_water_type: str | None = None,
    ) -> None:
        """Only the fields actually passed are sent -- the firmware's own
        POST /api/config leaves any field not present in the request body
        untouched (see provisioning_server.cpp's HandleApiConfigPost()),
        so a partial update here is a partial update there too, not a
        reset of the other two fields.
        """
        payload: dict[str, Any] = {}
        if default_amount_ml is not None:
            payload["default_amount_ml"] = default_amount_ml
        if amount_step_ml is not None:
            payload["amount_step_ml"] = amount_step_ml
        if default_water_type is not None:
            payload["default_water_type"] = default_water_type
        status_code, body = await self._request("POST", "/api/config", json=payload)
        if status_code != 200:
            raise GroheDialCommandRejected(body.get("reason", "unknown"), status_code)

    async def dispense(self, amount_ml: int, water_type: str) -> None:
        status_code, body = await self._request(
            "POST", "/api/dispense", json={"amount_ml": amount_ml, "water_type": water_type}
        )
        if status_code != 200:
            raise GroheDialCommandRejected(body.get("reason", "unknown"), status_code)

    async def stop(self) -> None:
        status_code, body = await self._request("POST", "/api/stop")
        if status_code != 200:
            raise GroheDialCommandRejected(body.get("reason", "unknown"), status_code)
