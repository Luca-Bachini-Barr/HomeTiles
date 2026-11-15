#pragma once

#include <stdint.h>

// Loop service of the To-do tiles (todo_store.h): parses queued items and
// refreshes the tiles and the popup; 0 drains everything.
void process_todo_updates(uint8_t budget);
