const consoleEl = document.querySelector(".console");
const transcript = document.querySelector("#transcript");
const uplinkBadge = document.querySelector("#uplinkBadge");
const form = document.querySelector("#composer");
const bodyEl = document.querySelector("#messageBody");
const initiativeNameEl = document.querySelector("#initiativeName");
const parentTaskIdEl = document.querySelector("#parentTaskId");
const requestedByRoleEl = document.querySelector("#requestedByRole");
const auditLog = document.querySelector("#auditLog");
const clockEl = document.querySelector("#clock");
const armButton = document.querySelector("#armButton");
const sendButton = document.querySelector("#sendButton");
const taskTableBody = document.querySelector("#taskTableBody");
const settingsPanel = document.querySelector("#settingsPanel");
const settingsForm = document.querySelector("#settingsForm");
const taskDetail = document.querySelector("#taskDetail");
const seen = new Set();

const roles = [
  ["pgm", "program_manager", "Program Manager", "PGM", 200, "claude"],
  ["pm", "project_manager", "Project Manager", "PM", 215, "claude"],
  ["swar", "software_architect", "Software Architect", "SWAR", 260, "gemini"],
  ["sysar", "systems_architect", "Systems Architect", "SYSAR", 275, "gemini"],
  ["fe", "frontend_dev", "Frontend Developer", "FE", 165, "chatgpt"],
  ["be", "backend_dev", "Backend Developer", "BE", 145, "chatgpt"],
  ["dbe", "database_engineer", "Database Engineer", "DBE", 125, "chatgpt"],
  ["dba", "database_administrator", "Database Admin", "DBA", 110, "all"],
  ["sa", "systems_administrator", "System Admin", "SA", 60, "all"],
  ["neta", "network_administrator", "Network Admin", "NETA", 45, "all"],
  ["depl", "deployment_engineer", "Deployment Engineer", "DEPL", 30, "all"],
  ["perf", "performance_engineer", "Performance Engineer", "PERF", 350, "gemini"],
  ["test", "tester", "Tester", "TEST", 320, "chatgpt"],
  ["revu", "code_reviewer", "Code Reviewer", "REVU", 295, "chatgpt"],
  ["intg", "integration_specialist", "Integration Specialist", "INTG", 240, "all"],
  ["wrtr", "technical_writer", "Technical Writer", "WRTR", 185, "claude"],
  ["mock", "mockup_artist", "Mockup Artist", "MOCK", 15, "all"],
  ["gfx", "graphic_artist", "Graphic Artist", "GFX", 335, "all"]
].map(([id, roleId, name, short, hue, agent]) => ({id, roleId, name, short, hue, agent}));

const roleById = Object.fromEntries(roles.map(role => [role.id, role]));
const roleByBackendId = Object.fromEntries(roles.map(role => [role.roleId, role]));

const presets = [
  {id: "scout", name: "Scout", tokens: 500000, agents: 3, hours: 2, priority: "low"},
  {id: "build", name: "Build", tokens: 2000000, agents: 6, hours: 8, priority: "normal"},
  {id: "fleet", name: "Fleet", tokens: 5000000, agents: 14, hours: 24, priority: "high"},
  {id: "warroom", name: "WarRoom", tokens: 10000000, agents: 24, hours: 72, priority: "critical"}
];

let state = {
  selectedRole: "pm",
  selectedPreset: "build",
  dispatchMode: "package",
  armed: false,
  armTimer: null,
  audit: [],
  taskStats: {roleCounts: {}, requestCount: 0, blockedCount: 0, activeAgents: 3},
  selectedTaskId: "",
  selectedTaskStatus: "",
  selectedInitiativeId: "",
  roleFilter: "all",
  agents: [],
  hosts: [],
  capacity: {agents: [], initiatives: [], parents: [], caps: {}},
  tokens: {},
  config: {
    tokenTelemetryEnabled: true,
    tokenDailyBudget: 2000000,
    tokenMonthlyBudget: 50000000,
    tokenWarningPercent: 80,
    tokenCostPerMillionCents: 1000
  }
};

function fmtTokens(value) {
  value = Number(value || 0);
  if (value >= 1000000) return `${(value / 1000000).toFixed(1)}M`;
  if (value >= 1000) return `${(value / 1000).toFixed(1)}k`;
  return String(value);
}

function fmtMoneyFromCents(value) {
  return `$${(Number(value || 0) / 100).toFixed(2)}`;
}

function setUplink(connected) {
  uplinkBadge.textContent = connected ? "● UPLINK" : "● LINK";
  uplinkBadge.className = `uplink ${connected ? "" : "disconnected"}`;
}

function updateClock() {
  clockEl.textContent = new Date().toISOString().replace("T", " ").slice(0, 19);
}

function roleForSender(senderName) {
  if (senderName === "claude") return "CLAUDE";
  if (senderName === "chatgpt") return "CODEX";
  if (senderName === "gemini") return "GEMINI";
  if (senderName === "ceo") return "CEO";
  return "SYS";
}

function renderMessage(message) {
  if (seen.has(message.messageId)) return;
  seen.add(message.messageId);

  const item = document.createElement("article");
  item.className = `message kind-${message.messageType}`;
  const stamp = new Date(message.createdAtUnixMs).toISOString().slice(11, 19);
  item.innerHTML = `
    <span class="time"></span>
    <span class="agent"></span>
    <span class="role"></span>
    <span class="body"></span>
  `;
  item.querySelector(".time").textContent = stamp;
  item.querySelector(".agent").textContent = message.senderName;
  item.querySelector(".role").textContent = roleForSender(message.senderName);
  item.querySelector(".body").textContent = prefixForKind(message.messageType) + message.messageBody;
  transcript.appendChild(item);
  transcript.scrollTop = transcript.scrollHeight;
  addAudit(message.senderName, `${message.messageType} to ${message.targetName}: ${message.messageBody.slice(0, 84)}`);
}

function prefixForKind(kind) {
  if (kind === "task.request") return "⇢ ";
  if (kind === "task.assign") return "→ ";
  if (kind === "task.status") return "✓ ";
  if (kind === "task.result") return "■ ";
  if (kind === "status") return "✓ ";
  if (kind === "error") return "✗ ";
  if (kind === "directive") return "⏵ ";
  return "";
}

async function loadRecent() {
  const response = await fetch("/api/messages?limit=100");
  if (!response.ok) throw new Error("message load failed");
  const messages = await response.json();
  messages.forEach(renderMessage);
}

async function loadTasks() {
  const response = await fetch("/api/task-summaries");
  if (!response.ok) return;
  const tasks = await response.json();
  summarizeProjectedTasks(tasks);
  renderTasks(tasks);
}

async function loadInitiatives() {
  const response = await fetch("/api/initiatives");
  if (!response.ok) return;
  const payload = await response.json();
  if (!payload.ok || !Array.isArray(payload.initiatives)) return;
  renderInitiatives(payload.initiatives);
}

/*
 * Sprint 4: fetch and render the accepted-asset inventory for an initiative.
 * Visible only when an initiative is focused; clears on defocus.
 */
async function loadInitiativeAssets(initiativeId) {
  const panel = document.querySelector("#initiativeAssetsPanel");
  if (!panel) return;
  if (!initiativeId) {
    panel.hidden = true;
    return;
  }
  panel.hidden = false;
  const response = await fetch(`/api/initiative-artifacts?initiativeId=${encodeURIComponent(initiativeId)}`);
  if (!response.ok) {
    renderInitiativeAssets({initiativeId, artifacts: [], acceptedCount: 0, pendingCount: 0});
    return;
  }
  const payload = await response.json();
  if (payload.ok) renderInitiativeAssets(payload);
}

function renderInitiativeAssets(payload) {
  const list = document.querySelector("#initiativeAssetsList");
  const countEl = document.querySelector("#initiativeAssetsCount");
  const metaEl = document.querySelector("#initiativeAssetsMeta");
  if (!list || !countEl) return;
  countEl.textContent = String(payload.acceptedCount || 0);
  metaEl.textContent = `${payload.acceptedCount || 0} accepted · ${payload.pendingCount || 0} pending · ${payload.initiativeId}`;
  list.innerHTML = "";
  if (!Array.isArray(payload.artifacts) || !payload.artifacts.length) {
    const empty = document.createElement("div");
    empty.className = "initiative-asset-row";
    empty.innerHTML = "<strong>No artifact decisions yet</strong><small></small><span class=\"asset-state\">--</span>";
    list.appendChild(empty);
    return;
  }
  payload.artifacts.forEach(asset => {
    const row = document.createElement("div");
    row.className = "initiative-asset-row";
    const acceptedAt = asset.acceptedAtUnixMs ? new Date(asset.acceptedAtUnixMs).toISOString().slice(0, 19).replace("T", " ") : "";
    const meta = [
      asset.taskId,
      asset.acceptedArtifactPath || "—",
      asset.lastReviewer || "unknown",
      acceptedAt,
    ].filter(Boolean).join(" · ");
    row.innerHTML = "<strong></strong><small></small><span class=\"asset-state\"></span>";
    row.querySelector("strong").textContent = asset.title || asset.taskId;
    row.querySelector("small").textContent = meta;
    const stateEl = row.querySelector(".asset-state");
    stateEl.textContent = asset.artifactState || "--";
    if (asset.artifactState === "accepted") stateEl.classList.add("accepted");
    if (asset.artifactState === "rejected") stateEl.classList.add("rejected");
    row.addEventListener("click", () => loadTaskDetail(asset.taskId));
    list.appendChild(row);
  });
}

/*
 * Phase 3 Sprint 3: deliverables sub-panel. Pulls /api/initiative-audit for
 * the focused initiative and renders its `deliverables` + `secretScans`
 * arrays as a single list. The audit endpoint is the canonical source - it
 * already joins both arrays and handles pagination.
 */
async function loadInitiativeDeliverables(initiativeId) {
  const panel = document.querySelector("#initiativeDeliverablesPanel");
  if (!panel) return;
  if (!initiativeId) { panel.hidden = true; return; }
  panel.hidden = false;
  const response = await fetch(`/api/initiative-audit?initiativeId=${encodeURIComponent(initiativeId)}`);
  if (!response.ok) {
    renderInitiativeDeliverables({initiativeId, deliverables: [], secretScans: []});
    return;
  }
  const payload = await response.json();
  if (payload.ok !== false) renderInitiativeDeliverables(payload);
}

function renderInitiativeDeliverables(payload) {
  const list = document.querySelector("#initiativeDeliverablesList");
  const countEl = document.querySelector("#initiativeDeliverablesCount");
  const metaEl = document.querySelector("#initiativeDeliverablesMeta");
  if (!list || !countEl) return;
  const deliverables = Array.isArray(payload.deliverables) ? payload.deliverables : [];
  const scans = Array.isArray(payload.secretScans) ? payload.secretScans : [];
  countEl.textContent = String(deliverables.length);
  /* Index scan results by deliverable_id for quick lookup. */
  const scanByDeliverable = {};
  scans.forEach(scan => { if (scan.deliverableId) scanByDeliverable[scan.deliverableId] = scan; });
  const hitCount = scans.filter(scan => scan.matched).length;
  metaEl.textContent = `${deliverables.length} shipped · ${hitCount} scan hits · ${payload.initiativeId || ""}`;
  list.innerHTML = "";
  if (!deliverables.length) {
    const empty = document.createElement("div");
    empty.className = "initiative-deliverable-row";
    empty.innerHTML = "<strong>No deliverables shipped yet</strong><small></small>" +
                      "<span class=\"deliverable-mode\">--</span><span class=\"deliverable-scan\">--</span>";
    list.appendChild(empty);
    return;
  }
  /* Newest first. */
  deliverables.slice().sort((a, b) => (b.createdAtUnixMs || 0) - (a.createdAtUnixMs || 0)).forEach(d => {
    const row = document.createElement("div");
    row.className = "initiative-deliverable-row";
    const when = d.createdAtUnixMs ? new Date(d.createdAtUnixMs).toISOString().slice(0, 19).replace("T", " ") : "";
    const sizeKb = d.sizeBytes ? `${(d.sizeBytes / 1024).toFixed(1)} KiB` : "0 KiB";
    const shaPrefix = (d.sha256 || "").slice(0, 12);
    const supersedesText = d.supersedes ? ` supersedes ${d.supersedes.slice(-12)}` : "";
    const meta = `${d.taskId || ""} · ${shaPrefix} · ${sizeKb} · ${d.reviewer || "unknown"} · ${when}`;
    row.innerHTML = '<strong></strong><small></small>' +
                    '<span class="deliverable-mode"></span>' +
                    '<span class="deliverable-scan"></span>';
    row.querySelector("strong").textContent = (d.deliverablePath || "").split("/").slice(-2).join("/");
    row.querySelector("small").textContent = meta + supersedesText;
    row.querySelector(".deliverable-mode").textContent = d.packagingMode || "copy";
    const scan = scanByDeliverable[d.deliverableId];
    const scanEl = row.querySelector(".deliverable-scan");
    if (!scan) {
      scanEl.textContent = "no scan";
    } else if (scan.matched) {
      scanEl.textContent = `${scan.hitCount} hit${scan.hitCount === 1 ? "" : "s"}`;
      scanEl.classList.add("hit");
    } else {
      scanEl.textContent = "clean";
      scanEl.classList.add("clean");
    }
    list.appendChild(row);
  });
}

function summarizeProjectedTasks(tasks) {
  const roleCounts = {};
  let requestCount = 0;
  let blockedCount = 0;
  tasks.forEach(task => {
    roleCounts[task.assignedRole || "unassigned"] = (roleCounts[task.assignedRole || "unassigned"] || 0) + 1;
    if (task.requestedByRole) requestCount++;
    if (task.status === "blocked") blockedCount++;
  });
  state.taskStats = {
    roleCounts,
    requestCount,
    blockedCount,
    activeAgents: new Set(tasks.map(task => task.assignedAgent).filter(Boolean)).size || 3
  };
}

async function loadConfig() {
  const response = await fetch("/api/config");
  if (!response.ok) throw new Error("config load failed");
  const config = await response.json();
  state.config = {...state.config, ...config};
  document.querySelector("#configTokenTelemetryEnabled").value = config.tokenTelemetryEnabled ? "1" : "0";
  document.querySelector("#configTokenDailyBudget").value = config.tokenDailyBudget || 0;
  document.querySelector("#configTokenMonthlyBudget").value = config.tokenMonthlyBudget || 0;
  document.querySelector("#configTokenWarningPercent").value = config.tokenWarningPercent || 80;
  document.querySelector("#configTokenCostPerMillionCents").value = config.tokenCostPerMillionCents || 0;
  document.querySelector("#configContextMessages").textContent = config.contextMessageCount ?? "--";
  document.querySelector("#configAgentPoll").textContent = config.agentPollMilliseconds ? `${config.agentPollMilliseconds}ms` : "--";
  document.querySelector("#configAdapterTimeout").textContent = config.adapterTimeoutSeconds ? `${config.adapterTimeoutSeconds}s` : "--";
  document.querySelector("#configRoleRoutingEnabled").value = config.roleRoutingEnabled ? "1" : "0";
  document.querySelector("#configMaxActiveTasksPerAgent").value = config.maxActiveTasksPerAgent || 4;
  document.querySelector("#configMaxSubtasksPerParent").value = config.maxSubtasksPerParent || 8;
  document.querySelector("#configMaxTasksPerInitiative").value = config.maxTasksPerInitiative || 32;
  document.querySelector("#configEnableCodexAdapter").value = config.enableCodexAdapter ? "1" : "0";
  document.querySelector("#configEnableClaudeAdapter").value = config.enableClaudeAdapter ? "1" : "0";
  document.querySelector("#configEnableGeminiAdapter").value = config.enableGeminiAdapter ? "1" : "0";
  document.querySelector("#configClaudeMode").value = config.claudeMode || "stub";
  document.querySelector("#configGeminiMode").value = config.geminiMode || "stub";
  document.querySelector("#configGptCommand").value = config.gptCommand || "codex";
  document.querySelector("#configClaudeCommand").value = config.claudeCommand || "claude";
  document.querySelector("#configGeminiCommand").value = config.geminiCommand || "gemini";
  /* Sprint 5 policy + budget knobs. */
  const blockedVendorsEl = document.querySelector("#configBlockedVendors");
  if (blockedVendorsEl) blockedVendorsEl.value = config.blockedVendors || "";
  const initiativeBudgetEl = document.querySelector("#configTokenBudgetPerInitiative");
  if (initiativeBudgetEl) initiativeBudgetEl.value = config.tokenBudgetPerInitiative || 0;
  const familyBudgetEl = document.querySelector("#configTokenBudgetPerModelFamily");
  if (familyBudgetEl) familyBudgetEl.value = config.tokenBudgetPerModelFamily || 0;
  document.querySelector("#configDefaultAutonomyLevel").value = config.defaultAutonomyLevel || "";
  document.querySelector("#configClaudeDefaultAutonomyLevel").value = config.claudeDefaultAutonomyLevel || "";
  document.querySelector("#configChatgptDefaultAutonomyLevel").value = config.chatgptDefaultAutonomyLevel || "";
  document.querySelector("#configGeminiDefaultAutonomyLevel").value = config.geminiDefaultAutonomyLevel || "";
  document.querySelector("#settingsStatus").textContent = config.configPath ? `Editing ${config.configPath}` : "Runtime started without a writable config path.";
  updateReadouts();
}

async function saveConfig(event) {
  event.preventDefault();
  const payload = {
    tokenTelemetryEnabled: Number(document.querySelector("#configTokenTelemetryEnabled").value),
    tokenDailyBudget: Number(document.querySelector("#configTokenDailyBudget").value),
    tokenMonthlyBudget: Number(document.querySelector("#configTokenMonthlyBudget").value),
    tokenWarningPercent: Number(document.querySelector("#configTokenWarningPercent").value),
    tokenCostPerMillionCents: Number(document.querySelector("#configTokenCostPerMillionCents").value),
    roleRoutingEnabled: Number(document.querySelector("#configRoleRoutingEnabled").value),
    maxActiveTasksPerAgent: Number(document.querySelector("#configMaxActiveTasksPerAgent").value),
    maxSubtasksPerParent: Number(document.querySelector("#configMaxSubtasksPerParent").value),
    maxTasksPerInitiative: Number(document.querySelector("#configMaxTasksPerInitiative").value),
    enableCodexAdapter: Number(document.querySelector("#configEnableCodexAdapter").value),
    enableClaudeAdapter: Number(document.querySelector("#configEnableClaudeAdapter").value),
    enableGeminiAdapter: Number(document.querySelector("#configEnableGeminiAdapter").value),
    claudeMode: document.querySelector("#configClaudeMode").value,
    geminiMode: document.querySelector("#configGeminiMode").value,
    gptCommand: document.querySelector("#configGptCommand").value.trim() || "codex",
    claudeCommand: document.querySelector("#configClaudeCommand").value.trim() || "claude",
    geminiCommand: document.querySelector("#configGeminiCommand").value.trim() || "gemini",
    /* Sprint 5 policy knobs. blockedVendors is sent as the operator typed it
     * (whitespace trimmed); the daemon will accept "a, b, c" the same as "a,b,c". */
    blockedVendors: (document.querySelector("#configBlockedVendors")?.value || "").trim(),
    tokenBudgetPerInitiative: Number(document.querySelector("#configTokenBudgetPerInitiative")?.value || 0),
    tokenBudgetPerModelFamily: Number(document.querySelector("#configTokenBudgetPerModelFamily")?.value || 0),
    defaultAutonomyLevel: document.querySelector("#configDefaultAutonomyLevel").value,
    claudeDefaultAutonomyLevel: document.querySelector("#configClaudeDefaultAutonomyLevel").value,
    chatgptDefaultAutonomyLevel: document.querySelector("#configChatgptDefaultAutonomyLevel").value,
    geminiDefaultAutonomyLevel: document.querySelector("#configGeminiDefaultAutonomyLevel").value,
  };
  const response = await fetch("/api/config", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(payload)
  });
  if (!response.ok) {
    document.querySelector("#settingsStatus").textContent = "Config save failed.";
    addAudit("ui", "config save failed");
    return;
  }
  const config = await response.json();
  state.config = {...state.config, ...config};
  document.querySelector("#settingsStatus").textContent = "Config saved.";
  addAudit("ui", "token configuration saved");
  await loadTokens();
  await loadAdapters();
  await loadCapacity();
  updateReadouts();
}

async function loadTokens() {
  const response = await fetch("/api/tokens");
  if (!response.ok) return;
  const tokens = await response.json();
  state.tokens = tokens;
  const dayBudget = tokens.tokenDailyBudget || 0;
  const monthBudget = tokens.tokenMonthlyBudget || 0;
  const dayPercent = dayBudget > 0 ? Math.min(999, Math.round((tokens.dayWindowAllocatedTokens || 0) * 100 / dayBudget)) : 0;
  const monthPercent = monthBudget > 0 ? Math.min(999, Math.round((tokens.monthWindowAllocatedTokens || 0) * 100 / monthBudget)) : 0;
  document.querySelector("#tokenMeta").textContent = tokens.enabled ? `${tokens.allTimePackages || 0} PKG · ${tokens.allTimeUsageEvents || 0} USAGE` : "DISABLED";
  document.querySelector("#tokenHeadline").textContent = fmtTokens(tokens.dayWindowAllocatedTokens || 0);
  document.querySelector("#tokenBudgetLabel").textContent = `/ ${fmtTokens(dayBudget)} 24h budget`;
  document.querySelector("#tokenDay").textContent = `${fmtTokens(tokens.dayWindowAllocatedTokens || 0)} / ${fmtTokens(dayBudget)}`;
  document.querySelector("#tokenMonth").textContent = `${fmtTokens(tokens.monthWindowActualTokens || 0)} / ${fmtTokens(monthBudget)}`;
  document.querySelector("#tokenCost").textContent = fmtMoneyFromCents(tokens.monthWindowActualCostCents || 0);
  document.querySelector("#tokenMeter").style.width = `${Math.min(100, dayPercent)}%`;
  document.querySelector("#tokenMeter").dataset.level = dayPercent >= (tokens.tokenWarningPercent || 80) ? "warn" : "ok";
  drawSparkline(Math.max(dayPercent, monthPercent));
  updateReadouts();
}

async function loadAdapters() {
  const response = await fetch("/api/adapters");
  if (!response.ok) return;
  const payload = await response.json();
  if (!payload.ok || !Array.isArray(payload.adapters)) return;
  renderVendors(payload.adapters.map(adapter => ({
    name: adapter.agent === "chatgpt" ? "Codex CLI" : adapter.agent === "claude" ? "Claude Code" : "Gemini CLI",
    cli: adapter.commandPath || adapter.command,
    state: adapter.preflight ? `${adapter.preflight.state} · ${adapter.preflight.reason}` : (adapter.enabled ? adapter.state : "disabled"),
    quota: adapter.preflight?.ok ? 100 : adapter.enabled ? 45 : 0,
    warn: !adapter.preflight?.ok,
    lastFailure: adapter.preflight?.lastFailure?.class || ""
  })));
}

async function loadCapacity() {
  const response = await fetch("/api/capacity");
  if (!response.ok) return;
  const payload = await response.json();
  if (!payload.ok) return;
  state.capacity = payload;
  const active = (payload.agents || []).reduce((sum, item) => sum + Number(item.activeTasks || 0), 0);
  const cap = payload.caps?.maxActiveTasksPerAgent || "--";
  document.querySelector("#subAgentSummary").textContent = `${active} active`;
  document.querySelector("#routingSummary").textContent = state.config.roleRoutingEnabled ? `router ${cap}/agent` : "manual";
  if (!state.agents.length) {
    document.querySelector("#agentMeta").textContent = `● ${state.taskStats.activeAgents} ACTIVE · ${state.taskStats.requestCount}R · ${active} CAPACITY`;
  }
}

async function loadAgents() {
  const response = await fetch("/api/agents");
  if (!response.ok) return;
  const payload = await response.json();
  if (!payload.ok || !Array.isArray(payload.agents)) return;
  state.agents = payload.agents;
  renderAgentControls();
}

async function loadHosts() {
  const response = await fetch("/api/hosts");
  if (!response.ok) return;
  const payload = await response.json();
  if (!payload.ok || !Array.isArray(payload.hosts)) return;
  state.hosts = payload.hosts;
  renderHosts();
}

function renderHosts() {
  const list = document.querySelector("#hostList");
  const meta = document.querySelector("#hostMeta");
  if (!list || !meta) return;
  const hosts = state.hosts || [];
  const uniqueHosts = new Set(hosts.map(item => item.host).filter(Boolean));
  meta.textContent = `${uniqueHosts.size} HOST${uniqueHosts.size === 1 ? "" : "S"}`;
  list.innerHTML = "";
  if (!hosts.length) {
    const empty = document.createElement("div");
    empty.className = "host-row empty";
    empty.innerHTML = "<strong>No remote hosts</strong><small></small>";
    list.appendChild(empty);
    return;
  }
  hosts.slice(-8).reverse().forEach(item => {
    const row = document.createElement("div");
    row.className = "host-row";
    row.innerHTML = "<strong></strong><span></span><small></small>";
    row.querySelector("strong").textContent = item.host || "unknown";
    row.querySelector("span").textContent = item.agent || "--";
    row.querySelector("small").textContent = item.profiles || "no profiles";
    list.appendChild(row);
  });
}

/*
 * Phase 3 Sprint 1: pull the aggregated status snapshot and render the status
 * board panel. The endpoint folds initiatives + agents + heartbeats + recent
 * milestones + tokens + adapters + policy knobs into one payload, so a single
 * poll keeps the board live without firing five separate requests.
 */
async function loadStatusBoard() {
  const response = await fetch("/api/status");
  if (!response.ok) return;
  const payload = await response.json();
  if (!payload || !payload.ok) return;
  renderStatusBoard(payload);
}

function renderStatusBoard(payload) {
  const heartbeatsEl = document.querySelector("#statusBoardHeartbeats");
  const milestonesEl = document.querySelector("#statusBoardMilestones");
  const metaEl = document.querySelector("#statusBoardMeta");
  if (!heartbeatsEl || !milestonesEl || !metaEl) return;
  const heartbeats = Array.isArray(payload.heartbeats) ? payload.heartbeats : [];
  const milestones = Array.isArray(payload.recentMilestones) ? payload.recentMilestones : [];
  const now = Number(payload.nowUnixMs || Date.now());
  /* meta line: number of agents reporting + oldest last-seen */
  if (!heartbeats.length) {
    metaEl.textContent = "no heartbeats yet";
  } else {
    const oldestSeen = heartbeats.reduce((max, hb) =>
      Math.max(max, now - Number(hb.lastSeenUnixMs || 0)), 0);
    const minutes = Math.floor(oldestSeen / 60000);
    metaEl.textContent = `${heartbeats.length} agent(s) reporting · oldest ${minutes}m ago`;
  }
  heartbeatsEl.innerHTML = "";
  heartbeats.forEach(hb => {
    const row = document.createElement("div");
    row.className = "status-board-row";
    const ageMs = now - Number(hb.lastSeenUnixMs || 0);
    const ageMin = Math.floor(ageMs / 60000);
    const ageStr = ageMin >= 60 ? `${Math.floor(ageMin / 60)}h` :
                   ageMin >= 1 ? `${ageMin}m` : "<1m";
    row.innerHTML = "<strong></strong><span class=\"age\"></span><small></small>";
    row.querySelector("strong").textContent = hb.agent || "?";
    row.querySelector(".age").textContent = ageStr;
    const taskFragment = hb.currentTaskId ? `${hb.currentTaskId} · ` : "";
    row.querySelector("small").textContent = `${taskFragment}${hb.statusLine || "idle"}`;
    heartbeatsEl.appendChild(row);
  });
  milestonesEl.innerHTML = "";
  /* Show the most recent 6 milestones - the panel is glanceable, not a log. */
  milestones.slice(0, 6).forEach(milestone => {
    const row = document.createElement("div");
    row.className = "status-board-milestone";
    row.dataset.milestone = milestone.milestone || "unknown";
    row.innerHTML = "<b></b><strong></strong><small></small>";
    row.querySelector("b").textContent = (milestone.milestone || "").toUpperCase();
    row.querySelector("strong").textContent = milestone.taskId || "";
    row.querySelector("small").textContent = milestone.message || milestone.createdBy || "";
    milestonesEl.appendChild(row);
  });
}

function summarizeTasks(rows) {
  const byId = new Map();
  const requests = new Map();
  const roleCounts = {};
  let blockedCount = 0;
  rows.forEach(row => {
    if (!row.taskId) return;
    if (row.schema === "woventeam.task_package.v0.1") {
      byId.set(row.taskId, {
        taskId: row.taskId,
        initiativeId: row.initiativeId || "init_phase0",
        status: row.status || "queued",
        assignedAgent: row.assignedAgent || "all",
        assignedRole: row.assignedRole || "",
        title: row.title || (row.task && row.task.title) || "Untitled task",
        parentTaskId: row.parentTaskId || "",
        requestedByRole: row.requestedByRole || "",
        dependencies: Array.isArray(row.dependencies) ? row.dependencies : []
      });
      roleCounts[row.assignedRole || "unassigned"] = (roleCounts[row.assignedRole || "unassigned"] || 0) + 1;
    } else if (row.schema === "woventeam.task_request.v0.1") {
      requests.set(row.taskId, row);
    } else if (row.schema === "woventeam.task_event.v0.1" && byId.has(row.taskId)) {
      const task = byId.get(row.taskId);
      task.status = row.status || task.status;
      task.assignedAgent = row.assignedAgent || task.assignedAgent;
    }
  });
  byId.forEach(task => {
    const request = requests.get(task.taskId);
    if (request) {
      task.requestedByRole = request.requestedByRole || task.requestedByRole;
      task.requestedRole = request.requestedRole || "";
    }
    if (task.status === "blocked") blockedCount++;
  });
  state.taskStats = {
    roleCounts,
    requestCount: requests.size,
    blockedCount,
    activeAgents: new Set(Array.from(byId.values()).map(task => task.assignedAgent).filter(Boolean)).size || 3
  };
  return Array.from(byId.values()).reverse();
}

function renderTasks(tasks) {
  /* Render alerts off the same data; cheap and keeps the two views consistent. */
  renderAlerts(tasks);
  const filtered = applyTaskFilters(tasks);
  document.querySelector("#requestCount").textContent = String(state.taskStats.requestCount);
  document.querySelector("#blockedCount").textContent = String(state.taskStats.blockedCount);
  document.querySelector("#agentCount").textContent = String(state.taskStats.activeAgents);
  document.querySelector("#agentBadge").textContent = String(state.taskStats.activeAgents);
  document.querySelector("#agentMeta").textContent = `● ${state.taskStats.activeAgents} ACTIVE · ${state.taskStats.requestCount}R · ${state.taskStats.blockedCount} BLOCKED`;
  renderRoles();
  if (!filtered.length) {
    taskTableBody.innerHTML = '<tr><td colspan="9">No task packages yet. Use the composer or wt-task to create one.</td></tr>';
    return;
  }
  taskTableBody.innerHTML = "";
  filtered.slice(0, 12).forEach(task => {
    const row = document.createElement("tr");
    row.dataset.taskId = task.taskId;
    row.innerHTML = `
      <td></td>
      <td><span class="status-pill"></span></td>
      <td></td>
      <td></td>
      <td></td>
      <td><div class="mini-progress"><span></span></div></td>
      <td></td>
      <td></td>
      <td></td>
    `;
    row.querySelector("td:nth-child(1)").textContent = task.taskId;
    row.querySelector(".status-pill").textContent = task.status;
    row.querySelector(".status-pill").dataset.status = task.status;
    row.querySelector("td:nth-child(3)").textContent = task.parentTaskId || "--";
    row.querySelector("td:nth-child(4)").textContent = task.assignedRole || "--";
    row.querySelector("td:nth-child(5)").textContent = task.assignedAgent || "all";
    row.querySelector(".mini-progress span").style.width = progressForStatus(task.status);
    row.querySelector("td:nth-child(7)").textContent = task.requestedByRole || "--";
    row.querySelector("td:nth-child(8)").textContent = task.eventCount ? `${task.eventCount} events` : "--";
    row.querySelector("td:nth-child(9)").textContent = task.title;
    row.addEventListener("click", () => loadTaskDetail(task.taskId));
    taskTableBody.appendChild(row);
  });
}

/*
 * Combine the active filters (initiative focus + role filter from the chat
 * panel dropdown) into a single filter pass. Centralizing here lets the alert
 * feed honor the same selections without duplicating logic.
 */
function applyTaskFilters(tasks) {
  let filtered = tasks;
  if (state.selectedInitiativeId) {
    filtered = filtered.filter(task => task.initiativeId === state.selectedInitiativeId);
  }
  if (state.roleFilter && state.roleFilter !== "all") {
    filtered = filtered.filter(task => task.assignedRole === state.roleFilter);
  }
  return filtered;
}

/*
 * Sprint 4: derive an alert list from the task summaries projection. Surface
 * failed, stuck, blocked, and revision_requested tasks. The same projection
 * fields drive the agent panel's stuck count, so totals stay consistent.
 */
function renderAlerts(tasks) {
  const listEl = document.querySelector("#alertsList");
  const metaEl = document.querySelector("#alertsMeta");
  if (!listEl || !metaEl) return;
  const nowMs = Date.now();
  const stuckMs = 15 * 60 * 1000;
  const alerts = [];
  tasks.forEach(task => {
    if (task.status === "failed") {
      alerts.push({task, kind: "failed", label: task.failureCause ? `failed (${task.failureCause})` : "failed"});
    } else if (task.status === "blocked") {
      alerts.push({task, kind: "blocked", label: "blocked"});
    } else if (task.status === "revision_requested") {
      alerts.push({task, kind: "revision", label: "revision requested"});
    } else if ((task.status === "leased" || task.status === "running") &&
               task.leasedAtUnixMs > 0 && nowMs - task.leasedAtUnixMs > stuckMs) {
      alerts.push({task, kind: "stuck", label: `stuck >15m (${task.assignedAgent || "?"})`});
    }
  });
  metaEl.textContent = `${alerts.length} OPEN`;
  listEl.innerHTML = "";
  if (!alerts.length) {
    const empty = document.createElement("div");
    empty.className = "alerts-empty";
    empty.textContent = "No failed, stuck, blocked, or revision-requested tasks.";
    listEl.appendChild(empty);
    return;
  }
  /* Limit to top 8 so the alert panel never crowds the status grid. */
  alerts.slice(0, 8).forEach(({task, kind, label}) => {
    const row = document.createElement("button");
    row.type = "button";
    row.className = `alert-row alert-${kind}`;
    row.innerHTML = "<span class=\"alert-kind\"></span><strong></strong><small></small>";
    row.querySelector(".alert-kind").textContent = label;
    row.querySelector("strong").textContent = task.title || task.taskId;
    row.querySelector("small").textContent = `${task.taskId} · ${task.assignedAgent || "all"}`;
    row.addEventListener("click", () => loadTaskDetail(task.taskId));
    listEl.appendChild(row);
  });
}

function renderInitiatives(initiatives) {
  const summary = document.querySelector("#initiativeSummary");
  document.querySelector("#initiativeCount").textContent = String(initiatives.length);
  document.querySelector("#initiativeBadge").textContent = String(initiatives.length);
  /* Keep the chat-panel initiative filter dropdown in sync with the latest
   * projection rows. Preserve the operator's current selection if it still
   * exists. */
  const initiativeFilter = document.querySelector("#initiativeFilter");
  if (initiativeFilter) {
    const previous = initiativeFilter.value;
    initiativeFilter.innerHTML = '<option value="">all initiatives</option>';
    initiatives.forEach(initiative => {
      const option = document.createElement("option");
      option.value = initiative.initiativeId;
      option.textContent = `${initiative.initiativeId} · ${initiative.title || ""}`.trim();
      initiativeFilter.appendChild(option);
    });
    initiativeFilter.value = state.selectedInitiativeId || previous || "";
  }
  if (!initiatives.length) {
    summary.innerHTML = '<button type="button" class="initiative-card"><strong>No initiatives</strong><span>Create a task package to start.</span></button>';
    return;
  }
  if (state.selectedInitiativeId && !initiatives.some(item => item.initiativeId === state.selectedInitiativeId)) {
    state.selectedInitiativeId = "";
  }
  summary.innerHTML = "";
  initiatives.slice(0, 8).forEach(initiative => {
    const card = document.createElement("button");
    card.type = "button";
    card.className = `initiative-card ${initiative.initiativeId === state.selectedInitiativeId ? "active" : ""}`;
    card.innerHTML = "<strong></strong><span></span><span></span>";
    card.querySelector("strong").textContent = initiative.title || initiative.initiativeId;
    card.querySelector("span:nth-child(2)").textContent = initiative.initiativeId;
    card.querySelector("span:nth-child(3)").textContent = `${initiative.activeTasks || 0} active · ${initiative.taskCount || 0} tasks · ${fmtTokens(initiative.maxTokens || 0)}`;
    card.addEventListener("click", async () => {
      state.selectedInitiativeId = state.selectedInitiativeId === initiative.initiativeId ? "" : initiative.initiativeId;
      await loadTasks();
      renderInitiatives(initiatives);
      await loadInitiativeAssets(state.selectedInitiativeId);
      await loadInitiativeDeliverables(state.selectedInitiativeId);
      addAudit("ui", state.selectedInitiativeId ? `focused ${state.selectedInitiativeId}` : "cleared initiative focus");
    });
    /* Sprint 5: per-card audit button opens the initiative audit JSON in a new
     * tab. Mirrors `wt-task audit INIT` on the CLI side. */
    const auditButton = document.createElement("button");
    auditButton.type = "button";
    auditButton.className = "initiative-audit-button";
    auditButton.textContent = "Audit";
    auditButton.title = "Open the combined initiative audit (tasks, events, policy, usage) in a new tab.";
    auditButton.addEventListener("click", event => {
      event.stopPropagation();
      const url = `/api/initiative-audit?initiativeId=${encodeURIComponent(initiative.initiativeId)}`;
      window.open(url, "_blank", "noopener");
      addAudit("ui", `opened audit for ${initiative.initiativeId}`);
    });
    card.appendChild(auditButton);
    summary.appendChild(card);
  });
}

async function loadTaskDetail(taskId) {
  state.selectedTaskId = taskId;
  const response = await fetch(`/api/task-detail?taskId=${encodeURIComponent(taskId)}`);
  if (!response.ok) {
    addAudit("ui", `task detail failed for ${taskId}`);
    return;
  }
  const detail = await response.json();
  if (!detail.ok) return;
  taskDetail.hidden = false;
  state.selectedTaskStatus = detail.task.status || "";
  state.selectedTaskDetail = detail.task;
  document.querySelector("#detailTitle").textContent = detail.task.title || taskId;
  /*
   * Build a meta line that always carries: id, status, assigned agent, budget.
   * Sprint 3 closeout: when the projection reports a failure cause, an active
   * lease holder, or a non-zero reclaim count, surface those in-line so the
   * operator does not have to open the raw events list to see why a task is in
   * its current state.
   */
  const metaParts = [
    detail.task.taskId,
    detail.task.status,
    detail.task.assignedAgent || "all",
    `${fmtTokens(detail.task.maxTokens || 0)} tokens`,
  ];
  if (detail.task.attemptCount > 0) metaParts.push(`attempt ${detail.task.attemptCount}`);
  if (detail.task.failureCause) metaParts.push(`cause: ${detail.task.failureCause}`);
  if (detail.task.leaseOwner) metaParts.push(`lease: ${detail.task.leaseOwner}`);
  if (detail.task.autonomyLevel) metaParts.push(`autonomy: ${detail.task.autonomyLevel}`);
  if (detail.task.reclaimCount > 0) {
    const reason = detail.task.lastReclaimReason || "operator";
    metaParts.push(`reclaimed ${detail.task.reclaimCount}× (${reason})`);
  }
  /* Sprint 4: surface artifact lifecycle state and accepted path in the meta
   * line. The artifact decision panel below shows the editable controls; this
   * is the read-only inline summary. */
  if (detail.task.artifactState) {
    const acceptedPath = detail.task.acceptedArtifactPath ? ` ${detail.task.acceptedArtifactPath}` : "";
    metaParts.push(`artifact: ${detail.task.artifactState}${acceptedPath}`);
  }
  document.querySelector("#detailMeta").textContent = metaParts.join(" · ");
  document.querySelector("#detailBody").textContent = detail.task.body || "";
  const events = document.querySelector("#detailEvents");
  events.innerHTML = "";
  detail.events.forEach(event => {
    const item = document.createElement("div");
    item.className = "detail-event";
    const stamp = event.createdAtUnixMs ? new Date(event.createdAtUnixMs).toISOString().slice(11, 19) : "--:--:--";
    item.innerHTML = "<b></b><span></span><p></p>";
    item.querySelector("b").textContent = `${stamp} ${event.eventType || event.schema}`;
    item.querySelector("span").textContent = event.status || event.createdBy || "";
    item.querySelector("p").textContent = event.message || "";
    events.appendChild(item);
  });
  const gateCount = detail.events.filter(event => event.eventType === "review_gate").length;
  document.querySelector("#gateBadge").textContent = String(gateCount);
  document.querySelector("#gateMeta").textContent = gateCount ? `${gateCount} gate event(s) on selected task` : "No gate decision recorded for selected task.";
  await loadTaskArtifacts(taskId);
}

async function loadTaskArtifacts(taskId) {
  const response = await fetch(`/api/task-artifacts?taskId=${encodeURIComponent(taskId)}`);
  if (!response.ok) return;
  const artifacts = await response.json();
  renderArtifacts(artifacts);
}

function renderArtifacts(artifacts) {
  const head = document.querySelector("#detailArtifacts .artifact-head span");
  const files = document.querySelector("#artifactFiles");
  const viewer = document.querySelector("#artifactViewer");
  files.innerHTML = "";
  viewer.innerHTML = "";
  if (!artifacts.exists) {
    head.textContent = "No task workspace yet";
    viewer.textContent = "Artifacts appear here after an adapter run creates a workspace.";
    populateArtifactDecisionPanel(artifacts, null);
    return;
  }
  head.textContent = `${artifacts.files.length} file(s) · ${artifacts.workspace}`;
  artifacts.files.forEach(file => {
    const chip = document.createElement("button");
    chip.type = "button";
    chip.textContent = `${file.name} · ${file.bytes}b`;
    chip.addEventListener("click", () => {
      const key = file.kind === "result" ? "resultText" :
        file.kind === "stdout" ? "stdoutText" :
        file.kind === "stderr" ? "stderrText" :
        file.kind === "manifest" ? "manifestText" : "";
      viewer.textContent = key ? artifacts[key] || "" : "File preview is not available for this artifact.";
      /* Selecting a file also pre-populates the decision panel path. */
      const select = document.querySelector("#artifactPathSelect");
      if (select) select.value = file.name;
    });
    files.appendChild(chip);
  });
  viewer.textContent = artifacts.resultText || artifacts.manifestText || artifacts.stdoutText || "Workspace exists, but no previewable artifact has content yet.";
  /* Cache artifacts for the export button to read text snippets without a refetch. */
  state.artifactsCache = artifacts;
  populateArtifactDecisionPanel(artifacts, state.selectedTaskDetail);
}

/*
 * Populate the artifact decision panel for the currently selected task. Fills
 * the path <select> with workspace files, defaults the reviewer to the current
 * operator name, and shows the latest recorded state + notes.
 */
function populateArtifactDecisionPanel(artifacts, task) {
  const stateLabel = document.querySelector("#artifactStateLabel");
  const pathSelect = document.querySelector("#artifactPathSelect");
  const reviewerInput = document.querySelector("#artifactReviewer");
  const notesInput = document.querySelector("#artifactNotes");
  if (!stateLabel || !pathSelect) return;
  pathSelect.innerHTML = "";
  const fileNames = (artifacts && artifacts.files) ? artifacts.files.map(file => file.name) : [];
  /* Always include the canonical result.md option even if the workspace is empty
   * so the operator can record decisions before an adapter run completes. */
  if (!fileNames.includes("result.md")) fileNames.unshift("result.md");
  fileNames.forEach(name => {
    const option = document.createElement("option");
    option.value = name;
    option.textContent = name;
    pathSelect.appendChild(option);
  });
  if (task && task.acceptedArtifactPath && fileNames.includes(task.acceptedArtifactPath)) {
    pathSelect.value = task.acceptedArtifactPath;
  }
  if (reviewerInput && !reviewerInput.value) {
    reviewerInput.value = (task && task.lastReviewer) || "ceo";
  }
  if (notesInput) {
    notesInput.value = (task && task.lastReviewNotes) || "";
  }
  if (task && task.artifactState) {
    const acceptedAt = task.acceptedAtUnixMs ? new Date(task.acceptedAtUnixMs).toISOString().slice(0, 19).replace("T", " ") : "";
    stateLabel.textContent = `${task.artifactState}${task.lastReviewer ? ` by ${task.lastReviewer}` : ""}${acceptedAt ? ` (${acceptedAt})` : ""}`;
  } else {
    stateLabel.textContent = "No decision recorded";
  }
  /* Sprint 3: reveal the ship strip only when the task has an accepted
   * artifact - shipping a non-accepted artifact would be an operator error. */
  const shipStrip = document.querySelector("#deliverableShipStrip");
  if (shipStrip) {
    shipStrip.hidden = !(task && task.artifactState === "accepted");
    const status = document.querySelector("#deliverableShipStatus");
    if (status) { status.textContent = ""; status.className = "deliverable-ship-status"; }
  }
}

/*
 * Phase 3 Sprint 3: react to mode changes - branch + pull-request need a
 * repo path; pr also needs the operator to flip a confirmation checkbox.
 */
function updateDeliverableShipStripVisibility() {
  const mode = (document.querySelector("#deliverableShipMode") || {}).value || "copy";
  const repoLabel = document.querySelector("label.deliverable-ship-repo");
  const yesLabel = document.querySelector("label.deliverable-ship-yes");
  if (repoLabel) repoLabel.hidden = !(mode === "branch" || mode === "pr");
  if (yesLabel) yesLabel.hidden = !(mode === "pr");
}

async function shipSelectedDeliverable() {
  if (!state.selectedTaskId) {
    addAudit("ui", "select a task before shipping a deliverable");
    return;
  }
  const mode = (document.querySelector("#deliverableShipMode") || {}).value || "copy";
  const repo = ((document.querySelector("#deliverableShipRepo") || {}).value || "").trim();
  const yes = (document.querySelector("#deliverableShipYes") || {}).checked || false;
  const status = document.querySelector("#deliverableShipStatus");
  if ((mode === "branch" || mode === "pr") && !repo) {
    if (status) { status.textContent = "repo path required for this mode"; status.className = "deliverable-ship-status error"; }
    return;
  }
  if (mode === "pr" && !yes) {
    if (status) { status.textContent = "pull-request mode requires the explicit yes checkbox"; status.className = "deliverable-ship-status error"; }
    return;
  }
  if (status) { status.textContent = `shipping (${mode})...`; status.className = "deliverable-ship-status"; }
  const body = {
    taskId: state.selectedTaskId,
    mode,
    reviewer: ((document.querySelector("#artifactReviewer") || {}).value || "ceo").trim(),
    repoPath: repo,
    yes,
  };
  const response = await fetch("/api/task-deliverable", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(body),
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok || payload.ok === false) {
    const reason = payload.error || `http ${response.status}`;
    if (status) { status.textContent = `ship failed: ${reason}`; status.className = "deliverable-ship-status error"; }
    addAudit("ui", `deliverable ship failed (${reason}) for ${state.selectedTaskId}`);
    return;
  }
  if (status) {
    status.textContent = `shipped ${payload.deliverableId} -> ${payload.deliverablePath}`;
    status.className = "deliverable-ship-status ok";
  }
  addAudit("ui", `deliverable shipped (${mode}) for ${state.selectedTaskId}`);
  if (state.selectedInitiativeId) await loadInitiativeDeliverables(state.selectedInitiativeId);
}

/*
 * Sprint 4: post an artifact decision. Mirrors the bin/wt-task artifact verbs:
 * promote (accepted), reviewed, rejected, superseded.
 */
async function postTaskArtifact(decisionState) {
  if (!state.selectedTaskId) {
    addAudit("ui", "select a task before recording an artifact decision");
    return;
  }
  const path = (document.querySelector("#artifactPathSelect") || {}).value || "result.md";
  const reviewer = ((document.querySelector("#artifactReviewer") || {}).value || "ceo").trim();
  const notes = ((document.querySelector("#artifactNotes") || {}).value || "").trim();
  const response = await fetch("/api/task-artifact", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      taskId: state.selectedTaskId,
      state: decisionState,
      reviewer: reviewer || "ceo",
      notes,
      artifactPath: path,
    }),
  });
  if (!response.ok) {
    addAudit("ui", `artifact ${decisionState} failed for ${state.selectedTaskId}`);
    return;
  }
  addAudit("ui", `artifact ${decisionState} recorded for ${state.selectedTaskId}`);
  await loadTasks();
  await loadTaskDetail(state.selectedTaskId);
  if (state.selectedInitiativeId) {
    await loadInitiativeAssets(state.selectedInitiativeId);
    await loadInitiativeDeliverables(state.selectedInitiativeId);
  }
}

/*
 * Copies the accepted artifact text to the clipboard, mirroring
 * `wt-task artifact export` from the CLI side.
 */
async function exportSelectedArtifact() {
  if (!state.selectedTaskId) {
    addAudit("ui", "select a task before exporting");
    return;
  }
  const path = (document.querySelector("#artifactPathSelect") || {}).value || "result.md";
  const artifacts = state.artifactsCache || {};
  const key = path === "stdout.log" ? "stdoutText" :
              path === "stderr.log" ? "stderrText" :
              path === "manifest.json" ? "manifestText" : "resultText";
  const text = artifacts[key] || "";
  try {
    await navigator.clipboard.writeText(text);
    addAudit("ui", `copied ${path} (${text.length} bytes) to clipboard`);
  } catch (err) {
    /* Fallback for environments without clipboard permission - dump into the
     * artifact viewer so the operator can copy manually. */
    const viewer = document.querySelector("#artifactViewer");
    if (viewer) viewer.textContent = text || "(empty artifact)";
    addAudit("ui", `clipboard unavailable; rendered ${path} into viewer`);
  }
}

async function postTaskLifecycle(status, message) {
  if (!state.selectedTaskId) {
    addAudit("ui", "select a task before using lifecycle controls");
    return;
  }
  const payload = {
    schema: "woventeam.task_event.v0.1",
    taskId: state.selectedTaskId,
    eventType: "lifecycle",
    status,
    message,
    createdBy: "ceo",
    createdAtUnixMs: nowMs()
  };
  const response = await fetch("/api/task-event", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(payload)
  });
  if (!response.ok) {
    addAudit("ui", `${status} failed for ${state.selectedTaskId}`);
    return;
  }
  addAudit("ui", `${status} posted for ${state.selectedTaskId}`);
  await loadTasks();
  await loadTaskDetail(state.selectedTaskId);
}

async function postAutonomyRevoke() {
  if (!state.selectedTaskId) {
    addAudit("ui", "select a task before revoking autonomy");
    return;
  }
  const response = await fetch("/api/autonomy-revoke", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      taskId: state.selectedTaskId,
      reason: "Revoked from the web console.",
      createdBy: "ceo"
    })
  });
  if (!response.ok) {
    addAudit("ui", `autonomy revoke failed for ${state.selectedTaskId}`);
    return;
  }
  addAudit("ui", `autonomy revoked for ${state.selectedTaskId}`);
  await loadTasks();
  await loadTaskDetail(state.selectedTaskId);
}

async function postTaskGate(action) {
  if (!state.selectedTaskId) {
    addAudit("ui", "select a task before using review gates");
    return;
  }
  const payload = {
    taskId: state.selectedTaskId,
    action,
    createdBy: "ceo"
  };
  const response = await fetch("/api/task-gate", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(payload)
  });
  if (!response.ok) {
    addAudit("ui", `${action} gate failed for ${state.selectedTaskId}`);
    return;
  }
  addAudit("ui", `${action} gate posted for ${state.selectedTaskId}`);
  await loadTasks();
  await loadTaskDetail(state.selectedTaskId);
}

function progressForStatus(status) {
  if (status === "complete") return "100%";
  if (status === "leased") return "35%";
  if (status === "running") return "55%";
  if (status === "blocked" || status === "failed") return "35%";
  return "12%";
}

function connectEvents() {
  const events = new EventSource("/events");
  events.addEventListener("open", () => setUplink(true));
  events.addEventListener("error", () => setUplink(false));
  events.addEventListener("message", event => {
    setUplink(true);
    renderMessage(JSON.parse(event.data));
  });
}

function addAudit(actor, text) {
  const time = new Date().toISOString().slice(11, 19);
  state.audit.push(`${time} ${actor} ${text}`);
  state.audit = state.audit.slice(-6);
  auditLog.textContent = state.audit.join("  ·  ");
}

function renderRoles() {
  const roleGrid = document.querySelector("#roleGrid");
  const roleButtons = document.querySelector("#roleButtons");
  roleGrid.innerHTML = "";
  roleButtons.innerHTML = "";

  const activeCounts = Object.fromEntries(roles.map(role => [role.id, state.taskStats.roleCounts[role.roleId] || 0]));
  roles.slice(0, 12).forEach(role => {
    const chip = document.createElement("div");
    chip.className = "role-chip";
    chip.style.setProperty("--role-hue", role.hue);
    chip.innerHTML = `<i></i><span>${role.short}</span><b>${activeCounts[role.id] || 0}</b>`;
    roleGrid.appendChild(chip);
  });

  roles.forEach(role => {
    const disabled = state.dispatchMode === "request" && !canRequestRole(requestedByRoleEl.value, role.roleId);
    const button = document.createElement("button");
    button.type = "button";
    button.className = `role-button ${role.id === state.selectedRole ? "active" : ""}`;
    button.style.setProperty("--role-hue", role.hue);
    button.title = role.name;
    button.disabled = disabled;
    button.setAttribute("aria-disabled", disabled ? "true" : "false");
    button.innerHTML = `<i></i><span>${role.short}</span>`;
    button.addEventListener("click", () => {
      if (button.disabled) return;
      state.selectedRole = role.id;
      document.querySelector("#selectedRoleLabel").textContent = role.name;
      renderRoles();
    });
    roleButtons.appendChild(button);
  });
}

async function postAgentControl(agent, action) {
  const response = await fetch("/api/agent-control", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({agent, action, message: `${action} requested from web console`, createdBy: "ceo"})
  });
  if (!response.ok) {
    addAudit("ui", `${action} failed for ${agent}`);
    return;
  }
  addAudit("ui", `${action} posted for ${agent}`);
  await loadAgents();
}

/*
 * Sprint 3 closeout: operator-visible task reclaim. POST releases the most
 * recent lease so a stuck task returns to the queued pool. The selected task is
 * implied by state.selectedTaskId; the reason is hard-coded to "operator"
 * because the UI button is explicitly an operator action.
 */
async function postTaskReclaim(taskId, reason) {
  if (!taskId) {
    addAudit("ui", "reclaim ignored: no task selected");
    return;
  }
  const response = await fetch("/api/task-reclaim", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      taskId,
      reason: reason || "operator",
      message: `Reclaim requested from web console (${reason || "operator"}).`,
      createdBy: "ceo",
    }),
  });
  if (!response.ok) {
    addAudit("ui", `reclaim failed for ${taskId}`);
    return;
  }
  addAudit("ui", `reclaim posted for ${taskId}`);
  await loadTasks();
  await loadTaskDetail(taskId);
  await loadAgents();
}

function renderAgentControls() {
  const container = document.querySelector("#agentControls");
  if (!container) return;
  const agents = state.agents.length ? state.agents : ["claude", "chatgpt", "gemini"].map(agent => ({
    agent, state: "active", activeTasks: 0, leasedTasks: 0, runningTasks: 0, stuckTasks: 0, attempts: 0
  }));
  const stuck = agents.reduce((sum, agent) => sum + Number(agent.stuckTasks || 0), 0);
  const paused = agents.filter(agent => agent.state === "paused").length;
  document.querySelector("#agentMeta").textContent =
    `● ${agents.length - paused} ACTIVE · ${paused} PAUSED · ${stuck} STUCK`;
  container.innerHTML = "";
  agents.forEach(agent => {
    const card = document.createElement("div");
    const isPaused = agent.state === "paused";
    const stuckCount = Number(agent.stuckTasks || 0);
    card.className = `agent-control-card ${isPaused ? "paused" : ""} ${stuckCount > 0 ? "stuck" : ""}`;
    card.innerHTML = "<strong></strong><span></span><div class=\"agent-control-buttons\"></div>";
    card.querySelector("strong").textContent = `${agent.agent} · ${agent.state}`;
    card.querySelector("span").textContent =
      `${agent.leasedTasks || 0} leased · ${agent.runningTasks || 0} running · ${stuckCount} stuck · ${agent.attempts || 0} attempts`;
    const buttonRow = card.querySelector(".agent-control-buttons");
    const pauseButton = document.createElement("button");
    pauseButton.type = "button";
    pauseButton.textContent = isPaused ? "RESUME" : "PAUSE";
    pauseButton.addEventListener("click", () => postAgentControl(agent.agent, isPaused ? "resume" : "pause"));
    buttonRow.appendChild(pauseButton);
    /*
     * Reclaim affordance: only shown when this agent has at least one stuck
     * task. It reclaims the currently selected task, which is the common case
     * for an operator who clicked into a stuck task to investigate. If no task
     * is selected the helper records an audit warning rather than failing.
     */
    if (stuckCount > 0) {
      const reclaimButton = document.createElement("button");
      reclaimButton.type = "button";
      reclaimButton.className = "reclaim";
      reclaimButton.textContent = "RECLAIM TASK";
      reclaimButton.title = "Release the most recent lease on the currently selected task.";
      reclaimButton.addEventListener("click", () => postTaskReclaim(state.selectedTaskId, "operator"));
      buttonRow.appendChild(reclaimButton);
    }
    container.appendChild(card);
  });
}

function renderPresets() {
  const container = document.querySelector("#presetButtons");
  container.innerHTML = "";
  presets.forEach(preset => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `preset-button ${preset.id === state.selectedPreset ? "active" : ""}`;
    button.innerHTML = `<b>${preset.name}</b><span>${fmtTokens(preset.tokens)} · ${preset.agents}ag · ${preset.hours}h</span>`;
    button.addEventListener("click", () => applyPreset(preset.id));
    container.appendChild(button);
  });
}

function applyPreset(id) {
  const preset = presets.find(item => item.id === id);
  if (!preset) return;
  state.selectedPreset = id;
  document.querySelector("#tokenBudget").value = preset.tokens;
  document.querySelector("#maxAgents").value = preset.agents;
  document.querySelector("#timeframeHours").value = preset.hours;
  document.querySelector("#priority").value = preset.priority;
  document.querySelector("#selectedPresetLabel").textContent = preset.name;
  renderPresets();
  updateReadouts();
}

function updateReadouts() {
  const tokens = Number(document.querySelector("#tokenBudget").value);
  const agents = Number(document.querySelector("#maxAgents").value);
  const hours = Number(document.querySelector("#timeframeHours").value);
  const priority = document.querySelector("#priority").value;
  const projectedDay = (state.tokens.dayWindowAllocatedTokens || 0) + tokens;
  const projectedMonth = (state.tokens.monthWindowAllocatedTokens || 0) + tokens;
  const dayBudget = state.tokens.tokenDailyBudget || state.config.tokenDailyBudget || 0;
  const monthBudget = state.tokens.tokenMonthlyBudget || state.config.tokenMonthlyBudget || 0;
  const overBudget = (dayBudget > 0 && projectedDay > dayBudget) || (monthBudget > 0 && projectedMonth > monthBudget);
  document.querySelector("#tokenReadout").textContent = fmtTokens(tokens);
  document.querySelector("#agentReadout").textContent = String(agents);
  document.querySelector("#hoursReadout").textContent = `${hours}h`;
  document.querySelector("#burnEstimate").textContent = fmtMoneyFromCents(tokens * (state.config.tokenCostPerMillionCents || 0) / 1000000);
  document.querySelector("#subAgentSummary").textContent = `0 / ${agents}`;
  document.querySelector("#prioritySummary").textContent = priority;
  /*
   * Sprint 5: the budget pressure note also lists active policy levers so the
   * operator knows what will trip on EXECUTE. Blocked vendors are read from
   * config; per-initiative/per-family caps render only when configured.
   */
  const policySegments = [];
  if (state.config.blockedVendors) policySegments.push(`blocked: ${state.config.blockedVendors}`);
  if (state.config.tokenBudgetPerInitiative > 0) {
    policySegments.push(`per-initiative cap ${fmtTokens(state.config.tokenBudgetPerInitiative)}`);
  }
  if (state.config.tokenBudgetPerModelFamily > 0) {
    policySegments.push(`per-family cap ${fmtTokens(state.config.tokenBudgetPerModelFamily)}`);
  }
  const policyNote = policySegments.length ? ` · Policy: ${policySegments.join(", ")}.` : "";
  document.querySelector("#budgetPressureNote").textContent = (overBudget ?
    "Budget hard stop: this package would exceed the configured 24h or 30d allocation budget." :
    `Budget pressure: ${fmtTokens(projectedDay)} projected in 24h, ${fmtTokens(projectedMonth)} projected in 30d.`) + policyNote;
  sendButton.disabled = !state.armed || overBudget;
}

function renderVendors(vendors = null) {
  vendors = vendors || [
    {name: "Claude Code", cli: "claude", state: "launchable", quota: 34},
    {name: "Codex CLI", cli: "codex", state: "opt-in adapter", quota: 41},
    {name: "Gemini CLI", cli: "gemini", state: "launchable", quota: 22},
    {name: "Manager Tasks", cli: "task.request", state: "live", quota: 100},
    {name: "Gate Controls", cli: "review", state: "disabled", quota: 0, warn: true}
  ];
  const grid = document.querySelector("#vendorGrid");
  grid.innerHTML = "";
  vendors.forEach(vendor => {
    const card = document.createElement("div");
    card.className = "vendor-card";
    card.innerHTML = `
      <div class="vendor-name"><span>${vendor.name}</span><i class="dot ${vendor.warn ? "warn" : ""}"></i></div>
      <p>${vendor.cli} · ${vendor.state}</p>
      ${vendor.lastFailure ? `<p>last failure · ${vendor.lastFailure}</p>` : ""}
      <div class="quota"><span style="width:${vendor.quota}%"></span></div>
    `;
    grid.appendChild(card);
  });
  const enabled = vendors.filter(vendor => !vendor.warn).length;
  const disabled = vendors.length - enabled;
  document.querySelector("#vendorMeta").textContent = `● ${enabled} LIVE · ● ${disabled} DISABLED`;
}

function drawSparkline(percent = 0) {
  const svg = document.querySelector("#tokenSparkline");
  const load = Math.max(0, Math.min(100, Number(percent) || 0)) / 100;
  const points = Array.from({length: 34}, (_, i) => {
    const y = 30 - load * 18 - Math.sin(i / 3) * (4 + load * 6) - Math.cos(i / 5) * 3;
    return `${(i / 33) * 310},${Math.max(5, Math.min(33, y))}`;
  }).join(" ");
  svg.innerHTML = `
    <defs>
      <linearGradient id="sparkFill" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0%" stop-color="currentColor" stop-opacity="0.38"></stop>
        <stop offset="100%" stop-color="currentColor" stop-opacity="0"></stop>
      </linearGradient>
    </defs>
    <polyline points="0,36 ${points} 310,36" fill="url(#sparkFill)" stroke="none"></polyline>
    <polyline points="${points}" fill="none" stroke="currentColor" stroke-width="1.5"></polyline>
    <circle cx="310" cy="18" r="2.5" fill="currentColor"></circle>
  `;
}

function armComposer() {
  state.armed = true;
  armButton.textContent = "ARMED";
  armButton.classList.add("armed");
  sendButton.disabled = false;
  updateReadouts();
  clearTimeout(state.armTimer);
  state.armTimer = setTimeout(disarmComposer, 5000);
}

function disarmComposer() {
  state.armed = false;
  armButton.textContent = "ARM";
  armButton.classList.remove("armed");
  sendButton.disabled = true;
}

function nowMs() {
  return Date.now();
}

function newTaskId() {
  const cryptoApi = globalThis.crypto;
  if (cryptoApi && cryptoApi.randomUUID) {
    return `task_${cryptoApi.randomUUID().replace(/-/g, "")}`;
  }
  return `task_${Date.now()}_${Math.floor(Math.random() * 100000)}`;
}

function newInitiativeId() {
  const raw = initiativeNameEl.value.trim().toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "").slice(0, 48);
  return `init_${raw || Date.now()}`;
}

function roleAgent(role) {
  return state.config.roleRoutingEnabled ? "router" : (role.agent || "all");
}

function roleModel(role) {
  const agent = role.agent || "chatgpt";
  if (agent === "claude") return "anthropic/claude-sonnet";
  if (agent === "gemini") return "google/gemini-pro";
  return "openai/gpt-5.3-codex";
}

function toolPolicy(profile = "observe") {
  const writable = profile === "repo_branch" || profile === "test_local";
  return {
    profile,
    filesystem: writable ? "workspace_write" : "read_only",
    network: profile === "test_local" ? "loopback" : "none",
    system: "none",
    git: writable ? "branch_only" : "none"
  };
}

function canRequestRole(requesterRole, requestedRole) {
  if (requesterRole === "program_manager") {
    return ["project_manager", "software_architect", "systems_architect"].includes(requestedRole);
  }
  if (requesterRole === "project_manager") {
    return !["program_manager", "project_manager"].includes(requestedRole);
  }
  return false;
}

function ensureSelectedRoleAllowed() {
  if (state.dispatchMode !== "request") return;
  const requesterRole = requestedByRoleEl.value;
  const selected = roleById[state.selectedRole];
  if (selected && canRequestRole(requesterRole, selected.roleId)) return;
  const fallback = roles.find(role => canRequestRole(requesterRole, role.roleId));
  if (fallback) {
    state.selectedRole = fallback.id;
    document.querySelector("#selectedRoleLabel").textContent = fallback.name;
  }
}

function buildTaskPackage() {
  const role = roleById[state.selectedRole];
  const name = initiativeNameEl.value.trim() || "Untitled initiative";
  const body = bodyEl.value.trim();
  const priority = document.querySelector("#priority").value;
  const maxTokens = Number(document.querySelector("#tokenBudget").value);
  const autonomyLevel = document.querySelector("#autonomyLevel").value;
  const autonomyScope = document.querySelector("#autonomyScope").value.trim();
  const autonomyTtl = Number(document.querySelector("#autonomyTtl").value || 0);
  const taskId = newTaskId();
  const payload = {
    schema: "woventeam.task_package.v0.1",
    taskId,
    initiativeId: newInitiativeId(),
    createdBy: "ceo",
    assignedRole: role.roleId,
    assignedAgent: roleAgent(role),
    modelId: roleModel(role),
    priority: priority === "critical" ? "urgent" : priority,
    status: "queued",
    title: name,
    body,
    task: {title: name, body, deliverables: []},
    contextRefs: [],
    acceptanceCriteria: ["Task result is recorded in the room and task ledger."],
    toolPolicy: toolPolicy("observe"),
    budget: {timeoutSeconds: 1800, maxOutputBytes: 1048576, maxCostUsd: 1.0, maxTokens},
    dependencies: [],
    createdAtUnixMs: nowMs()
  };
  if (autonomyLevel) payload.autonomyLevel = autonomyLevel;
  if (autonomyLevel || autonomyScope || autonomyTtl > 0) {
    payload.autonomyGrant = {
      scope: autonomyScope,
      ttlSeconds: autonomyTtl,
      maxWallClockSeconds: 1800,
      maxCostUsd: 1.0,
      maxTokens,
      network: autonomyScope ? "intranet" : "none",
      credentialClass: autonomyScope.includes("git") ? "repo-write" : "none",
      requiresCleanWorktree: false
    };
  }
  return payload;
}

function buildTaskRequest() {
  const role = roleById[state.selectedRole];
  const title = initiativeNameEl.value.trim() || "Untitled subtask";
  const body = bodyEl.value.trim();
  const priority = document.querySelector("#priority").value;
  const maxTokens = Number(document.querySelector("#tokenBudget").value);
  const autonomyLevel = document.querySelector("#autonomyLevel").value;
  const autonomyScope = document.querySelector("#autonomyScope").value.trim();
  const autonomyTtl = Number(document.querySelector("#autonomyTtl").value || 0);
  const requestedTaskId = newTaskId();
  const payload = {
    schema: "woventeam.task_request.v0.1",
    taskId: requestedTaskId,
    parentTaskId: parentTaskIdEl.value.trim(),
    requestedTaskId,
    initiativeId: newInitiativeId(),
    requestedBy: requestedByRoleEl.value,
    requestedByRole: requestedByRoleEl.value,
    requestedRole: role.roleId,
    assignedAgent: roleAgent(role),
    modelId: roleModel(role),
    priority: priority === "critical" ? "urgent" : priority,
    title,
    body,
    toolPolicy: {profile: "observe"},
    budget: {maxTokens},
    createdAtUnixMs: nowMs()
  };
  if (autonomyLevel) payload.autonomyLevel = autonomyLevel;
  if (autonomyLevel || autonomyScope || autonomyTtl > 0) {
    payload.autonomyGrant = {
      scope: autonomyScope,
      ttlSeconds: autonomyTtl,
      maxWallClockSeconds: 1800,
      maxTokens,
      network: autonomyScope ? "intranet" : "none",
      credentialClass: autonomyScope.includes("git") ? "repo-write" : "none",
      requiresCleanWorktree: false
    };
  }
  return payload;
}

function buildDirective() {
  const role = roleById[state.selectedRole];
  const name = initiativeNameEl.value.trim() || "Untitled initiative";
  const body = bodyEl.value.trim();
  const tokens = Number(document.querySelector("#tokenBudget").value);
  const agents = Number(document.querySelector("#maxAgents").value);
  const hours = Number(document.querySelector("#timeframeHours").value);
  const priority = document.querySelector("#priority").value;
  return [
    "TASK PACKAGE REQUEST (temporary room directive)",
    `Initiative: ${name}`,
    `Lead role: ${role.name} (${role.short})`,
    `Authority preset: ${state.selectedPreset}`,
    `Budget: ${fmtTokens(tokens)} tokens, ${agents} max sub-agents, ${hours}h, priority ${priority}`,
    "",
    body
  ].join("\n");
}

async function submitDirective(event) {
  event.preventDefault();
  if (!state.armed) return;
  const messageBody = bodyEl.value.trim();
  if (!messageBody) return;
  if (state.dispatchMode === "request" && !parentTaskIdEl.value.trim()) {
    addAudit("ui", "parent task id required for manager subtask");
    disarmComposer();
    return;
  }
  const path = state.dispatchMode === "request" ? "/api/task-request" : "/api/task-package";
  const payload = state.dispatchMode === "request" ? buildTaskRequest() : buildTaskPackage();
  const response = await fetch(path, {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(payload)
  });
  if (!response.ok) {
    /* Sprint 5: surface the central policy evaluator's classified reason so
     * the operator sees blocked_vendor / initiative_budget / etc. inline rather
     * than a generic "dispatch failed". */
    let detail = "";
    try {
      const errorPayload = await response.json();
      if (errorPayload && errorPayload.reason) {
        detail = `${errorPayload.reason} - ${errorPayload.detail || errorPayload.error || ""}`;
      } else if (errorPayload && errorPayload.error) {
        detail = errorPayload.error;
      }
    } catch (_) { /* response body not JSON; fall back to generic message */ }
    addAudit("ui", `${state.dispatchMode} denied${detail ? ": " + detail : ""}`);
    disarmComposer();
    return;
  }
  addAudit("ui", `${state.dispatchMode} dispatched ${payload.taskId || payload.requestedTaskId}`);
  bodyEl.value = "";
  initiativeNameEl.value = "";
  parentTaskIdEl.value = "";
  disarmComposer();
  await loadTasks();
}

function setDispatchMode(mode) {
  state.dispatchMode = mode;
  ensureSelectedRoleAllowed();
  document.querySelectorAll(".mode-button").forEach(button => button.classList.toggle("active", button.dataset.mode === mode));
  document.querySelector("#requestFields").hidden = mode !== "request";
  document.querySelector("#selectedModeLabel").textContent = mode === "request" ? "Manager subtask" : "CEO task package";
  document.querySelector("#composerModeLabel").textContent = mode === "request" ? "TASK REQUEST MODE" : "TASK PACKAGE MODE";
  renderRoles();
}

function wireControls() {
  document.querySelectorAll(".swatch").forEach(button => {
    button.addEventListener("click", () => {
      document.querySelectorAll(".swatch").forEach(item => item.classList.remove("active"));
      button.classList.add("active");
      consoleEl.dataset.accent = button.dataset.accent;
    });
  });

  document.querySelectorAll(".placement button").forEach(button => {
    button.addEventListener("click", () => {
      document.querySelectorAll(".placement button").forEach(item => item.classList.remove("active"));
      button.classList.add("active");
      consoleEl.dataset.placement = button.dataset.placement;
    });
  });

  document.querySelectorAll(".rail-button[data-view]:not(:disabled)").forEach(button => {
    button.addEventListener("click", () => {
      document.querySelectorAll(".rail-button").forEach(item => item.classList.remove("active"));
      button.classList.add("active");
    });
  });

  document.querySelector("#settingsButton").addEventListener("click", () => {
    settingsPanel.hidden = !settingsPanel.hidden;
    document.querySelector("#settingsButton").classList.toggle("active", !settingsPanel.hidden);
  });
  document.querySelector("#settingsCloseButton").addEventListener("click", () => {
    settingsPanel.hidden = true;
    document.querySelector("#settingsButton").classList.remove("active");
  });
  settingsForm.addEventListener("submit", saveConfig);
  document.querySelectorAll("[data-task-action]").forEach(button => {
    button.addEventListener("click", () => {
      const action = button.dataset.taskAction;
      if (action === "retry") postTaskLifecycle("queued", "Task reopened for retry from the web console.");
      if (action === "cancel") postTaskLifecycle("cancelled", "Task cancelled from the web console.");
      if (action === "revoke-autonomy") postAutonomyRevoke();
      if (action === "close") postTaskLifecycle("closed", "Task closed from the web console.");
    });
  });
  document.querySelectorAll("[data-gate-action]").forEach(button => {
    button.addEventListener("click", () => postTaskGate(button.dataset.gateAction));
  });
  /* Sprint 4 artifact decision wiring. */
  document.querySelectorAll("[data-artifact-action]").forEach(button => {
    button.addEventListener("click", () => postTaskArtifact(button.dataset.artifactAction));
  });
  const exportButton = document.querySelector("#artifactExportButton");
  if (exportButton) exportButton.addEventListener("click", exportSelectedArtifact);
  /* Sprint 3 deliverable ship wiring. */
  const shipModeSelect = document.querySelector("#deliverableShipMode");
  if (shipModeSelect) shipModeSelect.addEventListener("change", updateDeliverableShipStripVisibility);
  const shipButton = document.querySelector("#deliverableShipButton");
  if (shipButton) shipButton.addEventListener("click", shipSelectedDeliverable);
  updateDeliverableShipStripVisibility();

  document.querySelectorAll(".mode-button").forEach(button => {
    button.addEventListener("click", () => setDispatchMode(button.dataset.mode));
  });
  requestedByRoleEl.addEventListener("change", () => {
    ensureSelectedRoleAllowed();
    renderRoles();
  });
  document.querySelector("#drawerFab").addEventListener("click", () => {
    consoleEl.dataset.placement = "right";
    document.querySelectorAll(".placement button").forEach(item => item.classList.toggle("active", item.dataset.placement === "right"));
    bodyEl.focus();
  });

  /* Sprint 4 parity wiring: initiative + role filter dropdowns. */
  const initiativeFilter = document.querySelector("#initiativeFilter");
  if (initiativeFilter) {
    initiativeFilter.addEventListener("change", async () => {
      state.selectedInitiativeId = initiativeFilter.value || "";
      await loadTasks();
      await loadInitiatives();
      await loadInitiativeAssets(state.selectedInitiativeId);
      await loadInitiativeDeliverables(state.selectedInitiativeId);
      addAudit("ui", state.selectedInitiativeId ? `filter focused ${state.selectedInitiativeId}` : "cleared initiative filter");
    });
  }
  const roleFilter = document.querySelector("#roleFilter");
  if (roleFilter) {
    /* Populate the role filter dropdown from the canonical role list. */
    roles.forEach(role => {
      const option = document.createElement("option");
      option.value = role.roleId;
      option.textContent = role.name;
      roleFilter.appendChild(option);
    });
    roleFilter.addEventListener("change", async () => {
      state.roleFilter = roleFilter.value || "all";
      await loadTasks();
      addAudit("ui", state.roleFilter === "all" ? "cleared role filter" : `role filter: ${state.roleFilter}`);
    });
  }

  /* Sprint 4 help overlay wiring. */
  const helpButton = document.querySelector("#helpButton");
  const helpOverlay = document.querySelector("#helpOverlay");
  const helpClose = document.querySelector("#helpClose");
  const toggleHelp = (force) => {
    if (!helpOverlay) return;
    const next = typeof force === "boolean" ? !force : !helpOverlay.hidden;
    helpOverlay.hidden = next;
  };
  if (helpButton) helpButton.addEventListener("click", () => toggleHelp());
  if (helpClose) helpClose.addEventListener("click", () => toggleHelp(false));
  if (helpOverlay) {
    helpOverlay.addEventListener("click", event => {
      if (event.target === helpOverlay) toggleHelp(false);
    });
  }

  armButton.addEventListener("click", armComposer);
  form.addEventListener("submit", submitDirective);
  ["tokenBudget", "maxAgents", "timeframeHours", "priority"].forEach(id => {
    document.querySelector(`#${id}`).addEventListener("input", updateReadouts);
  });

  window.addEventListener("keydown", event => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      addAudit("ui", "command palette disabled until task action indexing exists");
    }
    const isTyping = ["INPUT", "TEXTAREA", "SELECT"].includes(document.activeElement.tagName);
    if (!isTyping && event.key.toLowerCase() === "n") {
      bodyEl.focus();
    }
    /* Sprint 4: ? toggles help; Esc closes the overlay or settings panel. */
    if (!isTyping && event.key === "?") {
      event.preventDefault();
      const overlay = document.querySelector("#helpOverlay");
      if (overlay) overlay.hidden = !overlay.hidden;
    }
    if (event.key === "Escape") {
      const overlay = document.querySelector("#helpOverlay");
      if (overlay && !overlay.hidden) {
        overlay.hidden = true;
      } else if (!settingsPanel.hidden) {
        settingsPanel.hidden = true;
        document.querySelector("#settingsButton").classList.remove("active");
      }
    }
  });
}

async function init() {
  consoleEl.dataset.placement = "right";
  setInterval(updateClock, 1000);
  updateClock();
  renderRoles();
  renderPresets();
  renderVendors();
  renderAgentControls();
  drawSparkline();
  updateReadouts();
  wireControls();
  try {
    await loadConfig();
    await loadRecent();
    await loadInitiatives();
    await loadTasks();
    await loadTokens();
    await loadAdapters();
    await loadAgents();
    await loadHosts();
    await loadCapacity();
    await loadStatusBoard();
    connectEvents();
    setInterval(loadInitiatives, 5000);
    setInterval(loadTasks, 5000);
    setInterval(loadTokens, 5000);
    setInterval(loadAdapters, 15000);
    setInterval(loadAgents, 5000);
    setInterval(loadHosts, 7000);
    setInterval(loadCapacity, 5000);
    /* Phase 3 Sprint 1: status board polls /api/status which folds heartbeats,
     * milestones, and policy snapshot into one payload. The 7-second cadence
     * is a deliberate offset from the 5-second poll set so the dashboard
     * doesn't burst-load when all timers align. */
    setInterval(loadStatusBoard, 7000);
  } catch (error) {
    setUplink(false);
    addAudit("system", "room API unavailable");
  }
}

init();
