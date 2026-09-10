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
