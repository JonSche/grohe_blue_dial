"""Constants for the Grohe Dial integration.

Mirrors the firmware's own local HTTP API exactly -- see
docs/m15_ha_integration.md in the grohe_blue_dial firmware repository
for the authoritative API contract this integration is a client of. Not
speculative: every field/endpoint name here is read directly from
components/provisioning/provisioning_server.cpp's own JSON
(de)serialization, not guessed or duplicated with drift risk.
"""

from __future__ import annotations

DOMAIN = "grohe_dial"

CONF_API_TOKEN = "api_token"

DEFAULT_PORT = 8080
DEFAULT_SCAN_INTERVAL_SECONDS = 10

# M15.1: prefix marking a config entry's unique_id as the stable,
# hardware-derived (MAC-based) form rather than the original host/IP one
# -- see __init__.py's _async_migrate_to_stable_unique_id() for the
# migration this enables and config_flow.py's own use of it for brand
# new entries. Lives here (not in __init__.py, where it was first added)
# so config_flow.py can share the exact same value without a circular
# import between the two.
STABLE_UNIQUE_ID_PREFIX = "mac-"

# M15.3: entry.data key for the Grohe Cloud appliance_id this dial was
# provisioned against (config_flow.py's GroheDialOptionsFlow) -- not a
# secret, the same kind of identifier ha-grohe_smarthome's own device
# registry entry is already keyed by (see __init__.py's own
# via_device_id resolution). Absent from entry.data entirely for a dial
# that has never been provisioned through this integration's own Options
# Flow (e.g. scripts/provision.sh was used directly instead) -- that
# dial simply never gets a via_device link, same as
# ha-grohe_smarthome not being installed at all.
CONF_GROHE_APPLIANCE_ID = "grohe_appliance_id"

# M15.3: the domain string github.com/Flo-Schilli/ha-grohe_smarthome's
# own const.py hardcodes for its DOMAIN -- verified by reading that
# integration's real source directly (entities/entity/sensor.py's own
# DeviceInfo(identifiers={(self._domain, self._device.appliance_id)}),
# constructed with domain=DOMAIN='grohe_smarthome' at every call site),
# not guessed. A third-party integration's own domain, not something
# this project owns or can change if it's ever renamed -- see
# __init__.py's own via_device_id resolution for how a stale/absent
# match degrades gracefully rather than erroring.
GROHE_SMARTHOME_DOMAIN = "grohe_smarthome"

# HTTP header the firmware's /api/* endpoints require -- see
# provisioning_server.cpp's kApiAuthHeader.
API_TOKEN_HEADER = "X-Api-Token"

# M16: HTTP header POST /provision requires -- see provisioning_server.cpp's
# kProvisionAuthHeader. A separate secret from API_TOKEN_HEADER above (that
# one gates /api/*, this one gates /provision) -- unchanged, pre-existing
# firmware behavior (M13.2), not new in M16.
PROVISION_TOKEN_HEADER = "X-Provision-Token"

# dial_state::WaterType's own three values (dial_state.hpp) -- the
# firmware rejects anything else with 422. Order matches the physical
# dial's own long-press cycle (Still -> Medium -> Sparkling -> Still).
WATER_TYPE_STILL = "STILL"
WATER_TYPE_MEDIUM = "MEDIUM"
WATER_TYPE_SPARKLING = "SPARKLING"
WATER_TYPES = (WATER_TYPE_STILL, WATER_TYPE_MEDIUM, WATER_TYPE_SPARKLING)

# dial_state::kMinAmountMl/kMaxAmountMl (dial_state.hpp) -- the firmware's
# own bounds, duplicated here only for client-side form validation
# (min/max on the number entities); the firmware re-validates
# independently regardless (see provisioning_server.cpp), so a drift here
# is a worse UX (a rejected request), never a safety issue.
MIN_AMOUNT_ML = 100
MAX_AMOUNT_ML = 2000

SERVICE_DISPENSE = "dispense"
SERVICE_STOP = "stop"
ATTR_AMOUNT_ML = "amount_ml"
ATTR_WATER_TYPE = "water_type"
