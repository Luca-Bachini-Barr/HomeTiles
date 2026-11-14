#pragma once

#include <Arduino.h>

#include "src/core/config/config_manager.h"
#include "src/core/i18n/i18n.h"
#include "src/types/todo/todo_items.h"

// Texts of the To-do tile and popup, from the central translations.
namespace todo_text {

inline const char* language() { return configManager.getConfig().language; }
inline const char* label(i18n::TodoLabel which) { return i18n::todo_label(language(), which); }

// "2 open", "1 open" or "All done"; "--" while nothing arrived.
inline String count_caption(const todo_items::List* list) {
  if (!list || !list->valid) return String("--");
  if (!list->available) return String(label(i18n::TodoLabel::NoData));
  if (!list->open_count) return String(label(i18n::TodoLabel::AllDone));
  if (list->open_count == 1) return String(label(i18n::TodoLabel::OneOpen));
  char text[48];
  snprintf(text, sizeof(text), label(i18n::TodoLabel::OpenCount), static_cast<unsigned>(list->open_count));
  return String(text);
}

// The bare number for the large 1x1 count; "--" while nothing is known.
inline String count_value(const todo_items::List* list) {
  if (!list || !list->valid || !list->available) return String("--");
  return String(static_cast<unsigned>(list->open_count));
}

// "+3 more".
inline String more(uint32_t count) {
  char text[48];
  snprintf(text, sizeof(text), label(i18n::TodoLabel::More), static_cast<unsigned>(count));
  return String(text);
}

}  // namespace todo_text
