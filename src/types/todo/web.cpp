#include "src/types/todo/web.h"

#include <ArduinoJson.h>

#include "src/core/config/config_manager.h"
#include "src/core/i18n/i18n.h"
#include "src/types/todo/todo_store.h"
#include "src/web/server/web_admin_utils.h"

namespace {

const char* language() { return configManager.getConfig().language; }

const char* text(i18n::TodoLabel label) { return i18n::todo_label(language(), label); }

}  // namespace

void append_todo_fields_html(String& html, const String& tab_id) {
  const auto& tr = i18n::strings(language());
  html += "<div id=\"";
  html += tab_id;
  html += "_todo_fields\" class=\"type-fields\">";
  appendEntityPickerField(html, tab_id, "todo_entity", text(i18n::TodoLabel::Entity), "todos");
  if (tab_id != "screensaver") {
    html += "<label>";
    html += tr.popup_open;
    html += "</label><select id=\"";
    html += tab_id;
    html += "_todo_popup_open_mode\"><option value=\"1\">";
    html += tr.short_press;
    html += "</option><option value=\"0\">";
    html += tr.long_press;
    html += "</option></select>";
  }
  html += "</div>\n";
}

bool apply_todo_fields_from_request(WebServer& server, Tile& tile) {
  // The field, or sensor_entity from an import without it.
  String entity = server.hasArg("todo_entity") ? server.arg("todo_entity")
                                               : (server.hasArg("sensor_entity") ? server.arg("sensor_entity") : String());
  entity.trim();
  bool valid = true;
  if (entity.length() && !todo_store::valid_entity(entity)) {
    valid = false;
    entity = "";
  }
  tile.sensor_entity = entity;
  const uint8_t popup_mode =
      server.hasArg("popup_open_mode") && server.arg("popup_open_mode").toInt() == TILE_POPUP_OPEN_LONG_PRESS
          ? TILE_POPUP_OPEN_LONG_PRESS
          : TILE_POPUP_OPEN_SHORT_PRESS;
  setTilePopupOpenMode(tile, popup_mode);
  tile.sensor_unit = "";
  tile.sensor_display_mode = 0;
  tile.sensor_decimals = 0;
  tile.sensor_gauge_min = 0;
  tile.sensor_gauge_max = 100;
  tile.key_code = 0;
  tile.key_modifier = 0;
  return valid;
}

void append_todo_scripts(String& html) {
  // The preview's words from the central translations (escaped for a script
  // element).
  JsonDocument doc;
  const char* lang = language();
  JsonArray labels = doc["labels"].to<JsonArray>();
  for (size_t i = 0; i < static_cast<size_t>(i18n::TodoLabel::Count); ++i) {
    labels.add(i18n::todo_label(lang, static_cast<i18n::TodoLabel>(i)));
  }
  String json;
  serializeJson(doc, json);
  json.replace("<", "\\u003c");
  html += "<script>const TODO_I18N=Object.freeze(";
  html += json;
  html += ");</script>\n";
}

void append_todo_styles(String&) {}
