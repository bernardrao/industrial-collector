// config.js — 配置页 CRUD 逻辑
let _fullCfg = null;       // 当前完整配置快照
let _editIdx  = -1;        // 正在编辑的设备下标（-1=新建）

const PROTO_LABELS = {
  modbus:'Modbus TCP', iec104:'IEC 104', iec61850:'IEC 61850',
  opcua:'OPC UA', dlt645:'DL/T 645', dlt698:'DL/T 698', can:'CAN / CAN FD'
};

// ── 加载 ──────────────────────────────────────────────────────────────────────
async function cfgLoad() {
  try {
    _fullCfg = await fetchJ('/api/config/full');
    cfgPopulateBasic(_fullCfg);
    cfgRenderDevTable(_fullCfg.devices || []);
  } catch(e) {
    alert('配置加载失败: ' + e.message);
  }
}

// ── 填充基础设置表单 ──────────────────────────────────────────────────────────
function cfgPopulateBasic(cfg) {
  const m = cfg.mqtt || {};
  setVal('cfg-mqtt-broker',       m.broker       ?? '');
  setVal('cfg-mqtt-port',         m.port         ?? 1883);
  setVal('cfg-mqtt-client_id',    m.client_id    ?? '');
  setVal('cfg-mqtt-username',     m.username     ?? '');
  setVal('cfg-mqtt-password',     m.password     ?? '');
  setVal('cfg-mqtt-topic_prefix', m.topic_prefix ?? '');
  setVal('cfg-mqtt-qos',          String(m.qos   ?? 1));
  setVal('cfg-mqtt-keepalive',    m.keepalive    ?? 60);
  setVal('cfg-mqtt-retain',       String(m.retain ?? false));

  const w = cfg.web || {};
  setVal('cfg-web-port',        w.port        ?? 8080);
  setVal('cfg-web-bind',        w.bind        ?? '0.0.0.0');
  setVal('cfg-web-tls_enabled', String(w.tls_enabled ?? false));
  setVal('cfg-web-tls_cert',    w.tls_cert    ?? 'cert.pem');
  setVal('cfg-web-tls_key',     w.tls_key     ?? 'key.pem');
  setVal('cfg-web-auth_enabled', String(w.auth_enabled ?? false));
  setVal('cfg-web-auth_user',    w.auth_user    ?? 'admin');
  setVal('cfg-web-auth_password',w.auth_password?? '');

  const c = cfg.cache || {};
  setVal('cfg-cache-enabled',        String(c.enabled         ?? true));
  setVal('cfg-cache-db_path',        c.db_path                ?? 'cache.db');
  setVal('cfg-cache-retention_hours',c.retention_hours        ?? 24);
  setVal('cfg-cache-max_rows',       c.max_rows               ?? 1000000);
  setVal('cfg-cache-history_topic',  c.history_topic          ?? '');
  setVal('cfg-cache-command_topic',  c.command_topic          ?? '');
  setVal('cfg-cache-replay_rate_ms', c.replay_rate_ms         ?? 50);
  setVal('cfg-cache-replay_chunk',   c.replay_chunk           ?? 200);

  const l = cfg.logging || {};
  setVal('cfg-log-level',        l.level        ?? 'info');
  setVal('cfg-log-file',         l.file         ?? 'collector.log');
  setVal('cfg-log-max_bytes',    l.max_bytes    ?? 10485760);
  setVal('cfg-log-backup_count', l.backup_count ?? 5);
}

// ── 保存基础设置 ──────────────────────────────────────────────────────────────
async function cfgSaveBasic() {
  if (!_fullCfg) { alert('配置未加载'); return; }
  const cfg = JSON.parse(JSON.stringify(_fullCfg));

  cfg.mqtt = {
    broker:       getVal('cfg-mqtt-broker'),
    port:         parseInt(getVal('cfg-mqtt-port')),
    client_id:    getVal('cfg-mqtt-client_id'),
    username:     getVal('cfg-mqtt-username'),
    password:     getVal('cfg-mqtt-password'),
    topic_prefix: getVal('cfg-mqtt-topic_prefix'),
    qos:          parseInt(getVal('cfg-mqtt-qos')),
    retain:       getVal('cfg-mqtt-retain') === 'true',
    keepalive:    parseInt(getVal('cfg-mqtt-keepalive')),
  };
  // 合并而非整块替换：表单里没有的字段（debug / debug_root）必须原样保留，
  // 否则保存基础设置会把它们悄悄重置掉。
  cfg.web = Object.assign({}, cfg.web, {
    port:          parseInt(getVal('cfg-web-port')),
    bind:          getVal('cfg-web-bind'),
    enabled:       true,
    tls_enabled:   getVal('cfg-web-tls_enabled') === 'true',
    tls_cert:      getVal('cfg-web-tls_cert'),
    tls_key:       getVal('cfg-web-tls_key'),
    auth_enabled:  getVal('cfg-web-auth_enabled') === 'true',
    auth_user:     getVal('cfg-web-auth_user'),
    auth_password: getVal('cfg-web-auth_password'),
  });
  cfg.cache = {
    enabled:         getVal('cfg-cache-enabled') === 'true',
    db_path:         getVal('cfg-cache-db_path'),
    retention_hours: parseInt(getVal('cfg-cache-retention_hours')),
    max_rows:        parseInt(getVal('cfg-cache-max_rows')),
    history_topic:   getVal('cfg-cache-history_topic'),
    command_topic:   getVal('cfg-cache-command_topic'),
    replay_rate_ms:  parseInt(getVal('cfg-cache-replay_rate_ms')),
    replay_chunk:    parseInt(getVal('cfg-cache-replay_chunk')),
  };
  cfg.logging = {
    level:        getVal('cfg-log-level'),
    file:         getVal('cfg-log-file'),
    max_bytes:    parseInt(getVal('cfg-log-max_bytes')),
    backup_count: parseInt(getVal('cfg-log-backup_count')),
  };
  await cfgPost(cfg);
}

async function cfgPost(cfg) {
  try {
    const r = await fetch('/api/config/full', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cfg)
    });
    const j = await r.json();
    if (j.ok) {
      _fullCfg = cfg;
      alert('✓ ' + j.message);
    } else {
      alert('✗ ' + j.error);
    }
  } catch(e) {
    alert('✗ 保存失败: ' + e.message);
  }
}

// ── 设备列表渲染 ──────────────────────────────────────────────────────────────
function cfgRenderDevTable(devices) {
  const tb = document.getElementById('devTableBody');
  if (!devices.length) {
    tb.innerHTML = '<tr><td colspan="7" class="dim tc">暂无设备，点击"添加设备"</td></tr>';
    return;
  }
  tb.innerHTML = devices.map((d, i) => {
    const proto = d.protocol || 'modbus';
    const ep = devEndpoint(d);
    const cnt = devPointCount(d);
    const interval = devInterval(d);
    const enBadge = d.enabled
      ? '<span class="badge badge-green">启用</span>'
      : '<span class="badge badge-yellow" style="color:var(--dim)">禁用</span>';
    return `<tr>
      <td><code>${d.id}</code></td>
      <td><span class="badge badge-blue">${PROTO_LABELS[proto]||proto}</span></td>
      <td style="font-family:var(--mono);font-size:12px">${ep}</td>
      <td style="font-family:var(--mono);font-size:12px">${cnt}</td>
      <td style="font-family:var(--mono);font-size:12px">${interval}</td>
      <td>${enBadge}</td>
      <td>
        <button class="btn btn-sm btn-default" onclick="devEdit(${i})">编辑</button>
        <button class="btn btn-sm btn-danger"  style="margin-left:4px" onclick="devDelete(${i})">删除</button>
      </td>
    </tr>`;
  }).join('');
}

function devEndpoint(d) {
  const p = d.protocol;
  if (p === 'modbus')   return (d.modbus?.host || '') + ':' + (d.modbus?.port || '');
  if (p === 'iec104')   return (d.iec104?.host || '') + ':' + (d.iec104?.port || '');
  if (p === 'iec61850') return (d.iec61850?.host || '') + ':' + (d.iec61850?.port || '');
  if (p === 'opcua')    return d.opcua?.endpoint_url || '';
  if (p === 'dlt645')   return d.dlt645?.connection_type==='tcp'
    ? (d.dlt645?.host||'') + ':' + (d.dlt645?.tcp_port||'') : (d.dlt645?.serial_port||'');
  if (p === 'dlt698')   return d.dlt698?.connection_type==='tcp'
    ? (d.dlt698?.host||'') + ':' + (d.dlt698?.tcp_port||'') : (d.dlt698?.serial_port||'');
  if (p === 'can')      return (d.can?.interface||'') + (d.can?.fd ? ' (FD)' : '');
  return '';
}

function devPointCount(d) {
  const p = d.protocol;
  if (p === 'modbus')   return (d.modbus?.points?.length   || 0) + ' 点';
  if (p === 'iec104')   return (d.iec104?.points?.length   || 0) + ' 点';
  if (p === 'iec61850') return (d.iec61850?.points?.length || 0) + ' 点';
  if (p === 'opcua')    return (d.opcua?.points?.length    || 0) + ' 点';
  if (p === 'dlt645')   return (d.dlt645?.meters?.length   || 0) + ' 表';
  if (p === 'dlt698')   return (d.dlt698?.meters?.length   || 0) + ' 表';
  if (p === 'can')      return (d.can?.signals?.length     || 0) + ' 信号';
  return '—';
}

function devInterval(d) {
  const p = d.protocol;
  if (p === 'modbus')   return d.modbus?.poll_interval   ?? '—';
  if (p === 'iec104')   return d.iec104?.poll_interval   ?? '—';
  if (p === 'iec61850') return d.iec61850?.poll_interval ?? '—';
  if (p === 'opcua')    return d.opcua?.poll_interval    ?? '—';
  if (p === 'dlt645')   return d.dlt645?.poll_interval   ?? '—';
  if (p === 'dlt698')   return d.dlt698?.poll_interval   ?? '—';
  if (p === 'can')      return d.can?.poll_interval      ?? '—';
  return '—';
}

// ── 添加设备 ──────────────────────────────────────────────────────────────────
function devAdd() {
  _editIdx = -1;
  document.getElementById('devModalTitle').textContent = '添加设备';
  devModalReset();
  document.getElementById('devModal').style.display = 'flex';
}

// ── 编辑设备 ──────────────────────────────────────────────────────────────────
function devEdit(i) {
  _editIdx = i;
  document.getElementById('devModalTitle').textContent = '编辑设备';
  const d = _fullCfg.devices[i];
  devModalReset();
  setVal('dev-id',        d.id);
  setVal('dev-protocol',  d.protocol || 'modbus');
  setVal('dev-reconnect', d.reconnect_interval_sec ?? 5);
  document.getElementById('dev-enabled').checked = d.enabled !== false;
  devProtoChange();  // 先切换显示

  const p = d.protocol || 'modbus';
  if (p === 'modbus' && d.modbus) {
    setVal('modbus-host',          d.modbus.host          ?? '');
    setVal('modbus-port',          d.modbus.port          ?? 502);
    setVal('modbus-unit_id',       d.modbus.unit_id       ?? 1);
    setVal('modbus-timeout',       d.modbus.timeout       ?? 3);
    setVal('modbus-poll_interval', d.modbus.poll_interval ?? 1.0);
    renderPointsTable('modbus', d.modbus.points || []);
  } else if (p === 'iec104' && d.iec104) {
    setVal('iec104-host',           d.iec104.host           ?? '');
    setVal('iec104-port',           d.iec104.port           ?? 2404);
    setVal('iec104-common_address', d.iec104.common_address ?? 1);
    setVal('iec104-timeout',        d.iec104.timeout        ?? 10);
    setVal('iec104-poll_interval',  d.iec104.poll_interval  ?? 2.0);
    renderPointsTable('iec104', d.iec104.points || []);
  } else if (p === 'iec61850' && d.iec61850) {
    setVal('iec61850-host',          d.iec61850.host          ?? '');
    setVal('iec61850-port',          d.iec61850.port          ?? 102);
    setVal('iec61850-timeout_ms',    d.iec61850.timeout_ms    ?? 5000);
    setVal('iec61850-poll_interval', d.iec61850.poll_interval ?? 1.0);
    setVal('iec61850-auth_password', d.iec61850.auth_password ?? '');
    setVal('iec61850-use_reports',   String(d.iec61850.use_reports ?? false));
    renderPointsTable('iec61850', d.iec61850.points || []);
  } else if (p === 'opcua' && d.opcua) {
    setVal('opcua-endpoint_url',   d.opcua.endpoint_url   ?? '');
    setVal('opcua-timeout_ms',     d.opcua.timeout_ms     ?? 5000);
    setVal('opcua-poll_interval',  d.opcua.poll_interval  ?? 1.0);
    setVal('opcua-security_policy',d.opcua.security_policy?? 'None');
    setVal('opcua-username',       d.opcua.username       ?? '');
    setVal('opcua-password',       d.opcua.password       ?? '');
    renderPointsTable('opcua', d.opcua.points || []);
  } else if (p === 'dlt645' && d.dlt645) {
    setVal('dlt-connection_type', d.dlt645.connection_type ?? 'tcp');
    setVal('dlt-host',            d.dlt645.host           ?? '');
    setVal('dlt-tcp_port',        d.dlt645.tcp_port       ?? 20108);
    setVal('dlt-serial_port',     d.dlt645.serial_port    ?? '/dev/ttyUSB0');
    setVal('dlt-baud_rate',       d.dlt645.baud_rate      ?? 9600);
    setVal('dlt-timeout_ms',      d.dlt645.timeout_ms     ?? 3000);
    setVal('dlt-poll_interval',   d.dlt645.poll_interval  ?? 5.0);
    dltConnChange();
    renderMeters('dlt645', d.dlt645.meters || []);
  } else if (p === 'dlt698' && d.dlt698) {
    setVal('dlt-connection_type', d.dlt698.connection_type ?? 'tcp');
    setVal('dlt-host',            d.dlt698.host           ?? '');
    setVal('dlt-tcp_port',        d.dlt698.tcp_port       ?? 20108);
    setVal('dlt-serial_port',     d.dlt698.serial_port    ?? '/dev/ttyUSB0');
    setVal('dlt-baud_rate',       d.dlt698.baud_rate      ?? 9600);
    setVal('dlt-timeout_ms',      d.dlt698.timeout_ms     ?? 5000);
    setVal('dlt-poll_interval',   d.dlt698.poll_interval  ?? 5.0);
    dltConnChange();
    renderMeters('dlt698', d.dlt698.meters || []);
  } else if (p === 'can' && d.can) {
    setVal('can-interface',        d.can.interface        ?? 'can0');
    setVal('can-fd',               String(d.can.fd        ?? false));
    setVal('can-poll_interval',    d.can.poll_interval    ?? 1.0);
    setVal('can-stale_timeout_ms', d.can.stale_timeout_ms ?? 5000);
    renderPointsTable('can', d.can.signals || []);
  }
  document.getElementById('devModal').style.display = 'flex';
}

// ── 删除设备 ──────────────────────────────────────────────────────────────────
async function devDelete(i) {
  const d = _fullCfg.devices[i];
  if (!confirm(`删除设备 "${d.id}"？`)) return;
  const cfg = JSON.parse(JSON.stringify(_fullCfg));
  cfg.devices.splice(i, 1);
  if (!cfg.devices.length) { alert('至少保留一个设备'); return; }
  await cfgPost(cfg);
  if (_fullCfg) cfgRenderDevTable(_fullCfg.devices);
}

// ── 保存设备 ──────────────────────────────────────────────────────────────────
async function devSave() {
  const id = getVal('dev-id').trim();
  if (!id) { alert('设备 ID 不能为空'); return; }
  const proto = getVal('dev-protocol');
  const entry = {
    id,
    enabled: document.getElementById('dev-enabled').checked,
    protocol: proto,
    reconnect_interval_sec: parseInt(getVal('dev-reconnect')) || 5,
  };

  // 协议参数
  if (proto === 'modbus') {
    entry.modbus = {
      host:          getVal('modbus-host'),
      port:          parseInt(getVal('modbus-port')),
      unit_id:       parseInt(getVal('modbus-unit_id')),
      timeout:       parseInt(getVal('modbus-timeout')),
      poll_interval: parseFloat(getVal('modbus-poll_interval')),
      points:        collectPoints('modbus'),
    };
  } else if (proto === 'iec104') {
    entry.iec104 = {
      host:           getVal('iec104-host'),
      port:           parseInt(getVal('iec104-port')),
      common_address: parseInt(getVal('iec104-common_address')),
      timeout:        parseInt(getVal('iec104-timeout')),
      poll_interval:  parseFloat(getVal('iec104-poll_interval')),
      points:         collectPoints('iec104'),
    };
  } else if (proto === 'iec61850') {
    entry.iec61850 = {
      host:          getVal('iec61850-host'),
      port:          parseInt(getVal('iec61850-port')),
      timeout_ms:    parseInt(getVal('iec61850-timeout_ms')),
      poll_interval: parseFloat(getVal('iec61850-poll_interval')),
      auth_password: getVal('iec61850-auth_password'),
      use_reports:   getVal('iec61850-use_reports') === 'true',
      points:        collectPoints('iec61850'),
    };
  } else if (proto === 'opcua') {
    entry.opcua = {
      endpoint_url:   getVal('opcua-endpoint_url'),
      timeout_ms:     parseInt(getVal('opcua-timeout_ms')),
      poll_interval:  parseFloat(getVal('opcua-poll_interval')),
      security_policy:getVal('opcua-security_policy'),
      username:       getVal('opcua-username'),
      password:       getVal('opcua-password'),
      points:         collectPoints('opcua'),
    };
  } else if (proto === 'dlt645') {
    entry.dlt645 = {
      connection_type: getVal('dlt-connection_type'),
      host:            getVal('dlt-host'),
      tcp_port:        parseInt(getVal('dlt-tcp_port')),
      serial_port:     getVal('dlt-serial_port'),
      baud_rate:       parseInt(getVal('dlt-baud_rate')),
      timeout_ms:      parseInt(getVal('dlt-timeout_ms')),
      poll_interval:   parseFloat(getVal('dlt-poll_interval')),
      meters:          collectMeters('dlt645'),
    };
  } else if (proto === 'dlt698') {
    entry.dlt698 = {
      connection_type: getVal('dlt-connection_type'),
      host:            getVal('dlt-host'),
      tcp_port:        parseInt(getVal('dlt-tcp_port')),
      serial_port:     getVal('dlt-serial_port'),
      baud_rate:       parseInt(getVal('dlt-baud_rate')),
      timeout_ms:      parseInt(getVal('dlt-timeout_ms')),
      poll_interval:   parseFloat(getVal('dlt-poll_interval')),
      meters:          collectMeters('dlt698'),
    };
  } else if (proto === 'can') {
    entry.can = {
      interface:        getVal('can-interface'),
      fd:               getVal('can-fd') === 'true',
      poll_interval:    parseFloat(getVal('can-poll_interval')),
      stale_timeout_ms: parseInt(getVal('can-stale_timeout_ms')),
      signals:          collectPoints('can'),
    };
  }

  const cfg = JSON.parse(JSON.stringify(_fullCfg));
  if (_editIdx < 0) {
    // 检查 ID 重复
    if (cfg.devices.some(d => d.id === id)) { alert(`设备 ID "${id}" 已存在`); return; }
    cfg.devices.push(entry);
  } else {
    cfg.devices[_editIdx] = entry;
  }
  await cfgPost(cfg);
  devModalClose();
  if (_fullCfg) cfgRenderDevTable(_fullCfg.devices);
}

// ── Modal 辅助 ────────────────────────────────────────────────────────────────
function devModalClose() {
  document.getElementById('devModal').style.display = 'none';
}

function devModalReset() {
  setVal('dev-id', '');
  setVal('dev-protocol', 'modbus');
  setVal('dev-reconnect', 5);
  document.getElementById('dev-enabled').checked = true;
  devProtoChange();
  document.getElementById('points-container').innerHTML = '';
}

function devProtoChange() {
  const p = getVal('dev-protocol');
  document.querySelectorAll('.proto-section').forEach(s => s.style.display = 'none');
  const isDlt = p === 'dlt645' || p === 'dlt698';
  if (isDlt) {
    document.getElementById('proto-dlt').style.display = '';
    document.getElementById('points-section-title').textContent = '电表配置';
    document.getElementById('addPointBtn').textContent = '＋ 添加电表';
    document.getElementById('addPointBtn').onclick = () => meterAdd(p);
  } else {
    const el = document.getElementById('proto-' + p);
    if (el) el.style.display = '';
    document.getElementById('points-section-title').textContent = '采集点';
    document.getElementById('addPointBtn').textContent = '＋ 添加点';
    document.getElementById('addPointBtn').onclick = pointAdd;
  }
  document.getElementById('points-container').innerHTML = '';
  renderPointsTable(p, []);
}

function dltConnChange() {
  const isTcp = getVal('dlt-connection_type') === 'tcp';
  document.getElementById('dlt-tcp-host-group').style.display  = isTcp ? '' : 'none';
  document.getElementById('dlt-tcp-port-group').style.display  = isTcp ? '' : 'none';
  document.getElementById('dlt-serial-group').style.display    = isTcp ? 'none' : '';
  document.getElementById('dlt-baud-group').style.display      = isTcp ? 'none' : '';
}

// ── 采集点表格 ────────────────────────────────────────────────────────────────
const POINT_COLS = {
  modbus:   ['name','address','func_code','data_type','scale','offset','unit','description'],
  iec104:   ['name','ioa','type_id','scale','offset','unit','description'],
  iec61850: ['name','object_ref','fc','scale','offset','unit','description'],
  opcua:    ['name','node_id','scale','offset','unit','description'],
  can:      ['name','can_id','extended','start_bit','bit_length','byte_order','is_signed',
             'scale','offset','unit','description'],
};
const POINT_COL_LABELS = {
  name:'名称', address:'地址', func_code:'功能码', data_type:'类型',
  ioa:'IOA', type_id:'TypeID', object_ref:'对象引用', fc:'FC',
  node_id:'NodeID', scale:'系数', offset:'偏移', unit:'单位', description:'描述',
  can_id:'CAN ID', extended:'扩展帧', start_bit:'起始位', bit_length:'位长',
  byte_order:'字节序', is_signed:'有符号'
};
const POINT_DEFAULTS = {
  modbus:   {name:'',address:0,func_code:3,data_type:'float',scale:1.0,offset:0,unit:'',description:''},
  iec104:   {name:'',ioa:0,type_id:13,scale:1.0,offset:0,unit:'',description:''},
  iec61850: {name:'',object_ref:'',fc:'MX',scale:1.0,offset:0,unit:'',description:''},
  opcua:    {name:'',node_id:'',scale:1.0,offset:0,unit:'',description:''},
  can:      {name:'',can_id:'0x100',extended:false,start_bit:0,bit_length:16,
             byte_order:'little',is_signed:false,scale:1.0,offset:0,unit:'',description:''},
};
// 这些列在 collectPoints 里按类型转换，其余按字符串原样提交
const POINT_INT_COLS   = ['address','ioa','type_id','func_code','start_bit','bit_length'];
const POINT_FLOAT_COLS = ['scale','offset'];
const POINT_BOOL_COLS  = ['extended','is_signed'];

function renderPointsTable(proto, points) {
  if (proto === 'dlt645' || proto === 'dlt698') return;
  const cols = POINT_COLS[proto] || [];
  const con = document.getElementById('points-container');
  if (!cols.length) { con.innerHTML = ''; return; }

  const headerCells = cols.map(c => `<th>${POINT_COL_LABELS[c]||c}</th>`).join('') + '<th></th>';
  const rows = points.map((p, ri) => pointRow(proto, p, ri, cols)).join('');
  con.innerHTML = `<table class="pt-table" id="ptTable">
    <thead><tr>${headerCells}</tr></thead>
    <tbody id="ptTbody">${rows}</tbody>
  </table>`;
}

function pointRow(proto, p, ri, cols) {
  const cells = cols.map(c => {
    const v = p[c] ?? '';
    if (c === 'data_type') {
      const opts = ['bool','int16','uint16','int32','uint32','float'].map(o =>
        `<option${o===v?' selected':''}>${o}</option>`).join('');
      return `<td><select data-col="${c}" data-row="${ri}">${opts}</select></td>`;
    }
    if (c === 'func_code') {
      const opts = [3,4,1,2].map(o =>
        `<option value="${o}"${o==v?' selected':''}>${o}</option>`).join('');
      return `<td><select data-col="${c}" data-row="${ri}">${opts}</select></td>`;
    }
    if (c === 'fc') {
      const opts = ['MX','ST','CO','CF','SP'].map(o =>
        `<option${o===v?' selected':''}>${o}</option>`).join('');
      return `<td><select data-col="${c}" data-row="${ri}">${opts}</select></td>`;
    }
    if (c === 'byte_order') {
      const opts = [['little','Intel 小端'],['big','Motorola 大端']].map(([o,lab]) =>
        `<option value="${o}"${o===v?' selected':''}>${lab}</option>`).join('');
      return `<td><select data-col="${c}" data-row="${ri}">${opts}</select></td>`;
    }
    if (POINT_BOOL_COLS.includes(c)) {
      const cur = String(v) === 'true';
      const opts = [['false','否'],['true','是']].map(([o,lab]) =>
        `<option value="${o}"${(o==='true')===cur?' selected':''}>${lab}</option>`).join('');
      return `<td><select data-col="${c}" data-row="${ri}">${opts}</select></td>`;
    }
    // can_id 保持文本：允许 0x18FF50E5 这类十六进制写法（后端两种都收）
    const t = POINT_INT_COLS.includes(c) ? 'number' : 'text';
    return `<td><input type="${t}" value="${escHtml(String(v))}" data-col="${c}" data-row="${ri}"></td>`;
  }).join('');
  return `<tr id="ptrow-${ri}">${cells}<td><button class="pt-del" onclick="pointDel(${ri})">✕</button></td></tr>`;
}

function pointAdd() {
  const proto = getVal('dev-protocol');
  const cols  = POINT_COLS[proto];
  if (!cols) return;
  const tbody = document.getElementById('ptTbody');
  if (!tbody) { renderPointsTable(proto, []); return pointAdd(); }
  const ri = tbody.children.length;
  const def = Object.assign({}, POINT_DEFAULTS[proto] || {});
  tbody.insertAdjacentHTML('beforeend', pointRow(proto, def, ri, cols));
}

function pointDel(ri) {
  const row = document.getElementById('ptrow-' + ri);
  if (row) row.remove();
  // 重新编号
  const tbody = document.getElementById('ptTbody');
  if (tbody) [...tbody.children].forEach((r, i) => {
    r.id = 'ptrow-' + i;
    r.querySelectorAll('[data-row]').forEach(el => el.dataset.row = i);
  });
}

function collectPoints(proto) {
  const tbody = document.getElementById('ptTbody');
  if (!tbody) return [];
  const cols = POINT_COLS[proto] || [];
  const points = [];
  for (const row of tbody.children) {
    const p = {};
    cols.forEach(c => {
      const el = row.querySelector(`[data-col="${c}"]`);
      if (!el) return;
      const v = el.value;
      if (POINT_INT_COLS.includes(c))        p[c] = parseInt(v) || 0;
      else if (POINT_FLOAT_COLS.includes(c)) p[c] = parseFloat(v) || 0;
      else if (POINT_BOOL_COLS.includes(c))  p[c] = v === 'true';
      else p[c] = v;
    });
    points.push(p);
  }
  return points;
}

// ── 电表 Accordion ────────────────────────────────────────────────────────────
function renderMeters(proto, meters) {
  const con = document.getElementById('points-container');
  con.innerHTML = meters.map((m, mi) => meterBlock(proto, m, mi)).join('');
}

function meterBlock(proto, m, mi) {
  const pts645 = (m.points || []).map((p, pi) => meterPtRow645(proto, p, mi, pi)).join('');
  const ptField = proto === 'dlt645' ? 'data_id' : 'oad';
  const ptLabel = proto === 'dlt645' ? '数据ID' : 'OAD';
  return `<div class="meter-block" id="mblock-${mi}">
    <div class="meter-hdr" onclick="meterToggle(${mi})">
      <span>电表 <code id="m-id-label-${mi}">${escHtml(m.id||'meter_'+mi)}</code>
        &nbsp;<span class="dim" style="font-size:11px">${m.meter_address||''}</span>
      </span>
      <span>
        <span class="dim" style="font-size:11px">${(m.points||[]).length} 点</span>
        &nbsp;
        <button class="btn btn-sm btn-danger" style="font-size:11px;padding:1px 7px" onclick="meterDel(event,${mi})">删除</button>
      </span>
    </div>
    <div class="meter-body" id="mbody-${mi}">
      <div class="form-row" style="margin-bottom:10px">
        <div class="form-group fg-3"><label>表 ID</label>
          <input class="form-control" id="m-id-${mi}" value="${escHtml(m.id||'')}" oninput="document.getElementById('m-id-label-${mi}').textContent=this.value">
        </div>
        <div class="form-group fg-4"><label>表地址（12位BCD）</label>
          <input class="form-control" id="m-addr-${mi}" value="${escHtml(m.meter_address||'000000000001')}">
        </div>
      </div>
      <table class="pt-table" id="mptable-${mi}">
        <thead><tr><th>名称</th><th>${ptLabel}</th><th>系数</th><th>偏移</th><th>单位</th><th>描述</th><th></th></tr></thead>
        <tbody id="mptbody-${mi}">${pts645}</tbody>
      </table>
      <button class="btn btn-sm btn-success meter-add-btn" style="margin-top:8px" onclick="meterPtAdd('${proto}',${mi})">＋ 添加点</button>
    </div>
  </div>`;
}

function meterPtRow645(proto, p, mi, pi) {
  const ptField = proto === 'dlt645' ? 'data_id' : 'oad';
  return `<tr id="mrow-${mi}-${pi}">
    <td><input value="${escHtml(p.name||'')}"       data-mi="${mi}" data-pi="${pi}" data-col="name"       class="form-control" style="font-size:12px"></td>
    <td><input value="${escHtml(p[ptField]||'')}"   data-mi="${mi}" data-pi="${pi}" data-col="${ptField}"  class="form-control" style="font-family:var(--mono);font-size:12px"></td>
    <td><input value="${p.scale??1.0}"              data-mi="${mi}" data-pi="${pi}" data-col="scale"      class="form-control" style="font-size:12px" type="number" step="any"></td>
    <td><input value="${p.offset??0}"               data-mi="${mi}" data-pi="${pi}" data-col="offset"     class="form-control" style="font-size:12px" type="number" step="any"></td>
    <td><input value="${escHtml(p.unit||'')}"        data-mi="${mi}" data-pi="${pi}" data-col="unit"       class="form-control" style="font-size:12px"></td>
    <td><input value="${escHtml(p.description||'')}" data-mi="${mi}" data-pi="${pi}" data-col="description" class="form-control" style="font-size:12px"></td>
    <td><button class="pt-del" onclick="meterPtDel(${mi},${pi})">✕</button></td>
  </tr>`;
}

function meterToggle(mi) {
  const b = document.getElementById('mbody-' + mi);
  if (b) b.style.display = b.style.display === 'none' ? '' : 'none';
}

function meterAdd(proto) {
  const con = document.getElementById('points-container');
  const mi = con.querySelectorAll('.meter-block').length;
  con.insertAdjacentHTML('beforeend', meterBlock(proto, {id:'meter_'+mi, meter_address:'000000000001', points:[]}, mi));
}

function meterDel(event, mi) {
  event.stopPropagation();
  const b = document.getElementById('mblock-' + mi);
  if (b) b.remove();
}

function meterPtAdd(proto, mi) {
  const tbody = document.getElementById('mptbody-' + mi);
  if (!tbody) return;
  const pi = tbody.children.length;
  const ptField = proto === 'dlt645' ? 'data_id' : 'oad';
  tbody.insertAdjacentHTML('beforeend', meterPtRow645(proto, {name:'',scale:1.0,offset:0,[ptField]:'',unit:'',description:''}, mi, pi));
}

function meterPtDel(mi, pi) {
  const row = document.getElementById(`mrow-${mi}-${pi}`);
  if (row) row.remove();
}

function collectMeters(proto) {
  const con = document.getElementById('points-container');
  const ptField = proto === 'dlt645' ? 'data_id' : 'oad';
  return [...con.querySelectorAll('.meter-block')].map((blk, mi) => {
    const idEl   = document.getElementById('m-id-'   + mi);
    const addrEl = document.getElementById('m-addr-' + mi);
    const tbody  = document.getElementById('mptbody-' + mi);
    const pts = tbody ? [...tbody.children].map(row => {
      const g = col => row.querySelector(`[data-col="${col}"]`)?.value ?? '';
      return { name: g('name'), [ptField]: g(ptField),
               scale: parseFloat(g('scale'))||1.0, offset: parseFloat(g('offset'))||0,
               unit: g('unit'), description: g('description') };
    }) : [];
    return { id: idEl?.value||('meter_'+mi), meter_address: addrEl?.value||'000000000001', points: pts };
  });
}

// ── 工具函数 ─────────────────────────────────────────────────────────────────
function setVal(id, v) {
  const el = document.getElementById(id);
  if (!el) return;
  el.value = v;
}
function getVal(id) {
  const el = document.getElementById(id);
  return el ? el.value : '';
}
function escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}
