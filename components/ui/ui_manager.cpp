#include "ui/ui_manager.hpp"

namespace ui {
namespace {
constexpr lv_color_t kBackgroundColor = LV_COLOR_MAKE(0x10, 0x14, 0x18);
constexpr lv_color_t kArcTrackColor = LV_COLOR_MAKE(0x2a, 0x30, 0x38);
constexpr lv_color_t kAccentColor = LV_COLOR_MAKE(0x35, 0xa7, 0xe0);
constexpr lv_color_t kPrimaryTextColor = LV_COLOR_MAKE(0xf0, 0xf2, 0xf5);
constexpr lv_color_t kMutedTextColor = LV_COLOR_MAKE(0x8a, 0x93, 0xa6);

constexpr int32_t kArcDiameter = 210;
// 0 in "arc value space" is rotated to 270 (screen-space degrees, 0 = 3
// o'clock, clockwise), so the ring fills starting at 12 o'clock.
constexpr int32_t kArcRotation = 270;
constexpr int32_t kArcWidth = 8;

void ApplyLetterSpacedCaps(lv_obj_t* label) {
  lv_obj_set_style_text_letter_space(label, 2, 0);
}
}  // namespace

void UiManager::Init(lv_display_t* display) {
  lv_obj_t* screen = lv_display_get_screen_active(display);
  lv_obj_set_style_bg_color(screen, kBackgroundColor, 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  // Circular progress ring: the pour amount mapped onto the display's own
  // round shape, not a generic rectangular gauge.
  arc_ = lv_arc_create(screen);
  lv_obj_set_size(arc_, kArcDiameter, kArcDiameter);
  lv_obj_center(arc_);
  lv_arc_set_bg_angles(arc_, 0, 360);
  lv_arc_set_rotation(arc_, kArcRotation);
  lv_arc_set_mode(arc_, LV_ARC_MODE_NORMAL);
  lv_arc_set_range(arc_, dial_state::kMinAmountMl, dial_state::kMaxAmountMl);
  // Read-only indicator, not a touch control: no knob, no click handling.
  lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc_, kArcWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc_, kArcTrackColor, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_, kArcWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc_, kAccentColor, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc_, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(arc_, LV_OPA_TRANSP, LV_PART_MAIN);

  // "500 ml" reads as one composed unit, not two stacked labels -- a row
  // that hugs its content and stays centered as a group regardless of
  // whether the amount is 3 or 4 digits (LV_SIZE_CONTENT + flex center,
  // rather than centering the number alone and leaving "ml" unbalanced to
  // one side).
  lv_obj_t* amount_row = lv_obj_create(screen);
  lv_obj_remove_style_all(amount_row);
  lv_obj_set_size(amount_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(amount_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(amount_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(amount_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(amount_row, LV_ALIGN_CENTER, 0, -16);

  amount_label_ = lv_label_create(amount_row);
  lv_obj_set_style_text_color(amount_label_, kPrimaryTextColor, 0);
  lv_obj_set_style_text_font(amount_label_, &lv_font_montserrat_48, 0);

  lv_obj_t* unit_label = lv_label_create(amount_row);
  lv_label_set_text(unit_label, "ml");
  lv_obj_set_style_text_color(unit_label, kMutedTextColor, 0);
  lv_obj_set_style_text_font(unit_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_pad_left(unit_label, 4, 0);
  lv_obj_set_style_pad_bottom(unit_label, 6, 0);

  water_type_label_ = lv_label_create(screen);
  lv_obj_set_style_text_color(water_type_label_, kPrimaryTextColor, 0);
  lv_obj_set_style_text_font(water_type_label_, &lv_font_montserrat_14, 0);
  ApplyLetterSpacedCaps(water_type_label_);
  lv_obj_align(water_type_label_, LV_ALIGN_CENTER, 0, 34);

  lv_obj_t* hint_label = lv_label_create(screen);
  lv_label_set_text(hint_label, "PRESS TO POUR");
  lv_obj_set_style_text_color(hint_label, kMutedTextColor, 0);
  lv_obj_set_style_text_opa(hint_label, LV_OPA_60, 0);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
  ApplyLetterSpacedCaps(hint_label);
  lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, 66);

  // M7: the appliance's decoded protocol state -- a compact technical
  // readout, deliberately not letter-spaced like the labels above (this
  // isn't a UI title, it's diagnostic text), kept short enough to fit the
  // round display's narrowing chord this far from center (see Render()).
  appliance_status_label_ = lv_label_create(screen);
  lv_obj_set_style_text_color(appliance_status_label_, kMutedTextColor, 0);
  lv_obj_set_style_text_opa(appliance_status_label_, LV_OPA_60, 0);
  lv_obj_set_style_text_font(appliance_status_label_, &lv_font_montserrat_14,
                             0);
  lv_obj_align(appliance_status_label_, LV_ALIGN_CENTER, 0, 88);
}

void UiManager::Render(const dial_state::DialState& state) {
  lv_arc_set_value(arc_, state.amount_ml);
  lv_label_set_text_fmt(amount_label_, "%d", state.amount_ml);
  lv_label_set_text(water_type_label_,
                     dial_state::WaterTypeLabel(state.water_type));

  if (!state.appliance_response_received) {
    lv_label_set_text(appliance_status_label_, "APPL --");
  } else if (state.appliance_response_success) {
    lv_label_set_text(appliance_status_label_, "APPL OK");
  } else {
    const char* name =
        dial_state::ResponseCodeName(state.appliance_response_code);
    if (name != nullptr) {
      lv_label_set_text_fmt(appliance_status_label_, "APPL %s", name);
    } else {
      lv_label_set_text_fmt(appliance_status_label_, "APPL CODE %d",
                            state.appliance_response_code);
    }
  }
}

}  // namespace ui
