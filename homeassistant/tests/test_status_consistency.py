"""Regression tests documenting the GET /api/status field-consistency
contract -- see dial_state.hpp's own field comments (the firmware's
single source of truth for what each field means) and
dial_controller.cpp's HandleCommandOutcome()/Tick()/
HandleConnectionLost() (the only places that ever mutate them). Written
for app::App::Status()'s fix (see that method's own comment in
app.cpp): it used to call DialController::State() directly from the
httpd task, racing the app task's own multi-field updates for a single
logical transition; it now round-trips the read through the app task's
own queue instead, the same place every write already happens.

IMPORTANT SCOPE: this file exercises DialStatus, the plain dataclass
api.py parses a JSON response into -- it does NOT and cannot exercise
the real firmware's FreeRTOS task scheduling. This project has no
host-side C++/FreeRTOS test harness (see docs/m15_ha_integration.md's
own test-structure investigation) -- verifying the fix actually closes
the race in the real binary is a matter of code review (App::Status()
now only ever reads dial_controller_.State() from the app task itself,
including in its timeout fallback -- see DialController::State()'s own
comment) plus a clean build, not something this suite can prove by
execution. What this file *does* verify: (1) the field-relationship
invariants the firmware's state machine actually guarantees are
precisely and executably documented here, not just in prose, and (2)
the checker below genuinely has teeth -- proven below by feeding it the
exact torn combination that motivated the fix and confirming it's
rejected.
"""

from __future__ import annotations

import pytest

from grohe_dial.api import DialStatus


def _assert_consistent_snapshot(status: DialStatus) -> None:
    """Field-relationship invariants dial_controller.cpp's state machine
    guarantees for any DialState that ever really existed -- see
    dial_state.hpp's own field comments and dial_controller.cpp's
    HandleCommandOutcome()/Tick()/HandleConnectionLost(). Raises
    AssertionError the moment any single field combination doesn't
    correspond to a state the dial could actually have been in.
    """
    if status.dispense_status == "IDLE":
        assert status.delivered_ml == 0, (
            "IDLE must report delivered_ml == 0 (dial_state.hpp: "
            "'Meaningful only while kDispensing or kStopping ... 0 otherwise')"
        )
    elif status.dispense_status in ("DISPENSING", "STOPPING", "FINISHED"):
        assert status.active_dispense_amount_ml > 0, (
            f"{status.dispense_status} must report a positive "
            "active_dispense_amount_ml -- it is only ever set, alongside "
            "dispense_status itself, at the moment a dispense is accepted "
            "(dial_controller.cpp's HandleCommandOutcome())"
        )
        assert 0 <= status.delivered_ml <= status.active_dispense_amount_ml, (
            f"{status.dispense_status}: delivered_ml ({status.delivered_ml}) "
            f"must never exceed active_dispense_amount_ml "
            f"({status.active_dispense_amount_ml}) -- Tick()'s own count-up "
            "can only ever approach it (rounded down to the nearest 10ml), "
            "never pass it"
        )
    elif status.dispense_status == "FAILED":
        assert status.delivered_ml == 0, (
            "FAILED means a dispense request was rejected before it ever "
            "started counting up (dial_controller.cpp's own comment on "
            "its kFailed Tick() handling: 'already 0')"
        )

    if status.connection_status == "CONNECTION_LOST":
        assert status.dispense_status == "IDLE", (
            "A lost BLE connection forces dispense_status back to IDLE "
            "unconditionally (dial_controller.cpp's HandleConnectionLost()) "
            "-- DISPENSING/STOPPING can never coexist with CONNECTION_LOST"
        )


def _status(**overrides) -> DialStatus:
    base = dict(
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
    base.update(overrides)
    return DialStatus(**base)


@pytest.mark.parametrize(
    "status",
    [
        pytest.param(_status(), id="idle"),
        pytest.param(
            _status(dispense_status="DISPENSING", active_dispense_amount_ml=300, delivered_ml=150),
            id="dispensing-mid-pour",
        ),
        pytest.param(
            _status(dispense_status="STOPPING", active_dispense_amount_ml=300, delivered_ml=180),
            id="stopping-frozen-at-last-delivered-value",
        ),
        pytest.param(
            # Tick()'s 10ml-rounddown count-up can still be short of the
            # full committed amount at the exact instant kFinished is
            # entered (session-duration-based, not delivered_ml-based) --
            # not a bug, a real, valid combination.
            _status(dispense_status="FINISHED", active_dispense_amount_ml=300, delivered_ml=290),
            id="finished-slightly-short-of-full-amount-is-valid",
        ),
        pytest.param(
            _status(dispense_status="FAILED", active_dispense_amount_ml=0, delivered_ml=0),
            id="failed-request-never-started-counting",
        ),
        pytest.param(
            _status(connection_status="CONNECTION_LOST", dispense_status="IDLE"),
            id="ble-disconnected",
        ),
    ],
)
def test_real_state_combinations_pass_the_consistency_check(status: DialStatus) -> None:
    _assert_consistent_snapshot(status)  # must not raise


@pytest.mark.parametrize(
    "status",
    [
        pytest.param(
            # The exact example from the concurrency review this fix
            # addresses.
            _status(dispense_status="DISPENSING", active_dispense_amount_ml=0, delivered_ml=500),
            id="the-torn-example-from-the-concurrency-review",
        ),
        pytest.param(
            _status(dispense_status="DISPENSING", active_dispense_amount_ml=300, delivered_ml=301),
            id="delivered-exceeds-committed-amount",
        ),
        pytest.param(
            _status(dispense_status="IDLE", delivered_ml=50),
            id="idle-with-stale-nonzero-delivered_ml",
        ),
        pytest.param(
            _status(
                connection_status="CONNECTION_LOST",
                dispense_status="DISPENSING",
                active_dispense_amount_ml=300,
                delivered_ml=100,
            ),
            id="dispensing-survives-a-lost-connection",
        ),
    ],
)
def test_checker_rejects_torn_combinations(status: DialStatus) -> None:
    # Proves the checker above actually has teeth -- if this test ever
    # started passing (i.e. _assert_consistent_snapshot stopped raising
    # for one of these), the checker itself would have silently
    # regressed into a no-op.
    with pytest.raises(AssertionError):
        _assert_consistent_snapshot(status)
