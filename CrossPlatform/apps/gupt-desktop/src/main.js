import "./styles.css";
import {
  MessageType,
  decodeFrame,
  decodeMessages,
  encodeMessage,
  encodeConnectRequest,
  encodeConnectResponse,
  decodeClipboardText,
  encodeClipboardText,
  encodeKeyboard,
  encodeMouseButton,
  encodeMouseMove,
  encodeMouseWheel,
} from "./protocol.js";

const SIGNALING_HOST = "gupt-signal-server-560359652969.us-central1.run.app";
const RELAY_BASE = `wss://${SIGNALING_HOST}/relay`;
const HTTP_BASE = `https://${SIGNALING_HOST}`;
const FRAME_INTERVAL_MS = 16; // 60 FPS for low latency
const JPEG_QUALITY = 0.9;
const MAX_RECONNECT_ATTEMPTS = 30;
const MAX_RELAY_BUFFER_BYTES = 2_000_000;
const MAX_PENDING_FRAMES = 2;

const statusMsg = document.getElementById("status");
const streamStatus = document.getElementById("streamStatus");
const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });

const fullScreenBtn = document.getElementById("fullScreenBtn");
const exitFullScreenBtn = document.getElementById("exitFullScreenBtn");
const viewModeSelect = document.getElementById("viewModeSelect");
const streamSizeSelect = document.getElementById("streamSizeSelect");
const clipboardSyncToggle = document.getElementById("clipboardSyncToggle");

if (fullScreenBtn && exitFullScreenBtn) {
  fullScreenBtn.addEventListener("click", () => {
    document.documentElement.requestFullscreen().catch((err) => {
      console.warn("Fullscreen error:", err);
    });
  });

  exitFullScreenBtn.addEventListener("click", () => {
    if (document.fullscreenElement) {
      document.exitFullscreen().catch(err => console.warn(err));
    }
  });

  document.addEventListener("fullscreenchange", () => {
    if (document.fullscreenElement) {
      fullScreenBtn.hidden = true;
      exitFullScreenBtn.hidden = false;
    } else {
      fullScreenBtn.hidden = false;
      exitFullScreenBtn.hidden = true;
    }
  });
}

const sidebarToggle = document.getElementById("sidebarToggle");
const sidebar = document.getElementById("sidebar");

if (sidebarToggle && sidebar) {
  sidebarToggle.addEventListener("click", () => {
    sidebar.classList.toggle("open");
  });
}

const launcher = document.querySelector("#launcher");
const viewer = document.querySelector("#viewer");
const statusEl = document.querySelector("#status");
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
let drawScaleX = 1;
let drawScaleY = 1;
let drawX = 0;
let drawY = 0;
let hostCapture = null;
let hostCanvas = null;
let intentionalDisconnect = false;
let reconnectAttempts = 0;
let reconnectTimer = null;
let viewMode = "contain";
let streamScale = 1;
let lastErrorReportAt = 0;
let clipboardSyncEnabled = false;
let lastLocalClipboardText = "";
let lastRemoteClipboardText = "";
let isPollingClipboard = false;

clientModeBtn.addEventListener("click", () => setMode("client"));
hostModeBtn.addEventListener("click", () => setMode("host"));
connectBtn.addEventListener("click", startClient);
startHostBtn.addEventListener("click", startHost);
disconnectBtn.addEventListener("click", disconnect);
viewModeSelect?.addEventListener("change", () => {
  viewMode = viewModeSelect.value;
  resizeCanvas();
});
streamSizeSelect?.addEventListener("change", () => {
  streamScale = Number(streamSizeSelect.value) || 1;
});
clipboardSyncToggle?.addEventListener("change", () => {
  clipboardSyncEnabled = clipboardSyncToggle.checked;
  streamStatus.textContent = clipboardSyncEnabled ? "Clipboard sync on" : "Clipboard sync off";
  if (clipboardSyncEnabled) {
    pollClipboardSync({ reportBlocked: true });
  }
});

window.addEventListener("resize", resizeCanvas);
window.addEventListener("error", (event) => {
  reportError("window-error", event.error || event.message);
});
window.addEventListener("unhandledrejection", (event) => {
  reportError("unhandled-rejection", event.reason);
});

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
  intentionalDisconnect = false;
  connectRelay("client", session);
}

async function startHost() {
  try {
    statusEl.textContent = "Registering host...";
    const register = await fetch(`${HTTP_BASE}/register`);
    const { sessionId } = await register.json();
    sessionInput.value = sessionId;
    intentionalDisconnect = false;

    hostCapture = await navigator.mediaDevices.getDisplayMedia({
      video: {
        cursor: "always",
        frameRate: { ideal: 60, max: 60 },
        width: { ideal: 3840 },
        height: { ideal: 2160 }
      },
      audio: false,
    });
    hostCanvas = typeof OffscreenCanvas !== "undefined"
      ? new OffscreenCanvas(3840, 2160)
      : document.createElement("canvas");

    connectRelay("host", sessionId);
  } catch (err) {
    console.error("Host Error:", err);
    reportError("host-start", err);
    statusEl.textContent = `Host Error: Screen sharing blocked or unsupported (${err.message}). Try opening in Chrome.`;
  }
}

function openRelay(role, session) {
  const ws = new WebSocket(`${RELAY_BASE}?role=${encodeURIComponent(role)}&session=${encodeURIComponent(session)}`);
  ws.binaryType = "arraybuffer";
  return ws;
}

function connectRelay(role, session) {
  clearTimeout(reconnectTimer);
  relay?.close();
  relay = openRelay(role, session);

  relay.addEventListener("open", () => {
    reconnectAttempts = 0;
    if (role === "client") {
      relay.send(encodeConnectRequest(session));
      streamStatus.textContent = "Waiting for host approval...";
    } else {
      statusEl.textContent = `Hosting. Session ID: ${session}`;
      if (!hostActive) startHostStreaming();
    }
  });

  relay.addEventListener("message", role === "client" ? handleClientMessage : handleHostMessage);
  relay.addEventListener("close", () => handleRelayClose(role, session));
  relay.addEventListener("error", () => {
    reportError(`${role}-relay`, `Relay websocket error for session ${session}`);
  });
}

function handleRelayClose(role, session) {
  if (intentionalDisconnect) {
    if (role === "host") stopHostStreaming();
    if (role === "client") streamStatus.textContent = "Disconnected";
    return;
  }

  reconnectAttempts += 1;
  if (reconnectAttempts > MAX_RECONNECT_ATTEMPTS) {
    if (role === "host") {
      statusEl.textContent = "Relay disconnected. Reconnect limit reached.";
    } else {
      streamStatus.textContent = "Disconnected";
    }
    reportError(`${role}-relay`, "Relay disconnected. Reconnect limit reached.");
    return;
  }

  const delay = Math.min(1000 + reconnectAttempts * 500, 6000);
  if (role === "host") {
    statusEl.textContent = `Relay lost. Reconnecting (${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})...`;
  } else {
    streamStatus.textContent = `Reconnecting (${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})...`;
  }
  reconnectTimer = setTimeout(() => connectRelay(role, session), delay);
}

let pendingFrames = [];
let isRendering = false;

async function processFrames() {
  if (isRendering) return;
  isRendering = true;
  
  try {
    let frameCount = 0;
    while (pendingFrames.length > 0) {
      const payload = pendingFrames.shift();
      try {
        await renderFrame(payload);
      } catch (err) {
        console.error("Frame render error:", err);
        reportError("client-render-frame", err);
      }
      
      frameCount++;
      if (frameCount % 5 === 0) {
        await new Promise(r => setTimeout(r, 0)); // Batch yield to eliminate scroll latency
      }
    }
  } finally {
    isRendering = false;
  }
}

function handleClientMessage(event) {
  if (typeof event.data === "string") return;
  for (const msg of decodeMessages(event.data)) {
    if (msg.type === MessageType.ConnectResponse) {
      streamStatus.textContent = msg.payload[0] ? "Connected" : "Denied";
    } else if (msg.type === MessageType.FrameData) {
      if (msg.payload.byteLength >= 41) {
        const view = new DataView(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength);
        const isDelta = view.getUint8(32) !== 0;
        if (!isDelta) {
          pendingFrames = []; // Discard stale full frames to prevent flickering and lag
        } else if (pendingFrames.length >= MAX_PENDING_FRAMES) {
          pendingFrames = pendingFrames.slice(-1);
        }
      }
      pendingFrames.push(msg.payload);
      processFrames().catch(err => console.error("Render loop error:", err));
    } else if (msg.type === MessageType.Disconnect) {
      disconnect();
    } else if (msg.type === MessageType.ClipboardText) {
      receiveClipboardText(msg.payload);
    }
  }
}

function handleHostMessage(event) {
  if (typeof event.data === "string") return;
  for (const msg of decodeMessages(event.data)) {
    if (msg.type === MessageType.ConnectRequest) {
      const allow = confirm("Incoming remote control request. Allow connection?");
      relay?.send(encodeConnectResponse(allow, allow ? "OK" : "Denied"));
    } else if (msg.type === MessageType.MouseEvent) {
      if (window.__TAURI__ && window.__TAURI__.core) {
        const view = new DataView(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength);
        const x = view.getFloat32(0, true);
        const y = view.getFloat32(4, true);
        const button = view.getUint8(8);
        const isDown = view.getUint8(9) !== 0;
        const wheelDelta = view.getInt32(10, true);
        window.__TAURI__.core.invoke("inject_mouse", { x, y, button, isDown, wheelDelta }).catch((err) => {
          console.error(err);
          reportError("host-inject-mouse", err);
        });
      }
    } else if (msg.type === MessageType.KeyboardEvent) {
      if (window.__TAURI__ && window.__TAURI__.core) {
        const view = new DataView(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength);
        const keycode = view.getUint16(0, true);
        const isDown = view.getUint8(2) !== 0;
        window.__TAURI__.core.invoke("inject_keyboard", { keycode, isDown }).catch((err) => {
          console.error(err);
          reportError("host-inject-keyboard", err);
        });
      }
    } else if (msg.type === MessageType.ClipboardText) {
      receiveClipboardText(msg.payload);
    }
  }
}

async function renderFrame(payload) {
  try {
    const frame = decodeFrame(payload);
    if (!frame) return;

    if (frame.totalWidth !== remoteWidth || frame.totalHeight !== remoteHeight) {
      remoteWidth = frame.totalWidth;
      remoteHeight = frame.totalHeight;
      resizeCanvas();
    }

    const blob = new Blob([frame.jpeg], { type: "image/jpeg" });
    const bitmap = await createImageBitmap(blob);
    ctx.drawImage(
      bitmap,
      frame.targetX * drawScaleX + drawX,
      frame.targetY * drawScaleY + drawY,
      frame.updateWidth * drawScaleX,
      frame.updateHeight * drawScaleY
    );
    bitmap.close();
  } catch (err) {
    console.error("RENDER ERROR:", err);
    throw err;
  }
}

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  canvas.width = window.innerWidth * dpr;
  canvas.height = window.innerHeight * dpr;
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  if (!remoteWidth || !remoteHeight) return;
  const scaleX = canvas.width / remoteWidth;
  const scaleY = canvas.height / remoteHeight;
  if (viewMode === "stretch") {
    drawScale = 1;
    drawScaleX = scaleX;
    drawScaleY = scaleY;
    drawX = 0;
    drawY = 0;
    return;
  }
  drawScale = viewMode === "cover" ? Math.max(scaleX, scaleY) : Math.min(scaleX, scaleY);
  drawScaleX = drawScale;
  drawScaleY = drawScale;
  drawX = Math.floor((canvas.width - remoteWidth * drawScale) / 2);
  drawY = Math.floor((canvas.height - remoteHeight * drawScale) / 2);
}

function sendInput(bytes) {
  if (relay?.readyState === WebSocket.OPEN) relay.send(bytes);
}

let lastMouseTime = 0;
canvas.addEventListener("mousemove", (event) => {
  const now = Date.now();
  if (now - lastMouseTime < 16) return;
  lastMouseTime = now;
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

let lastWheelTime = 0;
canvas.addEventListener("wheel", (event) => {
  const now = Date.now();
  if (now - lastWheelTime < 16) return;
  lastWheelTime = now;
  const pos = normalizedPosition(event);
  if (pos) sendInput(encodeMouseWheel(pos.x, pos.y, -Math.trunc(event.deltaY)));
  event.preventDefault();
}, { passive: false });

let isMouseOverScreen = false;
canvas.addEventListener("mouseenter", () => isMouseOverScreen = true);
canvas.addEventListener("mouseleave", () => isMouseOverScreen = false);

window.addEventListener("keydown", (event) => {
  if (!viewer.hidden && isMouseOverScreen) {
    sendInput(encodeKeyboard(event.keyCode || event.which, true));
    event.preventDefault();
  }
});

window.addEventListener("keyup", (event) => {
  if (!viewer.hidden && isMouseOverScreen) {
    sendInput(encodeKeyboard(event.keyCode || event.which, false));
    event.preventDefault();
  }
});

function normalizedPosition(event) {
  const dpr = window.devicePixelRatio || 1;
  const physicalX = event.clientX * dpr;
  const physicalY = event.clientY * dpr;
  const x = (physicalX - drawX) / (remoteWidth * drawScaleX);
  const y = (physicalY - drawY) / (remoteHeight * drawScaleY);
  if (x < 0 || x > 1 || y < 0 || y > 1) return null;
  return { x, y };
}

function showViewer() {
  launcher.hidden = true;
  viewer.hidden = false;
  resizeCanvas();
}

function disconnect() {
  intentionalDisconnect = true;
  clearTimeout(reconnectTimer);
  stopHostStreaming();
  relay?.close();
  relay = null;
  viewer.hidden = true;
  launcher.hidden = false;
}

let hostActive = false;

let currentQuality = JPEG_QUALITY;

async function hostLoop(video, hostCtx) {
  let isEncoding = false;

  const processFrame = async () => {
    if (!hostActive) return;
    
    if (isEncoding) {
      scheduleNext();
      return;
    }

    if (!relay || relay.readyState !== WebSocket.OPEN || !video.videoWidth || !video.videoHeight) {
      scheduleNext();
      return;
    }

    // Adaptive Streaming: Skip encoding entirely if network is completely jammed
    if (relay.bufferedAmount > MAX_RELAY_BUFFER_BYTES) {
      scheduleNext();
      return;
    }

    // Smoother Adaptive Quality
    if (relay.bufferedAmount > 1_000_000) {
      currentQuality = 0.5; // High motion/buffer pressure
    } else if (relay.bufferedAmount > 400_000) {
      currentQuality = 0.8; // Medium pressure
    } else {
      currentQuality = JPEG_QUALITY; // 0.9 - Crystal clear
    }

    isEncoding = true;
    try {
      const targetWidth = Math.max(1, Math.round(video.videoWidth * streamScale));
      const targetHeight = Math.max(1, Math.round(video.videoHeight * streamScale));
      if (hostCanvas.width !== targetWidth || hostCanvas.height !== targetHeight) {
        hostCanvas.width = targetWidth;
        hostCanvas.height = targetHeight;
      }
      hostCtx.drawImage(video, 0, 0, targetWidth, targetHeight);

      let blob;
      if (hostCanvas.convertToBlob) {
        blob = await hostCanvas.convertToBlob({ type: "image/jpeg", quality: currentQuality });
      } else {
        blob = await new Promise((resolve) => hostCanvas.toBlob(resolve, "image/jpeg", currentQuality));
      }
      
      if (blob && hostActive) {
        const jpeg = new Uint8Array(await blob.arrayBuffer());
        relay.send(encodeBrowserFrame(targetWidth, targetHeight, jpeg));
      }
    } catch (e) {
      console.error("Host encode error:", e);
      reportError("host-encode-frame", e);
    } finally {
      isEncoding = false;
      scheduleNext();
    }
  };

  const scheduleNext = () => {
    if (!hostActive) return;
    setTimeout(processFrame, FRAME_INTERVAL_MS);
  };

  scheduleNext();
}

function startHostStreaming() {
  const video = document.createElement("video");
  video.srcObject = hostCapture;
  video.muted = true;
  video.play();
  const hostCtx = hostCanvas.getContext("2d");
  hostCtx.imageSmoothingEnabled = true;
  hostCtx.imageSmoothingQuality = "high";
  
  hostActive = true;
  hostLoop(video, hostCtx);
}

function stopHostStreaming() {
  hostActive = false;
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

function sendClipboardTextValue(text, transferMode = "auto") {
  lastLocalClipboardText = text;
  sendInput(encodeClipboardText(text, transferMode));
}

async function receiveClipboardText(payload, options = {}) {
  try {
    const { text, transferMode } = decodeClipboardText(payload);
    if ((options.automatic || transferMode === "auto") && !clipboardSyncEnabled) return;

    lastRemoteClipboardText = text;
    lastLocalClipboardText = text;
    await navigator.clipboard.writeText(text);
    streamStatus.textContent = transferMode === "auto" ? "Clipboard synced" : "Clipboard received";
  } catch (err) {
    reportError("clipboard-receive", err);
  }
}

async function pollClipboardSync(options = {}) {
  if (!clipboardSyncEnabled || isPollingClipboard || relay?.readyState !== WebSocket.OPEN) return;

  isPollingClipboard = true;
  try {
    const text = await navigator.clipboard.readText();
    if (text !== lastLocalClipboardText && text !== lastRemoteClipboardText) {
      sendClipboardTextValue(text, "auto");
      streamStatus.textContent = "Clipboard synced";
    }
    lastLocalClipboardText = text;
  } catch (err) {
    streamStatus.textContent = "Clipboard permission needed";
    if (options.reportBlocked) {
      reportError("clipboard-sync-poll", err);
    }
  } finally {
    isPollingClipboard = false;
  }
}

async function reportError(context, err) {
  const now = Date.now();
  if (now - lastErrorReportAt < 1000) return;
  lastErrorReportAt = now;

  const message = errorMessage(err);
  const screenshotDataUrl = await captureErrorScreenshot().catch(() => null);
  if (window.__TAURI__?.core) {
    window.__TAURI__.core.invoke("save_error_report", {
      role: mode,
      context,
      message,
      screenshotDataUrl,
    }).catch((saveErr) => console.error("Error report save failed:", saveErr));
  } else {
    console.error(`[${context}] ${message}`);
  }
}

async function captureErrorScreenshot() {
  if (!viewer.hidden && canvas.width > 0 && canvas.height > 0) {
    return canvas.toDataURL("image/png");
  }
  if (hostCanvas?.convertToBlob) {
    const blob = await hostCanvas.convertToBlob({ type: "image/png" });
    return blobToDataUrl(blob);
  }
  if (hostCanvas?.toDataURL) {
    return hostCanvas.toDataURL("image/png");
  }
  return null;
}

function blobToDataUrl(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result);
    reader.onerror = () => reject(reader.error);
    reader.readAsDataURL(blob);
  });
}

function errorMessage(err) {
  if (err instanceof Error) return `${err.name}: ${err.message}\n${err.stack || ""}`;
  if (typeof err === "string") return err;
  try {
    return JSON.stringify(err);
  } catch {
    return String(err);
  }
}

setMode("client");

setInterval(() => {
  if (relay?.readyState === WebSocket.OPEN) {
    sendInput(encodeMessage(MessageType.Heartbeat));
  }
}, 2000);

setInterval(() => {
  pollClipboardSync();
}, 1000);
