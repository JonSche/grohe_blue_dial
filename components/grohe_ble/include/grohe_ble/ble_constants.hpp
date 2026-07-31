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

// The two GATT characteristics inside the Grohe service, both confirmed on
// real hardware in M5 (identical across 20+ discovery trials): the
// appliance declares them as full Bluetooth-Base-UUID encodings of the
// 16-bit values 0x1706/0x1705 -- i.e. on the wire, and as ble_uuid_to_str()
// renders them, they are 00001706-0000-1000-8000-00805f9b34fb /
// 00001705-0000-1000-8000-00805f9b34fb, type BLE_UUID_TYPE_128 -- not the
// alternate 128-bit vendor-form UUIDs the Python reference implementation
// also defines but never actually uses (WRITE_CHARACTERISTIC_UUID_128 /
// READ_CHARACTERISTIC_UUID_128 in its constants.py).
//
// These must be declared as ble_uuid128_t, not ble_uuid16_t:
// ble_uuid_cmp() rejects on a type mismatch before ever comparing the
// value (confirmed by reading ble_uuid.c), so a 16-bit constant would
// never match what the peer actually sent, no matter the numeric value.
// As with kGroheServiceUuid above, the byte order here is the canonical
// Bluetooth Base UUID string reversed; GroheProtocol logs each of these
// back through ble_uuid_to_str() when it caches the matching handle, so a
// transcription mistake here shows up as an obviously-wrong log line
// instead of a silent never-matches bug.
inline constexpr ble_uuid128_t kGroheReadCharUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00,
                     0x10, 0x00, 0x00, 0x06, 0x17, 0x00, 0x00);
inline constexpr ble_uuid128_t kGroheWriteCharUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00,
                     0x10, 0x00, 0x00, 0x05, 0x17, 0x00, 0x00);

}  // namespace grohe_ble
