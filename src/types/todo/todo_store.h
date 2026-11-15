#pragma once

#include <Arduino.h>

#include "src/types/todo/todo_items.h"

// The open items of every to-do list the Bridge offers (its `items` topic,
// docs-dev/todo.md). MQTT hands in the raw payload; the loop parses it once
// and tells the To-do tiles and the popup. Ticking an item off sends the
// command and removes the item at once (optimistic) until the Bridge's
// shorter list arrives. Everything runs on the loop task, like
// calendar_store.
namespace todo_store {

// MQTT: an `items` state arrived ({ha_prefix}/todo/<object>/items). The
// newest payload per list waits for process(); an oversized one is dropped
// with a rate-limited log.
void queue(const String& entity, const char* payload, size_t length);
// Loop: parses waiting payloads (at most `budget`, 0 = all) and refreshes the
// tiles and the popup showing them.
void process(uint8_t budget);

// The parsed list, nullptr while none arrived. Valid until the next
// process(), complete() or forget_except().
const todo_items::List* get(const String& entity);
// The last payload as received (Web Admin previews), "" while none.
String payload(const String& entity);
// Drops lists the Bridge no longer offers (one id per line in `offered`).
void forget_except(const String& offered);

// Ticks `uid` off: sends {base}/cmnd/todo and removes the item from the shown
// list. False when the list cannot update items or does not show `uid`.
bool complete(const String& entity, const char* uid);

// True for a todo.<object_id> entity id the panel accepts.
bool valid_entity(const String& entity);

}  // namespace todo_store
