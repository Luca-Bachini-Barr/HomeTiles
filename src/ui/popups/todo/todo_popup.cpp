#include "src/ui/popups/todo/todo_popup.h"

#include <algorithm>
#include <cstring>

#include "src/core/i18n/i18n.h"
#include "src/fonts/ui_fonts.h"
#include "src/tiles/icons/mdi_icons.h"
#include "src/tiles/runtime/tile_icon_source.h"
#include "src/tiles/runtime/tile_renderer_fonts.h"
#include "src/tiles/runtime/tile_renderer_shared.h"
#include "src/types/calendar/calendar_model.h"
#include "src/types/calendar/calendar_text.h"
#include "src/types/todo/todo_store.h"
#include "src/types/todo/todo_text.h"
#include "src/types/todo/todo_tile.h"
#include "src/ui/navigation/view_navigation.h"
#include "src/ui/popups/calendar/calendar_popup.h"
#include "src/ui/popups/camera/camera_popup.h"
#include "src/ui/popups/climate/climate_popup.h"
#include "src/ui/popups/cover/cover_popup.h"
#include "src/ui/popups/device/device_popup.h"
#include "src/ui/popups/energy/energy_popup.h"
#include "src/ui/popups/light/light_popup.h"
#include "src/ui/popups/media/media_popup.h"
#include "src/ui/popups/pin/pin_popup.h"
#include "src/ui/popups/popup_layout.h"
#include "src/ui/popups/popup_open.h"
#include "src/ui/popups/popup_shell.h"
#include "src/ui/popups/popup_surface.h"
#include "src/ui/popups/sensor/sensor_popup.h"
#include "src/ui/popups/vacuum/vacuum_popup.h"
#include "src/ui/popups/weather/weather_popup.h"
#include "src/ui/shared/title_label.h"
#include "src/ui/shared/ui_theme.h"

namespace {

using i18n::TodoLabel;

constexpr int kContentWidth = popup_layout::kContentWidth;
constexpr int kContentHeight = popup_layout::kCardHeight - 2 * popup_layout::kCardPad;
const char* const kDefaultIcon = "format-list-bulleted";

struct Popup {
  lv_obj_t* overlay = nullptr;
  lv_obj_t* card = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* icon = nullptr;
  lv_obj_t* close = nullptr;
  lv_obj_t* list = nullptr;
  lv_obj_t* note = nullptr;
  TodoPopupTarget target;
  // The uids of the rows shown, by row index (a row's user data).
  String uids[todo_items::kMaxItems];
  uint32_t signature = 0;
  uint32_t card_rgb = popup_surface::default_card();
  bool open = false;
} pop;

// Body geometry in the card's content box: below the header to the bottom.
int body_top() {
  return popup_layout::kHeaderCenterY + popup_layout::kCloseButtonSize / 2 + popup_layout::scale(8) -
         popup_layout::kCardPad;
}
int note_height() { return lv_font_get_line_height(popup_layout::font20()) + popup_layout::scale(10); }

lv_color_t card_color() {
  return pop.card ? lv_obj_get_style_bg_color(pop.card, LV_PART_MAIN) : lv_color_hex(ui_theme::popup_card());
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
  lv_obj_t* label = lv_label_create(parent);
  set_label_style(label, lv_color_hex(color), font);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(label, "");
  return label;
}

// The alert color for overdue and alerted items: the Calendar's orange.
uint32_t alert_color() { return calendar_model::calendar_color(3, ui_theme::light()); }

uint32_t hash_text(uint32_t hash, const char* text) {
  for (const char* c = text ? text : ""; *c; ++c) hash = (hash ^ static_cast<uint8_t>(*c)) * 16777619u;
  return (hash ^ 0xFF) * 16777619u;
}

void refresh(bool show);

void on_row(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const uintptr_t row = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  if (row >= todo_items::kMaxItems || !pop.uids[row].length()) return;
  // Copied: complete() refreshes the popup, which rebuilds the rows.
  const String uid = pop.uids[row];
  const ui_theme::SurfaceScope popup_scope = popup_surface::scope();
  todo_store::complete(pop.target.entity, uid.c_str());
}

void message(const char* text) {
  lv_obj_t* label = make_label(pop.list, popup_layout::font20(), ui_theme::text_secondary());
  lv_obj_set_width(label, kContentWidth);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, 0, popup_layout::scale(40));
}

void fill_list(const todo_items::List* list) {
  const bool tickable = list && list->can_complete();
  uint32_t signature = 2166136261u;
  const char* text = nullptr;
  if (!list) {
    text = "--";
  } else if (!list->valid || !list->available) {
    text = todo_text::label(TodoLabel::NoData);
  } else if (list->items.empty()) {
    text = todo_text::label(TodoLabel::AllDone);
  }
  if (text) {
    signature = hash_text(signature, text);
  } else {
    const calendar_text::Now clock_now = calendar_text::now();
    for (const todo_items::Item& item : list->items) {
      signature = hash_text(signature, item.title);
      signature = hash_text(signature, item.uid);
      signature ^= static_cast<uint32_t>(item.due_day) * 2654435761u;
      signature ^= static_cast<uint32_t>(item.due_min + 1) * 40503u;
      signature ^= todo_text::overdue(item, clock_now) ? 0x3Cu : 0;
    }
    signature = hash_text(signature, pop.target.highlight_uid.c_str());
    signature ^= list->open_count * 40503u;
  }
  signature ^= tickable ? 0x77u : 0;
  signature ^= ui_theme::light() ? 0x55u : 0;
  const calendar_text::Now now = calendar_text::now();
  signature ^= static_cast<uint32_t>(now.valid ? now.today : -1) * 97u;
  lv_obj_t* highlighted = nullptr;
  if (signature == pop.signature) return;
  pop.signature = signature;
  lv_obj_clean(pop.list);
  for (String& uid : pop.uids) uid = "";
  if (text) {
    message(text);
    return;
  }
  const lv_font_t* font = popup_layout::font20();
  const lv_font_t* small = FONT_SMALL;
  const int32_t line = lv_font_get_line_height(font);
  const int32_t row_h = line + lv_font_get_line_height(small) + popup_layout::scale(14);
  const int32_t mark_w = std::max<int32_t>(12, line * 7 / 10);
  const int32_t text_x = mark_w + popup_layout::scale(16);
  int32_t y = 0;
  for (size_t i = 0; i < list->items.size() && i < todo_items::kMaxItems; ++i) {
    const todo_items::Item& item = list->items[i];
    lv_obj_t* row = lv_obj_create(pop.list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, kContentWidth, row_h);
    lv_obj_set_pos(row, 0, y);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_radius(row, popup_layout::scale(12), 0);
    lv_obj_set_style_bg_color(row, lv_color_mix(lv_color_hex(ui_theme::text()), card_color(), LV_OPA_10), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    if (pop.target.highlight_uid.length() && pop.target.highlight_uid == item.uid) {
      // The item a due alert opened the popup for.
      lv_obj_set_style_bg_color(row, lv_color_mix(lv_color_hex(alert_color()), card_color(), LV_OPA_30), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      highlighted = row;
    }
    if (tickable && item.can_tick()) {
      pop.uids[i] = item.uid;
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    } else {
      lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t* mark = lv_obj_create(row);
    lv_obj_remove_style_all(mark);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mark, mark_w, mark_w);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, popup_layout::scale(4), 0);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mark, std::max(2, static_cast<int>(popup_layout::scale(3))), 0);
    lv_obj_set_style_border_color(mark, lv_color_hex(ui_theme::text_secondary()), 0);
    lv_obj_set_style_border_opa(mark, tickable && item.can_tick() ? LV_OPA_COVER : LV_OPA_40, 0);
    const bool due = item.has_due();
    lv_obj_t* name = make_label(row, font, ui_theme::text());
    lv_obj_set_width(name, std::max<int32_t>(1, kContentWidth - text_x - popup_layout::scale(4)));
    lv_label_set_text(name, item.title);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, text_x, due ? -lv_font_get_line_height(small) / 2 : 0);
    if (due) {
      lv_obj_t* when = make_label(row, small, todo_text::overdue(item, now) ? alert_color() : ui_theme::text_secondary());
      lv_obj_set_width(when, std::max<int32_t>(1, kContentWidth - text_x - popup_layout::scale(4)));
      lv_label_set_text(when, todo_text::due_text(item).c_str());
      lv_obj_align(when, LV_ALIGN_LEFT_MID, text_x, line / 2 + popup_layout::scale(2));
    }
    y += row_h;
  }
  if (list->open_count > list->items.size()) {
    lv_obj_t* more = make_label(pop.list, font, ui_theme::text_secondary());
    lv_obj_set_width(more, kContentWidth - text_x);
    lv_label_set_text(more, todo_text::more(list->open_count - static_cast<uint32_t>(list->items.size())).c_str());
    lv_obj_set_pos(more, text_x, y + popup_layout::scale(8));
  }
  if (highlighted) lv_obj_scroll_to_view(highlighted, LV_ANIM_OFF);
}

void on_close(lv_event_t* e) {
  const ui_theme::SurfaceScope popup_scope = popup_surface::scope();
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) hide_todo_popup();
}

void on_overlay_delete(lv_event_t*) { pop = Popup{}; }

void ensure_popup() {
  if (pop.card) return;
  const auto parts = create_popup_body(on_close, nullptr, pop.card_rgb);
  pop.overlay = parts.overlay;
  lv_obj_add_event_cb(pop.overlay, on_overlay_delete, LV_EVENT_DELETE, nullptr);
  pop.card = parts.card;
  pop.title = parts.title;
  pop.icon = parts.icon;
  pop.close = parts.close;
  disable_pressed_button_animation(parts.close);

  pop.list = lv_obj_create(pop.card);
  lv_obj_remove_style_all(pop.list);
  lv_obj_set_pos(pop.list, 0, body_top());
  lv_obj_set_size(pop.list, kContentWidth, std::max(1, kContentHeight - body_top()));
  lv_obj_add_flag(pop.list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(pop.list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(pop.list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(pop.list, LV_OBJ_FLAG_SCROLL_ELASTIC);

  pop.note = make_label(pop.card, popup_layout::font20(), ui_theme::text_secondary());
  lv_obj_set_width(pop.note, kContentWidth);
  lv_obj_set_style_text_align(pop.note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(pop.note, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(pop.note, LV_OBJ_FLAG_HIDDEN);

  lv_obj_move_foreground(pop.icon);
  lv_obj_move_foreground(pop.title);
  lv_obj_move_foreground(pop.close);
  lv_obj_add_flag(pop.card, LV_OBJ_FLAG_HIDDEN);
}

void refresh(bool show) {
  if (!pop.target.entity.length() && !show) return;
  ensure_popup();
  const todo_items::List* list = todo_store::get(pop.target.entity);
  lv_obj_set_style_bg_color(pop.card, lv_color_hex(pop.card_rgb), 0);
  const String icon = pop.target.icon_name.length() ? pop.target.icon_name : String(kDefaultIcon);
  lv_label_set_text(pop.icon, pop.target.icon_visible ? getMdiChar(icon).c_str() : "");
  const bool open = list && list->valid && list->available && list->open_count;
  lv_obj_set_style_text_color(pop.icon, lv_color_hex(open ? ui_theme::icon() : ui_theme::icon_inactive()), 0);
  const String title = pop.target.title.length() ? pop.target.title : String(todo_text::label(TodoLabel::Type));
  hometiles_title::set(pop.title, title.c_str());
  popup_layout::alignHeader(pop.card, pop.title, pop.icon);

  // A list Home Assistant cannot update says so under its items.
  const bool read_only = list && list->valid && list->available && !list->items.empty() && !list->can_complete();
  const int list_h = std::max(1, kContentHeight - body_top() - (read_only ? note_height() : 0));
  lv_obj_set_height(pop.list, list_h);
  if (read_only) {
    lv_label_set_text(pop.note, todo_text::label(TodoLabel::ReadOnly));
    lv_obj_set_style_text_color(pop.note, lv_color_hex(ui_theme::text_secondary()), 0);
    lv_obj_remove_flag(pop.note, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(pop.note, LV_OBJ_FLAG_HIDDEN);
  }
  fill_list(list);

  if (show) {
    pop.open = true;
    if (!pop.target.highlight_uid.length()) lv_obj_scroll_to_y(pop.list, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(pop.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pop.overlay, LV_OBJ_FLAG_CLICKABLE);
    viewNavigationPopupShown(pop.card, pop.target.entity.c_str());
    show_popup_shell(pop.overlay, pop.card, pop.title, pop.icon, pop.close, nullptr, nullptr);
  } else if (pop.open) {
    sync_popup_shell();
  }
}

uint32_t theme_default_card() {
  const ui_theme::SurfaceScope theme = popup_surface::theme_scope();
  return popup_surface::card_or_default(tileDefaultBgColor());
}

}  // namespace

void show_todo_popup(const TodoPopupTarget& target) {
  const ui_theme::SurfaceScope popup_scope = popup_surface::theme_scope();
  hide_pin_popup();
  hide_light_popup();
  hide_climate_popup();
  hide_cover_popup();
  hide_sensor_popup();
  hide_weather_popup();
  hide_energy_popup();
  hide_media_popup();
  hide_camera_popup();
  hide_device_popup();
  hide_vacuum_popup();
  hide_calendar_popup();
  if (!todo_store::valid_entity(target.entity)) return;
  pop.target = target;
  pop.signature = 0;
  if (!todo_tile_alive(target.source)) pop.target.source = nullptr;
  const uint32_t fallback = theme_default_card();
  if (!pop.target.source) tile_icon_source::open_popup_without_tile();
  pop.card_rgb = pop.target.source ? tile_icon_source::popup_background(pop.target.source, fallback) : fallback;
  const ui_theme::SurfaceScope open_scope = popup_surface::scope();
  refresh(true);
}

void hide_todo_popup() {
  if (!pop.card || !pop.overlay) return;
  pop.open = false;
  hide_popup_shell(pop.card);
  cancel_popup_open(pop.card);
  lv_obj_add_flag(pop.card, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(pop.overlay, LV_OBJ_FLAG_CLICKABLE);
}

void preload_todo_popup() {
  const ui_theme::SurfaceScope popup_scope = popup_surface::theme_scope();
  ensure_popup();
}

void todo_popup_follow_tile_color(uint32_t color) {
  const ui_theme::SurfaceScope popup_scope = popup_surface::scope();
  if (!pop.open || !pop.card) return;
  pop.card_rgb = color;
  lv_obj_set_style_bg_color(pop.card, lv_color_hex(color), 0);
  popup_shell_follow_tile_color(color);
}

void todo_popup_refresh(const String& entity) {
  if (!pop.open) return;
  if (entity != "*" && entity != pop.target.entity) return;
  const ui_theme::SurfaceScope popup_scope = popup_surface::scope();
  refresh(false);
}
