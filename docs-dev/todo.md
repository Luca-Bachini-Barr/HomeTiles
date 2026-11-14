# To-do tile contract

Tile type 30 (`TILE_TODO`, `src/types/todo/`), popup in `src/ui/popups/todo/`.
Visual reference: the Calendar tile (Sensor header, view per rendered tile,
popup on the shared shell).

## Persistence (`PackedTileV7`, unchanged)

| Field | Use |
| --- | --- |
| `sensor_entity` | One `todo.*` id |
| popup open mode | Short or long press, like the other popup tiles |

Layouts (`layout_of`, mirrored by the Web Admin's `todoPreviewLayout`):

| Size | Layout |
| --- | --- |
| half height | title and "3 open" / "All done" (`compact_todo`) |
| 1x1 | the open count, large, over its caption |
| 2+ wide, 1 row | the first open item over the count |
| 2+ rows | the open items with an empty circle each, "+N more" when they do not fit |

## Bridge

The Bridge announces `"entity_search": 7` and the lists `todos` and
`todo_meta`. The panel offers the picker list `todos` and subscribes
`{ha_prefix}/todo/<object>/items` for every offered list.

Retained payload (Bridge `todo_items.py`, panel `todo_items.h`):

```json
{"v":1,"state":"2","f":15,"n":2,
 "i":[{"u":"a1b2","t":"Buy milk"},{"u":"c3d4","t":"Call the plumber","d":"2026-11-20"}]}
```

- `state` is the entity state (the open count, or `"unavailable"`), `f` its
  `supported_features`, `n` the open count Home Assistant reported (can exceed
  the items sent), `d` the local due day, `YYYY-MM-DDTHH:MM` when the item
  has a due time.
- Bounds: at most 32 items, title 63 bytes, uid 160 bytes (an item with a
  longer uid is sent without `u` and cannot be ticked off), payload 8192
  bytes; the Bridge drops the last items first.
- The Bridge reads `todo.get_items` (status `needs_action`) after a snapshot,
  when the list's state changes, right after a tick, and every 5 minutes,
  debounced by 2 s and skipped when unchanged. A failing call keeps the
  retained payload. One entry per `ha_prefix` publishes.

Command `{base}/cmnd/todo` (sealed once paired, leaf `todo`):
`{"entity_id":"todo.home","action":"complete","uid":"a1b2"}`. The Bridge
accepts only a uid it published for that list and only when the list has the
`UPDATE_TODO_ITEM` feature (4), then calls `todo.update_item` with
`status: completed`.

Due alert: when an item with a due time becomes due (once per item, only
within 15 minutes of its due), the panel wakes, opens the popup of a To-do
tile showing the list with the item marked, and sends
`{"action":"announce","uid":...}`. The Bridge accepts it only for a published
item just due, once per item, and plays Home Assistant's announce chime and a
spoken reminder (preferred Assist pipeline's TTS) on the panel's speaker.

## Panel

- MQTT callbacks only queue the payload (`todo_store::queue`, coalesced, at
  most 24 lists); the loop parses it (two per pass) and refreshes the tiles
  and an open popup. Payloads and items live in PSRAM.
- A tick removes the item at once (optimistic) and publishes the command; the
  Bridge's shorter list replaces it. A list without the update feature shows
  its items with the "cannot be ticked off" note.

## Verification

- Host: `tools/tests/tiles/test-todo-tile.mjs` (wiring, translations, browser
  preview and form payload).
- Bridge: `tests/test_todo_items.py` (payload bounds, commands, publisher).
- Hardware: pending on Waveshare 7B (`waveshare_7b_rev3`) with Microsoft To Do
  lists through Home Assistant's `todo` platform.
