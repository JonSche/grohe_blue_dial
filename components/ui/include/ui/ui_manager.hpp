#pragma once

#include "dial_state/dial_state.hpp"
#include "lvgl.h"

namespace ui {

// Builds and owns the LVGL screen tree for the Grohe Dial's main screen: a
// circular progress ring showing the pour amount, the amount itself (or,
// while Finished, a checkmark), the selected water type, a hint, two tiny
// connectivity glyphs, and a small travelling highlight during Dispensing.
// Every widget is created once in Init(); Render() only ever updates their
// content/value from the given DialState and starts/stops the small set of
// LVGL animations this screen uses (see Render()'s own comment on why that
// still keeps it a pure function of DialState) -- it never contains
// business logic, and DialState (not any widget) is the single source of
// truth. This is the implementation of docs/ui/dispense_animation_mockups.md,
// the frozen, approved UI specification (M11) -- consult it, not this file,
// for *why* any of this looks the way it does.
//
// Callers must hold the LVGL lock (display::Gc9a01Display::Lock()) for the
// duration of Init() and Render(), since LVGL itself isn't thread-safe.
class UiManager {
 public:
  UiManager() = default;

  // Builds the dial screen on the given LVGL display. Widgets start empty;
  // call Render() immediately after to populate them from the real state.
  void Init(lv_display_t* display);

  // Updates every widget to reflect state. Safe to call repeatedly; this is
  // the only way the screen's content ever changes.
  void Render(const dial_state::DialState& state);

 private:
  // The four small, one-shot-or-looping LVGL animations this screen ever
  // starts, each called from Render() exactly on the state edge it belongs
  // to (see Render()'s own comments at each call site) -- never on every
  // Render() call while a state is merely unchanged.
  void StartPulse();
  void StartHighlight(int amount_ml);
  void StartSyncSweep();
  void StartConnectingHalo();


  // The one invariant ring (frozen UI spec): always the selected
  // dispense amount's position on the 100-2000 ml scale, set once per
  // state entry, never animated frame-by-frame. Render() feeds it
  // state.active_dispense_amount_ml while a dispense/stop/finish is in
  // progress, state.amount_ml otherwise -- see that field's own comment on
  // dial_state.hpp for why those are different things.
  lv_obj_t* arc_ = nullptr;

  // The travelling highlight: a second, small arc layered on top of
  // arc_, sharing its size/position/rotation, visible and animated only
  // while Dispensing. Never resized or recolored to represent progress --
  // its only two visual states are "oscillating within the dialled arc"
  // (the common case) and "breathing in place" (the small-arc fallback)
  // -- both driven by lv_anim_t, not a custom timer.
  lv_obj_t* highlight_arc_ = nullptr;

  // "500 ml" as one composed row; hidden and replaced by checkmark_label_
  // while Finished.
  lv_obj_t* amount_row_ = nullptr;
  lv_obj_t* amount_label_ = nullptr;
  lv_obj_t* checkmark_label_ = nullptr;

  // Water type -- opacity steps down while Dispensing/Stopping, restored
  // otherwise. Never animated; a one-time style change at the state
  // transition costs nothing beyond what the label already costs to
  // exist.
  lv_obj_t* water_type_label_ = nullptr;

  // M2's static hint, now the full presentation-priority ladder Render()
  // computes from connection_status/time_status/dispense_status -- see
  // Render()'s own implementation. As of M11.1 this is the only status
  // text on screen: it no longer shares the ring's lower half with a
  // separate protocol-response readout (removed -- see dial_state.hpp's
  // appliance_response_received comment for where that data still lives,
  // now log-only).
  lv_obj_t* hint_label_ = nullptr;

  // The two connectivity glyphs ("Interior Crown" placement): each
  // rendered from two objects toggled by visibility rather than one
  // object with three visual states, since "hollow ring" and "solid dot"
  // are different shapes, not different styles of the same shape.
  // Time glyph: a tiny ring (hollow when invalid, a sweeping arc while
  // syncing) plus a tiny solid dot (valid).
  lv_obj_t* time_glyph_arc_ = nullptr;
  lv_obj_t* time_glyph_dot_ = nullptr;
  // Connection glyph: a tiny dot (hollow outline when disconnected, solid
  // when connected) plus a soft halo shown only while connecting.
  lv_obj_t* connection_glyph_dot_ = nullptr;
  lv_obj_t* connection_glyph_halo_ = nullptr;

  // Edge-detection for animation lifecycle only -- Render() remains a pure
  // function of the DialState it's given (the same widgets end up in the
  // same visual state for the same input every time); these exist purely
  // so an lv_anim_t is started/stopped once per transition rather than
  // re-started on every Render() call while a state hasn't changed (which
  // would otherwise glitch/restart a continuous animation each time some
  // unrelated field, e.g. delivered_ml, triggers a re-render).
  dial_state::DispenseStatus last_dispense_status_ =
      dial_state::DispenseStatus::kIdle;
  dial_state::ConnectionStatus last_connection_status_ =
      dial_state::ConnectionStatus::kConnecting;
  dial_state::TimeStatus last_time_status_ = dial_state::TimeStatus::kSyncing;

  // Same purpose as the three trackers above, but for the ring's accent-
  // vs-desaturated colour specifically: unlike lv_arc_set_value() (which
  // no-ops internally if the value is unchanged), LVGL's generic style
  // path has no such guard -- lv_obj_set_style_arc_color() invalidates the
  // whole ring every time it's called, whether or not the colour actually
  // changed. Without this, Render() would force a full ring redraw on
  // every count-up tick during Dispensing. Deliberately initialised to
  // `true` (the opposite of the real initial "not ready" state at boot),
  // so the first real Render() call is always guaranteed to apply the
  // correct colour rather than skip it.
  bool last_ring_ready_ = true;
};

}  // namespace ui
