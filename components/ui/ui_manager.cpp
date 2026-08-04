#include "ui/ui_manager.hpp"

namespace ui {
namespace {
constexpr lv_color_t kBackgroundColor = LV_COLOR_MAKE(0x10, 0x14, 0x18);
constexpr lv_color_t kArcTrackColor = LV_COLOR_MAKE(0x2a, 0x30, 0x38);
constexpr lv_color_t kAccentColor = LV_COLOR_MAKE(0x35, 0xa7, 0xe0);
// Not-ready ring colour (M11): a genuinely desaturated grey-blue, never a
// dimmed/transparent version of kAccentColor -- see dial_state.hpp's
// ConnectionStatus/TimeStatus comments for why "not ready" and "error" both
// read as calm colour shifts, never as flashing or dimming.
constexpr lv_color_t kDesaturatedColor = LV_COLOR_MAKE(0x6b, 0x84, 0x94);
// The one "something is active" tint, shared by the startup pulse and the
// travelling highlight (frozen UI spec: "one active vocabulary, two
// scopes") -- deliberately the only third colour anywhere on this screen.
constexpr lv_color_t kHighlightColor = LV_COLOR_MAKE(0x7f, 0xd0, 0xf2);
constexpr lv_color_t kPrimaryTextColor = LV_COLOR_MAKE(0xf0, 0xf2, 0xf5);
constexpr lv_color_t kMutedTextColor = LV_COLOR_MAKE(0x8a, 0x93, 0xa6);

constexpr int32_t kArcDiameter = 210;
// 0 in "arc value space" is rotated to 270 (screen-space degrees, 0 = 3
// o'clock, clockwise), so the ring fills starting at 12 o'clock.
constexpr int32_t kArcRotation = 270;
// 10 px (was 8 pre-M11) -- chosen after rendering 8/10/12 px side by side
// specifically on short arcs (this ring is frequently a small fraction of
// the circle now that it never grows to a full circle during dispensing);
// see the frozen UI spec's ring-thickness section for the full comparison.
constexpr int32_t kArcWidth = 10;
// Momentary width during the startup pulse only -- never persisted.
constexpr int32_t kArcPulseWidth = 11;

// Startup pulse: brightness + thickness only, no scale, no bounce.
constexpr uint32_t kStartupPulseMs = 175;

// Travelling highlight: a short segment oscillating within the
// dialled arc, or -- for small arcs with no room to read as "travelling"
// -- a gentle uniform brightness pulse across the whole (small) arc.
constexpr int32_t kHighlightWidthDeg = 10;
constexpr int32_t kHighlightMinDialledArcDeg = 20;
constexpr uint32_t kHighlightSweepMs = 2600;
constexpr uint32_t kHighlightPulseMs = 1300;
constexpr lv_opa_t kHighlightPulseMinOpa = 80;
constexpr lv_opa_t kHighlightPulseMaxOpa = 255;

// Time glyph (tiny ring, r = 6 px -> 12 px object) and connection glyph
// (tiny dot, r ~ 5 px -> 9 px object), "Interior Crown" placement: centred
// above the numeral, local y ~ 52 (offset -68 from screen centre), 24 px
// apart.
constexpr int32_t kGlyphRowOffsetY = -68;
constexpr int32_t kTimeGlyphOffsetX = -12;
constexpr int32_t kConnectionGlyphOffsetX = 12;
constexpr int32_t kTimeGlyphDiameter = 12;
constexpr int32_t kTimeGlyphDotDiameter = 8;
constexpr int32_t kConnectionDotDiameter = 9;
constexpr int32_t kConnectionHaloDiameter = 16;
constexpr uint32_t kSyncSweepMs = 1200;
constexpr int32_t kSyncSweepWidthDeg = 90;
constexpr uint32_t kConnectingHaloMs = 1400;
constexpr lv_opa_t kConnectingHaloMinOpa = 40;
constexpr lv_opa_t kConnectingHaloMaxOpa = 160;
constexpr lv_opa_t kDisconnectedDotOpa = 64;   // ~25 % -- never fully invisible.
constexpr lv_opa_t kInvalidGlyphRingOpa = 90;  // ~35 %.
constexpr int32_t kGlyphStrokeWidth = 2;       // Shared by both tiny glyphs.

// Water-type de-emphasis while dispensing: opacity only, not a
// smaller font -- this build only compiles Montserrat 14/28/48 (see
// sdkconfig), so there is no smaller size to step down to. The frozen
// spec's own wording ("smaller and/or lower contrast") allows this;
// documented here and in docs/ARCHITECTURE.md as the one place this
// implementation's exact mechanism differs from the spec's illustrative
// mockups, which assumed a continuous font-size scale.
constexpr lv_opa_t kWaterTypeDimOpa = 128;  // ~50 %.

void ApplyLetterSpacedCaps(lv_obj_t* label) {
  lv_obj_set_style_text_letter_space(label, 2, 0);
}

// Shared by every tiny connectivity-glyph circle: a plain lv_obj sized and
// positioned as a small filled/outlined dot rather than a label or arc.
lv_obj_t* CreateGlyphDot(lv_obj_t* parent, int32_t diameter, int32_t offset_x) {
  lv_obj_t* dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, diameter, diameter);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dot, LV_ALIGN_CENTER, offset_x, kGlyphRowOffsetY);
  return dot;
}
}  // namespace

void UiManager::Init(lv_display_t* display) {
  lv_obj_t* screen = lv_display_get_screen_active(display);
  lv_obj_set_style_bg_color(screen, kBackgroundColor, 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  // Circular progress ring: the pour amount mapped onto the display's own
  // round shape, not a generic rectangular gauge. Its fill is set exactly
  // once per state entry (Render() never animates it frame-by-frame) --
  // see Render()'s own comment for the one invariant this ring holds from
  // Ready through Finished.
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

  // The travelling highlight: same size/position/rotation as arc_,
  // its own background/track fully transparent so only its indicator --
  // the small moving segment -- is ever visible. Hidden outside
  // Dispensing; angles are set directly (lv_arc_set_angles), not via
  // lv_arc_set_value, since this widget never represents a value on the
  // 100-2000 ml scale, only a position within the *already-drawn* arc_.
  highlight_arc_ = lv_arc_create(screen);
  lv_obj_set_size(highlight_arc_, kArcDiameter, kArcDiameter);
  lv_obj_center(highlight_arc_);
  lv_arc_set_rotation(highlight_arc_, kArcRotation);
  lv_obj_remove_style(highlight_arc_, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(highlight_arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(highlight_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(highlight_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(highlight_arc_, kArcWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(highlight_arc_, kHighlightColor, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(highlight_arc_, true, LV_PART_INDICATOR);
  lv_arc_set_angles(highlight_arc_, 0, 0);
  lv_obj_add_flag(highlight_arc_, LV_OBJ_FLAG_HIDDEN);

  // "500 ml" reads as one composed unit, not two stacked labels -- a row
  // that hugs its content and stays centered as a group regardless of
  // whether the amount is 3 or 4 digits (LV_SIZE_CONTENT + flex center,
  // rather than centering the number alone and leaving "ml" unbalanced to
  // one side).
  amount_row_ = lv_obj_create(screen);
  lv_obj_remove_style_all(amount_row_);
  lv_obj_set_size(amount_row_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(amount_row_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(amount_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(amount_row_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(amount_row_, LV_ALIGN_CENTER, 0, -16);

  amount_label_ = lv_label_create(amount_row_);
  lv_obj_set_style_text_color(amount_label_, kPrimaryTextColor, 0);
  lv_obj_set_style_text_font(amount_label_, &lv_font_montserrat_48, 0);

  lv_obj_t* unit_label = lv_label_create(amount_row_);
  lv_label_set_text(unit_label, "ml");
  lv_obj_set_style_text_color(unit_label, kMutedTextColor, 0);
  lv_obj_set_style_text_font(unit_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_pad_left(unit_label, 4, 0);
  lv_obj_set_style_pad_bottom(unit_label, 6, 0);

  // Finished: replaces amount_row_ entirely rather than overlaying
  // it, so there is never a moment both are laid out/visible at once.
  // LV_SYMBOL_OK, not a raw "check mark" character -- it's in the private
  // -use glyph range LVGL's built-in fonts actually ship (a literal
  // Unicode check mark is not present in the compiled Montserrat fonts and
  // would render as a blank/tofu glyph).
  checkmark_label_ = lv_label_create(screen);
  lv_label_set_text(checkmark_label_, LV_SYMBOL_OK);
  lv_obj_set_style_text_color(checkmark_label_, kAccentColor, 0);
  lv_obj_set_style_text_font(checkmark_label_, &lv_font_montserrat_48, 0);
  lv_obj_align(checkmark_label_, LV_ALIGN_CENTER, 0, -16);
  lv_obj_add_flag(checkmark_label_, LV_OBJ_FLAG_HIDDEN);

  water_type_label_ = lv_label_create(screen);
  lv_obj_set_style_text_color(water_type_label_, kPrimaryTextColor, 0);
  lv_obj_set_style_text_font(water_type_label_, &lv_font_montserrat_14, 0);
  ApplyLetterSpacedCaps(water_type_label_);
  lv_obj_align(water_type_label_, LV_ALIGN_CENTER, 0, 34);

  hint_label_ = lv_label_create(screen);
  lv_label_set_text(hint_label_, "");
  lv_obj_set_style_text_color(hint_label_, kMutedTextColor, 0);
  lv_obj_set_style_text_opa(hint_label_, LV_OPA_60, 0);
  lv_obj_set_style_text_font(hint_label_, &lv_font_montserrat_14, 0);
  ApplyLetterSpacedCaps(hint_label_);
  lv_obj_align(hint_label_, LV_ALIGN_CENTER, 0, 66);

  // Time glyph: a tiny ring for the hollow/syncing states...
  time_glyph_arc_ = lv_arc_create(screen);
  lv_obj_set_size(time_glyph_arc_, kTimeGlyphDiameter, kTimeGlyphDiameter);
  lv_obj_align(time_glyph_arc_, LV_ALIGN_CENTER, kTimeGlyphOffsetX,
               kGlyphRowOffsetY);
  lv_arc_set_bg_angles(time_glyph_arc_, 0, 360);
  lv_obj_remove_style(time_glyph_arc_, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(time_glyph_arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(time_glyph_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(time_glyph_arc_, kGlyphStrokeWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_color(time_glyph_arc_, kMutedTextColor, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(time_glyph_arc_, kInvalidGlyphRingOpa, LV_PART_MAIN);
  lv_obj_set_style_arc_width(time_glyph_arc_, kGlyphStrokeWidth,
                             LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(time_glyph_arc_, kAccentColor, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(time_glyph_arc_, true, LV_PART_INDICATOR);
  lv_arc_set_angles(time_glyph_arc_, 0, 0);

  // ...and a tiny solid dot for the valid state (a different shape, not a
  // different style of the ring above).
  time_glyph_dot_ =
      CreateGlyphDot(screen, kTimeGlyphDotDiameter, kTimeGlyphOffsetX);
  lv_obj_set_style_bg_color(time_glyph_dot_, kAccentColor, 0);
  lv_obj_set_style_bg_opa(time_glyph_dot_, LV_OPA_COVER, 0);
  lv_obj_add_flag(time_glyph_dot_, LV_OBJ_FLAG_HIDDEN);

  // Connection glyph: a tiny dot, hollow (border only) when disconnected,
  // solid when connecting/connected -- plus a soft halo shown only while
  // connecting.
  connection_glyph_dot_ = CreateGlyphDot(screen, kConnectionDotDiameter,
                                        kConnectionGlyphOffsetX);
  lv_obj_set_style_bg_color(connection_glyph_dot_, kAccentColor, 0);
  lv_obj_set_style_border_width(connection_glyph_dot_, kGlyphStrokeWidth, 0);
  lv_obj_set_style_border_color(connection_glyph_dot_, kMutedTextColor, 0);

  connection_glyph_halo_ = CreateGlyphDot(screen, kConnectionHaloDiameter,
                                         kConnectionGlyphOffsetX);
  lv_obj_set_style_bg_color(connection_glyph_halo_, kAccentColor, 0);
  lv_obj_move_to_index(connection_glyph_halo_, 0);  // Draw behind the dot.
  lv_obj_add_flag(connection_glyph_halo_, LV_OBJ_FLAG_HIDDEN);
}

void UiManager::Render(const dial_state::DialState& state) {
  using dial_state::ConnectionStatus;
  using dial_state::DispenseStatus;
  using dial_state::TimeStatus;

  const bool ready = state.connection_status == ConnectionStatus::kReady &&
                     state.time_status == TimeStatus::kAvailable;
  const bool in_dispense_lifecycle =
      state.dispense_status != DispenseStatus::kIdle;

  // The ring's one invariant (frozen UI spec): while a dispense/stop/
  // finish is in progress, its fill is the amount that pour was actually
  // started with (active_dispense_amount_ml), never the live, still-
  // rotatable amount_ml -- rotation is never blocked (see
  // DialController::HandleEvent's own comment), so amount_ml can keep
  // changing mid-pour; the ring must not.
  const int ring_amount_ml =
      in_dispense_lifecycle ? state.active_dispense_amount_ml : state.amount_ml;
  lv_arc_set_value(arc_, ring_amount_ml);
  // Guarded (unlike lv_arc_set_value() above, LVGL's generic style path
  // always invalidates on lv_obj_set_style_arc_color(), whether or not the
  // colour actually changed -- see last_ring_ready_'s own comment).
  // Without this check, every count-up tick during Dispensing would force
  // a full redraw of the ring, contradicting the one performance claim
  // this whole design is built on: the ring costs nothing per-tick.
  if (ready != last_ring_ready_) {
    lv_obj_set_style_arc_color(arc_, ready ? kAccentColor : kDesaturatedColor,
                               LV_PART_INDICATOR);
    last_ring_ready_ = ready;
  }

  // Numeral vs. checkmark.
  if (state.dispense_status == DispenseStatus::kFinished) {
    lv_obj_add_flag(amount_row_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(checkmark_label_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(amount_row_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(checkmark_label_, LV_OBJ_FLAG_HIDDEN);
    const bool counting_up = state.dispense_status == DispenseStatus::kDispensing ||
                             state.dispense_status == DispenseStatus::kStopping;
    lv_label_set_text_fmt(amount_label_, "%d",
                          counting_up ? state.delivered_ml : state.amount_ml);
  }

  // Water type + de-emphasis while dispensing.
  lv_label_set_text(water_type_label_,
                     dial_state::WaterTypeLabel(state.water_type));
  const bool deemphasize_water_type =
      state.dispense_status == DispenseStatus::kDispensing ||
      state.dispense_status == DispenseStatus::kStopping;
  lv_obj_set_style_text_opa(
      water_type_label_,
      deemphasize_water_type ? kWaterTypeDimOpa : LV_OPA_COVER, 0);

  // Hint text: a single priority ladder -- connection readiness, then time
  // availability, then the dispense state machine. Each level fully gates
  // the ones below it: there is no reachable combination where, say,
  // dispensing is in progress while connection_status != kReady (losing
  // the link forces dispense_status back to kIdle -- see
  // DialController::HandleConnectionLost()).
  //
  // M11.1: raw protocol response codes (APPL OK / INVALID_HMAC / ...) no
  // longer surface here at all -- those are log-only now (see
  // DialController::HandleApplianceState()'s own comment). A Ready+Idle
  // dial shows no status text; losing time sync and still-syncing read
  // identically ("Synchronising...") since neither is something the user
  // can act on differently.
  //
  // Three ASCII periods, not the frozen spec's single Unicode ellipsis
  // glyph (U+2026) -- same reasoning as LV_SYMBOL_OK for the checkmark:
  // this build's compiled Montserrat fonts are not guaranteed to include
  // codepoints outside Basic Latin, and three periods reads identically
  // at this size.
  if (state.connection_status == ConnectionStatus::kConnectionLost) {
    lv_label_set_text(hint_label_, "Connection lost");
  } else if (state.connection_status == ConnectionStatus::kConnecting) {
    lv_label_set_text(hint_label_, "Connecting...");
  } else if (state.time_status != TimeStatus::kAvailable) {
    lv_label_set_text(hint_label_, "Synchronising...");
  } else {
    switch (state.dispense_status) {
      case DispenseStatus::kIdle:
        lv_label_set_text(hint_label_, "");
        break;
      case DispenseStatus::kDispensing:
        lv_label_set_text(hint_label_, "PRESS TO STOP");
        break;
      case DispenseStatus::kStopping:
        lv_label_set_text(hint_label_, "Stopping...");
        break;
      case DispenseStatus::kFinished:
        lv_label_set_text(hint_label_, "");
        break;
    }
  }

  // Startup pulse: only on a genuine Idle -> Dispensing start, never
  // on a Stopping -> Dispensing revert (a rejected stop is not a new
  // acknowledgement).
  if (last_dispense_status_ == DispenseStatus::kIdle &&
      state.dispense_status == DispenseStatus::kDispensing) {
    StartPulse();
  }

  // Travelling highlight: (re)configured on any fresh entry into
  // Dispensing (from Idle, a real start, or from Stopping, a reverted
  // stop -- water never actually stopped flowing either way); deleted
  // whenever not Dispensing; visible for Dispensing and Stopping (frozen
  // in place while stopping), hidden otherwise.
  if (state.dispense_status == DispenseStatus::kDispensing) {
    if (last_dispense_status_ != DispenseStatus::kDispensing) {
      StartHighlight(state.active_dispense_amount_ml);
    }
  } else {
    lv_anim_delete(highlight_arc_, nullptr);
  }
  const bool highlight_visible = state.dispense_status == DispenseStatus::kDispensing ||
                                 state.dispense_status == DispenseStatus::kStopping;
  if (highlight_visible) {
    lv_obj_clear_flag(highlight_arc_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(highlight_arc_, LV_OBJ_FLAG_HIDDEN);
  }

  // Time glyph: hollow ring (unavailable), sweeping arc (syncing),
  // solid dot (available). Only the tiny glyph itself ever moves for these
  // states -- the main ring above is never touched here.
  switch (state.time_status) {
    case TimeStatus::kUnavailable:
      lv_anim_delete(time_glyph_arc_, nullptr);
      lv_obj_set_style_arc_opa(time_glyph_arc_, LV_OPA_TRANSP, LV_PART_INDICATOR);
      lv_obj_clear_flag(time_glyph_arc_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(time_glyph_dot_, LV_OBJ_FLAG_HIDDEN);
      break;
    case TimeStatus::kSyncing:
      if (last_time_status_ != TimeStatus::kSyncing) {
        StartSyncSweep();
      }
      lv_obj_clear_flag(time_glyph_arc_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(time_glyph_dot_, LV_OBJ_FLAG_HIDDEN);
      break;
    case TimeStatus::kAvailable:
      lv_anim_delete(time_glyph_arc_, nullptr);
      lv_obj_add_flag(time_glyph_arc_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(time_glyph_dot_, LV_OBJ_FLAG_HIDDEN);
      break;
  }

  // Connection glyph: hollow dot (lost), breathing halo + solid core
  // (connecting), solid dot only (ready).
  switch (state.connection_status) {
    case ConnectionStatus::kConnectionLost:
      lv_anim_delete(connection_glyph_halo_, nullptr);
      lv_obj_add_flag(connection_glyph_halo_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_opa(connection_glyph_dot_, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(connection_glyph_dot_, kDisconnectedDotOpa, 0);
      break;
    case ConnectionStatus::kConnecting:
      if (last_connection_status_ != ConnectionStatus::kConnecting) {
        StartConnectingHalo();
      }
      lv_obj_clear_flag(connection_glyph_halo_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_opa(connection_glyph_dot_, LV_OPA_COVER, 0);
      lv_obj_set_style_border_opa(connection_glyph_dot_, LV_OPA_TRANSP, 0);
      break;
    case ConnectionStatus::kReady:
      lv_anim_delete(connection_glyph_halo_, nullptr);
      lv_obj_add_flag(connection_glyph_halo_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_opa(connection_glyph_dot_, LV_OPA_COVER, 0);
      lv_obj_set_style_border_opa(connection_glyph_dot_, LV_OPA_TRANSP, 0);
      break;
  }

  last_dispense_status_ = state.dispense_status;
  last_connection_status_ = state.connection_status;
  last_time_status_ = state.time_status;
}

void UiManager::StartPulse() {
  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, arc_);
  lv_anim_set_values(&pulse, 0, 1);
  lv_anim_set_duration(&pulse, kStartupPulseMs);
  lv_anim_set_exec_cb(&pulse, [](void* var, int32_t value) {
    lv_obj_t* arc = static_cast<lv_obj_t*>(var);
    if (value == 0) {
      lv_obj_set_style_arc_width(arc, kArcPulseWidth, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(arc, kHighlightColor, LV_PART_INDICATOR);
    } else {
      lv_obj_set_style_arc_width(arc, kArcWidth, LV_PART_INDICATOR);
      // Dispensing only ever starts while ready (see the hint-text
      // priority ladder in Render()), so accent is always the correct
      // colour to revert to here.
      lv_obj_set_style_arc_color(arc, kAccentColor, LV_PART_INDICATOR);
    }
  });
  lv_anim_start(&pulse);
}

void UiManager::StartHighlight(int amount_ml) {
  lv_anim_delete(highlight_arc_, nullptr);

  const float dialled_arc_deg =
      static_cast<float>(amount_ml - dial_state::kMinAmountMl) * 360.0f /
      static_cast<float>(dial_state::kMaxAmountMl - dial_state::kMinAmountMl);

  lv_anim_t highlight;
  lv_anim_init(&highlight);
  lv_anim_set_var(&highlight, highlight_arc_);
  lv_anim_set_repeat_count(&highlight, LV_ANIM_REPEAT_INFINITE);

  if (dialled_arc_deg < kHighlightMinDialledArcDeg) {
    // Too little angular room to read as travelling -- breathe in place
    // across the whole (small) dialled arc instead.
    lv_arc_set_angles(highlight_arc_, 0, static_cast<int32_t>(dialled_arc_deg));
    lv_anim_set_values(&highlight, kHighlightPulseMinOpa, kHighlightPulseMaxOpa);
    lv_anim_set_duration(&highlight, kHighlightPulseMs);
    lv_anim_set_reverse_time(&highlight, kHighlightPulseMs);
    lv_anim_set_exec_cb(&highlight, [](void* var, int32_t value) {
      lv_obj_set_style_arc_opa(static_cast<lv_obj_t*>(var),
                               static_cast<lv_opa_t>(value), LV_PART_INDICATOR);
    });
  } else {
    lv_obj_set_style_arc_opa(highlight_arc_, LV_OPA_COVER, LV_PART_INDICATOR);
    const int32_t max_offset_deg =
        static_cast<int32_t>(dialled_arc_deg) - kHighlightWidthDeg;
    lv_anim_set_values(&highlight, 0, max_offset_deg);
    lv_anim_set_duration(&highlight, kHighlightSweepMs);
    lv_anim_set_reverse_time(&highlight, kHighlightSweepMs);
    lv_anim_set_exec_cb(&highlight, [](void* var, int32_t value) {
      lv_arc_set_angles(static_cast<lv_obj_t*>(var), value,
                        value + kHighlightWidthDeg);
    });
  }
  lv_anim_start(&highlight);
}

void UiManager::StartSyncSweep() {
  lv_anim_delete(time_glyph_arc_, nullptr);
  lv_obj_set_style_arc_opa(time_glyph_arc_, LV_OPA_COVER, LV_PART_INDICATOR);

  lv_anim_t sweep;
  lv_anim_init(&sweep);
  lv_anim_set_var(&sweep, time_glyph_arc_);
  lv_anim_set_values(&sweep, 0, 360);
  lv_anim_set_duration(&sweep, kSyncSweepMs);
  lv_anim_set_repeat_count(&sweep, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&sweep, [](void* var, int32_t value) {
    lv_arc_set_angles(static_cast<lv_obj_t*>(var), value,
                      value + kSyncSweepWidthDeg);
  });
  lv_anim_start(&sweep);
}

void UiManager::StartConnectingHalo() {
  lv_anim_delete(connection_glyph_halo_, nullptr);

  lv_anim_t halo;
  lv_anim_init(&halo);
  lv_anim_set_var(&halo, connection_glyph_halo_);
  lv_anim_set_values(&halo, kConnectingHaloMinOpa, kConnectingHaloMaxOpa);
  lv_anim_set_duration(&halo, kConnectingHaloMs);
  lv_anim_set_reverse_time(&halo, kConnectingHaloMs);
  lv_anim_set_repeat_count(&halo, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&halo, [](void* var, int32_t value) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var),
                            static_cast<lv_opa_t>(value), 0);
  });
  lv_anim_start(&halo);
}

}  // namespace ui
