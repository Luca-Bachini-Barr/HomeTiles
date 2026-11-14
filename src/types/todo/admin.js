  // To-do (30): one Home Assistant to-do list (types/todo/todo_tile.cpp). The
  // preview mirrors the device: the items payload of /api/sensor_values
  // todo_values (todo_items.h), the layouts of layout_of() and the words of
  // TODO_I18N (types/todo/web.cpp).
  // i18n::TodoLabel order.
  const TODO_LABEL = {type: 0, entity: 1, allDone: 2, openCount: 3, oneOpen: 4, more: 5, noData: 6, readOnly: 7};
  const TODO_DEFAULT_ICON = 'format-list-bulleted';

  function todoI18n() { return typeof TODO_I18N === 'object' ? TODO_I18N : {}; }
  function todoLabel(key, ...args) {
    let text = String(todoI18n().labels?.[TODO_LABEL[key]] ?? '');
    for (const arg of args) text = text.replace(/%[us]/, String(arg));
    return text;
  }

  // todo_store::valid_entity().
  function todoEntityOf(value) {
    const id = String(value || '').trim();
    return /^todo\.[a-z0-9_]+$/.test(id) ? id : '';
  }

  // todo_items::parse().
  function parseTodoPreviewPayload(value) {
    const out = {valid: false, available: false, features: 0, open: 0, items: []};
    const text = value === undefined || value === null ? '' : String(value).trim();
    if (!text.startsWith('{')) return out;
    let obj;
    try { obj = JSON.parse(text); } catch (e) { return out; }
    if (!obj || obj.v !== 1) return out;
    out.valid = true;
    out.available = typeof obj.state === 'string' && obj.state !== 'unavailable';
    out.features = Number.isInteger(obj.f) && obj.f >= 0 ? obj.f : 0;
    for (const item of Array.isArray(obj.i) ? obj.i.slice(0, 32) : []) {
      if (!item || typeof item !== 'object') continue;
      const title = String(item.t || '').trim();
      if (title) out.items.push({title, uid: typeof item.u === 'string' ? item.u : ''});
    }
    out.open = Number.isInteger(obj.n) && obj.n >= out.items.length ? obj.n : out.items.length;
    return out;
  }

  function todoPreviewList(tile, meta = sensorMetaCache) {
    const entity = todoEntityOf(tile.sensor_entity);
    return entity ? parseTodoPreviewPayload(meta?.todoValues?.[entity] ?? '') : null;
  }

  // todo_text::count_caption().
  function todoCountCaption(list) {
    if (!list || !list.valid) return '--';
    if (!list.available) return todoLabel('noData');
    if (!list.open) return todoLabel('allDone');
    return list.open === 1 ? todoLabel('oneOpen') : todoLabel('openCount', list.open);
  }

  function todoPreviewLayout(tile) {
    const w = Number(tile.span_w) || 1, h = Number(tile.span_h) || 1;
    if (h === 0.5) return 'compact';
    if (h >= 2) return 'list';
    return w >= 2 ? 'next' : 'count';
  }

  // The title the tile shows: none at half height (the chip says it).
  function todoPreviewTitle(tile, title) {
    return todoPreviewLayout(tile) === 'compact' ? '' : title;
  }

  // Bright while something is open, dimmed otherwise (todo_tile.cpp color_icon).
  function todoPreviewIconColor(tile, meta = sensorMetaCache) {
    const list = todoPreviewList(tile, meta);
    return list && list.valid && list.available && list.open ? '' : 'var(--pv-icon-inactive, #9E9E9E)';
  }

  function todoPreviewExtraHtml(tile, title, meta = sensorMetaCache) {
    const list = todoPreviewList(tile, meta);
    const layout = todoPreviewLayout(tile);
    const caption = todoCountCaption(list);
    if (layout === 'compact') {
      return '<div class="tile-title todo-preview-when">' + escapeHtml(title || todoLabel('type')) + '</div>' +
        '<div class="tile-value todo-preview-what">' + escapeHtml(caption) + '</div>';
    }
    if (layout === 'count' || layout === 'next') {
      let what = '--';
      if (layout === 'count') {
        what = list && list.valid && list.available ? String(list.open) : '--';
      } else if (list && list.valid) {
        what = !list.available ? todoLabel('noData') : (list.items.length ? list.items[0].title : todoLabel('allDone'));
      }
      return '<div class="todo-preview-body ' + layout + '">' +
        '<div class="todo-preview-what">' + escapeHtml(what) + '</div>' +
        '<div class="todo-preview-when">' + escapeHtml(caption) + '</div></div>';
    }
    let html = '<div class="todo-preview-body list">';
    if (!list) return html + '<div class="todo-preview-message">--</div></div>';
    if (!list.valid || !list.available) return html + '<div class="todo-preview-message">' + escapeHtml(todoLabel('noData')) + '</div></div>';
    if (!list.items.length) return html + '<div class="todo-preview-message">' + escapeHtml(todoLabel('allDone')) + '</div></div>';
    const fit = Math.max(1, Math.round((Number(tile.span_h) || 2) * 2.5 - 1.5));
    let rows = Math.min(list.items.length, fit, 12);
    let more = 0;
    if (list.open > rows) {
      if (rows === fit && rows > 1) --rows;
      more = list.open - rows;
    }
    for (let i = 0; i < rows; ++i) {
      html += '<div class="todo-preview-row"><i class="todo-preview-mark"></i>' +
        '<span class="todo-preview-title">' + escapeHtml(list.items[i].title) + '</span></div>';
    }
    if (more) html += '<div class="todo-preview-row todo-preview-more">' + escapeHtml(todoLabel('more', more)) + '</div>';
    return html + '</div>';
  }

  // The tile the live preview draws from the open fields.
  function todoTileFromInputs(tab, spanW, spanH) {
    return {
      sensor_entity: todoEntityOf(document.getElementById(tab + '_todo_entity')?.value || ''),
      span_w: spanW,
      span_h: spanH
    };
  }

  function loadTodoFields(tab, data) {
    const input = document.getElementById(tab + '_todo_entity');
    if (input) input.value = todoEntityOf(data.sensor_entity || data.todo_entity || '');
    const popup = document.getElementById(tab + '_todo_popup_open_mode');
    if (popup) popup.value = data.popup_open_mode !== undefined ? String(data.popup_open_mode) : '1';
    maybeFillTitleFromEntity(tab, '_todo_entity');
  }

  function saveTodoFields(tab, formData) {
    const entity = todoEntityOf(document.getElementById(tab + '_todo_entity')?.value || '');
    formData.append('todo_entity', entity);
    formData.append('sensor_entity', entity);
    const popup = document.getElementById(tab + '_todo_popup_open_mode');
    if (popup) formData.append('popup_open_mode', popup.value || '1');
  }

  function resetTodoFields(tab) {
    const input = document.getElementById(tab + '_todo_entity');
    if (input) input.value = '';
    const popup = document.getElementById(tab + '_todo_popup_open_mode');
    if (popup) popup.value = '1';
  }
