#pragma once

#include <Arduino.h>

#include "src/core/config/config_manager.h"
#include "src/core/i18n/i18n.h"
#include "src/types/calendar/calendar_text.h"
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

// The due as weekday, date and, when it has one, time ("Sat 14 Nov · 22:30"),
// in the panel's language and time format; "" without a due.
inline String due_text(const todo_items::Item& item) {
  if (!item.has_due()) return String();
  const char* lang = language();
  int y;
  unsigned m, d;
  calendar_events::civil_from_days(item.due_day, y, m, d);
  String text = i18n::locale(lang).weather_weekdays_short[calendar_events::weekday(item.due_day)];
  text += " ";
  text += i18n::format_short_date(lang, static_cast<int>(d), i18n::weather_month_short(lang, static_cast<int>(m)));
  if (item.has_due_time()) {
    text += " \xC2\xB7 ";
    text += calendar_text::clock(item.due_min);
  }
  return text;
}

// True once the due has passed: a due time when it is reached, a due day
// without a time from the next day on. False while the clock is not set.
inline bool overdue(const todo_items::Item& item, const calendar_text::Now& now) {
  if (!item.has_due() || !now.valid) return false;
  if (item.due_day != now.today) return item.due_day < now.today;
  return item.has_due_time() && item.due_min <= now.minute;
}

}  // namespace todo_text
