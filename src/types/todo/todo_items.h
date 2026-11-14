#pragma once

#include <ArduinoJson.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "src/types/calendar/calendar_events.h"

// The parsed items live in PSRAM on the panel, like the calendar events
// (calendar_events.h); host tests build without it.
#if CALENDAR_EVENTS_PSRAM
#include "src/core/memory/psram_allocator.h"
#endif

// The Bridge's retained `items` state of a to-do list entity
// ({ha_prefix}/todo/<object>/items, docs-dev/todo.md): the open items of one
// Home Assistant to-do list in its own order. Parsed defensively and bounded;
// nothing here touches LVGL, so host tests run it as it is.
//
// {"v":1,"state":"2","f":15,"n":2,
//  "i":[{"u":"a1b2","t":"Buy milk"},{"u":"c3d4","t":"Call the plumber","d":"2026-11-20"}]}
//
// `valid` false is no payload (or a broken one), `available` false a list
// Home Assistant does not have or reports unavailable. An item without "u"
// cannot be ticked off (its uid was too long for the Bridge to send).
namespace todo_items {

constexpr size_t kMaxPayload = 8192;
constexpr size_t kMaxItems = 32;
constexpr size_t kTitleBytes = 63;
constexpr size_t kUidBytes = 160;
// homeassistant.components.todo.TodoListEntityFeature.UPDATE_TODO_ITEM
constexpr uint32_t kFeatureUpdateItem = 4;
// Due day of an item without one.
constexpr int32_t kNoDue = INT32_MIN;

struct Item {
  char uid[kUidBytes + 1] = {};
  char title[kTitleBytes + 1] = {};
  // Days since 1970-01-01 in Home Assistant's local time, kNoDue without one.
  int32_t due_day = kNoDue;

  bool can_tick() const { return uid[0] != '\0'; }
  bool has_due() const { return due_day != kNoDue; }
};

#if CALENDAR_EVENTS_PSRAM
using ItemList = PsVector<Item>;
#else
using ItemList = std::vector<Item>;
#endif

struct List {
  bool valid = false;
  bool available = false;
  uint32_t features = 0;
  // Open items Home Assistant reported; can exceed items.size().
  uint32_t open_count = 0;
  ItemList items;

  bool can_complete() const { return available && (features & kFeatureUpdateItem) != 0; }
};

inline List parse(const char* payload, size_t length) {
  List out;
  if (!payload || !length || length > kMaxPayload) return out;
#if CALENDAR_EVENTS_PSRAM
  JsonDocument doc(calendar_events::detail::json_allocator());
#else
  JsonDocument doc;
#endif
  if (deserializeJson(doc, payload, length) || !doc.is<JsonObjectConst>()) return out;
  JsonVariantConst version = doc["v"];
  if (!version.is<int>() || version.as<int>() != 1) return out;
  JsonVariantConst state = doc["state"];
  const char* state_text = state.is<const char*>() ? state.as<const char*>() : nullptr;
  out.available = state_text && strcmp(state_text, "unavailable") != 0;
  JsonVariantConst features = doc["f"];
  out.features = features.is<uint32_t>() ? features.as<uint32_t>() : 0;
  JsonArrayConst items = doc["i"].as<JsonArrayConst>();
  out.valid = true;
  if (!items.isNull()) {
    out.items.reserve(items.size() < kMaxItems ? items.size() : kMaxItems);
    for (JsonVariantConst entry : items) {
      if (out.items.size() >= kMaxItems) break;
      if (!entry.is<JsonObjectConst>()) continue;
      Item item;
      calendar_events::detail::copy_text(entry["t"].as<const char*>(), item.title, sizeof(item.title));
      if (!item.title[0]) continue;
      const char* uid = entry["u"].as<const char*>();
      if (uid && strlen(uid) <= kUidBytes) strlcpy(item.uid, uid, sizeof(item.uid));
      int32_t day = 0;
      int16_t minute = 0;
      const char* due = entry["d"].as<const char*>();
      if (due && strlen(due) == 10 && calendar_events::parse_stamp(due, day, minute)) item.due_day = day;
      out.items.push_back(item);
    }
  }
  JsonVariantConst count = doc["n"];
  const uint32_t shown = static_cast<uint32_t>(out.items.size());
  out.open_count = count.is<uint32_t>() && count.as<uint32_t>() >= shown ? count.as<uint32_t>() : shown;
  return out;
}

// Removes the item `uid` after it was ticked off on the panel, before the
// Bridge's shorter list arrives; false when it is not in the list.
inline bool remove_item(List& list, const char* uid) {
  if (!uid || !*uid) return false;
  for (size_t i = 0; i < list.items.size(); ++i) {
    if (strcmp(list.items[i].uid, uid) != 0) continue;
    list.items.erase(list.items.begin() + static_cast<long>(i));
    if (list.open_count) --list.open_count;
    return true;
  }
  return false;
}

}  // namespace todo_items
