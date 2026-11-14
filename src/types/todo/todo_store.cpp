#include "src/types/todo/todo_store.h"

#include <ArduinoJson.h>

#include <map>
#include <memory>
#include <set>

#include "src/core/memory/psram_allocator.h"
#include "src/network/mqtt/mqtt_topics.h"
#include "src/network/network_manager.h"
#include "src/types/todo/todo_text.h"
#include "src/types/todo/todo_tile.h"
#include "src/types/todo/todo_updates.h"
#include "src/ui/popups/todo/todo_popup.h"

namespace todo_store {
namespace {

// A panel shows a handful of lists; the Bridge offers what it released.
constexpr size_t kMaxLists = 24;

// Payloads, map nodes and keys in PSRAM, like calendar_store.
struct Entry {
  PsString payload;
  bool pending = false;
  std::unique_ptr<todo_items::List> list;
};

using EntryMap = std::map<PsString, Entry, std::less<PsString>, PsramAllocator<std::pair<const PsString, Entry>>>;
EntryMap g_entries;
size_t g_pending = 0;

// Due alerts: an item whose due time passed within this window opens its
// list's popup once ("<entity>|<uid or title>" remembered, bounded).
constexpr int32_t kAlertWindowMin = 15;
constexpr size_t kMaxAlerted = 64;
constexpr uint32_t kAlertCheckMs = 5000;
using AlertSet = std::set<PsString, std::less<PsString>, PsramAllocator<PsString>>;
AlertSet g_alerted;

void publish_command(const String& entity, const char* action, const char* uid);

void check_due() {
  static uint32_t last_ms = 0;
  const uint32_t now_ms = millis();
  if (last_ms && now_ms - last_ms < kAlertCheckMs) return;
  last_ms = now_ms;
  const calendar_text::Now now = calendar_text::now();
  if (!now.valid) return;
  for (auto& item : g_entries) {
    const todo_items::List* list = item.second.list.get();
    if (!list || !list->valid || !list->available) continue;
    for (const todo_items::Item& entry : list->items) {
      if (!entry.has_due_time()) continue;
      const int32_t since = (now.today - entry.due_day) * 24 * 60 + (now.minute - entry.due_min);
      if (since < 0 || since >= kAlertWindowMin) continue;
      PsString key(item.first);
      key += "|";
      key += entry.uid[0] ? entry.uid : entry.title;
      if (g_alerted.count(key)) continue;
      if (g_alerted.size() >= kMaxAlerted) g_alerted.clear();
      g_alerted.insert(key);
      const String entity(item.first.c_str());
      Serial.printf("[Todo] Due: %s item %s\n", entity.c_str(), entry.uid[0] ? "with uid" : "without uid");
      // One alert per check; the next item follows five seconds later. The
      // Bridge plays the announce chime and a spoken reminder on this panel.
      if (todo_tile_alert(entity, entry.uid)) {
        if (entry.uid[0]) publish_command(entity, "announce", entry.uid);
        return;
      }
    }
  }
}

// {base}/cmnd/todo, sealed by the network layer once paired.
void publish_command(const String& entity, const char* action, const char* uid) {
  if (!networkManager.isMqttConnected()) return;
  JsonDocument doc;
  doc["entity_id"] = entity;
  doc["action"] = action;
  doc["uid"] = uid;
  String body;
  serializeJson(doc, body);
  const String topic = mqttTopics.deviceBase() + "/cmnd/todo";
  networkManager.mqttEnqueuePublish(topic.c_str(), body.c_str(), false);
}

void refresh(const String& entity) {
  todo_tiles_refresh(entity);
  todo_popup_refresh(entity);
}

}  // namespace

bool valid_entity(const String& entity) {
  if (!entity.startsWith("todo.") || entity.length() <= 5 || entity.length() > 128) return false;
  for (size_t i = 5; i < entity.length(); ++i) {
    const char c = entity[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

void queue(const String& entity, const char* payload, size_t length) {
  if (!valid_entity(entity) || !payload) return;
  if (length > todo_items::kMaxPayload) {
    static uint32_t last_log_ms = 0;
    const uint32_t now = millis();
    if (!last_log_ms || now - last_log_ms >= 10000) {
      last_log_ms = now;
      Serial.printf("[Todo] Oversized items dropped: %s (%u bytes)\n", entity.c_str(),
                    static_cast<unsigned>(length));
    }
    return;
  }
  const PsString key(entity.c_str());
  auto it = g_entries.find(key);
  if (it == g_entries.end()) {
    if (g_entries.size() >= kMaxLists) return;
    it = g_entries.emplace(key, Entry{}).first;
  }
  Entry& entry = it->second;
  // A retained state arrives again after every reconnect: unchanged, nothing
  // needs parsing or redrawing.
  if (!entry.pending && entry.list && entry.payload.size() == length &&
      entry.payload.compare(0, length, payload, length) == 0) {
    return;
  }
  entry.payload.assign(payload, length);
  if (!entry.pending) {
    entry.pending = true;
    ++g_pending;
  }
}

void process(uint8_t budget) {
  check_due();
  if (!g_pending) return;
  uint8_t done = 0;
  for (auto& item : g_entries) {
    Entry& entry = item.second;
    if (!entry.pending) continue;
    if (budget && done >= budget) break;
    entry.pending = false;
    --g_pending;
    ++done;
    std::unique_ptr<todo_items::List> parsed(
        new todo_items::List(todo_items::parse(entry.payload.data(), entry.payload.size())));
    if (!parsed->valid) {
      static uint32_t last_log_ms = 0;
      const uint32_t now = millis();
      if (!last_log_ms || now - last_log_ms >= 10000) {
        last_log_ms = now;
        Serial.printf("[Todo] Unreadable items of %s ignored\n", item.first.c_str());
      }
    }
    entry.list = std::move(parsed);
    refresh(String(item.first.c_str()));
  }
}

const todo_items::List* get(const String& entity) {
  const auto it = g_entries.find(PsString(entity.c_str()));
  return it == g_entries.end() ? nullptr : it->second.list.get();
}

String payload(const String& entity) {
  const auto it = g_entries.find(PsString(entity.c_str()));
  return it == g_entries.end() ? String() : String(it->second.payload.c_str());
}

void forget_except(const String& offered) {
  const String lines = "\n" + offered + "\n";
  for (auto it = g_entries.begin(); it != g_entries.end();) {
    if (lines.indexOf("\n" + String(it->first.c_str()) + "\n") >= 0) {
      ++it;
      continue;
    }
    if (it->second.pending) --g_pending;
    const String entity(it->first.c_str());
    it = g_entries.erase(it);
    refresh(entity);
  }
}

bool complete(const String& entity, const char* uid) {
  const auto it = g_entries.find(PsString(entity.c_str()));
  if (it == g_entries.end() || !it->second.list || !uid || !*uid) return false;
  todo_items::List& list = *it->second.list;
  if (!list.can_complete()) return false;
  bool shown = false;
  for (const todo_items::Item& item : list.items) shown = shown || strcmp(item.uid, uid) == 0;
  if (!shown || !networkManager.isMqttConnected()) return false;
  publish_command(entity, "complete", uid);
  todo_items::remove_item(list, uid);
  refresh(entity);
  return true;
}

}  // namespace todo_store

void process_todo_updates(uint8_t budget) { todo_store::process(budget); }
