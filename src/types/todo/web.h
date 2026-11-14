#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "src/tiles/config/tile_config.h"

// Web Admin of the To-do tile (`<tab>_todo_fields`): the to-do list picker
// (todo_entity) and the popup gesture. POST fields: todo_entity and
// popup_open_mode. The list is stored in Tile::sensor_entity.

void append_todo_fields_html(String& html, const String& tab_id);
// Applies the fields; false when the field names something else than a
// to-do list.
bool apply_todo_fields_from_request(WebServer& server, Tile& tile);
// TODO_I18N for the previews.
void append_todo_scripts(String& html);
void append_todo_styles(String& html);
