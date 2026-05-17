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
  taskStats: {roleCounts: {}, requestCount: 0, blockedCount: 0, activeAgents: 3}
};

function fmtTokens(value) {
  if (value >= 1000000) return `${(value / 1000000).toFixed(1)}M`;
  if (value >= 1000) return `${(value / 1000).toFixed(1)}k`;
  return String(value);
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
  const response = await fetch("/api/tasks");
  if (!response.ok) return;
  const rows = await response.json();
  const tasks = summarizeTasks(rows);
  renderTasks(tasks);
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
    row.querySelector("td:nth-child(8)").textContent = task.dependencies.length ? task.dependencies.join(", ") : "--";
    row.querySelector("td:nth-child(9)").textContent = task.title;
    taskTableBody.appendChild(row);
  });
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
  document.querySelector("#tokenReadout").textContent = fmtTokens(tokens);
  document.querySelector("#agentReadout").textContent = String(agents);
  document.querySelector("#hoursReadout").textContent = `${hours}h`;
  document.querySelector("#burnEstimate").textContent = `$${(tokens / 100000).toFixed(2)}`;
  document.querySelector("#subAgentSummary").textContent = `0 / ${agents}`;
  document.querySelector("#prioritySummary").textContent = priority;
}

function renderVendors() {
  const vendors = [
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
  document.querySelector("#vendorMeta").textContent = "● 4 LIVE · ● 1 DISABLED";
}

function drawSparkline() {
  const svg = document.querySelector("#tokenSparkline");
  const points = Array.from({length: 34}, (_, i) => {
    const y = 28 - Math.sin(i / 3) * 7 - Math.cos(i / 5) * 5;
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
  return role.agent || "all";
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
  const taskId = newTaskId();
  return {
    schema: "woventeam.task_package.v0.1",
    taskId,
    initiativeId: newInitiativeId(),
    createdBy: "ceo",
    assignedRole: role.roleId,
    assignedAgent: roleAgent(role),
    modelId: "openai/gpt-5.3-codex",
    priority: priority === "critical" ? "urgent" : priority,
    status: "queued",
    title: name,
    body,
    task: {title: name, body, deliverables: []},
    contextRefs: [],
    acceptanceCriteria: ["Task result is recorded in the room and task ledger."],
    toolPolicy: toolPolicy("observe"),
    budget: {timeoutSeconds: 1800, maxOutputBytes: 1048576, maxCostUsd: 1.0},
    dependencies: [],
    createdAtUnixMs: nowMs()
  };
}

function buildTaskRequest() {
  const role = roleById[state.selectedRole];
  const title = initiativeNameEl.value.trim() || "Untitled subtask";
  const body = bodyEl.value.trim();
  const priority = document.querySelector("#priority").value;
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
    modelId: "openai/gpt-5.3-codex",
    priority: priority === "critical" ? "urgent" : priority,
    title,
    body,
    toolPolicy: {profile: "observe"},
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
    await loadRecent();
    await loadTasks();
    connectEvents();
    setInterval(loadTasks, 5000);
  } catch (error) {
    setUplink(false);
    addAudit("system", "room API unavailable");
  }
}

init();
