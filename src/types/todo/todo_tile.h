#pragma once

#include <Arduino.h>
#include <lvgl.h>

#include "src/tiles/runtime/tile_renderer.h"

// The To-do tile: one Home Assistant to-do list, the Sensor tile's header
// (corner disc, title top right, the Calendar tile's reference) above what
// the size allows:
//   half height        the open count like a Sensor chip
//   one row, 1 wide    the open count, large
//   one row, 2+ wide   the first open item and the open count
//   two rows and more  the open items with an empty circle each
// The card opens the popup, where an item is ticked off. Every rendered tile
// keeps a small view owned by its card and found by list, like the Calendar
// tile, so hidden cached folder grids follow new items too.
lv_obj_t* render_todo_tile(lv_obj_t* parent, int col, int row, const Tile& tile, uint8_t index, GridType grid_type);

// Shows the current items on every tile of `entity` ("*": every tile).
void todo_tiles_refresh(const String& entity);
// True while `card` is a rendered To-do tile (compared, never dereferenced).
bool todo_tile_alive(lv_obj_t* card);
// A due alert (todo_store): wakes the panel and opens the popup of a To-do
// tile showing `entity`, with the item `uid` marked. False when no tile on
// this panel shows the list.
bool todo_tile_alert(const String& entity, const char* uid);
