#pragma once

#include <Arduino.h>
#include <lvgl.h>

// The To-do tile's popup on the shared popup shell (the Calendar popup's
// header and lifecycle): the list's open items, each with an empty circle; a
// tap ticks the item off (todo_store::complete) and it leaves the list. A
// list Home Assistant cannot update shows its items with a note instead. One
// cached popup, refilled for the tile that opens it.
struct TodoPopupTarget {
  String entity;
  String title;
  // The tile's configured MDI icon; "" shows the list icon.
  String icon_name;
  bool icon_visible = true;
  // The opening tile card (popup color and header disc), may be null.
  lv_obj_t* source = nullptr;
};

void show_todo_popup(const TodoPopupTarget& target);
void hide_todo_popup();
void preload_todo_popup();
void todo_popup_follow_tile_color(uint32_t color);
// The items of `entity` changed ("*": any list).
void todo_popup_refresh(const String& entity);
