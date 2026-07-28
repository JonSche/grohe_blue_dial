#include "ui/ui_manager.hpp"

#include "esp_log.h"  // TODO(debug): remove with the temporary logging below

namespace ui {
namespace {
constexpr char kTag[] = "ui_manager";  // TODO(debug): remove
constexpr lv_color_t kBackgroundColor = LV_COLOR_MAKE(0x10, 0x14, 0x18);
constexpr lv_color_t kStatusColor = LV_COLOR_MAKE(0x8a, 0x93, 0xa6);
}  // namespace

void UiManager::Init(lv_display_t* display) {
  ESP_LOGI(kTag, "Init() entered, display=%p", (void*)display);  // TODO(debug): remove
  lv_obj_t* screen = lv_display_get_screen_active(display);
  ESP_LOGI(kTag, "active screen=%p", (void*)screen);  // TODO(debug): remove
  lv_obj_set_style_bg_color(screen, kBackgroundColor, 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  title_label_ = lv_label_create(screen);
  lv_label_set_text(title_label_, "Grohe Dial");
  lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
  lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_28, 0);
  lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, -10);

  status_label_ = lv_label_create(screen);
  lv_label_set_text(status_label_, "");
  lv_obj_set_style_text_color(status_label_, kStatusColor, 0);
  lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 30);
  ESP_LOGI(kTag, "Init() done, title_label=%p status_label=%p",
           (void*)title_label_, (void*)status_label_);  // TODO(debug): remove
}

void UiManager::SetStatusText(std::string_view text) {
  if (status_label_ == nullptr) {
    return;
  }
  lv_label_set_text_fmt(status_label_, "%.*s", static_cast<int>(text.size()),
                         text.data());
}

}  // namespace ui
