const transcript = document.querySelector("#transcript");
const statusEl = document.querySelector("#status");
const form = document.querySelector("#composer");
const targetEl = document.querySelector("#targetName");
const bodyEl = document.querySelector("#messageBody");
const seen = new Set();

function setStatus(connected) {
  statusEl.textContent = connected ? "connected" : "disconnected";
  statusEl.className = `status ${connected ? "connected" : "disconnected"}`;
}

function renderMessage(message) {
  if (seen.has(message.messageId)) return;
  seen.add(message.messageId);
  const item = document.createElement("article");
  item.className = "message";
  const time = new Date(message.createdAtUnixMs).toISOString().replace(".000", "");
  item.innerHTML = `
    <div class="meta">
      <strong>#${message.messageId}</strong>
      <span>${time}</span>
      <span>${message.senderName} -> ${message.targetName}</span>
      <span>${message.messageType}</span>
    </div>
    <div class="body"></div>
  `;
  item.querySelector(".body").textContent = message.messageBody;
  transcript.appendChild(item);
  transcript.scrollTop = transcript.scrollHeight;
}

async function loadRecent() {
  const response = await fetch("/api/messages?limit=50");
  const messages = await response.json();
  messages.forEach(renderMessage);
}

function connectEvents() {
  const events = new EventSource("/events");
  events.addEventListener("open", () => setStatus(true));
  events.addEventListener("error", () => setStatus(false));
  events.addEventListener("message", event => {
    setStatus(true);
    renderMessage(JSON.parse(event.data));
  });
}

form.addEventListener("submit", async event => {
  event.preventDefault();
  const messageBody = bodyEl.value.trim();
  if (!messageBody) return;
  await fetch("/api/message", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      senderName: "ceo",
      targetName: targetEl.value,
      messageType: "chat",
      messageBody
    })
  });
  bodyEl.value = "";
});

document.querySelectorAll(".quickbar button").forEach(button => {
  button.addEventListener("click", () => {
    targetEl.value = button.dataset.target;
    bodyEl.focus();
  });
});

loadRecent();
connectEvents();
