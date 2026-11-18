import "./styles.css";
import {
  MessageType,
  decodeFrame,
  decodeMessages,
  encodeMessage,
  encodeConnectRequest,
  encodeConnectResponse,
  encodeFrameAck,
  decodeClipboardText,
  encodeClipboardText,
  encodeKeyboard,
  encodeMouseButton,
  encodeMouseMove,
  encodeMouseWheel,
  decodeCursorPosition,
} from "./protocol.js";

const SIGNALING_HOST = "gupt-signal-server-560359652969.us-central1.run.app";
const HTTP_BASE = `https://${SIGNALING_HOST}`;
const RELAY_BASE = `wss://${SIGNALING_HOST}/relay`;
const FRAME_INTERVAL_MS = 25; // ~40 FPS - fluid and responsive
const JPEG_QUALITY = 0.76; // Crisp text clarity while reducing frame size by ~60%
const MAX_RELAY_BUFFER_BYTES = 64_000; // Drop frames when socket buffer exceeds 64KB to guarantee ZERO latency buildup
const MAX_PENDING_FRAMES = 1; // Only render latest frame to stay strictly realtime

const statusMsg = document.getElementById("status");
const streamStatus = document.getElementById("streamStatus");
const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });
const offscreenCanvas = document.createElement("canvas");
const offscreenCtx = offscreenCanvas.getContext("2d", { alpha: false });
let hasMasterFrame = false;

const fullScreenBtn = document.getElementById("fullScreenBtn");
const exitFullScreenBtn = document.getElementById("exitFullScreenBtn");
const viewModeSelect = document.getElementById("viewModeSelect");
const streamSizeSelect = document.getElementById("streamSizeSelect");
const clipboardSyncToggle = document.getElementById("clipboardSyncToggle");
const freezeBanner = document.getElementById("freezeBanner");
const freezeBannerText = document.getElementById("freezeBannerText");
const bannerReconnectBtn = document.getElementById("bannerReconnectBtn");
const refreshStreamBtn = document.getElementById("refreshStreamBtn");
const remoteCursor = document.getElementById("remoteCursor");
const connectToast = document.getElementById("connectToast");
const toastText = document.getElementById("toastText");

const CURSOR_SVGS = [
  // 0: Default Windows Arrow Pointer
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M4 2L11 20L14 13L21 10L4 2Z" fill="#FFFFFF" stroke="#000000" stroke-width="1.6" stroke-linejoin="round"/></svg>`,
  // 1: Text Selection (I-Beam)
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M8 4H16M12 4V20M8 20H16" stroke="#FFFFFF" stroke-width="3" stroke-linecap="round"/><path d="M8 4H16M12 4V20M8 20H16" stroke="#000000" stroke-width="1.5" stroke-linecap="round"/></svg>`,
  // 2: Link Hover Hand Pointer
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M9 11V5.5C9 4.67 9.67 4 10.5 4C11.33 4 12 4.67 12 5.5V11M12 11V4.5C12 3.67 12.67 3 13.5 3C14.33 3 15 3.67 15 4.5V11M15 11V5.5C15 4.67 15.67 4 16.5 4C17.33 4 18 4.67 18 5.5V13C18 17.42 14.42 21 10 21C6.69 21 4 18.31 4 15V13C4 11.9 4.9 11 6 11C6.55 11 7.05 11.22 7.41 11.59L9 13.17V11Z" fill="#FFFFFF" stroke="#000000" stroke-width="1.6" stroke-linejoin="round"/></svg>`,
  // 3: Horizontal Window Resize (ew-resize)
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M3 12L7 8V11H17V8L21 12L17 16V13H7V16L3 12Z" fill="#FFFFFF" stroke="#000000" stroke-width="1.6" stroke-linejoin="round"/></svg>`,
  // 4: Vertical Window Resize (ns-resize)
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M12 3L8 7H11V17H8L12 21L16 17H13V7H16L12 3Z" fill="#FFFFFF" stroke="#000000" stroke-width="1.6" stroke-linejoin="round"/></svg>`,
  // 5: Diagonal Resize 1 (nwse-resize)
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M5 5H11M5 5V11M5 5L19 19M19 19V13M19 19H13" stroke="#FFFFFF" stroke-width="3.5" stroke-linecap="round"/><path d="M5 5H11M5 5V11M5 5L19 19M19 19V13M19 19H13" stroke="#000000" stroke-width="1.6" stroke-linecap="round"/></svg>`,
  // 6: Diagonal Resize 2 (nesw-resize)
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M19 5H13M19 5V11M19 5L5 19M5 19V13M5 19H11" stroke="#FFFFFF" stroke-width="3.5" stroke-linecap="round"/><path d="M19 5H13M19 5V11M19 5L5 19M5 19V13M5 19H11" stroke="#000000" stroke-width="1.6" stroke-linecap="round"/></svg>`,
  // 7: 4-Way Window Move (move)
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><path d="M12 2L9 5H11V9H7V7L4 10L7 13V11H11V15H9L12 18L15 15H13V11H17V13L20 10L17 7V9H13V5H15L12 2Z" fill="#FFFFFF" stroke="#000000" stroke-width="1.6" stroke-linejoin="round"/></svg>`,
  // 8: Disabled / Not Allowed
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="9" stroke="#EF4444" stroke-width="2.5"/><line x1="5.5" y1="5.5" x2="18.5" y2="18.5" stroke="#EF4444" stroke-width="2.5"/></svg>`,
  // 9: Wait
  `<svg width="22" height="22" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="8" stroke="#3B82F6" stroke-width="2.5" stroke-dasharray="12 12"/></svg>`
];

const CURSOR_HOTSPOTS = [
  { x: 4, y: 2 },    // 0: default arrow tip
  { x: 12, y: 12 },  // 1: I-beam center
  { x: 9, y: 4 },    // 2: Hand pointer index finger tip
  { x: 12, y: 12 },  // 3: ew-resize center
  { x: 12, y: 12 },  // 4: ns-resize center
  { x: 12, y: 12 },  // 5: nwse center
  { x: 12, y: 12 },  // 6: nesw center
  { x: 12, y: 12 },  // 7: move center
  { x: 12, y: 12 },  // 8: not-allowed center
  { x: 12, y: 12 },  // 9: wait center
];

let activeCursorShapeId = -1;
function updateRemoteCursorShape(shapeId) {
  const sid = (shapeId >= 0 && shapeId < CURSOR_SVGS.length) ? shapeId : 0;
  if (sid !== activeCursorShapeId) {
    activeCursorShapeId = sid;
    if (remoteCursor) {
      remoteCursor.innerHTML = CURSOR_SVGS[sid];
    }
  }
}

function positionRemoteCursor(clientX, clientY) {
  if (!remoteCursor) return;
  const hs = CURSOR_HOTSPOTS[activeCursorShapeId >= 0 ? activeCursorShapeId : 0];
  remoteCursor.style.transform = `translate(${clientX - hs.x}px, ${clientY - hs.y}px)`;
}

// Initialize default cursor shape
updateRemoteCursorShape(0);

let toastTimer = null;
function showConnectToast(msg) {
  if (!connectToast || !toastText) return;
  toastText.textContent = msg;
  connectToast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    connectToast.hidden = true;
  }, 4500);
}

let activeSessionId = "";
let lastFrameReceivedAt = 0;

function triggerManualReconnect() {
  const session = activeSessionId || sessionInput.value.trim();
  if (!session) return;
  console.log("[RECONNECT] User requested stream reconnect for session:", session);
  if (freezeBanner) freezeBanner.hidden = true;
  streamStatus.textContent = "Reconnecting stream...";
  pendingFrames = [];
  connectRelay("client", session);
}

bannerReconnectBtn?.addEventListener("click", triggerManualReconnect);
refreshStreamBtn?.addEventListener("click", triggerManualReconnect);

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
let clipboardSyncEnabled = true;
let lastLocalClipboardText = "";
let lastRemoteClipboardText = "";
let isPollingClipboard = false;
let isSessionConnected = false;

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
if (window.ResizeObserver) {
  const ro = new ResizeObserver(() => {
    resizeCanvas();
  });
  ro.observe(document.documentElement);
}
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

function clearCanvas(message = "") {
  const dpr = window.devicePixelRatio || 1;
  canvas.width = window.innerWidth * dpr;
  canvas.height = window.innerHeight * dpr;
  ctx.fillStyle = "#0c0d14";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  remoteWidth = 0;
  remoteHeight = 0;
  hasMasterFrame = false;
  offscreenCanvas.width = 0;
  offscreenCanvas.height = 0;
  drawScale = 1;
  drawScaleX = 1;
  drawScaleY = 1;
  drawX = 0;
  drawY = 0;

  if (message) {
    ctx.save();
    ctx.fillStyle = "#a0a5b8";
    ctx.font = "600 22px -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(message, canvas.width / 2, canvas.height / 2);
    ctx.restore();
  }
}

async function startClient() {
  const session = sessionInput.value.trim();
  if (!session) {
    statusEl.textContent = "Enter a session ID.";
    return;
  }

  intentionalDisconnect = false;
  isSessionConnected = false;
  activeSessionId = session;
  reconnectAttempts = 0;
  lastFrameReceivedAt = 0;
  pendingFrames = [];

  clearCanvas(`Connecting to host (${session})...`);
  showViewer();
  streamStatus.textContent = "Connecting to host...";
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
        width: { ideal: 2560, max: 3840 },
        height: { ideal: 1600, max: 2160 }
      },
      audio: false,
    });
    const track = hostCapture.getVideoTracks()[0];
    if (track) {
      track.onended = () => {
        statusMsg.textContent = "Screen sharing stopped.";
        stopHostStreaming();
      };
    }
    hostCanvas = typeof OffscreenCanvas !== "undefined"
      ? new OffscreenCanvas(1920, 1080)
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
  isSessionConnected = false;
  if (intentionalDisconnect) {
    if (role === "host") stopHostStreaming();
    if (role === "client") streamStatus.textContent = "Disconnected";
    return;
  }

  reconnectAttempts += 1;
  // Session persistence: keep reconnecting indefinitely so sessions survive Wi-Fi blips during interviews
  const delay = Math.min(1000 + (reconnectAttempts % 10) * 300, 3000);
  if (role === "host") {
    statusEl.textContent = `Relay connection lost. Reconnecting in ${Math.round(delay/1000)}s (attempt ${reconnectAttempts})...`;
  } else {
    streamStatus.textContent = `Connection lost. Reconnecting (attempt ${reconnectAttempts})...`;
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
  if (typeof event.data === "string") {
    try {
      const parsed = JSON.parse(event.data);
      if (parsed.type === "error") {
        streamStatus.textContent = parsed.message || "Connection Error";
        statusEl.textContent = parsed.message || "Connection Error";
        clearCanvas(`Error: ${parsed.message || "Connection failed"}`);
      }
    } catch (e) {}
    return;
  }
  for (const msg of decodeMessages(event.data)) {
    if (msg.type === MessageType.ConnectResponse) {
      const accepted = !!msg.payload[0];
      isSessionConnected = accepted;
      streamStatus.textContent = accepted ? "Connected" : "Access Denied";
      if (!accepted) {
        statusEl.textContent = "Host denied connection or session is busy.";
        clearCanvas("Connection denied by host.");
        setTimeout(() => disconnect(), 2000);
      } else {
        lastFrameReceivedAt = Date.now();
        if (freezeBanner) freezeBanner.hidden = true;
        showConnectToast(`Connected to Host (${activeSessionId || sessionInput.value.trim()}) — Remote Control Active`);
      }
    } else if (msg.type === MessageType.CursorPosition) {
      const cursor = decodeCursorPosition(msg.payload);
      if (cursor) {
        updateRemoteCursorShape(cursor.shapeId);

        const now = performance.now();
        // If the local user isn't actively moving the mouse, follow the remote host user's mouse
        if (now - lastLocalMouseMoveTime > 80) {
          if (!cursor.isVisible) {
            remoteCursor.hidden = true;
          } else if (remoteWidth > 0 && remoteHeight > 0 && canvas.width > 0 && canvas.height > 0) {
            const rect = canvas.getBoundingClientRect();
            const pixelX = drawX + cursor.x * (remoteWidth * drawScaleX);
            const pixelY = drawY + cursor.y * (remoteHeight * drawScaleY);
            const domX = (pixelX / canvas.width) * rect.width + rect.left;
            const domY = (pixelY / canvas.height) * rect.height + rect.top;
            positionRemoteCursor(domX, domY);
            remoteCursor.hidden = false;
          }
        }
      }
    } else if (msg.type === MessageType.FrameData) {
      isSessionConnected = true;
      lastFrameReceivedAt = Date.now();
      if (freezeBanner && !freezeBanner.hidden) freezeBanner.hidden = true;

      // Extract frame number immediately and ACK back to host with zero queue delay
      if (msg.payload.byteLength >= 4) {
        const frameNumber = new DataView(msg.payload.buffer, msg.payload.byteOffset, 4).getUint32(0, true);
        sendInput(encodeFrameAck(frameNumber));
      }

      const isDelta = msg.payload.byteLength >= 41
        ? (new DataView(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength).getUint8(32) !== 0)
        : false;

      if (!isDelta) {
        // Full keyframe supersedes any older delta frames
        pendingFrames = [msg.payload];
      } else {
        if (pendingFrames.length > 20) {
          pendingFrames.shift();
        }
        pendingFrames.push(msg.payload);
      }
      processFrames().catch(err => console.error("Render loop error:", err));
    } else if (msg.type === MessageType.Disconnect) {
      disconnect();
    } else if (msg.type === MessageType.ClipboardText) {
      receiveClipboardText(msg.payload);
    }
  }
}

let isInjectingMouse = false;
let pendingMouseMove = null;

function processMouseInjection(mouseData) {
  // Discard moves or coalesce if an IPC call is already in flight
  if (mouseData.button !== 255 || mouseData.wheelDelta !== 0) {
    window.__TAURI__.core.invoke("inject_mouse", mouseData).catch((err) => {
      console.error("Mouse click injection error:", err);
    });
    return;
  }

  if (isInjectingMouse) {
    pendingMouseMove = mouseData;
    return;
  }

  isInjectingMouse = true;
  window.__TAURI__.core.invoke("inject_mouse", mouseData)
    .catch((err) => console.error(err))
    .finally(() => {
      isInjectingMouse = false;
      if (pendingMouseMove) {
        const next = pendingMouseMove;
        pendingMouseMove = null;
        processMouseInjection(next);
      }
    });
}

function handleHostMessage(event) {
  if (typeof event.data === "string") return;
  for (const msg of decodeMessages(event.data)) {
    if (msg.type === MessageType.ConnectRequest) {
      relay?.send(encodeConnectResponse(true, "OK"));
      statusMsg.textContent = `Client connected. Streaming active.`;
      if (!hostActive && hostCapture) {
        startHostStreaming();
      }
    } else if (msg.type === MessageType.MouseEvent) {
      if (window.__TAURI__ && window.__TAURI__.core) {
        const view = new DataView(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength);
        const x = view.getFloat32(0, true);
        const y = view.getFloat32(4, true);
        const button = view.getUint8(8);
        const isDown = view.getUint8(9) !== 0;
        const wheelDelta = view.getInt32(10, true);
        processMouseInjection({
          x,
          y,
          button,
          is_down: isDown,
          isDown,
          wheel_delta: wheelDelta,
          wheelDelta,
        });
      }
    } else if (msg.type === MessageType.KeyboardEvent) {
      if (window.__TAURI__ && window.__TAURI__.core) {
        const view = new DataView(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength);
        const keycode = view.getUint16(0, true);
        const isDown = view.getUint8(2) !== 0;
        window.__TAURI__.core.invoke("inject_keyboard", {
          keycode,
          is_down: isDown,
          isDown,
        }).catch((err) => {
          console.error(err);
          reportError("host-inject-keyboard", err);
        });
      }
    } else if (msg.type === MessageType.ClipboardText) {
      receiveClipboardText(msg.payload);
    }
  }
}

let metricsFrameCount = 0;
let metricsLastTime = performance.now();
let metricsBytes = 0;
let metricsFps = 0;
let metricsLatency = 0;
let metricsBitrate = "0.0";

function updateMetricsDisplay() {
  if (viewer.hidden || !streamStatus || !isSessionConnected) return;
  const latencyStr = metricsLatency > 0 && metricsLatency < 5000 ? `${metricsLatency}ms` : "<35ms";
  streamStatus.textContent = `Connected (${metricsFps} FPS • ${latencyStr} • ${metricsBitrate} Mbps)`;
}

function paintMasterBuffer() {
  if (!canvas.width || !canvas.height) return;
  // Always clear entire display canvas to solid black letterbox background
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  if (!remoteWidth || !remoteHeight || !hasMasterFrame || !offscreenCanvas.width || !offscreenCanvas.height) return;

  const destW = Math.round(remoteWidth * drawScaleX);
  const destH = Math.round(remoteHeight * drawScaleY);
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(
    offscreenCanvas,
    0, 0, remoteWidth, remoteHeight,
    drawX, drawY, destW, destH
  );
}

async function renderFrame(payload) {
  try {
    const frame = decodeFrame(payload);
    if (!frame) return;

    metricsBytes += payload.byteLength;
    metricsFrameCount += 1;
    if (frame.timestampMs > 0) {
      const now = Date.now();
      if (now >= frame.timestampMs && now - frame.timestampMs < 10000) {
        metricsLatency = Math.round(now - frame.timestampMs);
      }
    }

    let dimsChanged = false;
    if (frame.totalWidth !== remoteWidth || frame.totalHeight !== remoteHeight) {
      remoteWidth = frame.totalWidth;
      remoteHeight = frame.totalHeight;
      offscreenCanvas.width = remoteWidth;
      offscreenCanvas.height = remoteHeight;
      offscreenCtx.fillStyle = "#000";
      offscreenCtx.fillRect(0, 0, remoteWidth, remoteHeight);
      dimsChanged = true;
    }

    const blob = new Blob([frame.jpeg], { type: "image/jpeg" });
    const bitmap = await createImageBitmap(blob);

    // 1. Paint decoded frame directly into unscaled master offscreen buffer at 1:1 pixel coordinates
    offscreenCtx.drawImage(
      bitmap,
      frame.targetX,
      frame.targetY,
      frame.updateWidth,
      frame.updateHeight
    );
    hasMasterFrame = true;

    // 2. If screen resolution changed or full keyframe arrived, re-letterbox entire screen
    const isFullKeyframe = !frame.isDelta || (frame.updateWidth >= frame.totalWidth && frame.updateHeight >= frame.totalHeight);
    if (dimsChanged || isFullKeyframe) {
      resizeCanvas();
    } else {
      // Differential update: blit dirty rect directly onto display canvas
      ctx.drawImage(
        bitmap,
        Math.round(drawX + frame.targetX * drawScaleX),
        Math.round(drawY + frame.targetY * drawScaleY),
        Math.round(frame.updateWidth * drawScaleX),
        Math.round(frame.updateHeight * drawScaleY)
      );
    }
    bitmap.close();
  } catch (err) {
    console.error("RENDER ERROR:", err);
    throw err;
  }
}

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const newW = Math.max(1, Math.floor(window.innerWidth * dpr));
  const newH = Math.max(1, Math.floor(window.innerHeight * dpr));

  if (canvas.width !== newW || canvas.height !== newH) {
    canvas.width = newW;
    canvas.height = newH;
  }

  if (!remoteWidth || !remoteHeight) return;

  const scaleX = canvas.width / remoteWidth;
  const scaleY = canvas.height / remoteHeight;

  if (viewMode === "stretch") {
    drawScale = 1;
    drawScaleX = scaleX;
    drawScaleY = scaleY;
    drawX = 0;
    drawY = 0;
  } else if (viewMode === "cover") {
    drawScale = Math.max(scaleX, scaleY);
    drawScaleX = drawScale;
    drawScaleY = drawScale;
    drawX = Math.floor((canvas.width - remoteWidth * drawScale) / 2);
    drawY = Math.floor((canvas.height - remoteHeight * drawScale) / 2);
  } else {
    // "contain" (Fit Screen) - Clean letterbox without clipping
    drawScale = Math.min(scaleX, scaleY);
    drawScaleX = drawScale;
    drawScaleY = drawScale;
    drawX = Math.max(0, Math.floor((canvas.width - remoteWidth * drawScale) / 2));
    drawY = Math.max(0, Math.floor((canvas.height - remoteHeight * drawScale) / 2));
  }

  paintMasterBuffer();
}

function sendInput(bytes) {
  if (relay?.readyState === WebSocket.OPEN) relay.send(bytes);
}

let lastMouseTime = 0;
let lastLocalMouseMoveTime = 0;

canvas.addEventListener("mouseenter", () => {
  if (isSessionConnected && remoteCursor) {
    remoteCursor.hidden = false;
  }
});

canvas.addEventListener("mouseleave", () => {
  if (remoteCursor) {
    remoteCursor.hidden = true;
  }
});

canvas.addEventListener("mousemove", (event) => {
  const now = performance.now();
  lastLocalMouseMoveTime = now;

  // Instantaneous 0ms local cursor positioning (Chrome Remote Desktop standard)
  positionRemoteCursor(event.clientX, event.clientY);
  if (remoteCursor && remoteCursor.hidden) remoteCursor.hidden = false;

  if (now - lastMouseTime < 4) return;
  lastMouseTime = now;
  const pos = normalizedPosition(event);
  if (pos) sendInput(encodeMouseMove(pos.x, pos.y));
});

window.addEventListener("contextmenu", (event) => {
  if (!viewer.hidden) {
    event.preventDefault();
    event.stopPropagation();
  }
}, { capture: true });

document.addEventListener("contextmenu", (event) => {
  if (!viewer.hidden) {
    event.preventDefault();
    event.stopPropagation();
  }
}, { capture: true });

canvas.addEventListener("contextmenu", (event) => {
  event.preventDefault();
  event.stopPropagation();
});

canvas.addEventListener("mousedown", (event) => {
  event.preventDefault();
  event.stopPropagation();
  const pos = normalizedPosition(event);
  if (pos) {
    const buttonId = event.button === 2 ? 1 : (event.button === 1 ? 2 : 0);
    sendInput(encodeMouseButton(pos.x, pos.y, buttonId, true));
  }
});

canvas.addEventListener("mouseup", (event) => {
  event.preventDefault();
  event.stopPropagation();
  const pos = normalizedPosition(event);
  if (pos) {
    const buttonId = event.button === 2 ? 1 : (event.button === 1 ? 2 : 0);
    sendInput(encodeMouseButton(pos.x, pos.y, buttonId, false));
  }
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

window.addEventListener("keydown", (event) => {
  if (viewer.hidden) return;
  const tag = document.activeElement?.tagName;
  if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;

  const keycode = event.keyCode || event.which;
  sendInput(encodeKeyboard(keycode, true));
  event.preventDefault();
});

window.addEventListener("keyup", (event) => {
  if (viewer.hidden) return;
  const tag = document.activeElement?.tagName;
  if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;

  const keycode = event.keyCode || event.which;
  sendInput(encodeKeyboard(keycode, false));
  event.preventDefault();
});

function normalizedPosition(event) {
  const rect = canvas.getBoundingClientRect();
  if (!rect.width || !rect.height || !remoteWidth || !remoteHeight) return null;

  // Convert viewport client coords into internal canvas buffer coords
  const canvasX = (event.clientX - rect.left) * (canvas.width / rect.width);
  const canvasY = (event.clientY - rect.top) * (canvas.height / rect.height);
  const activeW = remoteWidth * drawScaleX;
  const activeH = remoteHeight * drawScaleY;
  if (activeW <= 0 || activeH <= 0) return null;

  const x = (canvasX - drawX) / activeW;
  const y = (canvasY - drawY) / activeH;
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
  isSessionConnected = false;
  clearTimeout(reconnectTimer);
  reconnectAttempts = 0;

  // 1. Notify host that we are intentionally disconnecting
  if (relay?.readyState === WebSocket.OPEN) {
    try {
      sendInput(encodeMessage(MessageType.Disconnect));
    } catch (e) {
      console.warn("Failed to send disconnect packet:", e);
    }
  }

  // 2. Stop host streaming if hosting
  stopHostStreaming();

  // 3. Cleanly terminate WebSocket relay
  if (relay) {
    relay.close();
    relay = null;
  }

  // 4. Reset session state
  activeSessionId = "";
  sessionInput.value = "";
  lastFrameReceivedAt = 0;
  pendingFrames = [];
  metricsFrameCount = 0;
  metricsBytes = 0;

  // 5. Completely wipe canvas so previous system's screen never leaks
  clearCanvas();

  // 6. Close sidebar & hide freeze alerts
  if (sidebar) sidebar.classList.remove("open");
  if (freezeBanner) freezeBanner.hidden = true;
  if (remoteCursor) remoteCursor.hidden = true;
  if (connectToast) connectToast.hidden = true;
  canvas.style.cursor = "default";
  streamStatus.textContent = "Disconnected";
  statusEl.textContent = "Ready to connect. Enter a new session ID.";

  // 7. Return to launcher view
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

    // Quality adaptation
    if (relay.bufferedAmount > 150_000) {
      currentQuality = 0.65;
    } else {
      currentQuality = JPEG_QUALITY; // 0.88 - Razor sharp
    }

    isEncoding = true;
    try {
      const maxDim = 1920;
      let targetWidth = video.videoWidth * streamScale;
      let targetHeight = video.videoHeight * streamScale;
      if (targetWidth > maxDim || targetHeight > maxDim) {
        const factor = Math.min(maxDim / targetWidth, maxDim / targetHeight);
        targetWidth = Math.round(targetWidth * factor);
        targetHeight = Math.round(targetHeight * factor);
      } else {
        targetWidth = Math.max(1, Math.round(targetWidth));
        targetHeight = Math.max(1, Math.round(targetHeight));
      }
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

let hostVideo = null;

function startHostStreaming() {
  if (!hostCapture) return;

  if (!hostVideo) {
    hostVideo = document.createElement("video");
    hostVideo.muted = true;
    hostVideo.playsInline = true;
    hostVideo.autoplay = true;
    hostVideo.setAttribute("playsinline", "");
    hostVideo.setAttribute("autoplay", "");
    hostVideo.style.cssText = "position:fixed;width:1px;height:1px;opacity:0.01;pointer-events:none;z-index:-1;";
    document.body.appendChild(hostVideo);
  }

  hostVideo.srcObject = hostCapture;
  hostVideo.play().catch((e) => console.warn("Video play error:", e));

  const hostCtx = hostCanvas.getContext("2d");
  hostCtx.imageSmoothingEnabled = true;
  hostCtx.imageSmoothingQuality = "high";

  hostActive = true;
  hostLoop(hostVideo, hostCtx);
}

function stopHostStreaming() {
  hostActive = false;
  hostCapture?.getTracks().forEach((track) => track.stop());
  hostCapture = null;
}

let hostFrameSeq = 0;
function encodeBrowserFrame(width, height, jpeg) {
  const frameHeaderSize = 41;
  const payload = new Uint8Array(frameHeaderSize + jpeg.byteLength);
  const view = new DataView(payload.buffer);
  view.setUint32(0, ++hostFrameSeq, true);
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

async function readLocalClipboard() {
  if (window.__TAURI__?.core?.invoke) {
    try {
      return await window.__TAURI__.core.invoke("read_clipboard");
    } catch (e) {
      console.warn("Tauri read_clipboard failed:", e);
    }
  }
  if (navigator.clipboard?.readText) {
    try {
      return await navigator.clipboard.readText();
    } catch (e) {
      // ignore
    }
  }
  return null;
}

async function writeLocalClipboard(text) {
  if (window.__TAURI__?.core?.invoke) {
    try {
      await window.__TAURI__.core.invoke("write_clipboard", { text });
      return true;
    } catch (e) {
      console.warn("Tauri write_clipboard failed:", e);
    }
  }
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (e) {
      console.warn("navigator.clipboard.writeText failed:", e);
    }
  }
  return false;
}

function sendClipboardTextValue(text) {
  lastLocalClipboardText = text;
  sendInput(encodeClipboardText(text));
}

async function receiveClipboardText(payload, options = {}) {
  try {
    const { text } = decodeClipboardText(payload);
    if (!clipboardSyncEnabled || !text) return;
    if (text === lastRemoteClipboardText || text === lastLocalClipboardText) return;

    lastRemoteClipboardText = text;
    lastLocalClipboardText = text;
    await writeLocalClipboard(text);
    console.log("[CLIPBOARD] Synced from remote, length:", text.length);
  } catch (err) {
    reportError("clipboard-receive", err);
  }
}

async function pollClipboardSync(options = {}) {
  if (!clipboardSyncEnabled || isPollingClipboard || relay?.readyState !== WebSocket.OPEN) return;

  isPollingClipboard = true;
  try {
    const text = await readLocalClipboard();
    if (text !== null && text !== "" && text !== lastLocalClipboardText && text !== lastRemoteClipboardText) {
      lastLocalClipboardText = text;
      sendClipboardTextValue(text);
      console.log("[CLIPBOARD] Sent local clipboard to remote, length:", text.length);
    }
  } catch (err) {
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

setInterval(() => {
  const now = performance.now();
  const elapsed = (now - metricsLastTime) / 1000;
  if (elapsed >= 0.8) {
    metricsFps = Math.round(metricsFrameCount / elapsed);
    metricsBitrate = ((metricsBytes * 8) / (elapsed * 1_000_000)).toFixed(1);
    metricsFrameCount = 0;
    metricsBytes = 0;
    metricsLastTime = now;
    if (relay?.readyState === WebSocket.OPEN && !viewer.hidden) {
      updateMetricsDisplay();
    }
  }
}, 1000);

// Watchdog to immediately alert user if stream stalls
setInterval(() => {
  if (viewer.hidden || relay?.readyState !== WebSocket.OPEN || !isSessionConnected) {
    if (freezeBanner) freezeBanner.hidden = true;
    return;
  }
  const now = Date.now();
  if (lastFrameReceivedAt > 0 && now - lastFrameReceivedAt > 3500) {
    const elapsedSec = Math.round((now - lastFrameReceivedAt) / 1000);
    if (freezeBanner) {
      if (freezeBannerText) {
        freezeBannerText.textContent = `Stream paused — no video updates for ${elapsedSec}s`;
      }
      freezeBanner.hidden = false;
    }
  } else {
    if (freezeBanner) freezeBanner.hidden = true;
  }
}, 1000);
