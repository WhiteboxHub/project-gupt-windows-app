import "./styles.css";
import {
  MessageType,
  decodeFrame,
  decodeMessages,
  encodeMessage,
  encodeConnectRequest,
  encodeConnectResponse,
  encodeKeyboard,
  encodeMouseButton,
  encodeMouseMove,
  encodeMouseWheel,
} from "./protocol.js";

const SIGNALING_HOST = "gupt-signal-server-560359652969.us-central1.run.app";
const RELAY_BASE = `wss://${SIGNALING_HOST}/relay`;
const HTTP_BASE = `https://${SIGNALING_HOST}`;

const launcher = document.querySelector("#launcher");
const viewer = document.querySelector("#viewer");
const canvas = document.querySelector("#screen");
const ctx = canvas.getContext("2d");
const statusEl = document.querySelector("#status");
const streamStatus = document.querySelector("#streamStatus");
const sessionInput = document.querySelector("#sessionInput");
const connectBtn = document.querySelector("#connectBtn");
const startHostBtn = document.querySelector("#startHostBtn");
const clientModeBtn = document.querySelector("#clientMode");
const hostModeBtn = document.querySelector("#hostMode");
const disconnectBtn = document.querySelector("#disconnectBtn");

let mode = "client";
let relay = null;
let remoteWidth = 0;
let remoteHeight = 0;
let drawScale = 1;
let drawX = 0;
let drawY = 0;
let hostCapture = null;
let hostCanvas = null;
let hostTimer = null;

clientModeBtn.addEventListener("click", () => setMode("client"));
hostModeBtn.addEventListener("click", () => setMode("host"));
connectBtn.addEventListener("click", startClient);
startHostBtn.addEventListener("click", startHost);
disconnectBtn.addEventListener("click", disconnect);

window.addEventListener("resize", resizeCanvas);

function setMode(nextMode) {
  mode = nextMode;
  clientModeBtn.classList.toggle("selected", mode === "client");
  hostModeBtn.classList.toggle("selected", mode === "host");
  connectBtn.hidden = mode !== "client";
  startHostBtn.hidden = mode !== "host";
  sessionInput.toggleAttribute("readonly", mode === "host");
  statusEl.textContent = mode === "client" ? "Enter a Windows or Mac host session ID." : "Start host to generate a session ID.";
}

async function startClient() {
  const session = sessionInput.value.trim();
  if (!session) {
    statusEl.textContent = "Enter a session ID.";
    return;
  }

  showViewer();
  streamStatus.textContent = "Connecting...";
  relay = openRelay("client", session);
  relay.addEventListener("open", () => {
    relay.send(encodeConnectRequest(session));
    streamStatus.textContent = "Waiting for host approval...";
  });
  relay.addEventListener("message", handleClientMessage);
  relay.addEventListener("close", () => {
    streamStatus.textContent = "Disconnected";
  });
}

async function startHost() {
  statusEl.textContent = "Registering host...";
  const register = await fetch(`${HTTP_BASE}/register`);
  const { sessionId } = await register.json();
  sessionInput.value = sessionId;

  hostCapture = await navigator.mediaDevices.getDisplayMedia({
    video: { frameRate: 20 },
    audio: false,
  });
  hostCanvas = document.createElement("canvas");

  relay = openRelay("host", sessionId);
  relay.addEventListener("open", () => {
    statusEl.textContent = `Hosting. Session ID: ${sessionId}`;
    startHostStreaming();
  });
  relay.addEventListener("message", handleHostMessage);
  relay.addEventListener("close", () => {
    statusEl.textContent = "Host relay disconnected.";
    stopHostStreaming();
  });
}

function openRelay(role, session) {
  const ws = new WebSocket(`${RELAY_BASE}?role=${encodeURIComponent(role)}&session=${encodeURIComponent(session)}`);
  ws.binaryType = "arraybuffer";
  return ws;
}

function handleClientMessage(event) {
  if (typeof event.data === "string") return;
  for (const msg of decodeMessages(event.data)) {
    if (msg.type === MessageType.ConnectResponse) {
      streamStatus.textContent = msg.payload[0] ? "Connected" : "Denied";
    } else if (msg.type === MessageType.FrameData) {
      renderFrame(msg.payload);
    } else if (msg.type === MessageType.Disconnect) {
      disconnect();
    }
  }
}

function handleHostMessage(event) {
  if (typeof event.data === "string") return;
  for (const msg of decodeMessages(event.data)) {
    if (msg.type === MessageType.ConnectRequest) {
      relay?.send(encodeConnectResponse(true, "OK"));
    } else if (msg.type === MessageType.MouseEvent || msg.type === MessageType.KeyboardEvent) {
      // Native input injection is handled by Tauri commands in the packaged app.
      // The browser preview intentionally does not inject host OS input.
    }
  }
}

async function renderFrame(payload) {
  const frame = decodeFrame(payload);
  if (!frame) return;

  if (frame.totalWidth !== remoteWidth || frame.totalHeight !== remoteHeight) {
    remoteWidth = frame.totalWidth;
    remoteHeight = frame.totalHeight;
    resizeCanvas();
  }

  const blob = new Blob([frame.jpeg], { type: "image/jpeg" });
  const bitmap = await createImageBitmap(blob);
  ctx.drawImage(bitmap, frame.targetX * drawScale + drawX, frame.targetY * drawScale + drawY, frame.updateWidth * drawScale, frame.updateHeight * drawScale);
  bitmap.close();
}

function resizeCanvas() {
  canvas.width = window.innerWidth;
  canvas.height = window.innerHeight;
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  if (!remoteWidth || !remoteHeight) return;
  const scaleX = canvas.width / remoteWidth;
  const scaleY = canvas.height / remoteHeight;
  drawScale = Math.min(scaleX, scaleY);
  drawX = Math.floor((canvas.width - remoteWidth * drawScale) / 2);
  drawY = Math.floor((canvas.height - remoteHeight * drawScale) / 2);
}

function sendInput(bytes) {
  if (relay?.readyState === WebSocket.OPEN) relay.send(bytes);
}

canvas.addEventListener("mousemove", (event) => {
  const pos = normalizedPosition(event);
  if (pos) sendInput(encodeMouseMove(pos.x, pos.y));
});

canvas.addEventListener("mousedown", (event) => {
  const pos = normalizedPosition(event);
  if (pos) sendInput(encodeMouseButton(pos.x, pos.y, event.button === 2 ? 1 : 0, true));
});

canvas.addEventListener("mouseup", (event) => {
  const pos = normalizedPosition(event);
  if (pos) sendInput(encodeMouseButton(pos.x, pos.y, event.button === 2 ? 1 : 0, false));
});

canvas.addEventListener("wheel", (event) => {
  const pos = normalizedPosition(event);
  if (pos) sendInput(encodeMouseWheel(pos.x, pos.y, -Math.trunc(event.deltaY)));
  event.preventDefault();
}, { passive: false });

window.addEventListener("keydown", (event) => {
  if (!viewer.hidden) sendInput(encodeKeyboard(event.keyCode || event.which, true));
});

window.addEventListener("keyup", (event) => {
  if (!viewer.hidden) sendInput(encodeKeyboard(event.keyCode || event.which, false));
});

function normalizedPosition(event) {
  const x = (event.clientX - drawX) / (remoteWidth * drawScale);
  const y = (event.clientY - drawY) / (remoteHeight * drawScale);
  if (x < 0 || x > 1 || y < 0 || y > 1) return null;
  return { x, y };
}

function showViewer() {
  launcher.hidden = true;
  viewer.hidden = false;
  resizeCanvas();
}

function disconnect() {
  stopHostStreaming();
  relay?.close();
  relay = null;
  viewer.hidden = true;
  launcher.hidden = false;
}

function startHostStreaming() {
  const video = document.createElement("video");
  video.srcObject = hostCapture;
  video.muted = true;
  video.play();
  const hostCtx = hostCanvas.getContext("2d");

  hostTimer = window.setInterval(async () => {
    if (!relay || relay.readyState !== WebSocket.OPEN || !video.videoWidth || !video.videoHeight) return;
    hostCanvas.width = video.videoWidth;
    hostCanvas.height = video.videoHeight;
    hostCtx.drawImage(video, 0, 0);
    const blob = await new Promise((resolve) => hostCanvas.toBlob(resolve, "image/jpeg", 0.74));
    if (!blob) return;
    const jpeg = new Uint8Array(await blob.arrayBuffer());
    relay.send(encodeBrowserFrame(video.videoWidth, video.videoHeight, jpeg));
  }, 66);
}

function stopHostStreaming() {
  if (hostTimer) window.clearInterval(hostTimer);
  hostTimer = null;
  hostCapture?.getTracks().forEach((track) => track.stop());
  hostCapture = null;
}

function encodeBrowserFrame(width, height, jpeg) {
  const frameHeaderSize = 41;
  const payload = new Uint8Array(frameHeaderSize + jpeg.byteLength);
  const view = new DataView(payload.buffer);
  view.setUint32(0, 0, true);
  view.setUint32(4, width, true);
  view.setUint32(8, height, true);
  view.setUint32(12, width, true);
  view.setUint32(16, height, true);
  view.setUint32(20, 0, true);
  view.setUint32(24, 0, true);
  view.setUint32(28, 32, true);
  view.setUint8(32, 0);
  view.setBigUint64(33, BigInt(Date.now()), true);
  payload.set(jpeg, frameHeaderSize);
  return encodeMessage(MessageType.FrameData, payload);
}

setMode("client");
