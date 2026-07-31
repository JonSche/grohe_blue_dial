#pragma once

#include "host/ble_uuid.h"

// Grohe Blue BLE protocol constants. Every service/characteristic UUID this
// firmware knows about belongs here and nowhere else, so the protocol
// surface is reviewable in one place as later milestones add GATT work.
namespace grohe_ble {

// Service UUID advertised by the Grohe Blue appliance:
//
//     33f31bba-9782-43e2-b7d0-2439e8dbb0f1
//
// This identifier and the "match on advertised 128-bit service UUID"
// detection strategy both come from the existing Python reference
// implementation, which is the source of truth -- neither is re-derived
// here.
//
// NimBLE stores 128-bit UUIDs little-endian: value[15] holds the *first*
// byte of the canonical string form, so the initializer below is the
// canonical byte order reversed. StartScan() logs this constant back
// through ble_uuid_to_str() when scanning starts, which turns a byte-order
// mistake here into an obviously-wrong log line instead of a silent
// never-matches bug.
inline constexpr ble_uuid128_t kGroheServiceUuid =
    BLE_UUID128_INIT(0xf1, 0xb0, 0xdb, 0xe8, 0x39, 0x24, 0xd0, 0xb7, 0xe2,
                     0x43, 0x82, 0x97, 0xba, 0x1b, 0xf3, 0x33);

}  // namespace grohe_ble
