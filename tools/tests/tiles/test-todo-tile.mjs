// To-do tile (30): persistence, registry, Bridge routes, commands, the Web
// Admin editor and the preview (types/todo, docs-dev/todo.md).
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');

// Stable ID and policies.
assert.match(read('src/types/tile_type.h'), /TILE_VACUUM = 29,\n  TILE_TODO = 30\n/);
const policy = read('src/types/tile_type_policy.h');
assert.match(policy, /static constexpr bool tileTypeIsTodo\(int type\) \{ return type == TILE_TODO; \}/);
assert.equal((policy.match(/tileTypeIsCalendar\(type\) \|\| tileTypeIsTodo\(type\);/g) || []).length, 3,
  'stores its list and popup mode like the Calendar');
const geometry = read('src/tiles/config/tile_geometry.h');
assert.match(geometry, /inline bool compact_todo\(int type, float w, float h\) \{ return todo\(type\) && w >= 1 && h == 0\.5f; \}/);

// Registry: one descriptor with the browser functions and a localized label.
const registry = read('src/types/types_registry.cpp');
assert.match(registry, /TILE_TODO,\s*"To-do",\s*"todo",\s*"todo",\s*"todo",\s*nullptr,\s*"loadTodoFields",\s*"saveTodoFields",\s*"resetTodoFields"/);
assert.match(registry, /case TILE_TODO: return i18n::todo_label\(language, i18n::TodoLabel::Type\);/);

// Bridge: level 7, the `items` route, the picker list and the sealed leaf.
assert.match(read('src/network/bridge/ha_bridge_config.h'), /bool supportsTodos\(\) const \{ return entity_search_ >= 7; \}/);
assert.match(read('src/network/bridge/entity_search.cpp'), /\{"todos", TILE_TODO\}/);
const handlers = read('src/network/mqtt/mqtt_handlers.cpp');
assert.ok(handlers.includes('{&cfg.todos_text, "items"}'));
assert.match(handlers, /route\.topic\.endsWith\("\/items"\)\) \{\s*todo_store::queue\(route\.entity_id, payload, payload_len\);/);
assert.match(handlers, /todo_store::forget_except\(cfg\.todos_text\);/);
assert.match(read('src/network/secure/command_channel_core.h'), /kLeaves\[\] = \{"todo", /);
const store = read('src/types/todo/todo_store.cpp');
assert.match(store, /mqttTopics\.deviceBase\(\) \+ "\/cmnd\/todo"/);
assert.match(store, /doc\["action"\] = "complete";/);
assert.match(store, /if \(!list\.can_complete\(\)\) return false;/, 'only lists Home Assistant can update');

// Due: date and time instead of Today/Tomorrow, overdue in the alert color,
// and a due alert that wakes the panel and marks the item.
const items = read('src/types/todo/todo_items.h');
assert.match(items, /strlen\(due\) == 10 \|\| strlen\(due\) == 16/, 'due day with or without a time');
const text = read('src/types/todo/todo_text.h');
assert.match(text, /inline String due_text\(const todo_items::Item& item\)/);
assert.match(text, /inline bool overdue\(const todo_items::Item& item, const calendar_text::Now& now\)/);
const popup = read('src/ui/popups/todo/todo_popup.cpp');
assert.match(popup, /lv_label_set_text\(when, todo_text::due_text\(item\)\.c_str\(\)\);/);
assert.doesNotMatch(popup, /day_label/, 'no Today/Tomorrow for a due');
assert.match(popup, /pop\.target\.highlight_uid == item\.uid/);
assert.match(store, /kAlertWindowMin = 15;/);
assert.match(store, /if \(todo_tile_alert\(entity, entry\.uid\)\) return;/);
const tileSource = read('src/types/todo/todo_tile.cpp');
assert.match(tileSource, /hide_image_screensaver\(\);\s*powerManager\.wakeFromDisplaySleep\("todo due"\);\s*displayManager\.resetActivityTimer\(\);/);

// Popups close each other; the To-do popup is preloaded and hidden on navigation.
for (const popup of ['calendar', 'camera', 'climate', 'cover', 'device', 'energy', 'light', 'media', 'pin',
  'sensor', 'vacuum', 'weather']) {
  assert.match(read(`src/ui/popups/${popup}/${popup}_popup.cpp`), /hide_todo_popup\(\);/, popup);
}
assert.match(read('src/ui/ui_manager.cpp'), /preload_todo_popup\(\);/);
assert.match(read('src/ui/navigation/view_navigation.cpp'), /hide_todo_popup\(\);/);
assert.match(read('src/tiles/runtime/tile_update_service.h'), /process_todo_updates\(drain_all \? 0 : 2\);/);

// Web Admin: fields, entity options, preview values and every JS integration point.
const web = read('src/types/todo/web.cpp');
assert.match(web, /appendEntityPickerField\(html, tab_id, "todo_entity", text\(i18n::TodoLabel::Entity\), "todos"\);/);
assert.match(web, /_todo_popup_open_mode/);
const tiles = read('src/web/server/handlers/web_admin_tiles.cpp');
assert.match(tiles, /appendList\("todos", parseSensorList\(ha\.todos_text\)\);/);
assert.match(tiles, /json \+= ",\\"todo_values\\":\{";/);
assert.ok(read('src/web/admin/bundle.json').includes('"src/types/todo/admin.js"'));
assert.match(read('src/web/admin/tiles/editor.js'), /'_todo_entity'\), 'change', 'todoEntity'/);
assert.match(read('src/web/admin/tiles/import-export.js'), /safeType === 30\) \{[\s\S]*?fd\.append\('todo_entity'/);
assert.match(read('src/web/admin/tiles/state.js'), /todoValues: payload\.todo_values/);
assert.match(read('src/web/admin/tiles/entity-picker.js'), /'calendars', 'vacuums', 'todos'\]/);
assert.match(read('src/web/admin/tiles/layout.js'), /27, 29, 30\]\.includes\(Number\(type\)\) && halfHeight/);

// Translations: eight labels in every language, and the picker's kind name.
const header = read('src/core/i18n/i18n.h');
assert.match(header, /const char\* todo_labels\[8\];/);
assert.match(header, /static_assert\(static_cast<int>\(TodoLabel::Count\) == 8/);
const i18n = read('src/core/i18n/i18n.cpp');
const locales = [...i18n.matchAll(/static const LocaleProfile kLocale(\w+) = \{/g)].map(m => m[1]);
assert.ok(locales.length >= 8, 'every language');
for (const lang of locales) {
  const start = i18n.indexOf(`static const LocaleProfile kLocale${lang} = {`);
  const table = i18n.slice(start, i18n.indexOf('\n};', start));
  const labels = /\n    \{("[^"]+"(?:, "[^"]+"){7})\},\n    \{"[^"]+", "[^"]+", "[^"]*\{query\}/.exec(table);
  assert.ok(labels, `${lang}: the eight to-do labels before the entity picker labels`);
  const values = JSON.parse('[' + labels[1] + ']');
  assert.equal((values[3].match(/%u/g) || []).length, 1, `${lang}: open count pattern`);
  assert.equal((values[5].match(/%u/g) || []).length, 1, `${lang}: more pattern`);
}

// The browser preview: the real admin.js functions in a sandbox.
const sandbox = {
  sensorMetaCache: {},
  escapeHtml: value => String(value).replace(/[&<>"']/g, c => `&#${c.charCodeAt(0)};`),
  maybeFillTitleFromEntity: () => {},
  document: {getElementById: () => null},
  TODO_I18N: {labels: ['To-do', 'To-do list', 'All done', '%u open', '1 open', '+%u more', 'No list data',
    'Items cannot be ticked off on this list']},
};
vm.createContext(sandbox);
vm.runInContext(read('src/types/todo/admin.js') +
  '\n;Object.assign(globalThis, {parseTodoPreviewPayload, todoCountCaption, todoPreviewLayout, todoPreviewTitle,' +
  ' todoPreviewExtraHtml, todoEntityOf, saveTodoFields});', sandbox);

const payload = JSON.stringify({v: 1, state: '3', f: 15, n: 3,
  i: [{u: 'a1', t: 'Buy milk'}, {u: 'b2', t: 'Call <the> plumber', d: '2026-11-20'}, {t: 'No uid'}]});
const list = sandbox.parseTodoPreviewPayload(payload);
assert.equal(list.valid, true);
assert.equal(list.open, 3);
assert.equal(JSON.stringify(list.items.map(item => item.title)), JSON.stringify(['Buy milk', 'Call <the> plumber', 'No uid']));
assert.equal(list.items[2].uid, '', 'an item without a uid cannot be ticked off');
assert.equal(sandbox.parseTodoPreviewPayload('{"v":2}').valid, false);
assert.equal(sandbox.parseTodoPreviewPayload('').valid, false);

// Every localized runtime state.
assert.equal(sandbox.todoCountCaption(null), '--');
assert.equal(sandbox.todoCountCaption(list), '3 open');
assert.equal(sandbox.todoCountCaption({...list, open: 1}), '1 open');
assert.equal(sandbox.todoCountCaption({...list, open: 0}), 'All done');
assert.equal(sandbox.todoCountCaption({...list, available: false}), 'No list data');

// Layouts and the preview html, values escaped.
assert.equal(sandbox.todoPreviewLayout({span_w: 1, span_h: 0.5}), 'compact');
assert.equal(sandbox.todoPreviewLayout({span_w: 1, span_h: 1}), 'count');
assert.equal(sandbox.todoPreviewLayout({span_w: 2, span_h: 1}), 'next');
assert.equal(sandbox.todoPreviewLayout({span_w: 2, span_h: 2}), 'list');
assert.equal(sandbox.todoPreviewTitle({span_w: 1, span_h: 0.5}, 'Groceries'), '');
assert.equal(sandbox.todoPreviewTitle({span_w: 2, span_h: 2}, 'Groceries'), 'Groceries');
const meta = {todoValues: {'todo.home': payload}};
const tile = size => ({sensor_entity: 'todo.home', ...size});
assert.match(sandbox.todoPreviewExtraHtml(tile({span_w: 1, span_h: 0.5}), 'Groceries', meta), /Groceries[\s\S]*3 open/);
assert.match(sandbox.todoPreviewExtraHtml(tile({span_w: 1, span_h: 1}), '', meta), /todo-preview-what">3</);
assert.match(sandbox.todoPreviewExtraHtml(tile({span_w: 2, span_h: 1}), '', meta), /Buy milk/);
const listHtml = sandbox.todoPreviewExtraHtml(tile({span_w: 2, span_h: 2}), '', meta);
assert.match(listHtml, /Call &#60;the&#62; plumber/, 'titles are escaped');
assert.doesNotMatch(listHtml, /<the>/);
assert.match(sandbox.todoPreviewExtraHtml({sensor_entity: '', span_w: 2, span_h: 2}, '', meta), /todo-preview-message">--</);
assert.match(sandbox.todoPreviewExtraHtml(tile({span_w: 2, span_h: 2}), '',
  {todoValues: {'todo.home': '{"v":1,"state":"0","f":15,"n":0,"i":[]}'}}), /All done/);

// The form payload: the list and the popup gesture, an invalid id dropped.
const fields = {_todo_entity: {value: 'todo.home'}, _todo_popup_open_mode: {value: '0'}};
sandbox.document = {getElementById: id => fields[id.replace(/^tab0/, '')] || null};
const sent = [];
sandbox.saveTodoFields('tab0', {append: (key, value) => sent.push([key, value])});
assert.equal(JSON.stringify(sent), JSON.stringify([['todo_entity', 'todo.home'], ['sensor_entity', 'todo.home'], ['popup_open_mode', '0']]));
assert.equal(sandbox.todoEntityOf('script.oops'), '');

console.log('PASS test-todo-tile');
