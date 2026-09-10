"""M16.2: translates api.py's own GroheDialApiError hierarchy into
HA-native, translatable HomeAssistantError instances, at the one point
(entity actions in button.py, service handlers in services.py) where an
uncaught GroheDialConnectionError/GroheDialCommandRejected would
otherwise reach the user as a generic, unhelpful "Unknown error" --
found on real hardware during the M15 acceptance test.

Uses HomeAssistantError's own translation_domain/translation_key/
translation_placeholders kwargs (verified against the actual installed
homeassistant.exceptions source, not from memory) -- the displayed text
itself lives in strings.json's own top-level "exceptions" section, kept
in sync in translations/en.json, not hardcoded here. api.py itself
stays HA-free (see its own header comment) -- this is the one place
that bridges its exceptions to HA's.
"""

from __future__ import annotations

from typing import NoReturn

from homeassistant.exceptions import HomeAssistantError

from .api import GroheDialApiError, GroheDialCommandRejected, GroheDialConnectionError
from .const import DOMAIN


def raise_as_home_assistant_error(err: GroheDialApiError) -> NoReturn:
    """Re-raises `err` as a translatable HomeAssistantError. Always
    raises -- never returns -- so every call site can treat it exactly
    like propagating the original exception, just translated. `err`
    itself is preserved as `__cause__` (the `from err` below), so the
    original error is never lost, only given a clearer message on top.
    """
    if isinstance(err, GroheDialConnectionError):
        raise HomeAssistantError(
            translation_domain=DOMAIN, translation_key="dial_unreachable"
        ) from err
    if isinstance(err, GroheDialCommandRejected):
        raise HomeAssistantError(
            translation_domain=DOMAIN,
            translation_key="command_rejected",
            translation_placeholders={"reason": err.reason},
        ) from err
    # GroheDialAuthError shouldn't reach here in practice (the
    # coordinator's own poll is what discovers an auth failure and
    # triggers reauth, not a command call -- see coordinator.py); any
    # other/unexpected GroheDialApiError still gets a clear, non-generic
    # message instead of falling through to HA's own bare "Unknown
    # error".
    raise HomeAssistantError(
        translation_domain=DOMAIN,
        translation_key="unexpected_dial_error",
        translation_placeholders={"error": str(err)},
    ) from err
