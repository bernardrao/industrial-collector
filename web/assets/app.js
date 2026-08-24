// ── 页面路由 ──────────────────────────────────────────────────────────────────
function showPage(name, el) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
  const pg = document.getElementById('page-' + name);
  if (pg) pg.classList.add('active');
  if (el) el.classList.add('active');
  if (name === 'config')   cfgLoad();
  if (name === 'tasks')    tasksLoad();
  if (name === 'models')   modelsLoad();
  if (name === 'tree')     treeLoad();
  if (name === 'channels') chansLoad();
}

function toggleSidebar() {
  const sb = document.getElementById('sidebar');
  const cw = document.getElementById('wrapper').querySelector('.content-wrapper');
  const isMobile = window.innerWidth <= 768;
  if (isMobile) {
    sb.classList.toggle('open');
  } else {
    sb.classList.toggle('collapsed');
    cw.classList.toggle('expanded');
  }
}

function switchTab(el, groupId) {
  const group = document.getElementById(groupId);
  group.querySelectorAll('.tab-nav-item').forEach(t => t.classList.remove('active'));
  el.classList.add('active');
  const tabId = el.dataset.tab;
  const container = el.closest('.card-body') || el.closest('.modal-body') || document;
  container.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
  const pane = document.getElementById(tabId);
  if (pane) pane.classList.add('active');
}

// ── 时钟 ──────────────────────────────────────────────────────────────────────
setInterval(() => {
  document.getElementById('clock').textContent =
    new Date().toLocaleTimeString('zh-CN', { hour12: false });
}, 1000);

// ── 进度条 ───────────────────────────────────────────────────────────────────
function showBar() {
  const b = document.getElementById('rbar');
  b.style.width = '60%';
  setTimeout(() => b.style.width = '100%', 200);
  setTimeout(() => { b.style.transition = 'none'; b.style.width = '0';
    setTimeout(() => b.style.transition = 'width .3s', 50); }, 600);
}

// ── 监控页状态 ────────────────────────────────────────────────────────────────
const hist = {}; const HLEN = 60;
// SSE 每次推来的每设备快照：{ deviceId: {total, list:[...最多200条...]} }
let allDeviceData = {};
// 当前显示的这一页（服务端拉的完整页）——只有大设备才用；小设备直接用 SSE 数据
let pageCache = null;   // {device, total, page, size, list}
let pagePos   = { page: 1, size: 100 };

// ── 树形下钻状态 ─────────────────────────────────────────────────────────────
// viewMode: 'flat'（平铺分页）或 'tree'（树形下钻）
// **默认 tree**：这个界面 v2 起主打拓扑；扁平设备（Modbus 3 点、DLT 5 点）
// 首次拿到 SSE 才知道没拓扑，会自动切回 flat。左右布局：左树 / 右详情。
let viewMode = 'tree';
let treePath = '';        // 当前正在看的树节点路径，'' = 根
let treeData = null;      // /api/tree/live 返回的最新数据
let treeAutoPicked = {};  // 记录某设备已被自动切到树形，防止用户手动切回后又被抢回去
let _treePolling = false;
let _navBuilt   = false;  // 左侧树是否已构建（首帧后建）
const _navChildrenCache = new Map();   // parent_path → 该层节点数组，减少重复拉取

// 切视图。手动切换会锁定选择，SSE 不再自动改回。
function setViewMode(m, auto = false) {
  const first = !viewMode;
  if (!first && m === viewMode) return;
  viewMode = m;
  const dev = document.getElementById('dsel').value;
  if (!auto && dev) treeAutoPicked[dev] = true;   // 用户手动动过就别再自动改
  // 用 grid 显示切换而不是通用的 '' —— split-view 里 grid=none 会撕掉整列
  document.querySelectorAll('.flat-only').forEach(e => e.style.display = m === 'flat' ? '' : 'none');
  document.querySelectorAll('.tree-only').forEach(e => e.style.display = m === 'tree' ? '' : 'none');
  const vf = document.getElementById('vm-flat'); if (vf) vf.classList.toggle('active', m === 'flat');
  const vt = document.getElementById('vm-tree'); if (vt) vt.classList.toggle('active', m === 'tree');
  if (m === 'tree') {
    if (!_navBuilt) buildNav();
    // 进树时，若选中的是设备，就从设备根路径起（Modbus 站→ station 之类），
    // 否则从整棵树的根起
    treePath = dev && guessRootPath(dev) || '';
    refreshTree();
  } else {
    refreshPage();
  }
}
// 页面加载时按当前 viewMode（默认 tree）把 flat-only / tree-only 显隐调整好
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('.flat-only').forEach(e => e.style.display = viewMode === 'flat' ? '' : 'none');
  document.querySelectorAll('.tree-only').forEach(e => e.style.display = viewMode === 'tree' ? '' : 'none');
  if (viewMode === 'tree') buildNav();
});

// 由点位名反推树的进入点：ess_modbus_sim 的点全以 station.* 开头，从 station
// 进入；DLT 的点全以 <task>/ 开头，其实也可从 <task> 进入（tree_nodes 表里
// 每个任务本身也是一个自由节点）。找一个"所有点都以之为前缀的最长路径"。
function guessRootPath(dev) {
  const meta = allDeviceData[dev];
  if (!meta || !meta.list || !meta.list.length) return '';
  // 从第一条点位名 "a.b.c.d/key" 里取 "a.b.c.d" 去掉最后一级 = 它的直接父节点
  // 但我们要设备级根：取最长公共 dot 前缀
  const paths = meta.list.map(p => p.name.split('/')[0]);
  let common = paths[0];
  for (const p of paths.slice(1)) {
    let i = 0;
    while (i < common.length && i < p.length && common[i] === p[i]) i++;
    common = common.slice(0, i);
  }
  // 修剪到 '.' 边界
  const dot = common.lastIndexOf('.');
  if (dot > 0) common = common.slice(0, dot);
  return common || paths[0].split('.')[0];   // 至少给个顶级 code
}

// 有没有拓扑：点位名里含 '.'（`station.rack01.../voltage` 而非扁平 `pressure`）
function hasTopology(dev) {
  const meta = allDeviceData[dev];
  if (!meta || !meta.list || !meta.list.length) return false;
  return meta.list.some(p => (p.name.split('/')[0] || '').includes('.'));
}

async function refreshTree() {
  if (_treePolling) return;
  _treePolling = true;
  try {
    const r = await fetch('/api/tree/live?path=' + encodeURIComponent(treePath));
    if (r.status === 404) {
      // 该节点在 config.db 里不存在 —— 常见场景：本实例的设备没被迁进树
      // （config.db 是相对 CWD 的老遗留，跨实例共用时会撞上）。降级到平铺，
      // 别让用户对着一片红字发呆。
      document.getElementById('own-block').innerHTML =
        '<div class="grid-empty" style="padding:12px">当前设备不在设备树中，已切换为平铺视图</div>';
      setViewMode('flat', true);
      return;
    }
    if (!r.ok) throw new Error(r.status);
    treeData = await r.json();
    renderTree();
  } catch (e) {
    document.getElementById('own-block').innerHTML =
      '<div class="grid-empty" style="padding:12px;color:var(--red)">拉取树失败：' + esc(e.message || e) + '</div>';
  } finally { _treePolling = false; }
}

// 面包屑："根 › 电站 › 1号舱 › cluster1 › pack1"
function renderCrumbs() {
  const c = document.getElementById('crumbs');
  if (!c) return;
  const parts = treePath ? treePath.split('.') : [];
  const paths = [''];
  for (let i = 0; i < parts.length; i++) paths.push(parts.slice(0, i + 1).join('.'));
  const labels = ['(根)'].concat(parts);
  const html = labels.map((lbl, i) => {
    const p = paths[i];
    if (i === labels.length - 1)
      return `<span class="crumb-cur">${esc(treeData && treeData.name || lbl)}</span>`;
    // onclick 属性外层是双引号，参数必须【单引号】—— 直接嵌 JSON.stringify
    // 会把内嵌的双引号顶破外层属性引号，drill 链接一片不响应（实测症状：
    // "下钻链接未生效"）。path 只有 [a-z0-9._]，单引号包着够安全。
    return `<a class="crumb-link" onclick="drillTo('${esc(p)}');return false" href="#">${esc(lbl)}</a>`;
  }).join('<span class="crumb-sep">›</span>');
  c.innerHTML = html;
}
// 全局暴露 drillTo 供 onclick 用
window.drillTo = function (path) {
  treePath = path;
  refreshTree();
  // 同步在左侧树里高亮 + 展开父链
  highlightNav(path);
};

// ── 左侧树导航（复用 /api/tree 的懒加载）─────────────────────────────────────
// 与设备树页那棵各自独立：一台工控机可能只想在监控页专挑一个储能站的树看，
// 而设备树页的 lazy 展开是给编辑用的。共用一段 API 但各自维护 DOM 状态。
async function navFetchLevel(parentId) {
  const url = parentId == null ? '/api/tree' : '/api/tree?parent=' + parentId;
  const r = await fetch(url); if (!r.ok) throw new Error(r.status);
  return r.json();
}

function navRowHtml(n, depth) {
  const kids = n.child_count > 0
    ? `<span class="tnav-toggle" data-id="${n.id}" data-path="${esc(n.path)}">▸</span>`
    : `<span class="tnav-toggle leaf">·</span>`;
  const model = n.model ? `<span class="tnav-model">${esc(n.model)}</span>` : '';
  const cnt   = n.child_count > 0 ? `<span class="tnav-cnt">${n.child_count}</span>` : '';
  return `<div class="tnav-row" data-path="${esc(n.path)}" data-id="${n.id}"
               style="padding-left:${8 + depth*14}px">
      ${kids}<span class="tnav-lbl">${esc(n.name || n.code)}</span>${model}${cnt}
    </div><div class="tnav-children" id="tnav-c-${n.id}" data-depth="${depth+1}"></div>`;
}

async function buildNav() {
  const box = document.getElementById('tree-nav');
  if (!box) return;
  box.innerHTML = '<div class="grid-empty" style="padding:12px">加载中…</div>';
  try {
    const rows = await navFetchLevel(null);
    if (!rows.length) { box.innerHTML = '<div class="grid-empty" style="padding:12px">设备树为空</div>'; return; }
    box.innerHTML = rows.map(n => navRowHtml(n, 0)).join('');
    _navBuilt = true;
    // 首次挂事件：委托整个 nav 处理点击
    box.addEventListener('click', navClick);
    // 首帧就走一次高亮 / 默认下钻到最深的顶级
    if (treePath) highlightNav(treePath);
    else if (rows.length === 1) drillTo(rows[0].path);
  } catch (e) {
    box.innerHTML = '<div class="grid-empty" style="padding:12px;color:var(--red)">加载失败：' + esc(e.message) + '</div>';
  }
}

async function navClick(ev) {
  const tog = ev.target.closest('.tnav-toggle:not(.leaf)');
  const row = ev.target.closest('.tnav-row');
  if (tog) {
    ev.stopPropagation();
    await navToggle(tog);
    return;
  }
  if (row) drillTo(row.dataset.path);
}

async function navToggle(tog) {
  const id  = tog.dataset.id;
  const box = document.getElementById('tnav-c-' + id);
  if (!box) return;
  if (box.dataset.open === '1') {
    box.innerHTML = ''; box.dataset.open = '0'; tog.textContent = '▸'; return;
  }
  tog.textContent = '⋯';
  try {
    const depth = parseInt(box.dataset.depth || '1', 10);
    const rows = await navFetchLevel(id);
    _navChildrenCache.set(tog.dataset.path, rows);
    box.innerHTML = rows.map(n => navRowHtml(n, depth)).join('');
    box.dataset.open = '1'; tog.textContent = '▾';
  } catch (e) { tog.textContent = '▸'; }
}

// 高亮当前路径 + 沿祖先链把所有 toggle 展开。惰性触发，被点开过的节点靠
// treeToggle 展开；这里只处理"跳到深处节点自动展开父链"这个交互。
async function highlightNav(path) {
  const box = document.getElementById('tree-nav');
  if (!box) return;
  // 先清旧高亮
  box.querySelectorAll('.tnav-row.current').forEach(el => el.classList.remove('current'));
  if (!path) return;
  const parts = path.split('.');
  const chain = [];
  for (let i = 0; i < parts.length; i++) chain.push(parts.slice(0, i + 1).join('.'));
  // 逐层：若该 path 的行不在 DOM，就展开它的父行
  for (let i = 0; i < chain.length; i++) {
    let row = box.querySelector(`.tnav-row[data-path="${cssEsc(chain[i])}"]`);
    if (row) continue;   // 已在 DOM
    if (i === 0) continue; // 根级理论上首帧就在
    // 展开父行的 toggle
    const parentPath = chain[i - 1];
    const parentRow = box.querySelector(`.tnav-row[data-path="${cssEsc(parentPath)}"]`);
    if (!parentRow) return;   // 更上层还没展开，逐层递归失败就止步
    const tog = parentRow.querySelector('.tnav-toggle:not(.leaf)');
    if (tog && tog.dataset.id) await navToggle(tog);
  }
  const cur = box.querySelector(`.tnav-row[data-path="${cssEsc(path)}"]`);
  if (cur) { cur.classList.add('current'); cur.scrollIntoView({ block: 'nearest' }); }
}

// data-path 属性里可能有 '.'（e.g. 'station.rack01'），CSS 选择器里 '.' 是类
// 前缀，需要转义。querySelector 用 CSS.escape 或手工替换。
function cssEsc(s) {
  return (window.CSS && CSS.escape) ? CSS.escape(s)
    : String(s).replace(/[^a-zA-Z0-9_-]/g, ch => '\\' + ch);
}

// ── 趋势联动 ─────────────────────────────────────────────────────────────────
// 点了哪个具体点，趋势图就跟哪个。逻辑：设 tsel.value → 高亮该 cell → 重画。
// 单独抽出来是因为两处地方都要用（cell 点击、kid-kv 点击）。
function pickTrend(name) {
  if (!name) return;
  const tsel = document.getElementById('tsel');
  if (!tsel) return;
  // tsel 可能还没有这个 option（首次点它，或者只在树里刚出现）—— 追加一个
  if (![...tsel.options].some(o => o.value === name)) {
    const o = document.createElement('option');
    o.value = o.text = name;
    tsel.add(o);
  }
  tsel.value = name;
  // 视觉高亮：清旧、给当前的加 class
  document.querySelectorAll('.pickable.picked').forEach(el => el.classList.remove('picked'));
  document.querySelectorAll(`.pickable[data-tname="${cssEsc(name)}"]`).forEach(el => el.classList.add('picked'));
  drawTrend();
}

// 把可见点位名字同步进趋势下拉。保留【当前 tsel.value】——用户点过之后
// 就是明确选择，重渲染不能撞飞。找不到 current 才回退到第一个。
function syncTrendOptions(names) {
  const tsel = document.getElementById('tsel');
  if (!tsel) return;
  const cur = tsel.value;
  // 去重排序；已选中的即便不在当前视图里也留着 —— 用户可能在 pack 一层选了
  // 某个电芯的 voltage，下钻进 cell 层之后那名字不在 kid.points 里但仍要看
  const set = new Set(names);
  if (cur) set.add(cur);
  const arr = [...set];
  const same = arr.length === tsel.options.length
    && arr.every((n, i) => tsel.options[i].value === n);
  if (!same) tsel.innerHTML = arr.map(n => `<option>${esc(n)}</option>`).join('');
  if (cur && set.has(cur)) tsel.value = cur;
  else if (arr.length) tsel.value = arr[0];
  // 有 tsel.value 时同步高亮（重渲染 grid/kid 之后 picked class 会丢，得重挂）
  if (tsel.value) {
    document.querySelectorAll(`.pickable[data-tname="${cssEsc(tsel.value)}"]`)
      .forEach(el => el.classList.add('picked'));
  }
}

// 委托点击：只挂一次，捕获整个数据点面板下的 .pickable
document.addEventListener('DOMContentLoaded', () => {
  const detail = document.querySelector('.split-detail');
  if (!detail) return;
  detail.addEventListener('click', (ev) => {
    const pick = ev.target.closest('.pickable');
    if (!pick) return;
    // 别让 kid-kv 的点击顺带触发 kid-row 的 drillTo
    ev.stopPropagation();
    pickTrend(pick.dataset.tname);
  });
});

// 一格点位的通用渲染（复用 flat 的 .cell 样式）
function cellHtml(p) {
  const q = (p.quality || 'BAD').toLowerCase();
  const v = p.value != null
    ? (typeof p.value === 'boolean' ? (p.value ? 'ON' : 'OFF')
      : parseFloat(p.value).toLocaleString('zh-CN', { maximumFractionDigits: 3 }))
    : 'N/A';
  const shortName = p.name.split('/').pop() || p.name;
  // data-tname = 完整点位名，供 pickTrend 用（趋势联动的唯一 key）
  return `<div class="cell ${q} pickable" data-tname="${esc(p.name)}">
    <div class="cn">${esc(shortName)}</div>
    <div class="cv ${q}">${v}</div>
    <div class="cu">${esc(p.unit || '')}</div></div>`;
}

// 一子节点行：可点则进入；标题 + 若干关键点位；叶子节点直接列全部点位
function kidRowHtml(k) {
  const leaf = k.child_count === 0;
  const model = k.model ? `<span class="kid-model">${esc(k.model)}</span>` : '';
  const more  = leaf ? '' : `<span class="kid-more">▸ 下钻</span>`;
  const kvs = k.points.slice(0, 6).map(p => {
    const short = p.name.split('/').pop();
    const v = p.value != null
      ? (typeof p.value === 'boolean' ? (p.value ? 'ON' : 'OFF')
        : parseFloat(p.value).toLocaleString('zh-CN', { maximumFractionDigits: 3 }))
      : 'N/A';
    const q = (p.quality || 'BAD').toLowerCase();
    // pickable + data-tname：点它去看趋势；stopPropagation 在 tree-view 上做，
    // 避免跟 kid-row 的 drillTo onclick 打架（点值看趋势，点空白进下钻）。
    return `<span class="kid-kv ${q} pickable" data-tname="${esc(p.name)}">${esc(short)}<span class="kv-v">${v}</span><span class="kv-u">${esc(p.unit || '')}</span></span>`;
  }).join('');
  const more_pts = k.points.length > 6 ? `<span class="kid-cnt">+${k.points.length - 6}</span>` : '';
  const onclick = leaf ? '' : `onclick="drillTo('${esc(k.path)}');"`;
  return `<div class="kid-row${leaf ? ' leaf' : ''}" ${onclick}>
    <div class="kid-title">${esc(k.name || k.code)}${model}${more}</div>
    <div class="kid-values">${kvs}${more_pts}</div>
    <div class="kid-cnt">${leaf ? (k.points.length + ' 点') : (k.child_count + ' 子节点')}</div>
  </div>`;
}

// 聚合核对：对若干个"约定俗成"的键做求和/最大/最小，与本节点自报的对应键比。
// 差在阈值内绿色打勾，超过红色 —— 这才是"层级聚合"这个功能的真判据。
// 判据取模拟器已固化的关系（见 ess_modbus_sim.py 顶部注释）：
//   pack_voltage    = Σ children.voltage
//   cell_v_max/min  = max/min children.voltage
//   cell_t_max      = max   children.temp
//   cluster_voltage = Σ children.pack_voltage
// 别的键（rack_soc、power…）暂不核对，硬做要一套单位/量纲规则，留 P4 后续。
const AGG_RULES = [
  { own: 'pack_voltage',    child: 'voltage',      fn: 'sum', tol: 0.05 },
  { own: 'cell_v_max',      child: 'voltage',      fn: 'max', tol: 0.002 },
  { own: 'cell_v_min',      child: 'voltage',      fn: 'min', tol: 0.002 },
  { own: 'cell_t_max',      child: 'temp',         fn: 'max', tol: 0.15 },
  { own: 'cluster_voltage', child: 'pack_voltage', fn: 'sum', tol: 0.3  },
];

function computeAgg(rule) {
  const vals = [];
  for (const c of treeData.children) {
    for (const p of c.points) {
      if (p.name.endsWith('/' + rule.child) && typeof p.value === 'number') vals.push(p.value);
    }
  }
  if (!vals.length) return null;
  if (rule.fn === 'sum') return vals.reduce((a, b) => a + b, 0);
  if (rule.fn === 'max') return Math.max(...vals);
  if (rule.fn === 'min') return Math.min(...vals);
  return null;
}

function renderTree() {
  if (!treeData) return;
  renderCrumbs();
  const own = treeData.own_points || [];
  const ownEl = document.getElementById('own-block');
  const kidsEl = document.getElementById('kids-block');

  // 喂本节点自有点位 + 子节点点位 到 hist —— 树模式 renderPts 不跑，
  // 不喂的话点了趋势也看不到历史。趋势下拉里也补齐这批名字。
  const feed = [];
  own.forEach(p => feed.push(p));
  (treeData.children || []).forEach(c => (c.points || []).forEach(p => feed.push(p)));
  feed.forEach(p => {
    if (p.value == null || typeof p.value !== 'number') return;
    if (!hist[p.name]) hist[p.name] = [];
    hist[p.name].push(p.value);
    if (hist[p.name].length > HLEN) hist[p.name].shift();
  });
  syncTrendOptions(feed.map(p => p.name));

  // 自有点位 + 聚合核对（有就显示）
  let aggHtml = '';
  const ownByKey = {};
  own.forEach(p => { ownByKey[p.name.split('/').pop()] = p; });
  for (const r of AGG_RULES) {
    const rep = ownByKey[r.own];
    if (!rep || typeof rep.value !== 'number') continue;
    const computed = computeAgg(r);
    if (computed == null) continue;
    const diff = Math.abs(computed - rep.value);
    const ok = diff <= r.tol;
    aggHtml += `<div class="agg-row">
      <span>${r.fn.toUpperCase()}(子.${r.child}) = <b>${computed.toFixed(3)}</b></span>
      <span>vs 自报 ${r.own} = <b>${rep.value.toFixed(3)}</b></span>
      <span class="${ok ? 'agg-ok' : 'agg-warn'}">差 ${(diff * 1000).toFixed(1)} mV/单位 ${ok ? '✓' : '✗ 超出 ' + r.tol}</span>
    </div>`;
  }

  ownEl.innerHTML =
    (own.length ? '<h4>本节点自有点位</h4><div class="grid">' + own.map(cellHtml).join('') + '</div>' : '')
    + aggHtml
    + (!own.length && !aggHtml ? '<div class="grid-empty" style="padding:12px">此节点无直挂点位</div>' : '');

  const kids = treeData.children || [];
  kidsEl.innerHTML = kids.length
    ? '<h4 style="padding:8px 12px 6px;margin:0;font-size:12px;color:var(--dim);font-weight:normal;background:var(--panel2)">'
      + `子节点 <span class="dim">(${kids.length})</span></h4>`
      + kids.map(kidRowHtml).join('')
    : '<div class="grid-empty" style="padding:12px">无子节点</div>';
}

function onDevChange() {
  pagePos.page = 1;              // 切设备回到第 1 页
  pageCache = null;
  treeData = null;
  const dev = document.getElementById('dsel').value;
  // 有拓扑且用户没手动锁过就自动进树形；否则平铺
  if (dev && hasTopology(dev) && !treeAutoPicked[dev]) {
    setViewMode('tree', true);
  } else if (viewMode === 'tree') {
    // 切到平铺设备时降回平铺
    setViewMode('flat', true);
  } else {
    refreshPage();
  }
}

function pageStep(delta) {
  const total = currentTotal();
  const size  = pagePos.size;
  const max   = Math.max(1, Math.ceil(total / size));
  pagePos.page = Math.min(max, Math.max(1, pagePos.page + delta));
  refreshPage();
}

document.addEventListener('DOMContentLoaded', () => {
  const psel = document.getElementById('psize');
  if (psel) psel.addEventListener('change', () => {
    pagePos.size = parseInt(psel.value, 10) || 100;
    pagePos.page = 1;
    refreshPage();
  });
});

// 当前视图的总点数：单设备 = 该设备 total；"全部设备" = 所有设备各自 list 之和
// （聚合视图只吃 SSE 已下发的那 200 条 —— 13084 全塞进"全部"的 UI 是灾难）
function currentTotal() {
  const sel = document.getElementById('dsel').value;
  if (sel) return (allDeviceData[sel] || {}).total || 0;
  let n = 0;
  for (const v of Object.values(allDeviceData)) n += (v.list || []).length;
  return n;
}

// 拉当前设备当前页；小设备的 total ≤ SSE 覆盖数就直接用 SSE 数据，省一次 HTTP
async function refreshPage() {
  const sel = document.getElementById('dsel').value;
  if (!sel) { pageCache = null; renderPts(); return; }
  const meta = allDeviceData[sel];
  if (!meta) { pageCache = null; renderPts(); return; }
  // SSE 已经把整个设备发过来了（total ≤ list 长度）→ 不必拉
  if (meta.list && meta.total <= meta.list.length && pagePos.page === 1) {
    pageCache = null; renderPts(); return;
  }
  try {
    const r = await fetch(`/api/points?device=${encodeURIComponent(sel)}`
                          + `&page=${pagePos.page}&size=${pagePos.size}`);
    pageCache = await r.json();
  } catch (e) { pageCache = null; }
  renderPts();
}

// 决定要渲染哪些点。三种情形：
//   ① 单设备 + 有服务端页缓存    → 用 pageCache.list
//   ② 单设备 + 无缓存（小设备）  → 用 SSE 的 list（前 200 内）
//   ③ 全部设备                    → 拼所有设备 SSE 的 list（各设备最多前 200）
function getActivePts() {
  const sel = document.getElementById('dsel').value;
  if (sel && pageCache && pageCache.device === sel) return pageCache.list || [];
  if (sel) return (allDeviceData[sel] || {}).list || [];
  const all = [];
  for (const [did, meta] of Object.entries(allDeviceData))
    (meta.list || []).forEach(p => all.push({ ...p, name: did + '/' + p.name }));
  return all;
}

function renderPts(deviceData) {
  if (deviceData) allDeviceData = deviceData;
  const dsel = document.getElementById('dsel');
  const curDev = dsel.value;
  const ids = Object.keys(allDeviceData).sort();
  const existing = [...dsel.options].slice(1).map(o => o.value);
  if (existing.join(',') !== ids.join(',')) {
    while (dsel.options.length > 1) dsel.remove(1);
    ids.forEach(id => { const o = document.createElement('option'); o.value = o.text = id; dsel.add(o); });
    if (ids.includes(curDev)) dsel.value = curDev;
  }

  const pts   = getActivePts();
  const total = currentTotal();
  const size  = pagePos.size;
  const maxPage = Math.max(1, Math.ceil(total / size));
  if (pagePos.page > maxPage) pagePos.page = maxPage;

  document.getElementById('ptcnt').textContent =
    total > pts.length ? `${pts.length}/${total} 点` : `${total} 点`;
  const ppos = document.getElementById('ppos');
  if (ppos) ppos.textContent = total > 0 ? `第 ${pagePos.page} / ${maxPage} 页` : '';
  const pp = document.getElementById('pprev'), pn = document.getElementById('pnext');
  if (pp) pp.disabled = pagePos.page <= 1;
  if (pn) pn.disabled = pagePos.page >= maxPage;

  // 趋势下拉：只列当前页里已有的点，别把 13084 个塞进 <select>
  syncTrendOptions(pts.map(p => p.name));
  pts.forEach(p => {
    if (p.value != null && typeof p.value === 'number') {
      if (!hist[p.name]) hist[p.name] = [];
      hist[p.name].push(p.value);
      if (hist[p.name].length > HLEN) hist[p.name].shift();
    }
  });

  const grid = document.getElementById('grid');
  if (!pts.length) {
    grid.innerHTML = '<div class="grid-empty">等待数据...</div>';
  } else {
    // 用 cellHtml 复用 pickable / data-tname，平铺视图同样支持点值看趋势
    grid.innerHTML = pts.map(cellHtml).join('');
  }
  drawTrend();
}

function drawTrend() {
  const canvas = document.getElementById('tc');
  const name = document.getElementById('tsel').value;
  const data = hist[name] || [];
  const dpr = window.devicePixelRatio || 1;
  const W = canvas.parentElement.clientWidth - 28, H = 80;
  canvas.width = W * dpr; canvas.height = H * dpr;
  canvas.style.width = W + 'px'; canvas.style.height = H + 'px';
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr); ctx.clearRect(0, 0, W, H);
  if (data.length < 2) {
    ctx.fillStyle = '#6e7681'; ctx.font = '11px monospace';
    ctx.fillText('暂无历史数据', W / 2 - 44, H / 2 + 4); return;
  }
  const mn = Math.min(...data), mx = Math.max(...data), rng = mx - mn || 1;
  const P = { t: 6, b: 18, l: 44, r: 10 };
  const iW = W - P.l - P.r, iH = H - P.t - P.b;
  const xf = i => P.l + i / (data.length - 1) * iW;
  const yf = v => P.t + iH - ((v - mn) / rng) * iH;
  [0, .5, 1].forEach(fr => {
    const y = P.t + iH * (1 - fr);
    ctx.strokeStyle = '#21262d'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(P.l, y); ctx.lineTo(W - P.r, y); ctx.stroke();
    ctx.fillStyle = '#6e7681'; ctx.font = '9px monospace';
    ctx.fillText((mn + rng * fr).toFixed(2), 0, y + 4);
  });
  const g = ctx.createLinearGradient(0, P.t, 0, H - P.b);
  g.addColorStop(0, 'rgba(63,185,80,.3)'); g.addColorStop(1, 'rgba(63,185,80,0)');
  ctx.beginPath(); ctx.moveTo(xf(0), yf(data[0]));
  for (let i = 1; i < data.length; i++) ctx.lineTo(xf(i), yf(data[i]));
  ctx.lineTo(xf(data.length - 1), H - P.b); ctx.lineTo(xf(0), H - P.b);
  ctx.closePath(); ctx.fillStyle = g; ctx.fill();
  ctx.beginPath(); ctx.moveTo(xf(0), yf(data[0]));
  for (let i = 1; i < data.length; i++) ctx.lineTo(xf(i), yf(data[i]));
  ctx.strokeStyle = '#3fb950'; ctx.lineWidth = 1.5; ctx.stroke();
  const lx = xf(data.length - 1), ly = yf(data[data.length - 1]);
  ctx.beginPath(); ctx.arc(lx, ly, 3, 0, Math.PI * 2);
  ctx.fillStyle = '#3fb950'; ctx.fill();
  ctx.fillStyle = '#6e7681'; ctx.font = '9px monospace';
  ctx.fillText('-' + data.length + 's', P.l, H - 3);
  ctx.fillText('NOW', W - P.r - 24, H - 3);
}
document.getElementById('tsel').addEventListener('change', drawTrend);
window.addEventListener('resize', drawTrend);

function renderLogs(logs) {
  const c = document.getElementById('lc');
  const auto_ = document.getElementById('lauto').checked;
  c.innerHTML = logs.map(e => {
    const lv = e.level || 'INFO';
    return `<div class="ll"><span class="lt">${e.ts}</span>
      <span class="llv ${lv}">${lv}</span>
      <span class="lm">${e.msg.replace(/&/g, '&amp;').replace(/</g, '&lt;')}</span></div>`;
  }).join('');
  if (auto_) c.scrollTop = c.scrollHeight;
}

function renderStatus(s) {
  const paused = s.paused, run = s.running;
  document.getElementById('sdot').style.background = run && !paused ? 'var(--green)' : 'var(--red)';
  document.getElementById('s-gtotal').textContent = s.total_polls ?? '—';
  document.getElementById('s-gerr').textContent   = s.total_errors ?? '—';
  document.getElementById('s-gbad').textContent   = s.total_bad_points ?? '—';
  document.getElementById('s-gpub').textContent   = s.mqtt_published ?? '—';
  document.getElementById('s-devcnt').textContent = Object.keys(s.devices || {}).length;

  const curDev = document.getElementById('dsel').value;
  const ds = curDev && (s.devices || {})[curDev] ? s.devices[curDev] : null;
  if (ds) {
    document.getElementById('s-good').textContent = ds.good_polls ?? '—';
    const errEl = document.getElementById('s-err');
    // 错误 = 采集异常轮数。全 BAD 轮数与坏点数是不同的量，放进悬停提示，
    // 不再混进错误计数（CAN 被动监听总线静默时全 BAD 属正常）。
    errEl.textContent = ds.errors ?? '—';
    errEl.title = `采集异常 ${ds.errors ?? 0} 轮 · 全 BAD ${ds.dead_polls ?? 0} 轮 · `
                + `坏点 ${ds.bad_points ?? 0}/${ds.total_points ?? 0}`;
    document.getElementById('s-pub').textContent  = ds.mqtt_published ?? '—';
    const st = paused ? '已暂停' : (ds.status || '—');
    document.getElementById('s-st').textContent = st;
    document.getElementById('ptag').textContent = ds.protocol || '—';
    document.getElementById('pbadge').textContent = ds.protocol || '?';
  } else {
    document.getElementById('s-good').textContent = '—';
    document.getElementById('s-err').textContent  = s.total_errors ?? '—';
    document.getElementById('s-pub').textContent  = s.mqtt_published ?? '—';
    const stEl = document.getElementById('s-st');
    stEl.textContent = paused ? '已暂停' : run ? '运行中' : '已停止';
    document.getElementById('ptag').textContent = '多协议';
    document.getElementById('pbadge').textContent = 'MULTI';
  }
}

// ── API 调用 ──────────────────────────────────────────────────────────────────
let _refreshing = false;

async function fetchJ(url) {
  const ctrl = new AbortController();
  const tid  = setTimeout(() => ctrl.abort(), 5000);   // 5s 超时，避免挂 30s
  try {
    const r = await fetch(url, { cache: 'no-store', signal: ctrl.signal });
    if (!r.ok) throw new Error(r.status);
    return r.json();
  } finally {
    clearTimeout(tid);
  }
}

async function refresh() {
  if (_refreshing) return;
  _refreshing = true;
  showBar();
  try {
    const d = await fetchJ('/api/all?n=80');
    renderPts(d.points);
    renderStatus(d.status);
    renderLogs(d.logs);
    if (d.status.broker) document.getElementById('s-broker').textContent = d.status.broker;
  } catch (e) {
    document.getElementById('sdot').style.background = 'var(--red)';
  } finally {
    _refreshing = false;
  }
}

async function ctrl(action) {
  await fetch('/api/control', { method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action }) });
  setTimeout(refresh, 300);
}

function confirmStop() {
  if (!confirm('确定停止采集程序？')) return;
  ctrl('stop');
}

// ── SSE 实时推送（替代定时轮询）────────────────────────────────────────────────
let _sse = null;

function startSSE() {
  if (_sse) { _sse.close(); _sse = null; }
  const es = new EventSource('/api/stream');
  es.onmessage = (e) => {
    try {
      const d = JSON.parse(e.data);
      renderPts(d.points);
      renderStatus(d.status);
      renderLogs(d.logs);
      if (d.status && d.status.broker)
        document.getElementById('s-broker').textContent = d.status.broker;
      // 大设备的分页视图：SSE 只带前 200 个点，翻到 2+ 页时 SSE 帮不上忙 ——
      // 借这次 SSE 事件当"有新数据"信号，顺带刷一次当前页。频率 = SSE 心跳，
      // 恰好和现场的更新节奏对得上。
      // 树形视图同理：SSE 只给前 200 条平铺点，看不到 pack1 里 24 个电芯的实时值。
      if (viewMode === 'tree') {
        refreshTree();
      } else {
        const sel = document.getElementById('dsel').value;
        const meta = sel ? (allDeviceData[sel] || {}) : null;
        const isBigPaged = meta && meta.list && meta.total > meta.list.length;
        if (sel && (isBigPaged || pagePos.page > 1)) refreshPage();
      }
    } catch (_) {}
  };
  es.onerror = () => {
    document.getElementById('sdot').style.background = 'var(--red)';
  };
  _sse = es;
}

startSSE();
