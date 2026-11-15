#include "src/ui/assist/assist_screen.h"

#include <Arduino.h>
#include <lvgl.h>

#include <algorithm>
#include <math.h>

#include "src/audio/assist_view.h"
#include "src/audio/assist_view_request.h"
#include "src/audio/assist_wave.h"
#include "src/audio/speaker.h"
#include "src/core/config/config_manager.h"
#include "src/core/display/display_manager.h"
#include "src/core/i18n/i18n.h"
#include "src/core/power/power_manager.h"
#include "src/network/secure/command_channel.h"
#include "src/tiles/runtime/tile_renderer_shared.h"
#include "src/ui/popups/camera/camera_popup.h"
#include "src/ui/popups/popup_layout.h"
#include "src/ui/screensaver/image_screensaver.h"

// The Assist screen (user design 2026-11-15): the whole screen black, a wave
// in the middle drawn from the voice (the speaker's microphone while they
// talk, a slow breath while Home Assistant thinks, the spoken answer in a
// second color), and below it what was understood and the answer. A tap
// anywhere closes it. It is not a popup card: nothing else of the UI shows.
namespace assist_screen {
namespace {

using assist_view::Screen;
using i18n::AssistLabel;
using assist_wave::kBars;

constexpr const char* kLogPrefix = "[Assist]";
// While shown, the idle timer restarts this often (no screensaver mid-answer).
constexpr uint32_t kKeepAwakeMs = 1000;
constexpr uint32_t kLogIntervalMs = 10000;
constexpr uint32_t kFrameMs = 33;

// Always on black, in both themes.
constexpr uint32_t kVoiceColor = 0x4FC3F7;   // the speaker: light blue
constexpr uint32_t kAnswerColor = 0xFFB74D;  // the answer: orange
constexpr uint32_t kErrorColor = 0x616161;
constexpr uint32_t kHeardColor = 0x9E9E9E;
constexpr uint32_t kAnswerTextColor = 0xFFFFFF;

// Geometry from the screen: the wave in the upper middle, the text below.
constexpr int kScreenW = SCREEN_WIDTH;
constexpr int kScreenH = SCREEN_HEIGHT;
constexpr int kShort = kScreenW < kScreenH ? kScreenW : kScreenH;
constexpr int kWaveWidth = kScreenW * 7 / 10;
constexpr int kBarPitch = kWaveWidth / kBars;
constexpr int kBarWidth = kBarPitch * 11 / 20 > 3 ? kBarPitch * 11 / 20 : 3;
constexpr int kBarMax = kShort * 3 / 10;
constexpr int kWaveCenterY = kScreenH * 34 / 100;
constexpr int kTextTop = kWaveCenterY + kBarMax / 2 + kShort / 20;
constexpr int kTextWidth = kScreenW * 84 / 100;

struct Ui {
  lv_obj_t* overlay = nullptr;
  lv_obj_t* bars[kBars] = {};
  int16_t bar_height[kBars] = {};
  lv_obj_t* text = nullptr;
  lv_obj_t* status = nullptr;
  lv_obj_t* heard = nullptr;
  lv_obj_t* answer = nullptr;
  lv_timer_t* timer = nullptr;
  uint32_t color = 0;
} ui;

assist_view::Model g_model;
// The parsed command (about 1.6 KB): static, not on the loop task's stack.
assist_view::Update g_update;
assist_wave::Wave g_wave;
uint32_t g_drawn_revision = 0;
uint32_t g_wake_seen = 0;
bool g_wake_seen_valid = false;
uint32_t g_keep_awake_ms = 0;
uint32_t g_log_ms = 0;
// The full-screen camera was paused for this screen (camera_popup.h).
bool g_camera_paused = false;

const char* language() { return configManager.getConfig().language; }
const char* label(AssistLabel which) { return i18n::assist_label(language(), which); }

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
  lv_obj_t* label = lv_label_create(parent);
  set_label_style(label, lv_color_hex(color), font);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, kTextWidth);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(label, "");
  return label;
}

void on_tap(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  g_model.dismiss(millis());
}

void on_overlay_delete(lv_event_t*) {
  if (ui.timer) lv_timer_delete(ui.timer);
  ui = Ui{};
}

void set_wave_color(uint32_t color) {
  if (ui.color == color) return;
  ui.color = color;
  for (lv_obj_t* bar : ui.bars) lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
}

assist_wave::Source wave_source(Screen screen) {
  switch (screen) {
    case Screen::Listening: return assist_wave::Source::Voice;
    case Screen::Processing: return assist_wave::Source::Breath;
    case Screen::Responding: return assist_wave::Source::Answer;
    case Screen::Error:
    case Screen::Hidden: break;
  }
  return assist_wave::Source::Flat;
}

// One frame of the wave: the newest level in the middle, older ones move
// outwards on both sides.
void on_frame(lv_timer_t*) {
  if (!ui.overlay) return;
  const uint32_t now = millis();
  const uint8_t mic = speaker::takeMicLevel();
  const uint8_t out = speaker::takeOutputLevel();
  g_wave.step(wave_source(g_model.screen()), mic, out, now);
  for (int i = 0; i < kBars; ++i) {
    const int16_t height = static_cast<int16_t>(assist_wave::barHeight(
        g_wave.level(assist_wave::distanceFromMiddle(i)), assist_wave::distanceFromMiddle(i),
        kBarWidth, kBarMax));
    if (height == ui.bar_height[i]) continue;
    ui.bar_height[i] = height;
    lv_obj_set_height(ui.bars[i], height);
  }
}

void ensure_ui() {
  if (ui.overlay) return;
  // Over everything: popups and the camera indicator live on the top layer.
  ui.overlay = lv_obj_create(lv_layer_sys());
  lv_obj_remove_style_all(ui.overlay);
  lv_obj_set_size(ui.overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(ui.overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui.overlay, LV_OPA_COVER, 0);
  lv_obj_remove_flag(ui.overlay, LV_OBJ_FLAG_SCROLLABLE);
  // Swallows touches; a tap closes the screen.
  lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ui.overlay, on_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(ui.overlay, on_overlay_delete, LV_EVENT_DELETE, nullptr);
  // Below the system layer's rounded screen corners (ui_manager).
  lv_obj_move_to_index(ui.overlay, 0);

  for (int i = 0; i < kBars; ++i) {
    lv_obj_t* bar = lv_obj_create(ui.overlay);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(bar, kBarWidth, kBarWidth);
    // Vertically centred on the wave's line (LEFT_MID), so a new height grows
    // up and down alike.
    const int x = (i - kBars / 2) * kBarPitch;
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, kScreenW / 2 + x - kBarWidth / 2,
                 kWaveCenterY - kScreenH / 2);
    ui.bars[i] = bar;
    ui.bar_height[i] = kBarWidth;
  }

  // What was understood and the answer: a column that scrolls when the
  // answer is long (a tap on it closes the screen too).
  ui.text = lv_obj_create(ui.overlay);
  lv_obj_remove_style_all(ui.text);
  lv_obj_set_pos(ui.text, (kScreenW - kTextWidth) / 2, kTextTop);
  lv_obj_set_size(ui.text, kTextWidth, std::max(1, kScreenH - kTextTop - kShort / 20));
  lv_obj_add_flag(ui.text, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(ui.text, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(ui.text, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(ui.text, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ui.text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(ui.text, popup_layout::scale(14), 0);
  lv_obj_add_event_cb(ui.text, on_tap, LV_EVENT_CLICKED, nullptr);
  ui.status = make_label(ui.text, popup_layout::font24(), kHeardColor);
  ui.heard = make_label(ui.text, popup_layout::font28(), kHeardColor);
  ui.answer = make_label(ui.text, popup_layout::font32(), kAnswerTextColor);
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

void hide() {
  if (ui.timer) lv_timer_pause(ui.timer);
  if (ui.overlay) lv_obj_add_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
  // The camera's full screen comes back once the UI underneath shows again.
  if (g_camera_paused) {
    g_camera_paused = false;
    camera_popup_resume_full_screen();
  }
}

void draw() {
  const Screen screen = g_model.screen();
  if (screen == Screen::Hidden) {
    hide();
    return;
  }
  const bool was_hidden = !ui.overlay || lv_obj_has_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
  ensure_ui();

  const char* status = nullptr;
  uint32_t color = kVoiceColor;
  switch (screen) {
    case Screen::Listening:
      status = label(AssistLabel::Listening);
      break;
    case Screen::Processing:
      status = label(AssistLabel::Thinking);
      break;
    case Screen::Responding:
      color = kAnswerColor;
      break;
    case Screen::Error:
      color = kErrorColor;
      status = label(g_model.error() == assist_view::ErrorKind::NoSpeech ? AssistLabel::NoSpeech
                                                                         : AssistLabel::Failed);
      break;
    case Screen::Hidden:
      break;
  }
  set_wave_color(color);
  // The stage line only while nothing was understood yet (or on an error).
  const bool heard = g_model.heard()[0] != '\0';
  set_text(ui.status, heard && screen != Screen::Error ? nullptr : status);
  set_text(ui.heard, g_model.heard());
  set_text(ui.answer, screen == Screen::Responding ? g_model.answer() : nullptr);
  if (was_hidden || screen != Screen::Responding) lv_obj_scroll_to_y(ui.text, 0, LV_ANIM_OFF);

  if (was_hidden) {
    // The full-screen camera draws past LVGL into the frame buffer: paused
    // while this screen shows, back afterwards.
    g_camera_paused = camera_popup_pause_full_screen() || g_camera_paused;
    g_wave.reset();
    lv_obj_remove_flag(ui.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(ui.overlay, 0);
    if (!ui.timer) {
      ui.timer = lv_timer_create(on_frame, kFrameMs, nullptr);
    } else {
      lv_timer_resume(ui.timer);
    }
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
  // A paired panel shows only what its Bridge sent encrypted: an unencrypted
  // view command is refused before it is parsed.
  if (!sealed && command_channel::state() != command_channel::PairingState::Off) {
    if (!assist_view::looksLikeViewCommand(reinterpret_cast<const char*>(payload), length)) {
      return false;
    }
    if (millis() - g_log_ms >= kLogIntervalMs) {
      g_log_ms = millis();
      Serial.printf("%s Unencrypted view command ignored (panel is paired)\n", kLogPrefix);
    }
    return true;
  }
  const assist_view::ParseResult result = assist_view::parseViewCommand(
      reinterpret_cast<const char*>(payload), length, &g_update);
  if (result == assist_view::ParseResult::NotView) return false;
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
  // Always black: the theme does not change it.
}

bool visible() { return g_model.visible(); }

}  // namespace assist_screen
