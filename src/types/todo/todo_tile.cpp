#include "src/ui/shared/ui_surface_style.h"
#include "src/types/todo/todo_tile.h"

#include <algorithm>
#include <cstring>

#include "src/core/config/config_manager.h"
#include "src/core/i18n/i18n.h"
#include "src/tiles/config/tile_geometry.h"
#include "src/tiles/icons/mdi_icons.h"
#include "src/tiles/runtime/compact_sensor_layout.h"
#include "src/tiles/runtime/tile_icon_disc.h"
#include "src/tiles/runtime/tile_renderer_fonts.h"
#include "src/tiles/runtime/tile_renderer_shared.h"
#include "src/types/todo/todo_store.h"
#include "src/types/todo/todo_text.h"
#include "src/ui/popups/todo/todo_popup.h"
#include "src/ui/shared/title_label.h"
#include "src/ui/shared/tone_color.h"
#include "src/ui/shared/ui_theme.h"

namespace {

using i18n::TodoLabel;

// Compact: half height; Count: 1x1; Next: one row, two cells wide; List: two
// rows and more.
enum class Layout : uint8_t { Compact, Count, Next, List };

constexpr size_t kMaxRows = 12;
const char* const kDefaultIcon = "format-list-bulleted";

struct View {
  View* next = nullptr;
  GridType grid = GridType::TAB0;
  uint8_t index = 0;
  String entity;
  String title;
  String icon_name;
  bool icon_visible = true;
  Layout layout = Layout::Next;
  lv_obj_t* card = nullptr;
  lv_obj_t* icon = nullptr;
  lv_obj_t* title_label = nullptr;
  lv_obj_t* when = nullptr;
  lv_obj_t* what = nullptr;
  lv_obj_t* list = nullptr;
  int16_t content_y = 0;
  int16_t content_w = 0;
  int16_t content_h = 0;
  uint32_t signature = 0;
};

View* g_views = nullptr;

uint32_t hash_text(uint32_t hash, const char* text) {
  for (const char* c = text ? text : ""; *c; ++c) hash = (hash ^ static_cast<uint8_t>(*c)) * 16777619u;
  return (hash ^ 0xFF) * 16777619u;
}

lv_obj_t* plain_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
  lv_obj_t* label = lv_label_create(parent);
  set_label_style(label, lv_color_hex(color), font);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(label, "");
  return label;
}

lv_obj_t* plain_box(lv_obj_t* parent) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

void set_text(lv_obj_t* label, const char* text) {
  if (label && std::strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// The largest of the content fonts whose line fits `height`.
const lv_font_t* font_for(int height) {
  const lv_font_t* fonts[] = {tile_layout::content_font_28(), tile_layout::content_font_24(),
                              tile_layout::content_font_20(), FONT_SMALL};
  for (const lv_font_t* font : fonts) {
    if (lv_font_get_line_height(font) <= height) return font;
  }
  return FONT_SMALL;
}

// The icon is bright while something is open, dimmed when all is done or
// nothing is known.
void color_icon(View* view, const todo_items::List* list) {
  if (!view->icon) return;
  const bool open = list && list->valid && list->available && list->open_count;
  const uint32_t color = open ? ui_theme::icon() : ui_theme::icon_inactive();
  if (view->layout == Layout::Compact) {
    lv_obj_set_style_text_color(view->icon, lv_color_hex(color), 0);
  } else {
    tile_icon_disc::set_icon_color(view->icon, lv_color_hex(color));
  }
}

void show_count(View* view, const todo_items::List* list) {
  const String count = todo_text::count_value(list);
  const String caption = todo_text::count_caption(list);
  if (view->layout == Layout::Compact) {
    set_text(view->when, view->title.length() ? view->title.c_str() : todo_text::label(TodoLabel::Type));
    set_text(view->what, caption.c_str());
    return;
  }
  set_text(view->what, count.c_str());
  set_text(view->when, caption.c_str());
}

void show_next(View* view, const todo_items::List* list) {
  const char* what = "--";
  if (list && list->valid && list->available) {
    what = list->items.empty() ? todo_text::label(TodoLabel::AllDone) : list->items.front().title;
  } else if (list && list->valid) {
    what = todo_text::label(TodoLabel::NoData);
  }
  set_text(view->what, what);
  set_text(view->when, todo_text::count_caption(list).c_str());
}

void show_list(View* view, const todo_items::List* list) {
  const lv_font_t* font = tile_layout::content_font_20();
  const int32_t row_h = lv_font_get_line_height(font) + tile_layout::scale_480(10);
  const char* message = nullptr;
  size_t rows = 0;
  uint32_t more = 0;
  if (!list) {
    message = "--";
  } else if (!list->valid || !list->available) {
    message = todo_text::label(TodoLabel::NoData);
  } else if (list->items.empty()) {
    message = todo_text::label(TodoLabel::AllDone);
  } else {
    const size_t fit = static_cast<size_t>(std::max<int32_t>(1, view->content_h / row_h));
    rows = std::min({list->items.size(), fit, kMaxRows});
    // The last row says how many more there are when not everything fits.
    if (list->open_count > rows) {
      if (rows == fit && rows > 1) --rows;
      more = list->open_count - static_cast<uint32_t>(rows);
    }
  }
  uint32_t signature = 2166136261u;
  if (message) signature = hash_text(signature, message);
  for (size_t i = 0; i < rows; ++i) signature = hash_text(signature, list->items[i].title);
  signature ^= more * 2654435761u;
  signature ^= ui_theme::light() ? 0x55u : 0;
  if (signature == view->signature) return;
  view->signature = signature;
  lv_obj_clean(view->list);
  if (message) {
    lv_obj_t* label = plain_label(view->list, font, ui_theme::text_secondary());
    lv_obj_set_width(label, view->content_w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, message);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return;
  }
  const int32_t line = lv_font_get_line_height(font);
  // An empty circle per item, like an unticked box in Home Assistant's list.
  const int32_t mark_w = std::max<int32_t>(8, line * 6 / 10);
  const int32_t text_x = mark_w + tile_layout::scale_480(12);
  int32_t y = 0;
  for (size_t i = 0; i < rows; ++i) {
    lv_obj_t* mark = plain_box(view->list);
    lv_obj_set_size(mark, mark_w, mark_w);
    lv_obj_set_pos(mark, 0, y + (row_h - mark_w) / 2);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mark, std::max<int32_t>(1, tile_layout::scale_480(2)), 0);
    lv_obj_set_style_border_color(mark, lv_color_hex(ui_theme::text_secondary()), 0);
    lv_obj_set_style_border_opa(mark, LV_OPA_COVER, 0);
    lv_obj_t* name = plain_label(view->list, font, ui_theme::text());
    lv_obj_set_width(name, std::max<int32_t>(1, view->content_w - text_x));
    lv_obj_set_pos(name, text_x, y + (row_h - line) / 2);
    lv_label_set_text(name, list->items[i].title);
    y += row_h;
  }
  if (more) {
    lv_obj_t* label = plain_label(view->list, font, ui_theme::text_secondary());
    lv_obj_set_width(label, std::max<int32_t>(1, view->content_w - text_x));
    lv_obj_set_pos(label, text_x, y + (row_h - line) / 2);
    lv_label_set_text(label, todo_text::more(more).c_str());
  }
}

void show(View* view) {
  if (!view || !view->card) return;
  const ui_theme::SurfaceScope surface = tile_surface::scope(view->card);
  const todo_items::List* list = view->entity.length() ? todo_store::get(view->entity) : nullptr;
  color_icon(view, list);
  switch (view->layout) {
    case Layout::Compact:
    case Layout::Count: show_count(view, list); break;
    case Layout::Next: show_next(view, list); break;
    case Layout::List: show_list(view, list); break;
  }
}

void unregister(View* view) {
  for (View** link = &g_views; *link; link = &(*link)->next) {
    if (*link == view) {
      *link = view->next;
      break;
    }
  }
}

void on_card_event(lv_event_t* e) {
  View* view = static_cast<View*>(lv_event_get_user_data(e));
  if (!view) return;
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_DELETE) {
    unregister(view);
    delete view;
    return;
  }
  if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
  if (!view->entity.length()) return;
  TodoPopupTarget target;
  target.entity = view->entity;
  target.title = view->title;
  target.icon_name = view->icon_name;
  target.icon_visible = view->icon_visible;
  target.source = view->card;
  finish_press_before_popup(e);
  show_todo_popup(target);
}

Layout layout_of(const Tile& tile) {
  if (tile_geometry::compact_todo(tile.type, tile.span_w, tile.span_h)) return Layout::Compact;
  if (tile.span_h >= 2) return Layout::List;
  return tile.span_w >= 2 ? Layout::Next : Layout::Count;
}

}  // namespace

void todo_tiles_refresh(const String& entity) {
  const bool all = entity == "*";
  for (View* view = g_views; view; view = view->next) {
    if (all || view->entity == entity) show(view);
  }
}

bool todo_tile_alive(lv_obj_t* card) {
  if (!card) return false;
  for (View* view = g_views; view; view = view->next) {
    if (view->card == card) return true;
  }
  return false;
}

lv_obj_t* render_todo_tile(lv_obj_t* parent, int col, int row, const Tile& tile, uint8_t index, GridType grid_type) {
  lv_obj_t* card = lv_button_create(parent);
  const uint32_t color = tileBgColorOrDefault(tile, tileDefaultBgColor());
  lv_obj_set_style_bg_color(card, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_color(card, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
  const uint32_t pressed_color = tone_color::pressed_card(color);
  lv_obj_set_style_bg_color(card, lv_color_hex(pressed_color), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_grad_color(card, lv_color_hex(pressed_color), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_NONE, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  ui_surface_style::apply_radius(card, tile_layout::scale_480(22), 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  // The Sensor card paddings, like the Calendar tile.
  const int pad_x = tile_layout::scale_480(20);
  const int pad_y = tile_layout::scale_480(24);
  lv_obj_set_style_pad_hor(card, pad_x, 0);
  lv_obj_set_style_pad_ver(card, pad_y, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  disable_pressed_button_animation(card);
  place_tile_card(card, col, row, tile);

  View* view = new View();
  view->grid = grid_type;
  view->index = index;
  view->entity = tile.sensor_entity;
  view->entity.trim();
  if (!todo_store::valid_entity(view->entity)) view->entity = "";
  view->card = card;
  view->title = tile.title;
  view->icon_visible = !isMdiIconDisabled(tile.icon_name);
  view->icon_name = view->icon_visible ? normalizeMdiIconName(tile.icon_name) : String();
  view->layout = layout_of(tile);
  const ui_theme::SurfaceScope surface = tile_surface::scope(card);

  if (view->icon_visible && FONT_MDI_ICONS) {
    view->icon = lv_label_create(card);
    set_label_style(view->icon, lv_color_hex(ui_theme::icon_inactive()), FONT_MDI_ICONS);
    lv_label_set_text(view->icon,
                      getMdiChar(view->icon_name.length() ? view->icon_name : String(kDefaultIcon)).c_str());
    lv_obj_align(view->icon, LV_ALIGN_TOP_LEFT, tile_layout::scale_480(-8), tile_layout::scale_480(-8));
  }

  const int card_w = tile_geometry::extent(tile.col, std::max(1.0f, tile.span_w), GRID_CELL_W, GRID_GAP);
  const int card_h = tile_geometry::extent(tile.row, std::max(0.5f, tile.span_h), GRID_CELL_H, GRID_GAP);

  if (view->layout == Layout::Compact) {
    view->when = plain_label(card, compact_sensor_layout::title_font(), ui_theme::text_secondary());
    view->what = plain_label(card, compact_sensor_layout::value_font(tile.sensor_value_font), ui_theme::text());
    compact_sensor_layout::apply(card, view->icon, view->when, view->what, tile);
  } else {
    if (view->icon) tile_icon_disc::add_round(card, view->icon);
    // The Sensor tile's tall header: the title top right.
    if (tile.title.length()) {
      view->title_label = lv_label_create(card);
      set_label_style(view->title_label, lv_color_hex(ui_theme::text()), tile_layout::header_title_font());
      lv_label_set_long_mode(view->title_label, LV_LABEL_LONG_DOT);
      lv_obj_set_width(view->title_label, LV_PCT(70));
      lv_obj_set_style_text_align(view->title_label, LV_TEXT_ALIGN_RIGHT, 0);
      hometiles_title::tile(view->title_label, tile.title.c_str(), true);
      lv_obj_align(view->title_label, LV_ALIGN_TOP_RIGHT, tile_layout::scale_480(4), tile_layout::scale_480(4));
    }
    // The content below the header row, in the card's content box.
    const int header = tile_icon_disc::row_height();
    const int gap = tile_layout::scale_480(4);
    view->content_y = static_cast<int16_t>(header + gap - pad_y);
    view->content_w = static_cast<int16_t>(std::max(1, card_w - 2 * pad_x));
    view->content_h = static_cast<int16_t>(std::max(1, card_h - header - gap - pad_y));
    if (view->layout == Layout::List) {
      view->list = plain_box(card);
      lv_obj_set_size(view->list, view->content_w, view->content_h);
      lv_obj_set_pos(view->list, 0, view->content_y);
    } else {
      // Count: the large number over its caption; Next: the first item over
      // the count, both centred in the content box.
      const lv_font_t* when_font = tile_layout::content_font_20();
      const lv_font_t* what_font =
          view->layout == Layout::Next
              ? tile_layout::content_font_28()
              : font_for(std::max(1, view->content_h - static_cast<int>(lv_font_get_line_height(when_font))));
      const int block = lv_font_get_line_height(what_font) + lv_font_get_line_height(when_font);
      const int top = view->content_y + std::max(0, (view->content_h - block) / 2);
      const lv_text_align_t align = view->layout == Layout::Next ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER;
      view->what = plain_label(card, what_font, ui_theme::text());
      lv_obj_set_width(view->what, view->content_w);
      lv_obj_set_style_text_align(view->what, align, 0);
      lv_obj_set_pos(view->what, 0, top);
      view->when = plain_label(card, when_font, ui_theme::text_secondary());
      lv_obj_set_width(view->when, view->content_w);
      lv_obj_set_style_text_align(view->when, align, 0);
      lv_obj_set_pos(view->when, 0, top + lv_font_get_line_height(what_font));
    }
  }

  view->next = g_views;
  g_views = view;
  show(view);

  if (view->entity.length() && grid_type != GridType::SCREENSAVER) {
    const lv_event_code_t popup_event =
        getTilePopupOpenMode(tile) == TILE_POPUP_OPEN_SHORT_PRESS ? LV_EVENT_SHORT_CLICKED : LV_EVENT_LONG_PRESSED;
    lv_obj_add_event_cb(card, on_card_event, popup_event, view);
  }
  lv_obj_add_event_cb(card, on_card_event, LV_EVENT_DELETE, view);
  return card;
}
