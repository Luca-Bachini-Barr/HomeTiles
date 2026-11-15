#include "src/ui/assist/assist_screen.h"

#include <Arduino.h>
#include <lvgl.h>

#include <algorithm>

#include "src/audio/assist_view.h"
#include "src/audio/assist_view_request.h"
#include "src/audio/speaker.h"
#include "src/core/config/config_manager.h"
#include "src/core/display/display_manager.h"
#include "src/core/i18n/i18n.h"
#include "src/core/power/power_manager.h"
#include "src/fonts/ui_fonts.h"
#include "src/network/secure/command_channel.h"
#include "src/tiles/icons/mdi_icons.h"
#include "src/tiles/runtime/tile_renderer_shared.h"
#include "src/ui/popups/camera/camera_popup.h"
#include "src/ui/popups/popup_layout.h"
#include "src/ui/popups/popup_surface.h"
#include "src/ui/screensaver/image_screensaver.h"
#include "src/ui/shared/ui_pulse.h"
#include "src/ui/shared/ui_surface_style.h"
#include "src/ui/shared/ui_theme.h"

namespace assist_screen {
namespace {

using assist_view::Screen;
using i18n::AssistLabel;

constexpr const char* kLogPrefix = "[Assist]";
constexpr int kContentWidth = popup_layout::kContentWidth;
constexpr int kContentHeight = popup_layout::kCardHeight - 2 * popup_layout::kCardPad;
// While shown, the idle timer restarts this often (no screensaver mid-answer).
constexpr uint32_t kKeepAwakeMs = 1000;
constexpr uint32_t kLogIntervalMs = 10000;

struct Ui {
  lv_obj_t* overlay = nullptr;
  lv_obj_t* card = nullptr;
  lv_obj_t* disc = nullptr;
  lv_obj_t* icon = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* close = nullptr;
  lv_obj_t* body = nullptr;
  lv_obj_t* status = nullptr;
  lv_obj_t* heard = nullptr;
  lv_obj_t* answer = nullptr;
  bool light = false;
} ui;

assist_view::Model g_model;
// The parsed command (about 1.6 KB): static, not on the loop task's stack.
assist_view::Update g_update;
uint32_t g_drawn_revision = 0;
uint32_t g_wake_seen = 0;
bool g_wake_seen_valid = false;
uint32_t g_keep_awake_ms = 0;
uint32_t g_log_ms = 0;

const char* language() { return configManager.getConfig().language; }
const char* label(AssistLabel which) { return i18n::assist_label(language(), which); }

int body_top() {
  return popup_layout::kHeaderCenterY + popup_layout::kCloseButtonSize / 2 + popup_layout::scale(8) -
         popup_layout::kCardPad;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
  lv_obj_t* label = lv_label_create(parent);
  set_label_style(label, lv_color_hex(color), font);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, kContentWidth);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(label, "");
  return label;
}

void icon_pulse_exec(void* obj, int32_t) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), ui_pulse::opa_now(), 0);
}

void set_pulse(bool on) {
  if (!ui.icon) return;
  const bool running = lv_anim_get(ui.icon, icon_pulse_exec) != nullptr;
  if (on == running) return;
  if (on) {
    ui_pulse::start(ui.icon, icon_pulse_exec);
  } else {
    lv_anim_delete(ui.icon, icon_pulse_exec);
    lv_obj_set_style_opa(ui.icon, LV_OPA_COVER, 0);
  }
}

void on_close(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  g_model.dismiss(millis());
}

void on_overlay_delete(lv_event_t*) { ui = Ui{}; }

void ensure_ui() {
  const bool light = ui_theme::light();
  if (ui.overlay && ui.light == light) return;
  if (ui.overlay) lv_obj_delete(ui.overlay);  // on_overlay_delete clears ui
  const ui_theme::SurfaceScope theme = popup_surface::theme_scope();
  ui.light = light;

  // Over everything: popups and the camera indicator live on the top layer.
  ui.overlay = lv_obj_create(lv_layer_sys());
  lv_obj_remove_style_all(ui.overlay);
  lv_obj_set_size(ui.overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(ui.overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui.overlay, LV_OPA_50, 0);
  lv_obj_remove_flag(ui.overlay, LV_OBJ_FLAG_SCROLLABLE);
  // Swallows touches, so nothing underneath reacts while it shows.
  lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(ui.overlay, on_overlay_delete, LV_EVENT_DELETE, nullptr);
  // Below the system layer's rounded screen corners (ui_manager).
  lv_obj_move_to_index(ui.overlay, 0);

  // The popup card (popup_shell create_popup_body): size, radius, padding,
  // shadow and the standard header.
  ui.card = lv_obj_create(ui.overlay);
  lv_obj_set_size(ui.card, popup_layout::kCardWidth, popup_layout::kCardHeight);
  lv_obj_center(ui.card);
  lv_obj_set_style_bg_color(ui.card, lv_color_hex(ui_theme::popup_card()), 0);
  lv_obj_set_style_bg_opa(ui.card, LV_OPA_COVER, 0);
  ui_surface_style::apply_radius(ui.card, popup_layout::kCardRadius + Device::kGridGap, 0);
  lv_obj_set_style_border_width(ui.card, 0, 0);
  lv_obj_set_style_pad_all(ui.card, popup_layout::kCardPad, 0);
  if (!popup_layout::kCardFillsScreen) popup_layout::apply_card_shadow(ui.card);
  lv_obj_remove_flag(ui.card, LV_OBJ_FLAG_SCROLLABLE);

  ui.title = lv_label_create(ui.card);
  lv_obj_set_width(ui.title, LV_PCT(62));
  lv_obj_set_style_text_font(ui.title, popup_layout::headerTitleFont(), 0);
  lv_obj_set_style_text_color(ui.title, lv_color_hex(ui_theme::text()), 0);
  lv_label_set_long_mode(ui.title, LV_LABEL_LONG_DOT);
  lv_label_set_text(ui.title, "");
  ui.disc = popup_layout::createHeaderIconDisc(ui.card);
  ui.icon = lv_label_create(ui.card);
  lv_obj_set_style_text_font(ui.icon, FONT_MDI_ICONS, 0);
  popup_layout::applyIconScale(ui.icon);
  popup_layout::styleHeaderIcon(ui.icon);
  lv_obj_set_style_text_color(ui.icon, lv_color_hex(ui_theme::icon()), 0);
  lv_label_set_text(ui.icon, "");
  ui.close = popup_layout::createCloseButton(ui.card, on_close, nullptr);
  disable_pressed_button_animation(ui.close);
  for (lv_obj_t* object : {ui.title, ui.disc, ui.icon, ui.close}) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_IGNORE_LAYOUT);
  }

  // What was heard and the answer: a column that scrolls when the answer is
  // long.
  ui.body = lv_obj_create(ui.card);
  lv_obj_remove_style_all(ui.body);
  lv_obj_set_pos(ui.body, 0, body_top());
  lv_obj_set_size(ui.body, kContentWidth, std::max(1, kContentHeight - body_top()));
  lv_obj_add_flag(ui.body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(ui.body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(ui.body, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(ui.body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(ui.body, popup_layout::scale(18), 0);
  lv_obj_set_style_pad_top(ui.body, popup_layout::scale(12), 0);
  ui.heard = make_label(ui.body, popup_layout::font28(), ui_theme::text_secondary());
  ui.status = make_label(ui.body, popup_layout::font28(), ui_theme::text_secondary());
  ui.answer = make_label(ui.body, popup_layout::font32(), ui_theme::text());

  lv_label_set_text(ui.title, label(AssistLabel::Title));
  popup_layout::alignHeader(ui.card, ui.title, ui.icon, ui.disc);
  lv_obj_move_foreground(ui.icon);
  lv_obj_move_foreground(ui.title);
  lv_obj_move_foreground(ui.close);
}

void set_text(lv_obj_t* label, const char* text) {
  const bool empty = !text || !*text;
  if (empty) {
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
}

void wake_display(const char* reason) {
  hide_image_screensaver();
  powerManager.wakeFromDisplaySleep(reason);
  displayManager.resetActivityTimer();
  g_keep_awake_ms = millis();
}

void draw() {
  const Screen screen = g_model.screen();
  if (screen == Screen::Hidden) {
    if (!ui.overlay) return;
    set_pulse(false);
    lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const bool was_hidden = !ui.overlay || lv_obj_has_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
  ensure_ui();
  const ui_theme::SurfaceScope theme = popup_surface::theme_scope();
  lv_label_set_text(ui.title, label(AssistLabel::Title));

  const char* icon = "microphone";
  const char* status = nullptr;
  switch (screen) {
    case Screen::Listening:
      status = label(AssistLabel::Listening);
      break;
    case Screen::Processing:
      icon = "dots-horizontal";
      status = label(AssistLabel::Thinking);
      break;
    case Screen::Responding:
      icon = "volume-high";
      break;
    case Screen::Error:
      icon = "alert-circle-outline";
      status = label(g_model.error() == assist_view::ErrorKind::NoSpeech ? AssistLabel::NoSpeech
                                                                         : AssistLabel::Failed);
      break;
    case Screen::Hidden:
      break;
  }
  lv_label_set_text(ui.icon, getMdiChar(icon).c_str());
  set_pulse(screen == Screen::Listening || screen == Screen::Processing);
  // Heard first (what was said), then the stage line, then the answer.
  set_text(ui.heard, g_model.heard());
  set_text(ui.status, status);
  set_text(ui.answer, screen == Screen::Responding ? g_model.answer() : nullptr);
  if (was_hidden || screen != Screen::Responding) lv_obj_scroll_to_y(ui.body, 0, LV_ANIM_OFF);

  if (was_hidden) {
    // The full-screen camera draws past LVGL into the frame buffer.
    if (camera_popup_is_full_screen()) hide_camera_popup();
    lv_obj_remove_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(ui.overlay, 0);
    wake_display("assist");
  }
}

void redraw_if_changed() {
  if (g_model.revision() == g_drawn_revision) return;
  g_drawn_revision = g_model.revision();
  draw();
}

}  // namespace

bool handleCommandPayload(const uint8_t* payload, size_t length, bool sealed) {
  const assist_view::ParseResult result = assist_view::parseViewCommand(
      reinterpret_cast<const char*>(payload), length, &g_update);
  if (result == assist_view::ParseResult::NotView) return false;
  // A paired panel shows only what its Bridge sent encrypted.
  if (!sealed && command_channel::state() != command_channel::PairingState::Off) {
    if (millis() - g_log_ms >= kLogIntervalMs) {
      g_log_ms = millis();
      Serial.printf("%s Unencrypted view command ignored (panel is paired)\n", kLogPrefix);
    }
    return true;
  }
  if (result != assist_view::ParseResult::Ok) {
    if (millis() - g_log_ms >= kLogIntervalMs) {
      g_log_ms = millis();
      Serial.printf("%s Invalid view command ignored (%u bytes)\n", kLogPrefix,
                    static_cast<unsigned>(length));
    }
    return true;
  }
  g_model.apply(g_update, millis());
  redraw_if_changed();
  return true;
}

void service() {
  const uint32_t now = millis();
  const uint32_t wakes = speaker::wakeDetections();
  if (!g_wake_seen_valid) {
    g_wake_seen = wakes;
    g_wake_seen_valid = true;
  } else if (wakes != g_wake_seen) {
    g_wake_seen = wakes;
    g_model.onWake(now);
  }
  g_model.tick(now);
  redraw_if_changed();
  if (g_model.visible() && now - g_keep_awake_ms >= kKeepAwakeMs) {
    g_keep_awake_ms = now;
    displayManager.resetActivityTimer();
  }
}

void resetForTheme() {
  if (!ui.overlay) return;
  const bool shown = !lv_obj_has_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_delete(ui.overlay);
  if (shown) {
    // Rebuild in the new colors at once.
    g_drawn_revision = g_model.revision() - 1;
    redraw_if_changed();
  }
}

bool visible() { return g_model.visible(); }

}  // namespace assist_screen
