// web/assets/model.js — 模型驱动板块（采集任务 / 设备规格 / 设备树 / 通道）
//
// 后端接口一律返回 {ok:false,error:"..."} 形式的错误，此处统一弹出原文 ——
// 后端已把「组合成环」「父节点缺失」「第 N 行 scale 非数字」这类原因写清楚了，
// 前端再包一层"操作失败"只会把有用信息盖掉。

const esc = s => String(s == null ? '' : s)
  .replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
  .replace(/"/g,'&quot;');

async function api(method, url, body) {
  const opt = { method, headers: {} };
  // POST/PUT 必须带请求体：httplib 对没有 Content-Length 的 POST 直接回
  // 400 且响应体为空，处理函数根本不会被调用 —— 「生效」这类无参操作
  // 若不显式发 {}，表现就是"点了没反应，也没有任何错误提示"。
  if (body === undefined && (method === 'POST' || method === 'PUT')) body = {};
  if (body !== undefined) {
    opt.headers['Content-Type'] = 'application/json';
    opt.body = typeof body === 'string' ? body : JSON.stringify(body);
  }
  const r = await fetch(url, opt);
  const txt = await r.text();
  let j = null;
  try { j = txt ? JSON.parse(txt) : null; } catch (e) { /* 非 JSON 响应 */ }
  if (!r.ok || (j && j.ok === false)) {
    throw new Error((j && j.error) || txt || ('HTTP ' + r.status));
  }
  return j;
}

function fail(e) { alert('操作失败：\n' + e.message); }

// ══ 采集任务 ══════════════════════════════════════════════════════════════
let _tasks = [];

async function tasksLoad() {
  try { _tasks = await api('GET', '/api/tasks'); }
  catch (e) { return fail(e); }

  let h = '<thead><tr><th>任务 ID</th><th>协议</th><th>端点</th>'
        + '<th>周期</th><th>启用</th><th>操作</th></tr></thead><tbody>';
  if (!_tasks.length) {
    h += '<tr><td colspan="6" class="empty">暂无采集任务</td></tr>';
  }
  for (const t of _tasks) {
    const ep = t.endpoint || {};
    // 各协议的"端点"字段不同，挑出最能标识连接的那个
    const desc = ep.host ? `${ep.host}:${ep.port || ''}`
               : ep.endpoint_url ? ep.endpoint_url
               : ep.serial_port ? `${ep.serial_port} @${ep.baud_rate || ''}`
               : ep.interface ? ep.interface
               : '—';
    h += `<tr>
      <td><b>${esc(t.id)}</b></td>
      <td><span class="badge">${esc(t.protocol)}</span></td>
      <td class="mono">${esc(desc)}</td>
      <td>${t.interval_ms} ms</td>
      <td>${t.enabled ? '<span class="ok">是</span>' : '<span class="dim">否</span>'}</td>
      <td>
        <button class="btn btn-xs" onclick="taskToggle('${esc(t.id)}')">${t.enabled?'停用':'启用'}</button>
        <button class="btn btn-xs btn-danger" onclick="taskDel('${esc(t.id)}')">删除</button>
      </td></tr>`;
  }
  document.getElementById('tasksTbl').innerHTML = h + '</tbody>';
}

async function taskToggle(id) {
  const t = _tasks.find(x => x.id === id);
  if (!t) return;
  try {
    await api('POST', '/api/tasks', { ...t, enabled: !t.enabled });
    tasksLoad();
  } catch (e) { fail(e); }
}

async function taskDel(id) {
  if (!confirm(`删除采集任务 "${id}"？\n设备树上引用该任务的测点会失去关联。`)) return;
  try { await api('DELETE', '/api/tasks/' + encodeURIComponent(id)); tasksLoad(); }
  catch (e) { fail(e); }
}

async function taskNew() {
  const id = prompt('任务 ID（如 bms_modbus）'); if (!id) return;
  const protocol = prompt('协议：modbus / iec104 / iec61850 / opcua / dlt645 / dlt698 / can',
                          'modbus');
  if (!protocol) return;
  const host = prompt('目标地址（IP 或串口设备）', '127.0.0.1') || '';
  const ms   = parseInt(prompt('采集周期（毫秒）', '1000') || '1000', 10);
  try {
    await api('POST', '/api/tasks', {
      id, protocol, interval_ms: ms, enabled: false,
      endpoint: host.startsWith('/dev/') ? { serial_port: host, baud_rate: 9600 }
                                         : { host, port: 502 }
    });
    tasksLoad();
  } catch (e) { fail(e); }
}

// ══ 设备规格 ════════════════════════════════════════════════════════════════
let _models = [];

async function modelsLoad() {
  try { _models = await api('GET', '/api/models'); }
  catch (e) { return fail(e); }

  let h = '<thead><tr><th>型号</th><th>名称</th><th>组成</th><th>测点</th><th></th></tr></thead><tbody>';
  if (!_models.length)
    h += '<tr><td colspan="5" class="empty">暂无设备规格</td></tr>';
  for (const m of _models) {
    const comp = m.children.length
      ? m.children.map(c => `${c.count}×${esc(c.model)}`).join(' + ')
      : '<span class="dim">—</span>';
    h += `<tr>
      <td><a href="#" onclick="modelShow('${esc(m.code)}');return false"><b>${esc(m.code)}</b></a></td>
      <td>${esc(m.name)}</td>
      <td>${comp}</td>
      <td>${m.point_count}</td>
      <td><button class="btn btn-xs btn-danger" onclick="modelDel('${esc(m.code)}')">删除</button></td>
    </tr>`;
  }
  document.getElementById('modelsTbl').innerHTML = h + '</tbody>';
}

async function modelShow(code) {
  let m;
  try { m = await api('GET', '/api/models/' + encodeURIComponent(code)); }
  catch (e) { return fail(e); }

  document.getElementById('modelDetailTitle').textContent = `${m.code} · ${m.name || '未命名'}`;
  let h = '';

  h += '<div class="sec-title">属性</div>';
  h += m.attrs.length
    ? '<table class="tbl tbl-sm"><tbody>' +
      m.attrs.map(a => `<tr><td class="dim">${esc(a.key)}</td><td>${esc(a.value)}</td></tr>`).join('') +
      '</tbody></table>'
    : '<div class="dim">无</div>';

  h += '<div class="sec-title">数据点</div>';
  h += m.points.length
    ? '<table class="tbl tbl-sm"><thead><tr><th>键</th><th>名称</th><th>单位</th></tr></thead><tbody>' +
      m.points.map(p => `<tr><td class="mono">${esc(p.key)}</td><td>${esc(p.name)}</td><td>${esc(p.unit)}</td></tr>`).join('') +
      '</tbody></table>'
    : '<div class="dim">无</div>';

  h += '<div class="sec-title">子规格组合</div>';
  h += m.children.length
    ? '<table class="tbl tbl-sm"><thead><tr><th>序</th><th>型号</th><th>数量</th><th></th></tr></thead><tbody>' +
      m.children.map(c => `<tr><td class="dim">${c.ord}</td><td>${esc(c.model)}</td><td>${c.count}</td>
        <td><button class="btn btn-xs btn-danger"
             onclick="modelDelChild('${esc(m.code)}',${c.ord})">移除</button></td></tr>`).join('') +
      '</tbody></table>'
    : '<div class="dim">无（叶子规格）</div>';

  h += `<div style="margin-top:12px">
    <button class="btn btn-sm btn-primary" onclick="modelAddChild('${esc(m.code)}')">+ 添加子规格</button>
    <button class="btn btn-sm" onclick="modelEditPoints('${esc(m.code)}')">编辑数据点</button>
  </div>`;
  document.getElementById('modelDetail').innerHTML = h;
}

async function modelNew() {
  const code = prompt('型号编码（如 CELL001）'); if (!code) return;
  const name = prompt('名称（如 电芯）', '') || '';
  const kind = prompt('分类（cell / pack / cluster / rack / controller…）', '') || '';
  try { await api('POST', '/api/models', { code, name, kind }); modelsLoad(); }
  catch (e) { fail(e); }
}

async function modelAddChild(code) {
  const opts = _models.filter(m => m.code !== code).map(m => m.code).join(' / ');
  const child = prompt(`子规格\n可选：${opts}`); if (!child) return;
  const count = parseInt(prompt('数量', '1') || '1', 10);
  try {
    // 成环会被后端拒绝并说明是哪一环，原文弹出即可
    await api('POST', `/api/models/${encodeURIComponent(code)}/children`,
              { model: child, count });
    modelShow(code); modelsLoad();
  } catch (e) { fail(e); }
}

async function modelDelChild(code, ord) {
  try {
    await api('DELETE', `/api/models/${encodeURIComponent(code)}/children/${ord}`);
    modelShow(code); modelsLoad();
  } catch (e) { fail(e); }
}

async function modelEditPoints(code) {
  const m = await api('GET', '/api/models/' + encodeURIComponent(code));
  const cur = m.points.map(p => [p.key, p.name, p.unit].join(',')).join('\n');
  const txt = prompt('每行一个数据点：键,名称,单位', cur);
  if (txt === null) return;
  const points = txt.split('\n').map(s => s.trim()).filter(Boolean).map(line => {
    const f = line.split(',');
    return { key: (f[0]||'').trim(), name: (f[1]||'').trim(), unit: (f[2]||'').trim() };
  }).filter(p => p.key);
  try {
    await api('POST', '/api/models', { code: m.code, name: m.name, kind: m.kind, points });
    modelShow(code); modelsLoad();
  } catch (e) { fail(e); }
}

async function modelDel(code) {
  if (!confirm(`删除设备参数记录 "${code}"？`)) return;
  try { await api('DELETE', '/api/models/' + encodeURIComponent(code)); modelsLoad(); }
  catch (e) { fail(e); }
}

// ══ 设备树 ══════════════════════════════════════════════════════════════
// 按需展开：每次只取一层。一个储能站 6000+ 节点，整棵树一次拉下来约 0.9MB、
// 再渲染成 6000 行扁平 DOM，页面就废了。
const _nodeCache = new Map();   // id → 节点对象，供 nodeShow 取用

async function fetchLevel(parentId) {
  const url = parentId == null ? '/api/tree' : '/api/tree?parent=' + parentId;
  const rows = await api('GET', url);
  for (const n of rows) _nodeCache.set(n.id, n);
  return rows;
}

function nodeRowHtml(n, depth) {
  const badge = n.model ? `<span class="badge badge-model">${esc(n.model)}</span>`
                        : '<span class="badge badge-free">自由</span>';
  const cnt = n.points.length ? `<span class="dim"> ${n.points.length} 点</span>` : '';
  const kids = n.child_count > 0
    ? `<span class="tree-toggle" onclick="treeToggle(event,${n.id})">▸</span>`
    : '<span class="tree-toggle tree-leaf"></span>';
  const sub = n.child_count > 0 ? `<span class="dim"> (${n.child_count})</span>` : '';
  return `<div class="tree-node" data-id="${n.id}" style="padding-left:${depth*16}px">
      ${kids}<a href="#" onclick="nodeShow(${n.id});return false">${esc(n.code)}</a>
      ${badge}${cnt}${sub}
    </div><div class="tree-children" id="tc-${n.id}" data-depth="${depth+1}"></div>`;
}

async function treeToggle(ev, id) {
  ev.stopPropagation();
  const box = document.getElementById('tc-' + id);
  const tog = ev.target;
  if (!box) return;
  if (box.dataset.open === '1') {
    box.innerHTML = ''; box.dataset.open = '0'; tog.textContent = '▸';
    return;
  }
  tog.textContent = '⋯';
  try {
    const depth = parseInt(box.dataset.depth || '1', 10);
    const rows  = await fetchLevel(id);
    box.innerHTML = rows.map(n => nodeRowHtml(n, depth)).join('');
    box.dataset.open = '1';
    tog.textContent = '▾';
  } catch (e) { tog.textContent = '▸'; fail(e); }
}

async function treeLoad() {
  const view = document.getElementById('treeView');
  view.innerHTML = '<div class="empty">加载中…</div>';
  _nodeCache.clear();
  let rows;
  try { rows = await fetchLevel(null); }
  catch (e) { view.innerHTML = '<div class="empty">加载失败</div>'; return fail(e); }
  view.innerHTML = rows.length
    ? rows.map(n => nodeRowHtml(n, 0)).join('')
    : '<div class="empty">设备树为空，先建一个根节点</div>';
}

function nodeShow(id) {
  const n = _nodeCache.get(id);
  if (!n) return;
  document.getElementById('nodeDetailTitle').textContent = n.path;

  let h = `<table class="tbl tbl-sm"><tbody>
    <tr><td class="dim">路径</td><td class="mono">${esc(n.path)}</td></tr>
    <tr><td class="dim">名称</td><td>${esc(n.name) || '<span class="dim">—</span>'}</td></tr>
    <tr><td class="dim">设备参数</td><td>${n.model ? esc(n.model) : '<span class="dim">自由节点（手工配置采集参数）</span>'}</td></tr>
    <tr><td class="dim">子节点</td><td>${n.child_count != null ? n.child_count : '—'}</td></tr>
  </tbody></table>`;

  if (Object.keys(n.meta || {}).length) {
    h += '<div class="sec-title">节点元数据</div><table class="tbl tbl-sm"><tbody>';
    for (const k in n.meta)
      h += `<tr><td class="dim">${esc(k)}</td><td class="mono">${esc(n.meta[k])}</td></tr>`;
    h += '</tbody></table>';
  }

  h += '<div class="sec-title">测点</div>';
  h += n.points.length
    ? '<table class="tbl tbl-sm"><thead><tr><th>键</th><th>任务</th><th>地址</th><th>类型</th><th>系数</th></tr></thead><tbody>' +
      n.points.map(p => `<tr><td class="mono">${esc(p.key)}</td><td>${esc(p.task)}</td>
        <td class="mono">${esc(p.addr)}</td><td>${esc(p.dtype)}</td><td>${p.scale}</td></tr>`).join('') +
      '</tbody></table>'
    : (n.model ? '<div class="dim">测点由设备规格 ' + esc(n.model) +
                 ' 定义，将在「生效」编译时展开（P2）</div>'
               : '<div class="dim">无测点</div>');

  h += `<div style="margin-top:12px">
    <button class="btn btn-sm btn-primary" onclick="nodeAddChild(${n.id})">+ 子设备</button>
    <button class="btn btn-sm" onclick="nodeInstantiate(${n.id})">按规格实例化设备</button>
    <button class="btn btn-sm" onclick="nodeRename(${n.id})">改名</button>
    <button class="btn btn-sm btn-danger" onclick="nodeDel(${n.id})">删除</button>
  </div>`;
  document.getElementById('nodeDetail').innerHTML = h;
}

// 实例化：把整个设备规格展开成子树。先问后端要建多少节点，再让用户确认 ——
// RACK001 一次就是 809 个节点，点错了很难收拾。
async function nodeInstantiate(pid) {
  const model = prompt('要实例化的设备规格\n可选：' +
                       _models.map(m => m.code).join(' / '));
  if (!model) return;
  let size = '?';
  try {
    const r = await api('GET', `/api/models/${encodeURIComponent(model)}/instance_size`);
    size = r.nodes;
  } catch (e) { return fail(e); }
  const code = prompt(`实例节点 code（不能含 "."）\n\n展开 ${model} 将创建 ${size} 个节点`);
  if (!code) return;
  const name = prompt('名称', '') || '';
  try {
    const r = await api('POST', '/api/tree/instantiate',
                        { parent_id: pid, code, name, model });
    alert(`已创建 ${r.created} 个节点`);
    treeLoad();
  } catch (e) { fail(e); }
}

async function treeNewRoot() {
  const code = prompt('根节点 code（不能含 "."）'); if (!code) return;
  const name = prompt('名称', '') || '';
  try { await api('POST', '/api/tree/node', { parent_id: null, code, name }); treeLoad(); }
  catch (e) { fail(e); }
}

async function nodeAddChild(pid) {
  const code = prompt('子节点 code（不能含 "."）'); if (!code) return;
  const name = prompt('名称', '') || '';
  const model = prompt('设备规格（留空 = 自由节点）', '') || '';
  try {
    await api('POST', '/api/tree/node', { parent_id: pid, code, name, model });
    treeLoad();
  } catch (e) { fail(e); }
}

async function nodeRename(id) {
  const n = _nodes.find(x => x.id === id); if (!n) return;
  const code = prompt('新的 code', n.code); if (!code || code === n.code) return;
  try {
    // 后端会重写整棵子树的 path；测点靠 id 关联，不受影响
    await api('PUT', '/api/tree/node/' + id, { code });
    treeLoad();
  } catch (e) { fail(e); }
}

async function nodeDel(id) {
  const n = _nodes.find(x => x.id === id); if (!n) return;
  if (!confirm(`删除节点 "${n.path}"？\n其所有子节点与测点会一并删除。`)) return;
  try { await api('DELETE', '/api/tree/node/' + id); treeLoad();
        document.getElementById('nodeDetail').innerHTML =
          '<div class="empty">从左侧选择一个节点</div>'; }
  catch (e) { fail(e); }
}

async function treeImportCsv(input) {
  const f = input.files && input.files[0];
  if (!f) return;
  input.value = '';   // 允许再次选同一文件
  if (!confirm(`导入 "${f.name}"？\n\n这是【全量替换】：现有设备树会被该 CSV 的内容取代。\n若校验不通过则原树保持不变。`))
    return;
  try {
    const r = await api('POST', '/api/tree/csv', await f.text());
    alert(`导入成功：${r.nodes} 个节点 / ${r.points} 个测点`);
    treeLoad();
  } catch (e) { fail(e); }
}

// ══ 「生效」：编译设备树 ══════════════════════════════════════════════════
// 编译只写编译快照，【不影响正在跑的采集】—— 采集器读的是启动时载入的那一版，
// 要用新版本需重启（热切换属 P2b 后续）。故这里的确认文案不吓唬用户。
async function doCompile() {
  let r;
  try { r = await api('POST', '/api/compile'); }
  catch (e) { return fail(e); }

  let msg = `编译完成，版本 ${r.version}\n\n`
          + `节点 ${r.nodes}　测点 ${r.points}\n`
          + `　由设备规格公式展开 ${r.from_model}\n`
          + `　节点手工配置/覆盖 ${r.overridden}\n`;
  if (r.warning_count > 0) {
    msg += `\n告警 ${r.warning_count} 条`
         + (r.warnings_truncated ? `（下列为前 ${r.warnings.length} 条）` : '') + '：\n';
    msg += r.warnings.map(w => '· ' + w).join('\n');
  }
  // 版本落库与"换到采集线程上"是两回事：后者可能失败（快照无可运行任务等），
  // 此时编译仍是成功的。含糊地报一句"完成"会让用户以为现场已经在跑新点表了。
  msg += '\n\n' + (r.applied ? '✓ ' : '⚠ ') + (r.reload_msg || '');
  alert(msg);
}

// ══ 通讯通道 ══════════════════════════════════════════════════════════════
let _chans = [];

async function chansLoad() {
  try { _chans = await api('GET', '/api/channels'); }
  catch (e) { return fail(e); }

  const DIR = { up:'上行', down:'下行', both:'双向' };
  let h = '<thead><tr><th>通道 ID</th><th>类型</th><th>方向</th><th>目标</th>'
        + '<th>启用</th><th>操作</th></tr></thead><tbody>';
  if (!_chans.length)
    h += '<tr><td colspan="6" class="empty">暂无通道</td></tr>';
  for (const c of _chans) {
    const cf = c.config || {};
    const tgt = cf.broker ? `${cf.broker}:${cf.port||''}`
              : cf.brokers ? cf.brokers
              : cf.path ? cf.path : '—';
    h += `<tr>
      <td><b>${esc(c.id)}</b></td>
      <td><span class="badge">${esc(c.type)}</span></td>
      <td>${DIR[c.direction] || esc(c.direction)}</td>
      <td class="mono">${esc(tgt)}</td>
      <td>${c.enabled ? '<span class="ok">是</span>' : '<span class="dim">否</span>'}</td>
      <td><button class="btn btn-xs btn-danger" onclick="chanDel('${esc(c.id)}')">删除</button></td>
    </tr>`;
  }
  document.getElementById('chansTbl').innerHTML = h + '</tbody>';
}

async function chanNew() {
  const id = prompt('通道 ID（如 mqtt_main / kafka_dw）'); if (!id) return;
  const type = prompt('类型：mqtt / kafka / file', 'mqtt'); if (!type) return;
  const direction = prompt('方向：up（上行）/ down（下行）/ both（双向）', 'up') || 'up';
  let config = {};
  if (type === 'mqtt') {
    config = { broker: prompt('Broker 地址', '127.0.0.1') || '127.0.0.1',
               port: parseInt(prompt('端口', '1883') || '1883', 10),
               topic_prefix: prompt('主题前缀', 'factory/line1') || 'factory/line1' };
  } else if (type === 'kafka') {
    config = { brokers: prompt('Broker 列表（逗号分隔）', '127.0.0.1:9092') || '',
               topic: prompt('Topic', 'industrial') || 'industrial' };
  } else {
    config = { path: prompt('输出目录', './out') || './out',
               rotate_mb: parseInt(prompt('单文件上限 MB', '64') || '64', 10) };
  }
  try { await api('POST', '/api/channels', { id, type, direction, config, enabled:false });
        chansLoad(); }
  catch (e) { fail(e); }
}

async function chanDel(id) {
  if (!confirm(`删除通道 "${id}"？`)) return;
  try { await api('DELETE', '/api/channels/' + encodeURIComponent(id)); chansLoad(); }
  catch (e) { fail(e); }
}
