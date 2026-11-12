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

const fullScreenBtn = document.getElementById("fullScreenBtn");
const exitFullScreenBtn = document.getElementById("exitFullScreenBtn");
const viewModeSelect = document.getElementById("viewModeSelect");
const streamSizeSelect = document.getElementById("streamSizeSelect");
const clipboardSyncToggle = document.getElementById("clipboardSyncToggle");
const freezeBanner = document.getElementById("freezeBanner");
const freezeBannerText = document.getElementById("freezeBannerText");
const bannerReconnectBtn = document.getElementById("bannerReconnectBtn");
const refreshStreamBtn = document.getElementById("refreshStreamBtn");

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

  activeSessionId = session;
  lastFrameReceivedAt = Date.now();
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
  if (typeof event.data === "string") return;
  for (const msg of decodeMessages(event.data)) {
    if (msg.type === MessageType.ConnectResponse) {
      streamStatus.textContent = msg.payload[0] ? "Connected" : "Denied";
      lastFrameReceivedAt = Date.now();
      if (freezeBanner) freezeBanner.hidden = true;
    } else if (msg.type === MessageType.FrameData) {
      lastFrameReceivedAt = Date.now();
      if (freezeBanner && !freezeBanner.hidden) freezeBanner.hidden = true;

      const isDelta = msg.payload.byteLength >= 41
        ? (new DataView(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength).getUint8(32) !== 0)
        : false;

      if (!isDelta) {
        pendingFrames = [msg.payload];
      } else {
        if (pendingFrames.length > 8) {
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
        processMouseInjection({ x, y, button, isDown, wheelDelta });
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

let metricsFrameCount = 0;
let metricsLastTime = performance.now();
let metricsBytes = 0;
let metricsFps = 0;
let metricsLatency = 0;
let metricsBitrate = "0.0";

function updateMetricsDisplay() {
  if (viewer.hidden || !streamStatus) return;
  const latencyStr = metricsLatency > 0 && metricsLatency < 5000 ? `${metricsLatency}ms` : "<35ms";
  streamStatus.textContent = `Connected (${metricsFps} FPS • ${latencyStr} • ${metricsBitrate} Mbps)`;
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
  const newW = Math.floor(window.innerWidth * dpr);
  const newH = Math.floor(window.innerHeight * dpr);

  if (canvas.width !== newW || canvas.height !== newH) {
    if (canvas.width > 0 && canvas.height > 0) {
      const tempCanvas = document.createElement("canvas");
      tempCanvas.width = canvas.width;
      tempCanvas.height = canvas.height;
      const tempCtx = tempCanvas.getContext("2d");
      tempCtx.drawImage(canvas, 0, 0);

      canvas.width = newW;
      canvas.height = newH;
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(tempCanvas, 0, 0, newW, newH);
    } else {
      canvas.width = newW;
      canvas.height = newH;
      ctx.fillStyle = "#000";
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = "high";
    }
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
  const now = performance.now();
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
  const x = (canvasX - drawX) / (remoteWidth * drawScaleX);
  const y = (canvasY - drawY) / (remoteHeight * drawScaleY);
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
  if (viewer.hidden || relay?.readyState !== WebSocket.OPEN) {
    if (freezeBanner) freezeBanner.hidden = true;
    return;
  }
  const now = Date.now();
  if (lastFrameReceivedAt > 0 && now - lastFrameReceivedAt > 2200) {
    const elapsedSec = Math.round((now - lastFrameReceivedAt) / 1000);
    if (freezeBanner) {
      if (freezeBannerText) {
        freezeBannerText.textContent = `Connection stalled — no video received for ${elapsedSec}s`;
      }
      freezeBanner.hidden = false;
    }
    if (streamStatus) {
      streamStatus.textContent = `Stalled (${elapsedSec}s)`;
    }
  } else {
    if (freezeBanner) freezeBanner.hidden = true;
  }
}, 1000);
