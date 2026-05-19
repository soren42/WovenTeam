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
    geminiCommand: document.querySelector("#configGeminiCommand").value.trim() || "gemini"
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
    state: adapter.enabled ? adapter.state : "disabled",
    quota: adapter.enabled ? 70 : 0,
    warn: !adapter.enabled
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
  document.querySelector("#agentMeta").textContent = `● ${state.taskStats.activeAgents} ACTIVE · ${state.taskStats.requestCount}R · ${active} CAPACITY`;
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
  document.querySelector("#initiativeCount").textContent = String(tasks.length);
  document.querySelector("#initiativeBadge").textContent = String(tasks.length);
  document.querySelector("#requestCount").textContent = String(state.taskStats.requestCount);
  document.querySelector("#blockedCount").textContent = String(state.taskStats.blockedCount);
  document.querySelector("#agentCount").textContent = String(state.taskStats.activeAgents);
  document.querySelector("#agentBadge").textContent = String(state.taskStats.activeAgents);
  document.querySelector("#agentMeta").textContent = `● ${state.taskStats.activeAgents} ACTIVE · ${state.taskStats.requestCount}R · ${state.taskStats.blockedCount} BLOCKED`;
  renderRoles();
  if (!tasks.length) {
    taskTableBody.innerHTML = '<tr><td colspan="9">No task packages yet. Use the composer or wt-task to create one.</td></tr>';
    return;
  }
  taskTableBody.innerHTML = "";
  tasks.slice(0, 12).forEach(task => {
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
  document.querySelector("#detailTitle").textContent = detail.task.title || taskId;
  document.querySelector("#detailMeta").textContent = `${detail.task.taskId} · ${detail.task.status} · ${detail.task.assignedAgent || "all"} · ${fmtTokens(detail.task.maxTokens || 0)} tokens`;
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
  document.querySelector("#budgetPressureNote").textContent = overBudget ?
    "Budget hard stop: this package would exceed the configured 24h or 30d allocation budget." :
    `Budget pressure: ${fmtTokens(projectedDay)} projected in 24h, ${fmtTokens(projectedMonth)} projected in 30d.`;
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
  const taskId = newTaskId();
  return {
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
}

function buildTaskRequest() {
  const role = roleById[state.selectedRole];
  const title = initiativeNameEl.value.trim() || "Untitled subtask";
  const body = bodyEl.value.trim();
  const priority = document.querySelector("#priority").value;
  const maxTokens = Number(document.querySelector("#tokenBudget").value);
  const requestedTaskId = newTaskId();
  return {
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
    addAudit("ui", `${state.dispatchMode} dispatch failed`);
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
      if (action === "close") postTaskLifecycle("closed", "Task closed from the web console.");
    });
  });
  document.querySelectorAll("[data-gate-action]").forEach(button => {
    button.addEventListener("click", () => postTaskGate(button.dataset.gateAction));
  });

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
    if (!["INPUT", "TEXTAREA", "SELECT"].includes(document.activeElement.tagName) && event.key.toLowerCase() === "n") {
      bodyEl.focus();
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
  drawSparkline();
  updateReadouts();
  wireControls();
  try {
    await loadConfig();
    await loadRecent();
    await loadTasks();
    await loadTokens();
    await loadAdapters();
    await loadCapacity();
    connectEvents();
    setInterval(loadTasks, 5000);
    setInterval(loadTokens, 5000);
    setInterval(loadAdapters, 15000);
    setInterval(loadCapacity, 5000);
  } catch (error) {
    setUplink(false);
    addAudit("system", "room API unavailable");
  }
}

init();
