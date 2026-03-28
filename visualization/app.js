/**
 * 异构 NMS 仿真可视化 — 核心逻辑
 * 节点时序入网、动态位置、链路区分、图例、统计图表（趋势/性能/SPN 选举）
 */
(function () {
  var COLORS = {
    gmc: '#c80000',
    spn: '#e07020',
    /** 兼容旧图例 */
    tsn: '#8040c0',
    /** TSN 子网：普通节点填充色（与 LTE/自组网/数据链区分，非“特殊角色”） */
    subnetTsn: '#6eb8d4',
    lte: '#2060c0',
    adhoc: '#00a050',
    /** 自组网：纯簇首*/
    clusterHead: '#22c55e',
    datalink: '#e6b800',
    offline: '#606060',
    /** 统一退网节点填充（与 fill-opacity 0.5 配合） */
    offlineUnified: '#cccccc',
    /** 未到 joinTime：与「已退网」区分 */
    notJoined: '#7a8fa3',
    other: '#808080'
  };

  /** 时间轴在「仿真未启动」区间：仅显示 GMC，不显示链路与其余节点（与 t>0 的 join 回放区分） */
  var SIM_INITIAL_EPS = 1e-6;

  var state = {
    topology: { nodes: [], links: [] },
    events: [],
    stats: null,
    nodeStatesAtTime: null,
    businessFlows: [],
    businessFeatures: [],
    flowPerformance: [],
    networkRadarMetrics: {},
    networkRadarCosts: {},
    stateDrivenKpi: {},
    selectedFlowIds: {},
    selectedPerfFlowId: null,
    selectedPerfMetric: 'throughput',
    currentTime: 0,
    /** setTime 前一刻的时间，用于播放时节点/链路淡入判定 */
    prevTimelineForAnim: 0,
    playing: false,
    playId: null,
    // 拓扑视图：缩放与平移，用于滚轮缩放和拖拽
    topologyZoom: 1,
    topologyPan: { x: 0, y: 0 },
    topologyDrag: { start: null, panStart: null },
    /** 节点详情悬浮面板打开时对应的节点 id，用于时间轴步进时刷新面板 */
    nodeDetailNodeId: null,
    /** 由 events 解析：NODE_OFFLINE 时刻与原因摘要，供入网状态与拓扑联动 */
    offlineEventIndex: null,
    /** 首次加载 topology 时的 id -> { ip, role }，用于 IP/角色兜底（故障后导出缺字段时） */
    initialTopologyById: null,
    _topologySnapshotDone: false
  };

  var topologyEl = document.getElementById('topology');
  var timelineEl = document.getElementById('timeline');
  var timeLabelEl = document.getElementById('timeLabel');
  var eventListEl = document.getElementById('eventList');
  var legendEl = document.getElementById('topologyLegend');
  var nodeDetailContent = document.getElementById('nodeDetailContent');
  var nodeDetailOverlay = document.getElementById('nodeDetailOverlay');
  var nodeDetailBody = document.getElementById('nodeDetailBody');
  var nodeDetailPopup = document.getElementById('nodeDetailPopup');
  var businessFlowListEl = document.getElementById('businessFlowList');
  var chartPerfEl = document.getElementById('chartPerf');
  var chartPerfMetricEl = document.getElementById('chartPerfMetric');
  var perfFlowSelectEl = document.getElementById('perfFlowSelect');
  var btnPerfMetricThroughputEl = document.getElementById('btnPerfMetricThroughput');
  var btnPerfMetricDelayEl = document.getElementById('btnPerfMetricDelay');
  var btnPerfMetricLossEl = document.getElementById('btnPerfMetricLoss');
  var perfSummaryBoxEl = document.getElementById('perfSummaryBox');
  var chartSpnEl = document.getElementById('chartSpn');
  var chartBusinessEl = document.getElementById('chartBusiness');
  var chartBusinessRadarEl = document.getElementById('chartBusinessRadar');
  var businessGlobalKpiBoxEl = document.getElementById('businessGlobalKpiBox');
  var chartBusinessTableEl = document.getElementById('chartBusinessTable');
  var chartBusinessLinkQEl = document.getElementById('chartBusinessLinkQ');
  var chartPerfTableEl = document.getElementById('chartPerfTable');
  var timelineScaleEl = document.getElementById('timelineScale');
  var timelineMarkersEl = document.getElementById('timelineMarkers');
  /** 与仿真默认时长一致；时间轴最小覆盖 0~该值（秒），事件更晚则自动延长 */
  var TIMELINE_DEFAULT_MAX_SEC = 90;

  function ensureInitialTopologySnapshot() {
    if (state._topologySnapshotDone) return;
    state._topologySnapshotDone = true;
    state.initialTopologyById = {};
    (state.topology.nodes || []).forEach(function (n) {
      state.initialTopologyById[n.id] = { ip: n.ip, role: n.role };
    });
  }

  /** 在网节点：优先 stats 状态，否则 topology；空则从首次快照恢复并打 warn */
  function resolveDisplayIp(node) {
    ensureInitialTopologySnapshot();
    var st = getNodeStateAtTime(node.id, state.currentTime);
    var ip = (st && st.ip) || node.ip;
    if (!ip && state.initialTopologyById && state.initialTopologyById[node.id]) {
      var fb = state.initialTopologyById[node.id].ip;
      if (fb) {
        console.warn('[viz] IP 已从初始 topology 快照恢复 (node ' + node.id + ')');
        return fb;
      }
    }
    return ip || '—';
  }

  function resolveTopologyRoleFallback(node) {
    ensureInitialTopologySnapshot();
    var r = node.role;
    if (r != null && String(r).trim() !== '') return r;
    if (state.initialTopologyById && state.initialTopologyById[node.id] && state.initialTopologyById[node.id].role) {
      console.warn('[viz] 角色已从初始 topology 快照恢复 (node ' + node.id + ')');
      return state.initialTopologyById[node.id].role;
    }
    return r;
  }

  function getTimelineMaxT() {
    var evMax = 0;
    if (state.events && state.events.length) {
      evMax = Math.max.apply(null, state.events.map(function (e) { return e.t; }));
    }
    return Math.max(TIMELINE_DEFAULT_MAX_SEC, evMax);
  }

  function updateTimelineChrome() {
    rebuildOfflineEventIndex();
    var maxT = getTimelineMaxT();
    if (timelineEl) timelineEl.max = maxT;
    if (timelineScaleEl) {
      var ticks = [];
      var s;
      for (s = 0; s <= maxT + 1e-9; s += 10) {
        ticks.push('<span>' + (Math.round(s * 10) / 10) + '</span>');
      }
      var lastDecade = Math.floor(maxT / 10) * 10;
      if (maxT > 0 && Math.abs(maxT - lastDecade) > 0.05) {
        ticks.push('<span>' + (Math.round(maxT * 10) / 10) + '</span>');
      }
      timelineScaleEl.innerHTML = ticks.join('');
    }
    if (timelineMarkersEl) {
      var idx = state.offlineEventIndex;
      var markers = (idx && idx.offlineMarkers) ? idx.offlineMarkers.slice() : [];
      var frag = '';
      for (var mi = 0; mi < markers.length; mi++) {
        var mk = markers[mi];
        var leftPct = maxT > 1e-9 ? (mk.t / maxT) * 100 : 0;
        frag += '<div class="timeline-marker" style="left:' + leftPct + '%">' +
          '<span class="timeline-marker-label">⚡ NODE_OFFLINE（Node ' + mk.nodeId + '）</span></div>';
      }
      timelineMarkersEl.innerHTML = frag;
    }
  }

  function getSubnetFilter() {
    return {
      gmc: document.getElementById('filterGmc').checked,
      lte: document.getElementById('filterLte').checked,
      adhoc: document.getElementById('filterAdhoc').checked,
      datalink: document.getElementById('filterDatalink').checked,
      spn: document.getElementById('filterSpn').checked,
      tsn: document.getElementById('filterTsn').checked
    };
  }

  function isInitialTimelineState(t) {
    return t <= SIM_INITIAL_EPS;
  }

  /**
   * 从 events + 拓扑 joinTime 解析 NODE_OFFLINE（兼容旧日志 NODE_FAIL / SYSTEM 调度行）
   */
  function rebuildOfflineEventIndex() {
    var events = state.events || [];
    var joinMap = {};
    (state.topology.nodes || []).forEach(function (n) {
      joinMap[n.id] = n.joinTime != null ? n.joinTime : 0;
    });
    var offlineTime = {};
    var offlineReasonText = {};
    var rejoinTime = {};
    var offlineMarkers = [];

    function touchOffline(nid, t) {
      if (offlineTime[nid] == null || t < offlineTime[nid]) offlineTime[nid] = t;
    }

    for (var i = 0; i < events.length; i++) {
      var e = events[i];
      var d = e.details || '';
      if (e.event === 'SYSTEM' && /Scheduled NODE_OFFLINE/i.test(d)) {
        var ms = d.match(/target=Node\s*(\d+)/i);
        if (ms) {
          var n0 = parseInt(ms[1], 10);
          touchOffline(n0, e.t);
          offlineMarkers.push({ t: e.t, nodeId: n0 });
        }
      }
      if (e.event === 'SYSTEM' && /Executing event NODE_FAIL target=Node\s*(\d+)/i.test(d)) {
        var m = d.match(/target=Node\s*(\d+)/i);
        if (m) {
          var nid0 = parseInt(m[1], 10);
          touchOffline(nid0, e.t);
          offlineMarkers.push({ t: e.t, nodeId: nid0 });
        }
      }
      if (e.event === 'NODE_OFFLINE' && /退网/.test(d)) {
        var nid = Number(e.nodeId);
        touchOffline(nid, e.t);
        offlineMarkers.push({ t: e.t, nodeId: nid });
        if (/主动退网/.test(d)) offlineReasonText[nid] = '主动退网（内部 voluntary）';
        else if (/节点故障/.test(d)) offlineReasonText[nid] = '节点故障（内部 fault）';
      }
      if (e.event === 'NODE_JOIN') {
        var nidJ = Number(e.nodeId);
        if (rejoinTime[nidJ] == null || e.t > rejoinTime[nidJ]) rejoinTime[nidJ] = e.t;
      }
    }
    state.offlineEventIndex = {
      offlineTime: offlineTime,
      offlineReasonText: offlineReasonText,
      rejoinTime: rejoinTime,
      joinMap: joinMap,
      offlineMarkers: offlineMarkers
    };
  }

  function getMergedOfflineTime(node) {
    var oid = Number(node.id);
    var idx = state.offlineEventIndex || {};
    var et = idx.offlineTime != null ? idx.offlineTime[oid] : null;
    var ot = node.offlineTime;
    if (et != null && ot != null) return Math.min(et, ot);
    if (et != null) return et;
    return ot;
  }

  /**
   * 入网状态：未入网 | 已入网 | 已退网（NODE_OFFLINE 统一，不对 fault/voluntary 做视觉区分）
   */
  function getNodeNetworkStatus(node, t) {
    var oid = Number(node.id);
    var jt = node.joinTime != null ? node.joinTime : 0;
    if (t < jt) {
      return { kind: 'not_joined', label: '未入网', color: '#888888' };
    }
    var idx = state.offlineEventIndex || {};
    var offT = getMergedOfflineTime(node);
    var rj = idx.rejoinTime != null ? idx.rejoinTime[oid] : null;
    if (offT != null && t + 1e-9 >= offT) {
      if (rj != null && offT != null && rj > offT && t + 1e-9 >= rj) {
        return { kind: 'joined', label: '已入网', color: '#43a047' };
      }
      return { kind: 'offline', label: '已退网', color: '#aaaaaa' };
    }
    return { kind: 'joined', label: '已入网', color: '#43a047' };
  }

  /** 详情面板：指标冻结在退网时刻之前（用于 [最后记录]） */
  function getMetricsReferenceTime(node, t) {
    var st = getNodeNetworkStatus(node, t);
    var oid = Number(node.id);
    if (st.kind !== 'offline') return t;
    var idx = state.offlineEventIndex || {};
    var boundary = idx.offlineTime != null ? idx.offlineTime[oid] : null;
    if (boundary == null) boundary = node.offlineTime;
    if (boundary == null) return t;
    return Math.min(t, boundary);
  }

  /** 退网节点不绘制任何邻接链路 */
  function nodeShowsInTopologyLinks(n, t) {
    if (!n) return false;
    var jt = n.joinTime != null ? n.joinTime : 0;
    if (t < jt) return false;
    var st = getNodeNetworkStatus(n, t);
    return st.kind !== 'offline';
  }

  /** 当前时刻是否应对 Adhoc/DataLink 使用「选举前 TSN」样式（首次 SPN_ANNOUNCE 之前；t=0 若已有公告则不再强制选举前样式） */
  function isPreSpnAnnounceSubnetStyle(t, subnetStr) {
    var s = (subnetStr || '').toLowerCase();
    if (s !== 'adhoc' && s !== 'datalink') return false;
    return !hasSubnetAnnounceAtTime(t, subnetStr);
  }

  function hasSubnetAnnounceAtTime(t, subnetStr) {
    var key = subnetStringToAnnounceKey(subnetStr);
    if (key == null) return false;
    var ann = getSubnetAnnounceMapAtTime(t);
    return ann[key] != null;
  }

  /**
   * 各子网最近一次 SPN_ELECT 时间（仅统计「主 SPN」日志：elected as primary），且 e.t <= t
   */
  function getLatestPrimaryElectTimeBySubnet(t) {
    var out = { '1': -1, '2': -1 };
    var evs = state.events || [];
    var nodes = state.topology.nodes || [];
    function subnetOfNodeId(nid) {
      for (var i = 0; i < nodes.length; i++) {
        if (Number(nodes[i].id) === Number(nid)) return (nodes[i].subnet || '').toLowerCase();
      }
      return '';
    }
    for (var i = 0; i < evs.length; i++) {
      var e = evs[i];
      if (e.t > t) continue;
      if (e.event !== 'SPN_ELECT') continue;
      var d = e.details || '';
      if (!/elected as primary SPN/i.test(d)) continue;
      var sub = subnetOfNodeId(e.nodeId);
      var key = subnetStringToAnnounceKey(sub);
      if (key == null || out[key] === undefined) continue;
      if (e.t >= out[key]) out[key] = e.t;
    }
    return out;
  }

  /** 是否处于 SPN 选举动画窗口（旋转环 500ms）：该子网最近一次 primary 选举后 0.5s 内 */
  function isInSpnElectAnimationWindow(t, subnetStr) {
    if (isInitialTimelineState(t)) return false;
    var key = subnetStringToAnnounceKey(subnetStr);
    if (key == null) return false;
    var electMap = getLatestPrimaryElectTimeBySubnet(t);
    var te = electMap[key];
    if (te < 0) return false;
    return t >= te && t < te + 0.5;
  }

  function getSpnElectRotateDegrees(t, subnetStr) {
    var key = subnetStringToAnnounceKey(subnetStr);
    if (key == null) return 0;
    var electMap = getLatestPrimaryElectTimeBySubnet(t);
    var te = electMap[key];
    if (te < 0 || t < te) return 0;
    return ((t - te) / 0.5) * 360;
  }

  function shouldFadeInNodeOnPlay(n, t) {
    if (!state.playing) return false;
    if (isInitialTimelineState(t)) return false;
    var jt = n.joinTime != null ? n.joinTime : 0;
    var prev = state.prevTimelineForAnim != null ? state.prevTimelineForAnim : 0;
    if (isInitialTimelineState(prev)) {
      return jt <= t + 1e-9;
    }
    return prev < jt && t + 1e-9 >= jt;
  }

  function shouldFadeInLinkForJoin(link, t, nodeById) {
    if (!state.playing) return false;
    if (isInitialTimelineState(t)) return false;
    var fromNode = nodeById[link.from], toNode = nodeById[link.to];
    if (!fromNode || !toNode) return false;
    var jf = fromNode.joinTime != null ? fromNode.joinTime : 0;
    var jto = toNode.joinTime != null ? toNode.joinTime : 0;
    var jLink = Math.max(jf, jto);
    var prev = state.prevTimelineForAnim != null ? state.prevTimelineForAnim : 0;
    if (isInitialTimelineState(prev)) {
      return jLink <= t + 1e-9;
    }
    return prev < jLink && t + 1e-9 >= jLink;
  }

  function nodeVisible(node) {
    var f = getSubnetFilter();
    var s = (node.subnet || 'other').toLowerCase();
    var type = (node.type || '').toUpperCase();
    var isGmc = s === 'gmc' || node.role === 'gmc';
    var isLte = s === 'lte';
    var isAdhoc = s === 'adhoc';
    var isDatalink = s === 'datalink';
    var isTsn = s === 'tsn' || type === 'TSN';
    var roleLower = String(resolveTopologyRoleFallback(node) || node.role || '').toLowerCase();
    var isSpn = roleLower === 'spn' || roleLower === 'primary_spn' || roleLower === 'standby_spn'
      || roleLower === 'backup_spn';
    if (isGmc && f.gmc) return true;
    if (isLte && f.lte) return true;
    if (isAdhoc && f.adhoc) return true;
    if (isDatalink && f.datalink) return true;
    if (isTsn && f.tsn) return true;
    if (isSpn && f.spn) return true;
    return false;
  }

  /** 按子网分区：返回节点所属区域，用于布局与标签位置 */
  function getSubnetRegion(node) {
    var sid = Number(node.id);
    var role = (node.role || '').toLowerCase();
    var subnet = (node.subnet || '').toLowerCase();
    if (sid === 0 || role === 'gmc' || subnet === 'gmc') return 'center';
    if (subnet === 'lte') return 'left';
    if (subnet === 'datalink') return 'right';
    if (subnet === 'adhoc') return 'bottom';
    if (subnet === 'tsn') return 'bottom';
    return 'bottom';
  }

  /**
   * 按子网严格分区 + 区域内网格化，计算每个节点的固定坐标（逻辑坐标）。
   * GMC 中心；LTE 左侧；数据链 右侧；Ad-hoc/TSN 下方。区域内按网格排列防重叠。
   */
  function computeSubnetLayoutPositions(nodes) {
    var layout = {};
    var byRegion = { center: [], left: [], right: [], bottom: [] };
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      var r = getSubnetRegion(n);
      byRegion[r].push(n);
    }
    byRegion.center.forEach(function (n) { layout[n.id] = { x: 0, y: 0 }; });
    var grid = function (list, opts) {
      var cols = opts.cols || 3;
      var cellW = opts.cellW || 14;
      var cellH = opts.cellH || 14;
      var baseX = opts.baseX;
      var baseY = opts.baseY;
      list.sort(function (a, b) { return Number(a.id) - Number(b.id); });
      for (var k = 0; k < list.length; k++) {
        var row = Math.floor(k / cols);
        var col = k % cols;
        var x = baseX + col * cellW;
        var y = baseY + row * cellH;
        layout[list[k].id] = { x: x, y: y };
      }
    };
    // 网格间距略大于节点直径，避免子网内节点重叠（节点半径约 8～12）
    grid(byRegion.left, { cols: 3, cellW: 20, cellH: 20, baseX: -48, baseY: -28 });
    grid(byRegion.right, { cols: 3, cellW: 20, cellH: 20, baseX: 12, baseY: -28 });
    grid(byRegion.bottom, { cols: 4, cellW: 20, cellH: 18, baseX: -32, baseY: 20 });
    return layout;
  }

  /** 渲染时使用的节点位置：优先按子网布局，否则用数据中的 x,y */
  function getNodePositionAtTime(node, t, layoutPos) {
    if (layoutPos && layoutPos[node.id]) return layoutPos[node.id];
    var x = node.x != null ? Number(node.x) : 0;
    var y = node.y != null ? Number(node.y) : 0;
    if (Number(node.id) === 0 || (node.role && String(node.role).toLowerCase() === 'gmc')) return { x: 0, y: 0 };
    return { x: x, y: y };
  }

  /**
   * 各子网 Primary SPN（蜂窝 eNB 固定为 SPN；自组网/数据链为动态选举）。
   * spnCache 可选，由 renderTopology 一次性传入。
   */
  function getNodeColorAtTime(node, t, spnCache) {
    var joinTime = node.joinTime != null ? node.joinTime : 0;
    if (t < joinTime) return COLORS.notJoined;
    var netSt = getNodeNetworkStatus(node, t);
    if (netSt.kind === 'offline') return COLORS.offlineUnified;
    if (node.role === 'gmc') return COLORS.gmc;
    var sub = (node.subnet || '').toLowerCase();
    var typeUp = (node.type || '').toUpperCase();
    var nodes = state.topology.nodes || [];
    var sc = spnCache || {};
    var lteP = sc.ltePrimary !== undefined ? sc.ltePrimary : getPrimarySpnIdForSubnetAtTime(t, 'lte', nodes);
    var adhocP = sc.adhocPrimary !== undefined ? sc.adhocPrimary : getPrimarySpnIdForSubnetAtTime(t, 'adhoc', nodes);
    var adhocC = sc.adhocCh !== undefined ? sc.adhocCh : getClusterHeadIdForSubnetAtTime(t, 'adhoc', nodes);
    var dlP = sc.dlPrimary !== undefined ? sc.dlPrimary : getPrimarySpnIdForSubnetAtTime(t, 'datalink', nodes);
    var dlC = sc.dlCh !== undefined ? sc.dlCh : getClusterHeadIdForSubnetAtTime(t, 'datalink', nodes);
    var id = Number(node.id);
    if (sub === 'lte') {
      if (lteP != null && id === lteP) return COLORS.spn;
      return COLORS.subnetTsn;
    }
    /** 首次 SPN_ANNOUNCE 前：自组网/数据链统一为 TSN 浅蓝（选举完成后再显示 Primary/簇首等） */
    if (sub === 'adhoc') {
      if (isPreSpnAnnounceSubnetStyle(t, 'adhoc')) return COLORS.subnetTsn;
      if (adhocP != null && id === adhocP) return COLORS.spn;
      if (adhocC != null && id === adhocC) return COLORS.clusterHead;
      return COLORS.adhoc;
    }
    if (sub === 'datalink') {
      if (isPreSpnAnnounceSubnetStyle(t, 'datalink')) return COLORS.subnetTsn;
      if (dlP != null && id === dlP) return COLORS.spn;
      if (dlC != null && id === dlC) return COLORS.clusterHead;
      return COLORS.datalink;
    }
    if (sub === 'tsn' || typeUp === 'TSN') return COLORS.subnetTsn;
    var nr = String(node.role || '').toLowerCase();
    if (nr === 'spn' || nr === 'primary_spn') return COLORS.spn;
    if (nr === 'standby_spn' || nr === 'backup_spn') return '#d89b4a';
    return COLORS.other;
  }

  function renderLegend() {
    legendEl.innerHTML =
      '<span><span class="dot" style="background:' + COLORS.gmc + '"></span> GMC</span>' +
      '<span><span class="dot" style="background:' + COLORS.lte + '"></span> LTE 普通 UE</span>' +
      '<span><span class="dot" style="background:' + COLORS.adhoc + '"></span> 自组网普通</span>' +
      '<span><span class="dot" style="background:' + COLORS.datalink + '"></span> 数据链普通</span>' +
      '<span><span class="dot" style="background:' + COLORS.subnetTsn + '"></span> TSN 节点</span>' +
      '<span><span class="dot" style="background:' + COLORS.spn + '"></span> Primary SPN</span>' +
      '<span><span class="dot" style="background:' + COLORS.clusterHead + '"></span> 簇首</span>' +
      '<span style="display:inline-flex;align-items:center;vertical-align:middle"><svg width="18" height="18" style="vertical-align:middle;margin-right:4px"><circle cx="9" cy="9" r="6" fill="' + COLORS.spn + '"/><circle cx="9" cy="9" r="8" fill="none" stroke="#ff9944" stroke-width="1.5"/></svg> 橙色双圈 = Primary SPN</span>' +
      '<span style="display:inline-flex;align-items:center;vertical-align:middle"><svg width="22" height="22" style="vertical-align:middle;margin-right:4px"><circle cx="11" cy="11" r="10" fill="none" stroke="' + COLORS.clusterHead + '" stroke-width="1.5"/><circle cx="11" cy="11" r="7" fill="' + COLORS.spn + '"/><circle cx="11" cy="11" r="9" fill="none" stroke="#ff9944" stroke-width="1.2"/></svg> 橙双圈+绿边 = SPN 兼簇首</span>' +
      '<span><span class="dot" style="background:' + COLORS.notJoined + '"></span> 未入网</span>' +
      '<span><span class="dot" style="background:' + COLORS.offlineUnified + ';opacity:0.5"></span> 已退网</span>' +
      '<span><span class="line" style="background:' + COLORS.spn + ';height:3px"></span> 橙色实线 = Primary SPN 链路</span>' +
      '<span><span class="line" style="background:transparent;height:0;border-top:3px dashed ' + COLORS.spn + '"></span> 橙色虚线 = Backup SPN 链路</span>' +
      '<span><span class="line" style="background:' + COLORS.lte + ';height:1px;border-top:1px dashed #4080c0"></span> 数据</span>';
  }

  // 将数据坐标转为画布坐标（供连线、节点使用）
  function toCanvas(px, py, scale, ox, oy) {
    return { x: ox + px * scale, y: oy + py * scale };
  }

  function resolveGmcNodeId() {
    var nodes = state.topology.nodes || [];
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      if (n.id === 0) return 0;
      if (String(n.role || '').toLowerCase() === 'gmc') return n.id;
      if (String(n.subnet || '').toLowerCase() === 'gmc') return n.id;
    }
    return 0;
  }

  function nodeEligibleForLinkAtTime(n, t) {
    if (!n) return false;
    var jt = n.joinTime != null ? n.joinTime : 0;
    if (t < jt) return false;
    return nodeShowsInTopologyLinks(n, t);
  }

  /**
   * 由当前时刻 t 下各子网最新 SPN_ANNOUNCE 绘制 GMC↔Primary（橙实线）、GMC↔Backup（橙虚线）。
   */
  function getDynamicBackhaulSvgFragment(t, posAtT, nodeById, scale, ox, oy) {
    var gmcId = resolveGmcNodeId();
    var gmcNode = nodeById[gmcId];
    if (!gmcNode || !nodeEligibleForLinkAtTime(gmcNode, t)) return '';
    var pGmc = posAtT[gmcId];
    if (!pGmc) return '';
    var ann = getSubnetAnnounceMapAtTime(t);
    var orange = COLORS.spn;
    var x0 = ox + pGmc.x * scale;
    var y0 = oy + pGmc.y * scale;
    var keys = Object.keys(ann).sort();
    var frag = '';
    for (var i = 0; i < keys.length; i++) {
      var a = ann[keys[i]];
      var pri = a.primary;
      var bak = a.backup;
      var reason = a.reason ? String(a.reason) : '';
      if (pri == null || pri === gmcId) continue;
      var nPri = nodeById[pri];
      if (!nPri || !nodeEligibleForLinkAtTime(nPri, t) || !nodeVisible(nPri)) continue;
      var pPri = posAtT[pri];
      if (!pPri) continue;
      var x1 = ox + pPri.x * scale;
      var y1 = oy + pPri.y * scale;
      var tipPri = 'subnet=' + keys[i] + ' primary=' + pri + (reason ? ' reason=' + reason : '');
      frag += '<line x1="' + x0 + '" y1="' + y0 + '" x2="' + x1 + '" y2="' + y1 + '" stroke="' + orange + '" stroke-width="3" class="backhaul-primary-uplink">' +
        '<title>' + tipPri + '</title></line>';
      if (bak != null && bak !== pri && bak !== gmcId) {
        var nBak = nodeById[bak];
        if (!nBak || !nodeEligibleForLinkAtTime(nBak, t) || !nodeVisible(nBak)) continue;
        var pBak = posAtT[bak];
        if (!pBak) continue;
        var x2 = ox + pBak.x * scale;
        var y2 = oy + pBak.y * scale;
        var tipBak = 'subnet=' + keys[i] + ' backup=' + bak + (reason ? ' reason=' + reason : '');
        frag += '<line x1="' + x0 + '" y1="' + y0 + '" x2="' + x2 + '" y2="' + y2 + '" stroke="' + orange + '" stroke-width="2.5" stroke-dasharray="8,5" opacity="0.95" class="backhaul-backup-uplink">' +
          '<title>' + tipBak + '</title></line>';
      }
    }
    return frag;
  }

  function renderTopology() {
    rebuildOfflineEventIndex();
    var nodes = state.topology.nodes || [];
    var links = state.topology.links || [];
    var t = state.currentTime;
    var initialOnly = isInitialTimelineState(t);
    var w = topologyEl.clientWidth || 600;
    var h = topologyEl.clientHeight || 320;
    var padding = 36;
    // 按子网分区 + 区域内网格化，得到每个节点的固定逻辑坐标
    var layoutPos = computeSubnetLayoutPositions(nodes);
    var nodeById = {};
    var posAtT = {};
    var minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      if (!nodeVisible(n)) continue;
      var p = getNodePositionAtTime(n, t, layoutPos);
      minX = Math.min(minX, p.x); minY = Math.min(minY, p.y);
      maxX = Math.max(maxX, p.x); maxY = Math.max(maxY, p.y);
    }
    if (minX > maxX) minX = maxX = minY = maxY = 0;
    var rangeX = Math.max(maxX - minX, 1);
    var rangeY = Math.max(maxY - minY, 1);
    var scale = Math.min((w - 2 * padding) / rangeX, (h - 2 * padding) / rangeY);
    var ox = w / 2 - (minX + maxX) / 2 * scale;
    var oy = h / 2 - (minY + maxY) / 2 * scale;

    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      if (!nodeVisible(n)) continue;
      nodeById[n.id] = n;
      posAtT[n.id] = getNodePositionAtTime(n, t, layoutPos);
    }

    var pan = state.topologyPan;
    var zoom = state.topologyZoom;
    var transform = 'translate(' + (pan.x + w / 2) + ',' + (pan.y + h / 2) + ') scale(' + zoom + ') translate(' + (-w / 2) + ',' + (-h / 2) + ')';

    var svg = '<svg width="100%" height="100%" viewBox="0 0 ' + w + ' ' + h + '">';
    svg += '<defs><filter id="nodeShadow" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur in="SourceAlpha" stdDeviation="2"/><feOffset dx="0" dy="1" result="offsetblur"/><feComponentTransfer><feFuncA type="linear" slope="0.4"/></feComponentTransfer><feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>';
    svg += '<g transform="' + transform + '">';

    var allNodes = state.topology.nodes || [];
    var flows = state.businessFlows || [];
    var selectedFlowIds = state.selectedFlowIds || {};
    var flowColors = ['#e6b800', '#00c0c0', '#c060c0', '#60c080'];
    for (var fi = 0; fi < flows.length; fi++) {
      var flow = flows[fi];
      if (!selectedFlowIds[flow.id]) continue;
      var startT = flow.startTime != null ? flow.startTime : 0, endT = flow.endTime != null ? flow.endTime : 1e9;
      if (t < startT || t > endT) continue;
      var path = flow.path || [];
      if (path.length < 2) continue;
      var color = flow.color || flowColors[fi % flowColors.length];
      for (var pi = 0; pi < path.length - 1; pi++) {
        var fromId = path[pi], toId = path[pi + 1];
        var nFrom = allNodes.find(function(n){ return n.id === fromId; }), nTo = allNodes.find(function(n){ return n.id === toId; });
        if (!nFrom || !nTo) continue;
        if (getNodeNetworkStatus(nFrom, t).kind === 'offline' || getNodeNetworkStatus(nTo, t).kind === 'offline') continue;
        var p1 = getNodePositionAtTime(nFrom, t, layoutPos), p2 = getNodePositionAtTime(nTo, t, layoutPos);
        var pt1 = toCanvas(p1.x, p1.y, scale, ox, oy), pt2 = toCanvas(p2.x, p2.y, scale, ox, oy);
        svg += '<line x1="' + pt1.x + '" y1="' + pt1.y + '" x2="' + pt2.x + '" y2="' + pt2.y + '" stroke="' + color + '" stroke-width="3" stroke-dasharray="8,6" opacity="0.9" class="flow-path"/>';
      }
    }

    var useSpnAnnounceLinks = (state.events || []).some(function (e) { return e.event === 'SPN_ANNOUNCE'; });
    var seenLinks = {};
    for (var j = 0; j < links.length; j++) {
      var link = links[j];
      if (link.type === 'backhaul' && useSpnAnnounceLinks) continue;
      if (link.t != null && t < link.t) continue;
      var fromNode = nodeById[link.from], toNode = nodeById[link.to];
      if (!fromNode || !toNode || !nodeVisible(fromNode) || !nodeVisible(toNode)) continue;
      if (!nodeShowsInTopologyLinks(fromNode, t) || !nodeShowsInTopologyLinks(toNode, t)) continue;
      var joinFrom = fromNode.joinTime != null ? fromNode.joinTime : 0, joinTo = toNode.joinTime != null ? toNode.joinTime : 0;
      if (t < joinFrom || t < joinTo) continue;
      var linkKey = link.from < link.to ? link.from + ',' + link.to : link.to + ',' + link.from;
      if (seenLinks[linkKey]) continue;
      seenLinks[linkKey] = true;
      var p1 = posAtT[link.from], p2 = posAtT[link.to];
      var x1 = ox + p1.x * scale, y1 = oy + p1.y * scale, x2 = ox + p2.x * scale, y2 = oy + p2.y * scale;
      var isBackhaul = link.type === 'backhaul';
      var stroke = isBackhaul ? '#c03030' : '#4080c0';
      var strokeWidth = isBackhaul ? 3 : 1.5;
      if (link.linkQuality != null && link.linkQuality > 0) strokeWidth = Math.max(1, Math.min(4, 1 + link.linkQuality * 3));
      var dash = isBackhaul ? '' : 'stroke-dasharray:6,4';
      var linkFade = shouldFadeInLinkForJoin(link, t, nodeById) ? '<animate attributeName="opacity" from="0" to="1" dur="0.3s" fill="freeze" />' : '';
      svg += '<line x1="' + x1 + '" y1="' + y1 + '" x2="' + x2 + '" y2="' + y2 + '" stroke="' + stroke + '" stroke-width="' + strokeWidth + '" style="' + dash + '">' + linkFade + '</line>';
    }
    if (useSpnAnnounceLinks) {
      svg += getDynamicBackhaulSvgFragment(t, posAtT, nodeById, scale, ox, oy);
    }

    var spnCache = {
      ltePrimary: getPrimarySpnIdForSubnetAtTime(t, 'lte', nodes),
      adhocPrimary: getPrimarySpnIdForSubnetAtTime(t, 'adhoc', nodes),
      adhocBackup: getBackupSpnIdForSubnetAtTime(t, 'adhoc', nodes),
      adhocCh: getClusterHeadIdForSubnetAtTime(t, 'adhoc', nodes),
      dlPrimary: getPrimarySpnIdForSubnetAtTime(t, 'datalink', nodes),
      dlBackup: getBackupSpnIdForSubnetAtTime(t, 'datalink', nodes),
      /** 数据链暂不做簇首区分（仅 Primary/Backup SPN 与动态选举） */
      dlCh: null
    };

    for (var k = 0; k < nodes.length; k++) {
      var n = nodes[k];
      var joinTime = n.joinTime != null ? n.joinTime : 0;
      if (!nodeVisible(n)) continue;
      var p = posAtT[n.id];
      if (!p) continue;
      var cx = ox + p.x * scale, cy = oy + p.y * scale;
      var color = getNodeColorAtTime(n, t, spnCache);
      var subN = (n.subnet || '').toLowerCase();
      var isTsnNode = subN === 'tsn' || (n.type || '').toUpperCase() === 'TSN';
      var isAdhocNode = subN === 'adhoc';
      var isDlNode = subN === 'datalink';
      var isLteNode = subN === 'lte';
      var nid = Number(n.id);
      var preAd = isPreSpnAnnounceSubnetStyle(t, 'adhoc');
      var preDl = isPreSpnAnnounceSubnetStyle(t, 'datalink');
      var isPriAdhoc = isAdhocNode && !preAd && spnCache.adhocPrimary != null && nid === spnCache.adhocPrimary;
      var isChAdhoc = isAdhocNode && !preAd && spnCache.adhocCh != null && nid === spnCache.adhocCh;
      var isPriDl = isDlNode && !preDl && spnCache.dlPrimary != null && nid === spnCache.dlPrimary;
      var isChDl = isDlNode && !preDl && spnCache.dlCh != null && nid === spnCache.dlCh;
      var isPriLte = isLteNode && spnCache.ltePrimary != null && nid === spnCache.ltePrimary;
      var isBackupAdhoc = isAdhocNode && !preAd && spnCache.adhocBackup != null && nid === spnCache.adhocBackup && nid !== spnCache.adhocPrimary;
      var isBackupDl = isDlNode && !preDl && spnCache.dlBackup != null && nid === spnCache.dlBackup && nid !== spnCache.dlPrimary;
      var netSt = getNodeNetworkStatus(n, t);
      var fadeAnim = shouldFadeInNodeOnPlay(n, t) ? '<animate attributeName="opacity" from="0" to="1" dur="0.3s" fill="freeze" />' : '';
      var r = n.role === 'gmc' ? 12 : 8;

      if (netSt.kind === 'offline') {
        var labelOff = (n.label || ('N' + n.id)) + ' (退网)';
        var titleAttrOff = labelOff;
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="#CCCCCC" fill-opacity="0.5" stroke="none" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle node-offline-unified">' + fadeAnim + '</circle>';
        var regionOff = getSubnetRegion(n);
        if (regionOff === 'left') {
          svg += '<text x="' + (cx - r - 6) + '" y="' + cy + '" text-anchor="end" dominant-baseline="middle" font-size="8" fill="#aaa" class="node-label"><title>' + titleAttrOff + '</title>' + labelOff + '</text>';
        } else if (regionOff === 'right') {
          svg += '<text x="' + (cx + r + 6) + '" y="' + cy + '" text-anchor="start" dominant-baseline="middle" font-size="8" fill="#aaa" class="node-label"><title>' + titleAttrOff + '</title>' + labelOff + '</text>';
        } else {
          svg += '<text x="' + cx + '" y="' + (cy + r + 10) + '" text-anchor="middle" font-size="8" fill="#aaa" class="node-label"><title>' + titleAttrOff + '</title>' + labelOff + '</text>';
        }
        continue;
      }

      if (netSt.kind === 'not_joined') {
        var labelNj = (n.label || ('N' + n.id)) + ' (未入网)';
        var titleNj = labelNj;
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + COLORS.notJoined + '" fill-opacity="0.35" stroke="#5a6a80" stroke-width="1.5" stroke-dasharray="4,3" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle node-not-joined">' + fadeAnim + '</circle>';
        var regionNj = getSubnetRegion(n);
        if (regionNj === 'left') {
          svg += '<text x="' + (cx - r - 6) + '" y="' + cy + '" text-anchor="end" dominant-baseline="middle" font-size="8" fill="#889" class="node-label"><title>' + titleNj + '</title>' + labelNj + '</text>';
        } else if (regionNj === 'right') {
          svg += '<text x="' + (cx + r + 6) + '" y="' + cy + '" text-anchor="start" dominant-baseline="middle" font-size="8" fill="#889" class="node-label"><title>' + titleNj + '</title>' + labelNj + '</text>';
        } else {
          svg += '<text x="' + cx + '" y="' + (cy + r + 10) + '" text-anchor="middle" font-size="8" fill="#889" class="node-label"><title>' + titleNj + '</title>' + labelNj + '</text>';
        }
        continue;
      }

      var strokeColor = n.role === 'gmc' ? '#ff4444'
        : (isPriAdhoc || isPriDl || isPriLte ? '#ff9944' : (isTsnNode ? '#4a8aaa' : (isLteNode ? '#4a8aaa' : '#444')));
      var strokeW = n.role === 'gmc' ? 2.5 : 1.5;
      var ringOrange = '#ff9944';

      if (isAdhocNode && !initialOnly && isInSpnElectAnimationWindow(t, 'adhoc')) {
        var rotA = getSpnElectRotateDegrees(t, 'adhoc');
        svg += '<g transform="rotate(' + rotA + ' ' + cx + ' ' + cy + ')">';
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + (r + 7) + '" fill="none" stroke="#ff9944" stroke-width="1.5" stroke-dasharray="4 7" opacity="0.9"/>';
        svg += '</g>';
      }
      if (isDlNode && !initialOnly && isInSpnElectAnimationWindow(t, 'datalink')) {
        var rotD = getSpnElectRotateDegrees(t, 'datalink');
        svg += '<g transform="rotate(' + rotD + ' ' + cx + ' ' + cy + ')">';
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + (r + 7) + '" fill="none" stroke="#ff9944" stroke-width="1.5" stroke-dasharray="4 7" opacity="0.9"/>';
        svg += '</g>';
      }

      var drawSpnChRings = function (isPri, isCh, isBackup) {
        if (isBackup) {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="' + ringOrange + '" stroke-width="2.5" stroke-dasharray="6,5" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        } else if (isPri && isCh) {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + (r + 4) + '" fill="none" stroke="' + COLORS.clusterHead + '" stroke-width="2"/>';
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + (r + 2) + '" fill="none" stroke="' + ringOrange + '" stroke-width="2"/>';
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="' + ringOrange + '" stroke-width="' + strokeW + '" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        } else if (isPri) {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + (r + 2) + '" fill="none" stroke="' + ringOrange + '" stroke-width="3"/>';
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="' + ringOrange + '" stroke-width="2.5" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        } else {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="' + strokeColor + '" stroke-width="' + strokeW + '" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        }
      };
      if (isAdhocNode) {
        if (preAd) {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="#4a8aaa" stroke-width="' + strokeW + '" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        } else {
          drawSpnChRings(isPriAdhoc, isChAdhoc, isBackupAdhoc);
        }
      } else if (isDlNode) {
        if (preDl) {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="#4a8aaa" stroke-width="' + strokeW + '" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        } else {
          drawSpnChRings(isPriDl, isChDl, isBackupDl);
        }
      } else if (isLteNode) {
        if (isPriLte) {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + (r + 2) + '" fill="none" stroke="' + ringOrange + '" stroke-width="2"/>';
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="' + ringOrange + '" stroke-width="' + strokeW + '" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        } else {
          svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="' + strokeColor + '" stroke-width="' + strokeW + '" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
        }
      } else {
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="' + color + '" stroke="' + strokeColor + '" stroke-width="' + strokeW + '" filter="url(#nodeShadow)" data-node-id="' + n.id + '" class="node-circle">' + fadeAnim + '</circle>';
      }

      var label = n.label || ('N' + n.id);
      var tag = '';
      if (isAdhocNode) {
        if (preAd) tag = 'TSN Node';
        else if (isPriAdhoc && isChAdhoc) tag = 'SPN+簇首';
        else if (isPriAdhoc) tag = 'Primary SPN';
        else if (isBackupAdhoc) tag = 'Backup SPN';
        else if (isChAdhoc) tag = '簇首';
      } else if (isDlNode) {
        if (preDl) tag = 'TSN Node';
        else if (isPriDl) tag = 'Primary SPN';
        else if (isBackupDl) tag = 'Backup SPN';
      } else if (isLteNode) {
        if (isPriLte) tag = 'Primary SPN';
        else tag = 'TSN';
      } else {
        var roleNow = ((getNodeStateAtTime(n.id, t) || {}).role || n.role || '').toLowerCase();
        tag = ((roleNow === 'spn' || roleNow === 'primary_spn') ? 'Primary SPN'
          : ((roleNow === 'standby_spn' || roleNow === 'backup_spn') ? 'Backup SPN' : ''));
      }
      var region = getSubnetRegion(n);
      var titleAttr = (n.label || 'Node-' + n.id) + (tag ? ' [' + tag + ']' : '');
      if (region === 'left') {
        svg += '<text x="' + (cx - r - 6) + '" y="' + cy + '" text-anchor="end" dominant-baseline="middle" font-size="8" fill="#aaa" class="node-label"><title>' + titleAttr + '</title>' + label + '</text>';
      } else if (region === 'right') {
        svg += '<text x="' + (cx + r + 6) + '" y="' + cy + '" text-anchor="start" dominant-baseline="middle" font-size="8" fill="#aaa" class="node-label"><title>' + titleAttr + '</title>' + label + '</text>';
      } else {
        svg += '<text x="' + cx + '" y="' + (cy + r + 10) + '" text-anchor="middle" font-size="8" fill="#aaa" class="node-label"><title>' + titleAttr + '</title>' + label + '</text>';
      }
      if (tag) svg += '<text x="' + cx + '" y="' + (cy + r + 18) + '" text-anchor="middle" font-size="7" fill="#666">' + tag + '</text>';
    }
    svg += '</g></svg>';
    topologyEl.innerHTML = svg;
    renderLegend();
    topologyEl.querySelectorAll('.node-circle').forEach(function (el) {
      el.style.cursor = 'pointer';
    });
  }

  function getNodeStateAtTime(nodeId, t) {
    if (!state.nodeStatesAtTime || typeof state.nodeStatesAtTime !== 'object') return null;
    var keys = Object.keys(state.nodeStatesAtTime).map(parseFloat).sort(function (a, b) { return a - b; });
    if (!keys.length) return null;
    var best = keys[0];
    for (var i = 0; i < keys.length; i++) {
      if (keys[i] <= t) best = keys[i];
      else break;
    }
    var states = state.nodeStatesAtTime[String(best)];
    if (!states) return null;
    return states[String(nodeId)] || states[nodeId] || null;
  }

  /** 与 C++ SubnetType 一致：0=LTE, 1=Ad-hoc, 2=DataLink */
  function subnetStringToAnnounceKey(sub) {
    var s = (sub || '').toLowerCase();
    if (s === 'lte') return '0';
    if (s === 'adhoc') return '1';
    if (s === 'datalink') return '2';
    return null;
  }

  /** 当前时刻 t 之前（含）各子网最新 SPN_ANNOUNCE 的 primary/backup（按 subnet 数字键） */
  function getSubnetAnnounceMapAtTime(t) {
    var best = {};
    var evs = state.events || [];
    for (var i = 0; i < evs.length; i++) {
      var e = evs[i];
      if (e.t > t) continue;
      if (e.event !== 'SPN_ANNOUNCE') continue;
      var d = e.details || '';
      var ms = d.match(/subnet=(\d+)/i);
      var mp = d.match(/primary=(\d+)/i);
      var mb = d.match(/backup=(\d+)/i);
      if (!ms || !mp || !mb) continue;
      var subKey = ms[1];
      var entry = { primary: parseInt(mp[1], 10), backup: parseInt(mb[1], 10), t: e.t };
      var mr = d.match(/reason=(\w+)/i);
      if (mr) entry.reason = mr[1];
      if (!best[subKey] || e.t >= best[subKey].t) best[subKey] = entry;
    }
    return best;
  }

  /**
   * 蜂窝侧无日志时：用 topology 中 LTE 且 role=spn（export 对 eNB）或 type=enB 推断固定主 SPN。
   * 仿真里 LTE 不走 RunElection，仅靠 SPN_ANNOUNCE / 本回退显示 eNB。
   */
  function getLtePrimaryFallbackFromTopology(topologyNodes) {
    var list = topologyNodes || [];
    for (var i = 0; i < list.length; i++) {
      var n = list[i];
      if ((n.subnet || '').toLowerCase() !== 'lte') continue;
      if (String(n.role || '').toLowerCase() === 'spn') return n.id;
      if (String(n.type || '').toLowerCase() === 'enb') return n.id;
    }
    return null;
  }

  /**
   * 各子网 Primary SPN：SPN_ANNOUNCE 优先；否则按拓扑过滤 SPN_ELECT（避免多子网日志混用）。
   * 蜂窝：subnet=0；自组网=1；数据链=2。LTE 无事件时用 getLtePrimaryFallbackFromTopology。
   */
  function getPrimarySpnIdForSubnetAtTime(t, subnetStr, topologyNodes) {
    var key = subnetStringToAnnounceKey(subnetStr);
    var ann = getSubnetAnnounceMapAtTime(t);
    if (key != null && ann[key] && ann[key].primary != null) return ann[key].primary;
    var list = topologyNodes || [];
    var last = null;
    var lastT = -1;
    var evs = state.events || [];
    var subLower = subnetStr.toLowerCase();
    for (var i = 0; i < evs.length; i++) {
      var e = evs[i];
      if (e.t > t) continue;
      if (e.event !== 'SPN_ELECT') continue;
      var d = e.details || '';
      var pid = null;
      if (/elected as primary SPN/i.test(d)) {
        var nid = Number(e.nodeId);
        var matched = null;
        for (var j = 0; j < list.length; j++) {
          if (Number(list[j].id) === nid) {
            matched = list[j];
            break;
          }
        }
        if (matched && (matched.subnet || '').toLowerCase() === subLower) pid = nid;
      } else if (subLower === 'adhoc' && /Subnet\s*=\s*Adhoc/i.test(d)) {
        var m = d.match(/SPN NodeId\s*=\s*(\d+)/i);
        if (m) pid = parseInt(m[1], 10);
      } else if (subLower === 'datalink' && /Subnet\s*=\s*DataLink/i.test(d)) {
        var m2 = d.match(/SPN NodeId\s*=\s*(\d+)/i);
        if (m2) pid = parseInt(m2[1], 10);
      } else if (subLower === 'lte' && /Subnet\s*=\s*LTE/i.test(d)) {
        var m3 = d.match(/SPN NodeId\s*=\s*(\d+)/i);
        if (m3) pid = parseInt(m3[1], 10);
      }
      if (pid == null) {
        var mpLine = d.match(/Primary:\s*Node\s*(\d+)/i);
        if (mpLine) {
          var pTry = parseInt(mpLine[1], 10);
          var matchedP = null;
          for (var jj = 0; jj < list.length; jj++) {
            if (Number(list[jj].id) === pTry) { matchedP = list[jj]; break; }
          }
          if (matchedP && (matchedP.subnet || '').toLowerCase() === subLower) pid = pTry;
        }
      }
      if (pid != null && e.t >= lastT) {
        last = pid;
        lastT = e.t;
      }
    }
    if (subLower === 'lte' && last == null) last = getLtePrimaryFallbackFromTopology(list);
    return last;
  }

  /** 各子网 Backup SPN：SPN_ANNOUNCE 优先；否则解析 SPN_ELECT 多行「Backup:  Node X」 */
  function getBackupSpnIdForSubnetAtTime(t, subnetStr, topologyNodes) {
    var key = subnetStringToAnnounceKey(subnetStr);
    var ann = getSubnetAnnounceMapAtTime(t);
    if (key != null && ann[key] && ann[key].backup != null && ann[key].primary != null && ann[key].backup !== ann[key].primary) {
      return ann[key].backup;
    }
    var list = topologyNodes || [];
    var subLower = subnetStr.toLowerCase();
    var last = null;
    var lastT = -1;
    var evs = state.events || [];
    for (var i = 0; i < evs.length; i++) {
      var e = evs[i];
      if (e.t > t) continue;
      if (e.event !== 'SPN_ELECT') continue;
      var d = e.details || '';
      var subOk = (subLower === 'adhoc' && /SubNet:\s*Adhoc/i.test(d)) || (subLower === 'datalink' && /SubNet:\s*DataLink/i.test(d));
      if (!subOk) continue;
      var mb = d.match(/Backup:\s*Node\s*(\d+)/i);
      if (!mb) continue;
      var bid = parseInt(mb[1], 10);
      if (e.t >= lastT) {
        last = bid;
        lastT = e.t;
      }
    }
    if (last != null && list.length) {
      var matched = null;
      for (var j = 0; j < list.length; j++) {
        if (Number(list[j].id) === last) { matched = list[j]; break; }
      }
      if (!matched || (matched.subnet || '').toLowerCase() !== subLower) return null;
    }
    return last;
  }

  function getAdhocPrimarySpnIdAtTime(t) {
    return getPrimarySpnIdForSubnetAtTime(t, 'adhoc', state.topology.nodes || []);
  }

  /** NEIGHBOR_DISCOVER：被发现的邻居 nodeId → 被多少节点发现（入度） */
  function getNeighborDiscoverInDegreeAtTime(t) {
    var counts = {};
    var evs = state.events || [];
    for (var i = 0; i < evs.length; i++) {
      var e = evs[i];
      if (e.t > t) continue;
      if (e.event !== 'NEIGHBOR_DISCOVER') continue;
      var d = e.details || '';
      var m = d.match(/neighbor\s+nodeId\s*=\s*(\d+)/i);
      if (!m) m = d.match(/nodeId\s*=\s*(\d+)/);
      if (!m) continue;
      var nid = parseInt(m[1], 10);
      counts[nid] = (counts[nid] || 0) + 1;
    }
    return counts;
  }

  /**
   * 自组网簇首（数据链 subnet=datalink 暂不使用，见 spnCache.dlCh=null）。
   * 当前时刻下该子网内「被邻居发现次数」最多的节点（可与 SPN 重合）。
   */
  function getClusterHeadIdForSubnetAtTime(t, subnetStr, topologyNodes) {
    if ((subnetStr || '').toLowerCase() === 'datalink') return null;
    var counts = getNeighborDiscoverInDegreeAtTime(t);
    var subLower = subnetStr.toLowerCase();
    var inSubnet = {};
    var list = topologyNodes || [];
    for (var i = 0; i < list.length; i++) {
      var n = list[i];
      if ((n.subnet || '').toLowerCase() !== subLower) continue;
      if (String(n.role || '').toLowerCase() === 'gmc') continue;
      inSubnet[n.id] = true;
    }
    var bestId = null;
    var bestCnt = -1;
    for (var nidStr in counts) {
      var id = parseInt(nidStr, 10);
      if (!inSubnet[id]) continue;
      var c = counts[nidStr];
      if (c > bestCnt || (c === bestCnt && (bestId == null || id < bestId))) {
        bestCnt = c;
        bestId = id;
      }
    }
    if (bestCnt <= 0) return null;
    return bestId;
  }

  function getAdhocClusterHeadIdAtTime(t, topologyNodes) {
    return getClusterHeadIdForSubnetAtTime(t, 'adhoc', topologyNodes);
  }

  /** 仅自组网：是否为 NEIGHBOR_DISCOVER 簇首（数据链暂不区分） */
  function isClusterHeadNode(node, t) {
    var sub = (node.subnet || '').toLowerCase();
    if (sub !== 'adhoc') return false;
    var hid = getClusterHeadIdForSubnetAtTime(t, sub, state.topology.nodes || []);
    return hid != null && Number(node.id) === Number(hid);
  }

  /**
   * 节点角色：GMC | Primary/Backup SPN | 普通节点（TSN 子网）| 无则 —
   * 主/备由最新 SPN_ANNOUNCE；TSN 为普通节点，仅子网归属不同。
   */
  function getNodeRoleLabelFromEvents(node, t) {
    var nid = Number(node.id);
    var sub = (node.subnet || '').toLowerCase();
    if (nid === 0 || sub === 'gmc' || String(node.role || '').toLowerCase() === 'gmc') return 'GMC';
    var ns = getNodeNetworkStatus(node, t);
    if (ns.kind === 'offline') return '—';
    if (ns.kind === 'not_joined') return '未入网';
    var rTop = String(resolveTopologyRoleFallback(node) || '').toLowerCase();
    if (rTop === 'primary_spn') return 'Primary SPN';
    if (rTop === 'backup_spn') return sub === 'lte' ? 'TSN' : 'Backup SPN';
    if (rTop === 'ue' && sub === 'lte') return 'TSN';
    if (rTop === 'ue') return 'UE';
    if (rTop === 'tsn' && (sub === 'adhoc' || sub === 'datalink')) return 'TSN 节点';
    if (sub === 'adhoc' && isPreSpnAnnounceSubnetStyle(t, 'adhoc')) return 'TSN Node';
    if (sub === 'datalink' && isPreSpnAnnounceSubnetStyle(t, 'datalink')) return 'TSN Node';
    var ann = getSubnetAnnounceMapAtTime(t);
    var key = subnetStringToAnnounceKey(sub);
    if (key != null && ann[key]) {
      var a = ann[key];
      if (nid === a.primary) return 'Primary SPN';
      if (nid === a.backup && a.backup !== a.primary) return 'Backup SPN';
    }
    var nodesList = state.topology.nodes || [];
    if (sub === 'adhoc' || sub === 'datalink') {
      var priR = getPrimarySpnIdForSubnetAtTime(t, sub, nodesList);
      var bakR = getBackupSpnIdForSubnetAtTime(t, sub, nodesList);
      if (priR != null && Number(nid) === Number(priR)) return 'Primary SPN';
      if (bakR != null && Number(nid) === Number(bakR)) return 'Backup SPN';
    }
    if (sub === 'lte') {
      var nodesL = state.topology.nodes || [];
      var ltePri = getPrimarySpnIdForSubnetAtTime(t, 'lte', nodesL);
      if (ltePri != null && Number(nid) === Number(ltePri)) return 'Primary SPN';
      return 'TSN';
    }
    if (sub === 'tsn' || String(node.type || '').toUpperCase() === 'TSN') return '普通节点';
    return '—';
  }

  /** 当前时刻前该节点最近一次 STATE_CHANGE 的 energy / linkQ（取箭头右侧瞬时值） */
  function getLatestStateChangeMetrics(nodeId, t) {
    var last = { energy: null, linkQ: null };
    var evs = state.events || [];
    for (var i = 0; i < evs.length; i++) {
      var e = evs[i];
      if (e.t > t) continue;
      if (e.event !== 'STATE_CHANGE' || Number(e.nodeId) !== Number(nodeId)) continue;
      var d = e.details || '';
      var me = d.match(/energy:\s*[\d.eE+\-]+\s*->\s*([\d.eE+\-]+)/);
      var mq = d.match(/linkQ:\s*[\d.eE+\-]+\s*->\s*([\d.eE+\-]+)/i);
      if (me) last.energy = parseFloat(me[1]);
      if (mq) last.linkQ = parseFloat(mq[1]);
    }
    return last;
  }

  /** Score：PROXY_SWITCH 的 New 或 SPN_ELECT 的 score=，取当前时刻前最后一次 */
  function getLatestScoreFromEvents(nodeId, t) {
    var val = null;
    var evs = state.events || [];
    for (var i = 0; i < evs.length; i++) {
      var e = evs[i];
      if (e.t > t) continue;
      if (Number(e.nodeId) !== Number(nodeId)) continue;
      var d = e.details || '';
      if (e.event === 'PROXY_SWITCH') {
        var m = d.match(/New:\s*([\d.eE+\-]+)/i);
        if (m) val = parseFloat(m[1]);
      } else if (e.event === 'SPN_ELECT') {
        var m2 = d.match(/score=([\d.eE+\-]+)/i);
        if (m2) val = parseFloat(m2[1]);
      }
    }
    return val;
  }

  function formatRatioPercent(v) {
    if (v == null || isNaN(v)) return '—';
    return (Number(v) * 100).toFixed(1) + '%';
  }

  /** 链路质量：linkQ∈[0,1]（STATE_CHANGE 箭头右侧）→ 显示 0~100 纯数字，1 位小数，无单位 */
  function formatLinkQualityDisplay(linkQRatio) {
    if (linkQRatio == null || isNaN(linkQRatio)) return '—';
    var v = Number(linkQRatio) * 100;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v.toFixed(1);
  }

  function getOfflinePanelExtraRows(node) {
    var idx = state.offlineEventIndex || {};
    var oid = Number(node.id);
    var tOff = getMergedOfflineTime(node);
    if (tOff == null) return '';
    var reason = (idx.offlineReasonText && idx.offlineReasonText[oid]) ? idx.offlineReasonText[oid] : '见仿真日志 NODE_OFFLINE（fault/voluntary 内部区分）';
    return '<div class="row"><span class="label">退网时间</span>' + tOff.toFixed(2) + ' s</div>' +
      '<div class="row"><span class="label">退网原因</span>' + reason + '</div>';
  }

  function showNodeDetail(node) {
    rebuildOfflineEventIndex();
    state.nodeDetailNodeId = node.id;
    var t = state.currentTime;
    var joinTime = node.joinTime != null ? node.joinTime : 0;
    var netStatus = getNodeNetworkStatus(node, t);
    var tMet = getMetricsReferenceTime(node, t);
    var stateAt = getNodeStateAtTime(node.id, tMet);
    var roleLabel = getNodeRoleLabelFromEvents(node, t);
    var sc = getLatestStateChangeMetrics(node.id, tMet);
    var isOfflinePanel = (netStatus.kind === 'offline');
    var energyVal = formatRatioPercent(sc.energy);
    if (isOfflinePanel && energyVal !== '—') energyVal += ' <span class="detail-note">[最后记录]</span>';
    var linkQVal = isOfflinePanel ? '0' : formatLinkQualityDisplay(sc.linkQ);
    var speedVal = node.speed != null && !isNaN(node.speed) ? Number(node.speed).toFixed(2) + ' m/s' : '—';
    var scoreNum = getLatestScoreFromEvents(node.id, tMet);
    var scoreStr = scoreNum != null && !isNaN(scoreNum) ? scoreNum.toFixed(3) : '—';
    var scoreHtml = isOfflinePanel && scoreStr !== '—'
      ? '<span class="detail-score-strike">' + scoreStr + '</span>'
      : scoreStr;
    var clusterHeadRow = '';
    var subN = (node.subnet || '').toLowerCase();
    if (subN === 'adhoc' && !isOfflinePanel) {
      clusterHeadRow = '<div class="row"><span class="label">是否为簇首</span>' + (isClusterHeadNode(node, t) ? '是' : '否') + '</div>';
    }
    var statusHtml = '<span class="net-status net-status--' + netStatus.kind + '" style="color:' + netStatus.color + '">' + netStatus.label + '</span>';
    var offRows = isOfflinePanel ? getOfflinePanelExtraRows(node) : '';
    var html = '<div class="row"><span class="label">Node ID</span>' + node.id + '</div>' +
      '<div class="row"><span class="label">IP 地址</span>' + resolveDisplayIp(node) + '</div>' +
      '<div class="row"><span class="label">节点角色</span>' + roleLabel + '</div>' +
      clusterHeadRow +
      '<div class="row"><span class="label">子网归属</span>' + (node.subnet || '—') + '</div>' +
      '<div class="row"><span class="label">入网状态</span>' + statusHtml + '</div>' +
      offRows +
      '<div class="row"><span class="label">剩余能量</span>' + energyVal + '</div>' +
      '<div class="row"><span class="label">链路质量</span>' + linkQVal + '</div>' +
      '<div class="row"><span class="label">移动速度</span>' + speedVal + '</div>' +
      '<div class="row"><span class="label">当前 Score</span>' + scoreHtml + '</div>';
    if (nodeDetailBody) nodeDetailBody.innerHTML = html;
    if (nodeDetailOverlay) { nodeDetailOverlay.style.display = 'flex'; }
    if (nodeDetailContent) { nodeDetailContent.innerHTML = '已打开悬浮面板'; nodeDetailContent.classList.remove('empty'); }
  }

  function closeNodeDetailOverlay() {
    state.nodeDetailNodeId = null;
    if (nodeDetailOverlay) nodeDetailOverlay.style.display = 'none';
    if (nodeDetailContent) { nodeDetailContent.innerHTML = '点击拓扑中的节点查看详情'; nodeDetailContent.classList.add('empty'); }
  }

  function updateBusinessFlowList() {
    if (!businessFlowListEl) return;
    var flows = state.businessFlows || [];
    state.selectedFlowIds = state.selectedFlowIds || {};
    if (!flows.length) { businessFlowListEl.innerHTML = '<span class="empty">无业务流数据</span>'; return; }
    businessFlowListEl.innerHTML = flows.map(function (f) {
      var id = 'flow-' + f.id;
      var checked = state.selectedFlowIds[f.id] ? 'checked' : '';
      return '<label><input type="checkbox" id="' + id + '" data-flow-id="' + f.id + '" ' + checked + '> ' + (f.label || f.type || 'Flow' + f.id) + '</label>';
    }).join('');
    businessFlowListEl.querySelectorAll('input[data-flow-id]').forEach(function (cb) {
      cb.addEventListener('change', function () {
        state.selectedFlowIds[this.dataset.flowId] = this.checked;
        renderTopology();
      });
    });
  }

  function updateEventList() {
    var t = state.currentTime;
    if (isInitialTimelineState(t)) {
      eventListEl.innerHTML = '<div class="event-placeholder" style="color:#888;font-size:0.75rem;padding:8px;">等待仿真开始（将时间轴拖离 0s 后显示事件）</div>';
      eventListEl.scrollTop = 0;
      return;
    }
    var events = state.events.filter(function (e) { return e.t <= t; });
    eventListEl.innerHTML = events.slice(-40).reverse().map(function (e) {
      var isCurrent = events.length && e === events[events.length - 1];
      var idx = state.events.indexOf(e);
      var raw = (e.details || '');
      var disp = raw;
      if (e.event === 'SPN_ELECT' && raw.length > 48) {
        disp = raw.replace(/\n/g, ' ').substring(0, 220);
        if (raw.length > 220) disp += '…';
      } else {
        disp = raw.substring(0, 48);
        if (raw.length > 48) disp += '…';
      }
      return '<div class="event-item ' + (isCurrent ? 'current' : '') + '" data-index="' + idx + '" style="white-space:pre-wrap">' +
        '<span class="t">' + e.t.toFixed(2) + 's</span> <span class="ev">[' + e.event + ']</span> Node' + e.nodeId + ' ' + disp + '</div>';
    }).join('');
    eventListEl.scrollTop = 0;
  }

  function setTime(tt) {
    var maxT = getTimelineMaxT();
    updateTimelineChrome();
    state.prevTimelineForAnim = state.currentTime;
    state.currentTime = Math.max(0, Math.min(maxT, tt));
    timelineEl.value = state.currentTime;
    timeLabelEl.textContent = state.currentTime.toFixed(2) + ' s';
    updateEventList();
    renderTopology();
    if (state.nodeDetailNodeId != null && nodeDetailOverlay && nodeDetailOverlay.style.display === 'flex') {
      var nd = (state.topology.nodes || []).find(function (n) { return n.id === state.nodeDetailNodeId; });
      if (nd) showNodeDetail(nd);
    }
  }

  function play() {
    if (state.playing) return;
    state.playing = true;
    var maxT = getTimelineMaxT();
    var start = state.currentTime;
    var startReal = Date.now();
    function tick() {
      if (!state.playing) return;
      var newT = Math.min(start + (Date.now() - startReal) / 1000 * 2, maxT);
      setTime(newT);
      if (newT >= maxT) state.playing = false;
      else state.playId = requestAnimationFrame(tick);
    }
    state.playId = requestAnimationFrame(tick);
  }

  function pause() {
    state.playing = false;
    if (state.playId != null) cancelAnimationFrame(state.playId);
  }

  function showChart(name) {
    [chartPerfEl, chartSpnEl, chartBusinessEl].forEach(function (el) { if (el) el.classList.remove('active'); });
    document.querySelectorAll('.chart-tab').forEach(function (b) { b.classList.remove('active'); });
    if (name === 'perf') { if (chartPerfEl) chartPerfEl.classList.add('active'); document.querySelector('[data-chart="perf"]').classList.add('active'); }
    if (name === 'spn') { if (chartSpnEl) chartSpnEl.classList.add('active'); document.querySelector('[data-chart="spn"]').classList.add('active'); }
    if (name === 'business') { if (chartBusinessEl) chartBusinessEl.classList.add('active'); var btn = document.querySelector('[data-chart="business"]'); if (btn) btn.classList.add('active'); }
  }

  function updateCharts() {
    if (!window.echarts) return;
    var st = state.stats;
    var perf = state.flowPerformance || [];
    var spnCount = (st && st.spnElectionCount) || 0;
    var spnBySubnet = (st && st.spnElectionBySubnet) || {};

    var axisStyle = { color: '#aaa', fontSize: 12 };
    var gridOpt = { left: 55, right: 24, top: 32, bottom: 36 };
    var splitLine = { show: true, lineStyle: { color: '#333' } };
    var dataZoom = [{ type: 'inside' }, { type: 'slider', bottom: 4, height: 18 }];

    if (chartPerfEl.classList.contains('active') && perf.length) {
      if (perfFlowSelectEl) {
        perfFlowSelectEl.innerHTML = perf.map(function (f) {
          var label = f.label || ('Flow' + f.flowId);
          return '<option value="' + f.flowId + '">' + label + ' (ID=' + f.flowId + ')</option>';
        }).join('');
      }
      var selectedFlow = perf.find(function (f) { return String(f.flowId) === String(state.selectedPerfFlowId); });
      if (!selectedFlow) {
        selectedFlow = perf[0];
        state.selectedPerfFlowId = selectedFlow.flowId;
      }
      if (perfFlowSelectEl) perfFlowSelectEl.value = String(state.selectedPerfFlowId);
      var metric = state.selectedPerfMetric || 'throughput';
      [btnPerfMetricThroughputEl, btnPerfMetricDelayEl, btnPerfMetricLossEl].forEach(function (btn) {
        if (!btn) return;
        btn.style.background = '#2a2a2a';
        btn.style.color = '#ddd';
        btn.style.border = '1px solid #444';
      });
      var activeBtn = metric === 'delay' ? btnPerfMetricDelayEl : (metric === 'loss' ? btnPerfMetricLossEl : btnPerfMetricThroughputEl);
      if (activeBtn) {
        activeBtn.style.background = '#2060c0';
        activeBtn.style.color = '#fff';
        activeBtn.style.border = '1px solid #3a78d0';
      }
      var yName = metric === 'delay' ? '时延(ms)' : (metric === 'loss' ? '丢包率(%)' : '吞吐(Mbps)');
      var windowSec = selectedFlow.windowSec != null ? selectedFlow.windowSec : 0.5;
      var flowMeta = null;
      (state.businessFlows || []).some(function (f) {
        var fid = f.id != null ? f.id : f.flowId;
        if (String(fid) === String(state.selectedPerfFlowId)) { flowMeta = f; return true; }
        return false;
      });
      var cumulativeLossPct = (flowMeta && flowMeta.lossRate != null) ? (flowMeta.lossRate * 100.0) : null;
      var cumulativeThr = (flowMeta && flowMeta.throughputMbps != null) ? flowMeta.throughputMbps : null;
      var cumulativeDelay = (flowMeta && flowMeta.delayMs != null) ? flowMeta.delayMs : null;
      if (perfSummaryBoxEl) {
        perfSummaryBoxEl.innerHTML =
          '<div style="color:#8fb3ff;font-weight:600;margin-bottom:2px;">累计指标（全程）</div>' +
          '<div>窗口统计: <b style="color:#fff;">' + windowSec + 's</b> | 丢包率: <b style="color:#fff;">' + (cumulativeLossPct != null ? cumulativeLossPct.toFixed(2) + '%' : '--') + '</b></div>' +
          '<div>吞吐量: <b style="color:#fff;">' + (cumulativeThr != null ? cumulativeThr.toFixed(2) + ' Mbps' : '--') + '</b> | 时延: <b style="color:#fff;">' + (cumulativeDelay != null ? cumulativeDelay.toFixed(2) + ' ms' : '--') + '</b></div>';
      }
      var points = (selectedFlow.data || []).slice().sort(function (a, b) { return (a.t || 0) - (b.t || 0); });
      var seriesData = points.map(function (p) {
        var y = null;
        if (metric === 'delay') y = p.delay != null ? p.delay : null;
        else if (metric === 'loss') y = p.lossRate != null ? p.lossRate * 100.0 : null;
        else y = p.throughput != null ? p.throughput : null;
        return [p.t, y];
      });
      if (chartPerfMetricEl) {
        var c1 = echarts.getInstanceByDom(chartPerfMetricEl); if (c1) c1.dispose();
        c1 = echarts.init(chartPerfMetricEl);
        c1.setOption({
          backgroundColor: 'transparent', textStyle: { color: '#e0e0e0', fontSize: 12 }, tooltip: { trigger: 'axis' },
          legend: { data: [selectedFlow.label || ('Flow' + selectedFlow.flowId)], textStyle: { color: '#aaa', fontSize: 11 }, top: 0 },
          grid: gridOpt, xAxis: { type: 'value', name: '时间(s)', axisLabel: axisStyle },
          yAxis: { type: 'value', name: yName, axisLabel: axisStyle, splitLine: splitLine }, dataZoom: dataZoom,
          series: [{
            type: 'line',
            name: selectedFlow.label || ('Flow' + selectedFlow.flowId),
            smooth: false,
            step: 'end',
            showSymbol: true,
            lineStyle: { color: '#2060c0', width: 2 },
            itemStyle: { color: '#2060c0' },
            connectNulls: false,
            data: seriesData
          }]
        });
      }
      if (chartPerfTableEl) {
        var featuresPerf = state.businessFeatures || [];
        if (featuresPerf.length === 0 && (state.businessFlows || []).length > 0) {
          state.businessFlows.forEach(function (f) {
            featuresPerf.push({
              flowId: f.id != null ? f.id : f.flowId,
              type: f.type || 'data',
              priority: f.priority != null ? f.priority : 1,
              src: f.src != null ? f.src : 0,
              dst: f.dst != null ? f.dst : 0,
              path: f.path || []
            });
          });
        }
        if (featuresPerf.length > 0) {
          var tableHtmlPerf = '<table style="width:100%;border-collapse:collapse;color:#e0e0e0"><thead><tr><th style="border:1px solid #444;padding:4px">流ID</th><th style="border:1px solid #444;padding:4px">类型</th><th style="border:1px solid #444;padding:4px">优先级</th><th style="border:1px solid #444;padding:4px">源→目的</th><th style="border:1px solid #444;padding:4px">路径</th></tr></thead><tbody>';
          featuresPerf.forEach(function (row) {
            tableHtmlPerf += '<tr><td style="border:1px solid #444;padding:4px">' + (row.flowId != null ? row.flowId : '') + '</td><td style="border:1px solid #444;padding:4px">' + (row.type || '') + '</td><td style="border:1px solid #444;padding:4px">' + (row.priority != null ? row.priority : '') + '</td><td style="border:1px solid #444;padding:4px">' + (row.src != null ? row.src : '') + '→' + (row.dst != null ? row.dst : '') + '</td><td style="border:1px solid #444;padding:4px">' + (Array.isArray(row.path) ? row.path.join('-') : '') + '</td></tr>';
          });
          tableHtmlPerf += '</tbody></table>';
          chartPerfTableEl.innerHTML = '<div style="margin:8px 0 4px;color:#aaa">业务特征 businessFeatures</div>' + tableHtmlPerf;
        } else {
          chartPerfTableEl.innerHTML = '<div class="empty" style="color:#888; padding:8px;">无业务特征数据</div>';
        }
      }
    } else if (chartPerfEl.classList.contains('active')) {
      if (perfFlowSelectEl) perfFlowSelectEl.innerHTML = '';
      if (perfSummaryBoxEl) perfSummaryBoxEl.innerHTML = '';
      if (chartPerfMetricEl) {
        var ch = echarts.getInstanceByDom(chartPerfMetricEl);
        if (!ch) ch = echarts.init(chartPerfMetricEl);
        ch.setOption({
          backgroundColor: 'transparent', textStyle: { color: '#e0e0e0', fontSize: 12 },
          title: { text: '无业务性能数据（仅展示 business_flows）', left: 'center', top: 'middle', textStyle: { color: '#888', fontSize: 14 } },
          xAxis: { show: false }, yAxis: { show: false }, series: []
        });
      }
      if (chartPerfTableEl) chartPerfTableEl.innerHTML = '<div class="empty" style="color:#888; padding:8px;">无业务特征数据</div>';
    }

    if (chartSpnEl.classList.contains('active')) {
      var spnChart = echarts.init(chartSpnEl);
      var subnets = Object.keys(spnBySubnet);
      spnChart.setOption({
        backgroundColor: 'transparent',
        textStyle: { color: '#e0e0e0', fontSize: 12 },
        tooltip: {},
        grid: gridOpt,
        xAxis: { type: 'category', data: subnets.length ? subnets : ['SPN'], axisLabel: axisStyle },
        yAxis: { type: 'value', name: '次数', axisLabel: axisStyle, splitLine: splitLine },
        dataZoom: dataZoom,
        series: [{ type: 'bar', data: subnets.length ? subnets.map(function (s) { return spnBySubnet[s]; }) : [spnCount], itemStyle: { color: '#e07020' } }]
      });
    }

    var stateDrivenKpi = state.stateDrivenKpi || {};
    var features = state.businessFeatures || [];
    if (features.length === 0 && (state.businessFlows || []).length > 0) {
      state.businessFlows.forEach(function (f) {
        features.push({
          flowId: f.id != null ? f.id : f.flowId,
          type: f.type || 'data',
          priority: f.priority != null ? f.priority : 1,
          src: f.src != null ? f.src : 0,
          dst: f.dst != null ? f.dst : 0,
          path: f.path || []
        });
      });
    }
    if (chartBusinessEl && chartBusinessEl.classList.contains('active')) {
      var indicator = [
        { name: '低控制开销', max: 1 },
        { name: '抑制效率', max: 1 },
        { name: '上报稳定性', max: 1 },
        { name: '聚合效率', max: 1 },
        { name: '选举稳定性', max: 1 }
      ];
      var overheadAbility = 1.0 - Math.min((stateDrivenKpi.controlOverheadBps || 0) / 500.0, 1.0);
      var suppressAbility = Math.min((stateDrivenKpi.suppressionRatePct || 0) / 100.0, 1.0);
      var reportStability = 1.0 - Math.min(Math.abs((stateDrivenKpi.avgReportIntervalSec || 5.0) - 5.0) / 5.0, 1.0);
      var rawAgg = stateDrivenKpi.aggregateRawBytes || 0;
      var sentAgg = stateDrivenKpi.aggregateSentBytes || 0;
      var aggAbility = rawAgg > 0 ? Math.max(0.0, Math.min(1.0, 1.0 - (sentAgg / rawAgg))) : 0.0;
      var electStability = 1.0 - Math.min(spnCount / 10.0, 1.0);
      var radarCapabilityValues = [
        overheadAbility,
        suppressAbility,
        reportStability,
        aggAbility,
        electStability
      ];
      var businessFlowPerf = (state.businessFlows || []).filter(function (f) {
        return f && (f.throughputMbps != null || f.delayMs != null || f.lossRate != null);
      });
      var avgThr = 0, avgDelay = 0, avgLoss = 0;
      if (businessFlowPerf.length) {
        businessFlowPerf.forEach(function (f) {
          avgThr += (f.throughputMbps || 0);
          avgDelay += (f.delayMs || 0);
          avgLoss += ((f.lossRate || 0) * 100.0);
        });
        avgThr /= businessFlowPerf.length;
        avgDelay /= businessFlowPerf.length;
        avgLoss /= businessFlowPerf.length;
      }
      if (businessGlobalKpiBoxEl) {
        businessGlobalKpiBoxEl.innerHTML =
          '<div style="color:#8fb3ff;font-weight:700;font-size:13px;margin-bottom:6px;">全局业务硬指标（业务流）</div>' +
          '<div style="margin-bottom:4px;">平均吞吐量：<b style="color:#fff;font-size:15px;">' + avgThr.toFixed(2) + ' Mbps</b></div>' +
          '<div style="margin-bottom:4px;">平均时延：<b style="color:#fff;font-size:15px;">' + avgDelay.toFixed(2) + ' ms</b></div>' +
          '<div style="margin-bottom:6px;">平均丢包率：<b style="color:#fff;font-size:15px;">' + avgLoss.toFixed(2) + ' %</b></div>' +
          '<div style="color:#9aa6b2;font-size:11px;line-height:1.5;">统计口径：对 businessFlows 取平均（非控制信令）。</div>';
      }
      if (chartBusinessRadarEl && typeof echarts !== 'undefined') {
        try {
          var existingRadar = echarts.getInstanceByDom(chartBusinessRadarEl);
          if (existingRadar) existingRadar.dispose();
          var radarChart = echarts.init(chartBusinessRadarEl);
          radarChart.setOption({
            backgroundColor: 'transparent',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            tooltip: { trigger: 'item' },
            legend: { show: false },
            radar: { center: ['50%', '50%'], radius: '65%', indicator: indicator },
            series: [{
              type: 'radar',
              data: [
                { value: radarCapabilityValues, name: '' }
              ],
              areaStyle: { opacity: 0.25 }
            }]
          });
          radarChart.resize();
        } catch (e) { console.warn('系统雷达图初始化失败', e); }
      }
      if (chartBusinessLinkQEl && typeof echarts !== 'undefined') {
        try {
          var tsLq = (st && st.timeSeries && st.timeSeries.linkQuality) ? st.timeSeries.linkQuality : {};
          var tNow = state.currentTime;
          var seriesLq = [];
          var pal = ['#2060c0', '#00a050', '#e07020', '#e6b800', '#8040c0', '#6eb8d4', '#c80000'];
          var ci = 0;
          Object.keys(tsLq).sort(function (a, b) { return Number(a) - Number(b); }).forEach(function (nid) {
            var arr = tsLq[nid] || [];
            var pts = [];
            for (var pi = 0; pi < arr.length; pi++) {
              var pair = arr[pi];
              var tt = pair[0];
              if (tt > tNow) continue;
              var ratio = pair[1];
              if (ratio == null || isNaN(ratio)) continue;
              var y = Math.max(0, Math.min(100, Number(ratio) * 100));
              pts.push([tt, y]);
            }
            pts.sort(function (a, b) { return a[0] - b[0]; });
            if (pts.length) {
              seriesLq.push({
                type: 'line',
                name: 'Node ' + nid,
                data: pts,
                smooth: false,
                step: 'end',
                showSymbol: true,
                lineStyle: { width: 1.5, color: pal[ci % pal.length] },
                itemStyle: { color: pal[ci % pal.length] }
              });
              ci++;
            }
          });
          var lqEx = echarts.getInstanceByDom(chartBusinessLinkQEl);
          if (lqEx) lqEx.dispose();
          var lqChart = echarts.init(chartBusinessLinkQEl);
          if (seriesLq.length === 0) {
            lqChart.setOption({
              backgroundColor: 'transparent',
              title: { text: '无链路质量时序（需事件含 STATE_CHANGE/PERF 的 linkQ）', left: 'center', top: 'middle', textStyle: { color: '#888', fontSize: 13 } },
              xAxis: { show: false }, yAxis: { show: false }, series: []
            });
          } else {
            lqChart.setOption({
              backgroundColor: 'transparent',
              textStyle: { color: '#e0e0e0', fontSize: 12 },
              tooltip: {
                trigger: 'axis',
                valueFormatter: function (val) { return val != null && !isNaN(val) ? Number(val).toFixed(1) : ''; }
              },
              legend: { type: 'scroll', textStyle: { color: '#aaa', fontSize: 10 }, bottom: 0 },
              grid: { left: 52, right: 20, top: 28, bottom: seriesLq.length > 4 ? 56 : 36 },
              xAxis: { type: 'value', name: '时间(s)', axisLabel: axisStyle },
              yAxis: {
                type: 'value',
                name: '链路质量',
                min: 0,
                max: 100,
                axisLabel: axisStyle,
                splitLine: splitLine
              },
              dataZoom: dataZoom,
              series: seriesLq
            });
          }
          lqChart.resize();
        } catch (e) { console.warn('链路质量时序图初始化失败', e); }
      }
      if (chartBusinessTableEl) {
        var kpiHtml = '<div style="margin:8px 0 6px;color:#8fb3ff;font-weight:700;">系统指标解释（原始值 / 归一化 / 公式）</div>';
        kpiHtml += '<table style="width:100%;border-collapse:collapse;color:#dbe2ea;font-size:12px;">';
        kpiHtml += '<thead><tr>' +
          '<th style="border:1px solid #344054;padding:6px;">指标</th>' +
          '<th style="border:1px solid #344054;padding:6px;">原始值</th>' +
          '<th style="border:1px solid #344054;padding:6px;">归一化</th>' +
          '<th style="border:1px solid #344054;padding:6px;">计算公式</th>' +
          '</tr></thead><tbody>';
        kpiHtml += '<tr><td style="border:1px solid #344054;padding:6px;">低控制开销</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + ((stateDrivenKpi.controlOverheadBps || 0).toFixed(2)) + ' B/s</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + overheadAbility.toFixed(3) + '</td>' +
          '<td style="border:1px solid #344054;padding:6px;">1 - min(controlOverheadBps / 500, 1)</td></tr>';
        kpiHtml += '<tr><td style="border:1px solid #344054;padding:6px;">抑制效率</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + ((stateDrivenKpi.suppressionRatePct || 0).toFixed(2)) + ' %</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + suppressAbility.toFixed(3) + '</td>' +
          '<td style="border:1px solid #344054;padding:6px;">min(suppressionRatePct / 100, 1)</td></tr>';
        kpiHtml += '<tr><td style="border:1px solid #344054;padding:6px;">上报稳定性</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + ((stateDrivenKpi.avgReportIntervalSec || 0).toFixed(2)) + ' s</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + reportStability.toFixed(3) + '</td>' +
          '<td style="border:1px solid #344054;padding:6px;">1 - min(|avgReportIntervalSec-5| / 5, 1)</td></tr>';
        kpiHtml += '<tr><td style="border:1px solid #344054;padding:6px;">聚合效率</td>' +
          '<td style="border:1px solid #344054;padding:6px;">raw=' + rawAgg + ', sent=' + sentAgg + '</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + aggAbility.toFixed(3) + '</td>' +
          '<td style="border:1px solid #344054;padding:6px;">raw&gt;0 ? clamp(1 - sent/raw,0,1) : 0</td></tr>';
        kpiHtml += '<tr><td style="border:1px solid #344054;padding:6px;">选举稳定性</td>' +
          '<td style="border:1px solid #344054;padding:6px;">spnElectionCount=' + spnCount + '</td>' +
          '<td style="border:1px solid #344054;padding:6px;">' + electStability.toFixed(3) + '</td>' +
          '<td style="border:1px solid #344054;padding:6px;">1 - min(spnElectionCount / 10, 1)</td></tr>';
        kpiHtml += '</tbody></table>';
        chartBusinessTableEl.innerHTML = kpiHtml;
      }
    }
  }

  function loadFromFiles(files) {
    var list = Array.prototype.slice.call(files).filter(function (f) {
      return f.name === 'topology.json' || f.name === 'events.json' || f.name === 'stats.json';
    });
    if (!list.length) { alert('请选择 topology.json、events.json 或 stats.json'); return; }
    var read = function (f) {
      return new Promise(function (res, rej) {
        var r = new FileReader();
        r.onload = function () { res({ name: f.name, data: JSON.parse(r.result) }); };
        r.onerror = rej;
        r.readAsText(f, 'utf-8');
      });
    };
    Promise.all(list.map(read)).then(function (results) {
      results.forEach(function (r) {
        if (r.name === 'topology.json' || (r.data && r.data.nodes)) {
          state.topology = r.data;
          state._topologySnapshotDone = false;
          state.initialTopologyById = null;
        }
        if (r.name === 'events.json' || (Array.isArray(r.data) && r.data[0] && r.data[0].t !== undefined)) state.events = r.data;
        if (r.name === 'stats.json' || (r.data && (r.data.flows || r.data.timeSeries || r.data.businessFeatures || r.data.flowPerformance || r.data.networkRadarMetrics))) {
          state.stats = r.data;
          state.nodeStatesAtTime = (r.data && r.data.nodeStatesByTime) ? r.data.nodeStatesByTime : null;
          state.businessFlows = (r.data && r.data.businessFlows) ? r.data.businessFlows : [];
          state.businessFeatures = (r.data && r.data.businessFeatures) ? r.data.businessFeatures : [];
          state.flowPerformance = (r.data && r.data.flowPerformance) ? r.data.flowPerformance : [];
          state.networkRadarMetrics = (r.data && r.data.networkRadarMetrics) ? r.data.networkRadarMetrics : {};
          state.networkRadarCosts = (r.data && r.data.networkRadarCosts) ? r.data.networkRadarCosts : {};
          state.stateDrivenKpi = (r.data && r.data.stateDrivenKpi) ? r.data.stateDrivenKpi : {};
          state.selectedFlowIds = {};
          state.businessFlows.forEach(function(f) { state.selectedFlowIds[f.id] = true; });
        }
      });
      setTime(0);
      renderTopology();
      updateEventList();
      updateCharts();
      updateBusinessFlowList();
      if (chartBusinessEl && chartBusinessEl.classList.contains('active')) {
        requestAnimationFrame(function () {
          updateCharts();
          if (chartBusinessRadarEl) { var ch = echarts.getInstanceByDom(chartBusinessRadarEl); if (ch) ch.resize(); }
          if (chartBusinessLinkQEl) { var lq = echarts.getInstanceByDom(chartBusinessLinkQEl); if (lq) lq.resize(); }
        });
      }
      if (nodeDetailContent) { nodeDetailContent.innerHTML = '<span class="empty">点击拓扑中的节点查看详情</span>'; nodeDetailContent.classList.add('empty'); }
    }).catch(function (err) { alert('加载失败: ' + err.message); });
  }

  document.getElementById('loadDir').addEventListener('change', function () {
    if (this.files.length) loadFromFiles(this.files);
  });
  document.getElementById('btnLoad').addEventListener('click', function () {
    var path = document.getElementById('resultPath').value.trim();
    var candidates = [];
    if (path) {
      candidates.push(path.replace(/\/?$/, ''));
    } else {
      // 兼容两种启动方式：
      // 1) 在 visualization/ 目录起服务 -> data
      // 2) 在仓库根目录起服务 -> visualization/data
      candidates = ['data', 'visualization/data'];
    }
    var tryLoad = function (idx) {
      if (idx >= candidates.length) {
        alert('加载失败：请确认已执行导出命令并检查路径。\n建议命令：python3 export_visualization_data.py simulation_results/时间戳 -o visualization/data');
        return;
      }
      var base = candidates[idx];
      Promise.all([
        fetch(base + '/topology.json').then(function (r) { return r.ok ? r.json() : null; }),
        fetch(base + '/events.json').then(function (r) { return r.ok ? r.json() : null; }),
        fetch(base + '/stats.json').then(function (r) { return r.ok ? r.json() : null; })
      ]).then(function (arr) {
        if (!arr[0] || !arr[1] || !arr[2]) {
          tryLoad(idx + 1);
          return;
        }
        document.getElementById('resultPath').value = base;
      if (arr[0]) {
        state.topology = arr[0];
        state._topologySnapshotDone = false;
        state.initialTopologyById = null;
      }
      if (arr[1]) state.events = arr[1];
      if (arr[2]) {
        state.stats = arr[2];
        state.nodeStatesAtTime = (arr[2] && arr[2].nodeStatesByTime) ? arr[2].nodeStatesByTime : null;
        state.businessFlows = (arr[2] && arr[2].businessFlows) ? arr[2].businessFlows : [];
        state.businessFeatures = (arr[2] && arr[2].businessFeatures) ? arr[2].businessFeatures : [];
        state.flowPerformance = (arr[2] && arr[2].flowPerformance) ? arr[2].flowPerformance : [];
        state.networkRadarMetrics = (arr[2] && arr[2].networkRadarMetrics) ? arr[2].networkRadarMetrics : {};
        state.networkRadarCosts = (arr[2] && arr[2].networkRadarCosts) ? arr[2].networkRadarCosts : {};
        state.stateDrivenKpi = (arr[2] && arr[2].stateDrivenKpi) ? arr[2].stateDrivenKpi : {};
        state.selectedFlowIds = {};
        state.businessFlows.forEach(function(f) { state.selectedFlowIds[f.id] = true; });
      }
      setTime(0);
      renderTopology();
      updateEventList();
      updateCharts();
      updateBusinessFlowList();
      if (nodeDetailContent) { nodeDetailContent.innerHTML = '<span class="empty">点击拓扑中的节点查看详情</span>'; nodeDetailContent.classList.add('empty'); }
      }).catch(function () { tryLoad(idx + 1); });
    };
    tryLoad(0);
  });
  document.getElementById('btnExportCsv').addEventListener('click', function () {
    if (!state.events.length) { alert('无事件数据'); return; }
    var head = '时间(s),节点ID,事件类型,详情\n';
    var rows = state.events.map(function (e) { return e.t + ',"' + e.nodeId + '","' + (e.event || '') + '","' + (e.details || '').replace(/"/g, '""') + '"'; }).join('\n');
    var blob = new Blob(['\ufeff' + head + rows], { type: 'text/csv;charset=utf-8' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'nms_events.csv';
    a.click();
    URL.revokeObjectURL(a.href);
  });
  document.getElementById('btnExportCharts').addEventListener('click', function () {
    if (!window.echarts) return;
    var el = chartPerfEl.classList.contains('active')
      ? (chartPerfMetricEl || chartPerfEl)
      : (chartSpnEl.classList.contains('active') ? chartSpnEl : (chartBusinessEl && chartBusinessRadarEl && chartBusinessEl.classList.contains('active') ? chartBusinessRadarEl : (chartPerfMetricEl || chartPerfEl)));
    var chart = echarts.getInstanceByDom(el);
    if (chart) {
      var url = chart.getDataURL({ type: 'png' });
      var a = document.createElement('a');
      a.href = url;
      a.download = 'nms_chart.png';
      a.click();
    }
  });
  document.getElementById('btnExportChartCsv').addEventListener('click', function () {
    if (!window.echarts) return;
    var el = chartPerfEl.classList.contains('active')
      ? (chartPerfMetricEl || chartPerfEl)
      : (chartSpnEl.classList.contains('active') ? chartSpnEl : (chartBusinessEl && chartBusinessRadarEl && chartBusinessEl.classList.contains('active') ? chartBusinessRadarEl : (chartPerfMetricEl || chartPerfEl)));
    var chart = echarts.getInstanceByDom(el);
    if (!chart) return;
    var opt = chart.getOption();
    var series = opt.series || [];
    var rows = [];
    series.forEach(function (s, i) {
      var name = s.name || 'series' + i;
      var data = s.data || [];
      data.forEach(function (v, j) {
        var x = (opt.xAxis && opt.xAxis[0] && opt.xAxis[0].data && opt.xAxis[0].data[j]) ? opt.xAxis[0].data[j] : j;
        rows.push([name, x, typeof v === 'object' && v !== null ? (v.value != null ? v.value : v) : v].join(','));
      });
    });
    if (rows.length === 0) { alert('当前图表无数据'); return; }
    var csv = 'series,x,value\n' + rows.join('\n');
    var blob = new Blob(['\ufeff' + csv], { type: 'text/csv;charset=utf-8' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'nms_chart.csv';
    a.click();
    URL.revokeObjectURL(a.href);
  });
  timelineEl.addEventListener('input', function () { setTime(parseFloat(timelineEl.value)); });
  document.getElementById('btnPlay').addEventListener('click', play);
  document.getElementById('btnPause').addEventListener('click', pause);
  document.getElementById('btnReset').addEventListener('click', function () { setTime(0); });
  if (perfFlowSelectEl) {
    perfFlowSelectEl.addEventListener('change', function () {
      state.selectedPerfFlowId = this.value;
      updateCharts();
    });
  }
  if (btnPerfMetricThroughputEl) {
    btnPerfMetricThroughputEl.addEventListener('click', function () { state.selectedPerfMetric = 'throughput'; updateCharts(); });
  }
  if (btnPerfMetricDelayEl) {
    btnPerfMetricDelayEl.addEventListener('click', function () { state.selectedPerfMetric = 'delay'; updateCharts(); });
  }
  if (btnPerfMetricLossEl) {
    btnPerfMetricLossEl.addEventListener('click', function () { state.selectedPerfMetric = 'loss'; updateCharts(); });
  }
  ['filterGmc', 'filterLte', 'filterAdhoc', 'filterDatalink', 'filterSpn', 'filterTsn'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.addEventListener('change', renderTopology);
  });
  document.querySelectorAll('.chart-tab').forEach(function (btn) {
    btn.addEventListener('click', function () {
      var name = this.getAttribute('data-chart');
      showChart(name);
      if (name === 'business') {
        setTimeout(function () {
          updateCharts();
          setTimeout(function () {
            if (chartBusinessRadarEl) { var ch = echarts.getInstanceByDom(chartBusinessRadarEl); if (ch) ch.resize(); }
            if (chartBusinessLinkQEl) { var lq = echarts.getInstanceByDom(chartBusinessLinkQEl); if (lq) lq.resize(); }
          }, 100);
          setTimeout(function () {
            if (chartBusinessRadarEl) { var ch = echarts.getInstanceByDom(chartBusinessRadarEl); if (ch) ch.resize(); }
            if (chartBusinessLinkQEl) { var lq2 = echarts.getInstanceByDom(chartBusinessLinkQEl); if (lq2) lq2.resize(); }
          }, 400);
        }, 200);
      } else {
        updateCharts();
      }
    });
  });
  eventListEl.addEventListener('click', function (e) {
    var item = e.target.closest('.event-item');
    if (!item || item.dataset.index === undefined) return;
    var idx = parseInt(item.dataset.index, 10);
    if (state.events[idx]) setTime(state.events[idx].t);
  });

  topologyEl.addEventListener('click', function (e) {
    var circle = e.target.closest('.node-circle');
    if (!circle || !circle.dataset.nodeId) return;
    var id = parseInt(circle.dataset.nodeId, 10);
    var node = (state.topology.nodes || []).find(function (n) { return n.id === id; });
    if (node) showNodeDetail(node);
  });

  // 滚轮缩放：以鼠标位置为中心缩放，限制缩放范围
  topologyEl.addEventListener('wheel', function (e) {
    e.preventDefault();
    var rect = topologyEl.getBoundingClientRect();
    var w = topologyEl.clientWidth || 600, h = topologyEl.clientHeight || 320;
    var vx = e.clientX - rect.left, vy = e.clientY - rect.top;
    var zoom = state.topologyZoom;
    var pan = state.topologyPan;
    var factor = e.deltaY > 0 ? 0.9 : 1.1;
    var newZoom = Math.max(0.2, Math.min(5, zoom * factor));
    state.topologyZoom = newZoom;
    state.topologyPan.x = pan.x + (1 - newZoom / zoom) * (vx - pan.x - w / 2);
    state.topologyPan.y = pan.y + (1 - newZoom / zoom) * (vy - pan.y - h / 2);
    renderTopology();
  }, { passive: false });

  // 拖拽平移
  topologyEl.addEventListener('mousedown', function (e) {
    if (e.target.closest('.node-circle')) return;
    state.topologyDrag.start = { x: e.clientX, y: e.clientY };
    state.topologyDrag.panStart = { x: state.topologyPan.x, y: state.topologyPan.y };
  });
  document.addEventListener('mousemove', function (e) {
    if (!state.topologyDrag.start) return;
    state.topologyPan.x = state.topologyDrag.panStart.x + (e.clientX - state.topologyDrag.start.x);
    state.topologyPan.y = state.topologyDrag.panStart.y + (e.clientY - state.topologyDrag.start.y);
    renderTopology();
  });
  document.addEventListener('mouseup', function () { state.topologyDrag.start = null; });
  document.addEventListener('mouseleave', function () { state.topologyDrag.start = null; });

  var btnFitView = document.getElementById('topologyFitView');
  if (btnFitView) btnFitView.addEventListener('click', function () {
    state.topologyZoom = 1;
    state.topologyPan = { x: 0, y: 0 };
    renderTopology();
  });

  if (nodeDetailOverlay) {
    nodeDetailOverlay.addEventListener('click', function (e) {
      if (e.target === nodeDetailOverlay) closeNodeDetailOverlay();
    });
  }
  if (nodeDetailPopup) {
    nodeDetailPopup.addEventListener('click', function (e) { e.stopPropagation(); });
  }

  if (window.ResizeObserver) {
    new ResizeObserver(function () {
      if (state.topology.nodes && state.topology.nodes.length) renderTopology();
      if (window.echarts && (chartPerfEl.classList.contains('active') || chartSpnEl.classList.contains('active') || (chartBusinessEl && chartBusinessEl.classList.contains('active')))) {
        var el = chartPerfEl.classList.contains('active') ? (chartPerfMetricEl || chartPerfEl) : (chartSpnEl.classList.contains('active') ? chartSpnEl : chartBusinessRadarEl || chartBusinessEl);
        var ch = echarts.getInstanceByDom(el);
        if (ch) ch.resize();
        if (chartBusinessEl && chartBusinessEl.classList.contains('active')) {
          if (chartBusinessRadarEl) { var r = echarts.getInstanceByDom(chartBusinessRadarEl); if (r) r.resize(); }
          if (chartBusinessLinkQEl) { var lq = echarts.getInstanceByDom(chartBusinessLinkQEl); if (lq) lq.resize(); }
        }
      }
    }).observe(topologyEl);
  }

  updateTimelineChrome();
})();
