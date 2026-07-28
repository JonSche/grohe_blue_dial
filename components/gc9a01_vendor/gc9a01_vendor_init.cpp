#include "gc9a01_vendor/gc9a01_vendor_init.hpp"

namespace gc9a01_vendor {
namespace {

constexpr uint8_t kInit_eb[] = {0x14};
constexpr uint8_t kInit_84[] = {0x60};
constexpr uint8_t kInit_85[] = {0xff};
constexpr uint8_t kInit_86[] = {0xff};
constexpr uint8_t kInit_87[] = {0xff};
constexpr uint8_t kInit_8e[] = {0xff};
constexpr uint8_t kInit_8f[] = {0xff};
constexpr uint8_t kInit_88[] = {0x0a};
constexpr uint8_t kInit_89[] = {0x21};
constexpr uint8_t kInit_8a[] = {0x00};
constexpr uint8_t kInit_8b[] = {0x80};
constexpr uint8_t kInit_8c[] = {0x01};
constexpr uint8_t kInit_8d[] = {0x03};
constexpr uint8_t kInit_b5[] = {0x08, 0x09, 0x14, 0x08};
constexpr uint8_t kInit_b6[] = {0x00, 0x00};
constexpr uint8_t kInit_36[] = {0x48};  // MADCTL: MX + BGR (panel's native order)
constexpr uint8_t kInit_3a[] = {0x05};  // COLMOD: 16bpp, per this panel's datasheet
constexpr uint8_t kInit_90[] = {0x08, 0x08, 0x08, 0x08};
constexpr uint8_t kInit_bd[] = {0x06};
constexpr uint8_t kInit_ba[] = {0x01};
constexpr uint8_t kInit_bc[] = {0x00};
constexpr uint8_t kInit_ff[] = {0x60, 0x01, 0x04};
constexpr uint8_t kInit_c3[] = {0x13};
constexpr uint8_t kInit_c4[] = {0x13};
constexpr uint8_t kInit_c9[] = {0x25};
constexpr uint8_t kInit_be[] = {0x11};
constexpr uint8_t kInit_e1[] = {0x10, 0x0e};
constexpr uint8_t kInit_df[] = {0x21, 0x0c, 0x02};
constexpr uint8_t kInit_f0[] = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2a};
constexpr uint8_t kInit_f1[] = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6f};
constexpr uint8_t kInit_f2[] = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2a};
constexpr uint8_t kInit_f3[] = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6f};
constexpr uint8_t kInit_ed[] = {0x1b, 0x0b};
constexpr uint8_t kInit_ae[] = {0x77};
constexpr uint8_t kInit_cd[] = {0x63};
constexpr uint8_t kInit_70[] = {0x07, 0x07, 0x04, 0x0e, 0x0f, 0x09, 0x07, 0x08, 0x03};
constexpr uint8_t kInit_e8[] = {0x04};
constexpr uint8_t kInit_62[] = {0x18, 0x0d, 0x71, 0xed, 0x70, 0x70,
                                 0x18, 0x0f, 0x71, 0xef, 0x70, 0x70};
constexpr uint8_t kInit_63[] = {0x18, 0x11, 0x71, 0xf1, 0x70, 0x70,
                                 0x18, 0x13, 0x71, 0xf3, 0x70, 0x70};
constexpr uint8_t kInit_64[] = {0x28, 0x29, 0xf1, 0x01, 0xf1, 0x00, 0x07};
constexpr uint8_t kInit_66[] = {0x3c, 0x00, 0xcd, 0x67, 0x45,
                                 0x45, 0x10, 0x00, 0x00, 0x00};
constexpr uint8_t kInit_67[] = {0x00, 0x3c, 0x00, 0x00, 0x00,
                                 0x01, 0x54, 0x10, 0x32, 0x98};
constexpr uint8_t kInit_74[] = {0x10, 0x85, 0x80, 0x00, 0x00, 0x4e, 0x00};
constexpr uint8_t kInit_98[] = {0x3e, 0x07};
constexpr uint8_t kInit_99[] = {0x3e, 0x07};
constexpr uint8_t kInit_35[] = {0x00};
constexpr uint8_t kInit_44[] = {0x00, 0x4a};
constexpr uint8_t kInit_2a[] = {0x00, 0x00, 0x00, 0xef};
constexpr uint8_t kInit_2b[] = {0x00, 0x00, 0x00, 0xef};

constexpr gc9a01_lcd_init_cmd_t kVendorInitCmds[] = {
    {0xfe, nullptr, 0, 0},
    {0xef, nullptr, 0, 0},
    {0xeb, kInit_eb, sizeof(kInit_eb), 0},
    {0x84, kInit_84, sizeof(kInit_84), 0},
    {0x85, kInit_85, sizeof(kInit_85), 0},
    {0x86, kInit_86, sizeof(kInit_86), 0},
    {0x87, kInit_87, sizeof(kInit_87), 0},
    {0x8e, kInit_8e, sizeof(kInit_8e), 0},
    {0x8f, kInit_8f, sizeof(kInit_8f), 0},
    {0x88, kInit_88, sizeof(kInit_88), 0},
    {0x89, kInit_89, sizeof(kInit_89), 0},
    {0x8a, kInit_8a, sizeof(kInit_8a), 0},
    {0x8b, kInit_8b, sizeof(kInit_8b), 0},
    {0x8c, kInit_8c, sizeof(kInit_8c), 0},
    {0x8d, kInit_8d, sizeof(kInit_8d), 0},
    {0xb5, kInit_b5, sizeof(kInit_b5), 0},
    {0xb6, kInit_b6, sizeof(kInit_b6), 0},
    {0x36, kInit_36, sizeof(kInit_36), 0},
    {0x3a, kInit_3a, sizeof(kInit_3a), 0},
    {0x90, kInit_90, sizeof(kInit_90), 0},
    {0xbd, kInit_bd, sizeof(kInit_bd), 0},
    {0xba, kInit_ba, sizeof(kInit_ba), 0},
    {0xbc, kInit_bc, sizeof(kInit_bc), 0},
    {0xff, kInit_ff, sizeof(kInit_ff), 0},
    {0xc3, kInit_c3, sizeof(kInit_c3), 0},
    {0xc4, kInit_c4, sizeof(kInit_c4), 0},
    {0xc9, kInit_c9, sizeof(kInit_c9), 0},
    {0xbe, kInit_be, sizeof(kInit_be), 0},
    {0xe1, kInit_e1, sizeof(kInit_e1), 0},
    {0xdf, kInit_df, sizeof(kInit_df), 0},
    {0xf0, kInit_f0, sizeof(kInit_f0), 0},
    {0xf1, kInit_f1, sizeof(kInit_f1), 0},
    {0xf2, kInit_f2, sizeof(kInit_f2), 0},
    {0xf3, kInit_f3, sizeof(kInit_f3), 0},
    {0xed, kInit_ed, sizeof(kInit_ed), 0},
    {0xae, kInit_ae, sizeof(kInit_ae), 0},
    {0xcd, kInit_cd, sizeof(kInit_cd), 0},
    {0x70, kInit_70, sizeof(kInit_70), 0},
    {0xe8, kInit_e8, sizeof(kInit_e8), 0},
    {0x62, kInit_62, sizeof(kInit_62), 0},
    {0x63, kInit_63, sizeof(kInit_63), 0},
    {0x64, kInit_64, sizeof(kInit_64), 0},
    {0x66, kInit_66, sizeof(kInit_66), 0},
    {0x67, kInit_67, sizeof(kInit_67), 0},
    {0x74, kInit_74, sizeof(kInit_74), 0},
    {0x98, kInit_98, sizeof(kInit_98), 0},
    {0x99, kInit_99, sizeof(kInit_99), 0},
    {0x35, kInit_35, sizeof(kInit_35), 0},
    {0x44, kInit_44, sizeof(kInit_44), 0},
    {0x21, nullptr, 0, 0},  // display inversion on -- this panel is wired inverted
    {0x2a, kInit_2a, sizeof(kInit_2a), 0},
    {0x2b, kInit_2b, sizeof(kInit_2b), 0},
    {0x2c, nullptr, 0, 0},
    {0x11, nullptr, 0, 120},  // sleep out
    {0x29, nullptr, 0, 20},   // display on
};

}  // namespace

const gc9a01_vendor_config_t kGc9a01VendorConfig = {
    .init_cmds = kVendorInitCmds,
    .init_cmds_size = sizeof(kVendorInitCmds) / sizeof(kVendorInitCmds[0]),
};

}  // namespace gc9a01_vendor
