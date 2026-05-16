const consoleEl = document.querySelector(".console");
const transcript = document.querySelector("#transcript");
const uplinkBadge = document.querySelector("#uplinkBadge");
const form = document.querySelector("#composer");
const bodyEl = document.querySelector("#messageBody");
const initiativeNameEl = document.querySelector("#initiativeName");
const auditLog = document.querySelector("#auditLog");
const clockEl = document.querySelector("#clock");
const modalBackdrop = document.querySelector("#modalBackdrop");
const modalTitle = document.querySelector("#modalTitle");
const modalBody = document.querySelector("#modalBody");
const modalClose = document.querySelector("#modalClose");
const armButton = document.querySelector("#armButton");
const sendButton = document.querySelector("#sendButton");
const taskTableBody = document.querySelector("#taskTableBody");
const seen = new Set();

const roles = [
  ["pgm", "Program Manager", "PGM", 200],
  ["pm", "Project Manager", "PM", 215],
  ["swar", "Software Architect", "SWAR", 260],
  ["sysar", "Systems Architect", "SYSAR", 275],
  ["fe", "Frontend Developer", "FE", 165],
  ["be", "Backend Developer", "BE", 145],
  ["dbe", "Database Engineer", "DBE", 125],
  ["dba", "Database Admin", "DBA", 110],
  ["sa", "System Admin", "SA", 60],
  ["neta", "Network Admin", "NETA", 45],
  ["depl", "Deployment Engineer", "DEPL", 30],
  ["perf", "Performance Engineer", "PERF", 350],
  ["test", "Tester", "TEST", 320],
  ["revu", "Code Reviewer", "REVU", 295],
  ["intg", "Integration Specialist", "INTG", 240],
  ["wrtr", "Technical Writer", "WRTR", 185],
  ["mock", "Mockup Artist", "MOCK", 15],
  ["gfx", "Graphic Artist", "GFX", 335]
].map(([id, name, short, hue]) => ({id, name, short, hue}));

const roleById = Object.fromEntries(roles.map(role => [role.id, role]));

const presets = [
  {id: "scout", name: "Scout", tokens: 500000, agents: 3, hours: 2, priority: "low"},
  {id: "build", name: "Build", tokens: 2000000, agents: 6, hours: 8, priority: "normal"},
  {id: "fleet", name: "Fleet", tokens: 5000000, agents: 14, hours: 24, priority: "high"},
  {id: "warroom", name: "WarRoom", tokens: 10000000, agents: 24, hours: 72, priority: "critical"}
];

let state = {
  selectedRole: "pm",
  selectedPreset: "build",
  armed: false,
  armTimer: null,
  audit: []
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
  rows.forEach(row => {
    if (!row.taskId) return;
    if (row.schema === "woventeam.task_package.v0.1") {
      byId.set(row.taskId, {
        taskId: row.taskId,
        initiativeId: row.initiativeId || "init_phase0",
        status: row.status || "queued",
        assignedAgent: row.assignedAgent || "all",
        assignedRole: row.assignedRole || "",
        title: row.title || (row.task && row.task.title) || "Untitled task"
      });
    } else if (row.schema === "woventeam.task_event.v0.1" && byId.has(row.taskId)) {
      const task = byId.get(row.taskId);
      task.status = row.status || task.status;
      task.assignedAgent = row.assignedAgent || task.assignedAgent;
    }
  });
  return Array.from(byId.values()).reverse();
}

function renderTasks(tasks) {
  document.querySelector("#initiativeCount").textContent = String(tasks.length);
  document.querySelector("#initiativeBadge").textContent = String(tasks.length);
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
      <td>--</td>
      <td>--</td>
      <td><span class="vendor-tag"></span></td>
    `;
    row.querySelector("td:nth-child(1)").textContent = task.taskId;
    row.querySelector(".status-pill").textContent = task.status;
    row.querySelector(".status-pill").dataset.status = task.status;
    row.querySelector("td:nth-child(3)").textContent = task.title;
    row.querySelector("td:nth-child(4)").textContent = task.assignedRole || "--";
    row.querySelector("td:nth-child(5)").textContent = task.assignedAgent || "all";
    row.querySelector(".mini-progress span").style.width = task.status === "complete" ? "100%" : task.status === "running" ? "55%" : "12%";
    row.querySelector(".vendor-tag").textContent = task.assignedAgent || "router";
    taskTableBody.appendChild(row);
  });
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

  const activeCounts = {pm: 1, be: 1, test: 0, revu: 0, wrtr: 0, sysar: 0, dba: 0, neta: 0, fe: 0, depl: 0, sa: 0, pgm: 0, swar: 0, perf: 0, intg: 0, mock: 0, gfx: 0, dbe: 0};
  roles.slice(0, 12).forEach(role => {
    const chip = document.createElement("div");
    chip.className = "role-chip";
    chip.style.setProperty("--role-hue", role.hue);
    chip.innerHTML = `<i></i><span>${role.short}</span><b>${activeCounts[role.id] || 0}</b>`;
    roleGrid.appendChild(chip);
  });

  roles.forEach(role => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `role-button ${role.id === state.selectedRole ? "active" : ""}`;
    button.style.setProperty("--role-hue", role.hue);
    button.title = role.name;
    button.innerHTML = `<i></i><span>${role.short}</span>`;
    button.addEventListener("click", () => {
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
    {name: "Codex CLI", cli: "codex", state: "launchable", quota: 41},
    {name: "Gemini CLI", cli: "gemini", state: "launchable", quota: 22},
    {name: "Task Runner", cli: "adapter", state: "coming soon", quota: 0, warn: true}
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
  document.querySelector("#vendorMeta").textContent = "● 3 OK · ● 1 PENDING";
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

function openModal(title, body) {
  modalTitle.textContent = title;
  modalBody.textContent = body;
  modalBackdrop.hidden = false;
}

function closeModal() {
  modalBackdrop.hidden = true;
}

function closeModalFromPointer(event) {
  event.preventDefault();
  event.stopPropagation();
  closeModal();
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
  await fetch("/api/message", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      senderName: "ceo",
      targetName: "all",
      messageType: "directive",
      messageBody: buildDirective()
    })
  });
  bodyEl.value = "";
  initiativeNameEl.value = "";
  disarmComposer();
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

  document.querySelectorAll(".rail-button[data-view]").forEach(button => {
    button.addEventListener("click", () => {
      document.querySelectorAll(".rail-button").forEach(item => item.classList.remove("active"));
      button.classList.add("active");
      if (button.dataset.view !== "initiatives") {
        openModal(`${button.title} coming soon`, "This navigation destination is scaffolded. It will render once the matching backend endpoint and state model exist.");
      }
    });
  });

  document.querySelector("#paletteButton").addEventListener("click", () => {
    openModal("Command palette coming soon", "The command palette needs indexed agents, initiatives, review gates, and task actions.");
  });
  document.querySelector("#helpButton").addEventListener("click", () => {
    openModal("Keyboard shortcuts", "Ctrl+K opens the future command palette. N opens the composer drawer. Escape closes dialogs.");
  });
  document.querySelector("#settingsButton").addEventListener("click", () => {
    openModal("Settings coming soon", "Vendor CLI settings and runtime homes will connect after adapter execution is implemented.");
  });
  document.querySelector("#drawerFab").addEventListener("click", () => {
    consoleEl.dataset.placement = "right";
    document.querySelectorAll(".placement button").forEach(item => item.classList.toggle("active", item.dataset.placement === "right"));
    bodyEl.focus();
  });
  modalClose.addEventListener("click", closeModalFromPointer);
  modalClose.addEventListener("pointerup", closeModalFromPointer);
  modalClose.addEventListener("touchend", closeModalFromPointer, {passive: false});
  modalBackdrop.addEventListener("click", event => {
    if (event.target === modalBackdrop) closeModal();
  });
  document.addEventListener("click", event => {
    if (event.target instanceof Element && event.target.closest("#modalClose")) {
      closeModalFromPointer(event);
    }
  });

  armButton.addEventListener("click", armComposer);
  form.addEventListener("submit", submitDirective);
  ["tokenBudget", "maxAgents", "timeframeHours", "priority"].forEach(id => {
    document.querySelector(`#${id}`).addEventListener("input", updateReadouts);
  });

  window.addEventListener("keydown", event => {
    if (event.key === "Escape") closeModal();
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      openModal("Command palette coming soon", "Search and command execution require the task index and agent registry backend.");
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
