const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');
const http = require('http');
const url = require('url');

const port = process.env.PORT || 8080;

// Store active sessions: sessionId -> { hostWs, clientWs, hostIp }
const sessions = new Map();

// Store relay sessions: sessionId -> { hostWs, clientWs }
const relaySessions = new Map();

// --- HTTP Server for native C++ clients (Easy fetch) ---
const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    
    // CORS headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Content-Type', 'application/json');

    const forwarded = req.headers['x-forwarded-for'];
    const clientIp = forwarded ? forwarded.split(',')[0] : req.socket.remoteAddress;

    if (parsedUrl.pathname === '/health' || parsedUrl.pathname === '/') {
        res.writeHead(200);
        res.end(JSON.stringify({ status: 'healthy', version: '1.2.0' }));
        return;
    }

    if (parsedUrl.pathname === '/register') {
        const hIp = clientIp;
        const sessionId = uuidv4().substring(0, 8);
        sessions.set(sessionId, { hostWs: null, clientWs: null, hostIp: hIp });
        console.log(`[HTTP REGISTER] Remote: ${clientIp} | ID: ${sessionId}`);
        res.writeHead(200);
        res.end(JSON.stringify({ sessionId }));
        return;
    }

    if (parsedUrl.pathname === '/view') {
        const sessionId = parsedUrl.query.session || '';
        const pin = parsedUrl.query.pin || '';
        res.setHeader('Content-Type', 'text/html; charset=utf-8');
        res.writeHead(200);
        res.end(`<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gupt Remote Viewer</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #121218; color: #eee; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; overflow: hidden; height: 100vh; display: flex; flex-direction: column; }
  #topbar { height: 46px; background: #1c1c28; border-bottom: 1px solid #2e2e42; display: flex; align-items: center; justify-content: space-between; padding: 0 16px; user-select: none; }
  .brand { font-weight: 700; font-size: 15px; color: #a29bfe; display: flex; align-items: center; gap: 8px; }
  .pill { font-size: 12px; padding: 3px 10px; border-radius: 12px; background: #2a2a3e; color: #a0a0c0; }
  .pill.online { background: #1b4332; color: #74c69d; }
  .btn { background: #2f2f45; color: #fff; border: 1px solid #434363; border-radius: 6px; padding: 6px 12px; font-size: 12px; cursor: pointer; transition: 0.15s; }
  .btn:hover { background: #434363; }
  .btn.danger { background: #991b1b; border-color: #b91c1c; }
  #stage { flex: 1; position: relative; display: flex; align-items: center; justify-content: center; background: #000; overflow: hidden; }
  canvas { max-width: 100%; max-height: 100%; object-fit: contain; cursor: default; }
  #uac-banner { position: absolute; top: 12px; left: 50%; transform: translateX(-50%); background: #d97706; color: #fff; padding: 6px 18px; border-radius: 20px; font-size: 13px; font-weight: 600; box-shadow: 0 4px 14px rgba(0,0,0,0.6); display: none; z-index: 100; }
  #auth-modal { position: fixed; inset: 0; background: rgba(0,0,0,0.85); display: flex; align-items: center; justify-content: center; z-index: 1000; }
  .card { background: #1c1c28; border: 1px solid #323248; border-radius: 12px; padding: 28px; width: 360px; text-align: center; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
  .card h2 { font-size: 18px; margin-bottom: 8px; color: #fff; }
  .card p { font-size: 13px; color: #9ca3af; margin-bottom: 20px; }
  .card input { width: 100%; padding: 12px; border-radius: 8px; border: 1px solid #3b3b54; background: #12121a; color: #fff; font-size: 18px; text-align: center; letter-spacing: 3px; margin-bottom: 16px; outline: none; }
  .card input:focus { border-color: #6366f1; }
  .card button { width: 100%; padding: 12px; border-radius: 8px; border: none; background: #6366f1; color: #fff; font-size: 14px; font-weight: 600; cursor: pointer; transition: 0.15s; }
  .card button:hover { background: #4f46e5; }
</style>
</head>
<body>
<div id="topbar">
  <div class="brand">Gupt Remote Viewer <span id="status-pill" class="pill">Connecting...</span></div>
  <div style="display:flex; gap:8px; align-items:center;">
    <button class="btn" id="btn-fullscreen">Fullscreen</button>
    <button class="btn" id="btn-refresh">Refresh (F5)</button>
    <button class="btn" id="btn-display">Switch Screen</button>
    <button class="btn danger" id="btn-disconnect">Disconnect</button>
  </div>
</div>
<div id="stage">
  <div id="uac-banner">⚠️ Host is interacting with a Windows UAC / Secure Desktop prompt (Input paused)</div>
  <canvas id="screen" tabindex="0"></canvas>
</div>
<div id="auth-modal" style="display:none;">
  <div class="card">
    <h2>Host Security PIN Required</h2>
    <p>Enter the 6-digit PIN displayed on the remote host:</p>
    <input type="text" id="pin-input" maxlength="6" placeholder="••••••" autofocus>
    <button id="btn-submit-pin">Connect to Host</button>
  </div>
</div>

<script>
(() => {
  const params = new URLSearchParams(window.location.search);
  const sessionId = params.get('session') || '';
  let authPin = params.get('pin') || '';

  const canvas = document.getElementById('screen');
  const ctx = canvas.getContext('2d');
  const statusPill = document.getElementById('status-pill');
  const uacBanner = document.getElementById('uac-banner');
  const authModal = document.getElementById('auth-modal');
  const pinInput = document.getElementById('pin-input');
  const btnSubmitPin = document.getElementById('btn-submit-pin');

  let ws = null;
  let audioCtx = null;
  let activeMonitorIndex = 0;
  let monitorCount = 1;

  function setStatus(text, type) {
    statusPill.textContent = text;
    statusPill.className = 'pill' + (type ? ' ' + type : '');
  }

  function connect() {
    if (!sessionId) {
      setStatus('Missing Session ID in URL (?session=...)', 'warning');
      return;
    }
    const proto = (window.location.protocol === 'https:') ? 'wss:' : 'ws:';
    const wsUrl = \`\${proto}//\${window.location.host}/relay?role=client&session=\${sessionId}\`;
    setStatus('Connecting to Relay...', '');

    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
      setStatus('Waiting for Host approval...', '');
      sendConnectRequest(sessionId, authPin);
    };

    ws.onmessage = async (e) => {
      if (!(e.data instanceof ArrayBuffer)) return;
      parseAndHandleMessages(e.data);
    };

    ws.onclose = () => {
      setStatus('Disconnected', '');
    };

    ws.onerror = (err) => {
      console.error('[Gupt WS Error]', err);
      setStatus('Connection Failed', 'warning');
    };
  }

  function sendConnectRequest(sessId, pin) {
    const buf = new ArrayBuffer(5 + 96);
    const view = new DataView(buf);
    view.setUint8(0, 1); // ConnectRequest
    view.setUint32(1, 96, true);
    const enc = new TextEncoder();
    const u8 = new Uint8Array(buf);
    u8.set(enc.encode(sessId).subarray(0, 31), 5);
    u8.set(enc.encode(pin).subarray(0, 63), 5 + 32);
    ws.send(buf);
  }

  function sendFrameAck(fn) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const buf = new ArrayBuffer(5 + 4);
    const v = new DataView(buf);
    v.setUint8(0, 11); // FrameAck
    v.setUint32(1, 4, true);
    v.setUint32(5, fn, true);
    ws.send(buf);
  }

  function sendMouseEvent(nx, ny, btnId, isDown, wheelDelta = 0) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const buf = new ArrayBuffer(5 + 14);
    const v = new DataView(buf);
    v.setUint8(0, 4); // MouseEvent
    v.setUint32(1, 14, true);
    v.setFloat32(5, nx, true);
    v.setFloat32(9, ny, true);
    v.setUint8(13, btnId);
    v.setUint8(14, isDown ? 1 : 0);
    v.setInt32(15, wheelDelta, true);
    ws.send(buf);
  }

  function sendKeyEvent(vk, isDown) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const buf = new ArrayBuffer(5 + 3);
    const v = new DataView(buf);
    v.setUint8(0, 5); // KeyboardEvent
    v.setUint32(1, 3, true);
    v.setUint16(5, vk, true);
    v.setUint8(7, isDown ? 1 : 0);
    ws.send(buf);
  }

  function parseAndHandleMessages(ab) {
    const view = new DataView(ab);
    let offset = 0;
    while (offset + 5 <= ab.byteLength) {
      const type = view.getUint8(offset);
      const payloadSize = view.getUint32(offset + 1, true);
      if (offset + 5 + payloadSize > ab.byteLength) break;
      const payload = ab.slice(offset + 5, offset + 5 + payloadSize);
      handleMessage(type, payload);
      offset += 5 + payloadSize;
    }
  }

  async function handleMessage(type, payload) {
    if (type === 2) { // ConnectResponse
      const v = new DataView(payload);
      const accepted = v.getUint8(0) !== 0;
      if (accepted) {
        setStatus('Connected (Live 60 FPS)', 'online');
        authModal.style.display = 'none';
        // Send initial refresh
        const refBuf = new ArrayBuffer(6);
        new DataView(refBuf).setUint8(0, 10);
        new DataView(refBuf).setUint32(1, 1, true);
        new DataView(refBuf).setUint8(5, 1);
        ws.send(refBuf);
      } else {
        setStatus('PIN Denied by Host', 'warning');
        authModal.style.display = 'flex';
      }
    } else if (type === 3) { // FrameData
      if (payload.byteLength < 37) return;
      const hView = new DataView(payload, 0, 37);
      const fn = hView.getUint32(0, true);
      const tw = hView.getUint32(4, true);
      const th = hView.getUint32(8, true);
      const uw = hView.getUint32(12, true);
      const uh = hView.getUint32(16, true);
      const tx = hView.getUint32(20, true);
      const ty = hView.getUint32(24, true);
      const jpeg = payload.slice(37);

      const blob = new Blob([jpeg], { type: 'image/jpeg' });
      createImageBitmap(blob).then(bmp => {
        if (canvas.width !== tw || canvas.height !== th) {
          canvas.width = tw;
          canvas.height = th;
        }
        ctx.drawImage(bmp, tx, ty, uw, uh);
        bmp.close();
        sendFrameAck(fn);
      }).catch(() => {});
    } else if (type === 12) { // AudioData
      if (payload.byteLength < 12) return;
      const aView = new DataView(payload, 0, 12);
      const sampleRate = aView.getUint32(0, true);
      const channels = aView.getUint16(4, true);
      const bitsPerSample = aView.getUint16(6, true);
      const dataSize = aView.getUint32(8, true);
      if (payload.byteLength >= 12 + dataSize) {
        playPcm(payload.slice(12, 12 + dataSize), sampleRate, channels);
      }
    } else if (type === 15) { // SecureDesktopNotification
      if (payload.byteLength >= 1) {
        const uac = new DataView(payload).getUint8(0) !== 0;
        uacBanner.style.display = uac ? 'block' : 'none';
      }
    } else if (type === 18) { // MonitorInfo
      if (payload.byteLength >= 8) {
        const mv = new DataView(payload);
        activeMonitorIndex = mv.getUint32(0, true);
        monitorCount = Math.max(1, mv.getUint32(4, true));
        document.getElementById('btn-display').textContent = monitorCount > 1
          ? \`Screen \${activeMonitorIndex + 1}/\${monitorCount}\`
          : \`Screen \${activeMonitorIndex + 1}\`;
      }
    }
  }

  function playPcm(buf, sampleRate, channels) {
    if (!audioCtx) {
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      audioCtx = new AudioContextClass({ sampleRate: sampleRate });
    }
    if (audioCtx.state === 'suspended') {
      audioCtx.resume();
    }
    const samples = new Int16Array(buf);
    const frameCount = samples.length / channels;
    const audioBuf = audioCtx.createBuffer(channels, frameCount, sampleRate);
    for (let c = 0; c < channels; ++c) {
      const chData = audioBuf.getChannelData(c);
      for (let i = 0; i < frameCount; ++i) {
        chData[i] = samples[i * channels + c] / 32768.0;
      }
    }
    const src = audioCtx.createBufferSource();
    src.buffer = audioBuf;
    src.connect(audioCtx.destination);
    src.start();
  }

  window.addEventListener('click', () => { if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume(); });
  window.addEventListener('keydown', () => { if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume(); });

  // --- Input Interception ---
  function getCanvasCoords(e) {
    const rect = canvas.getBoundingClientRect();
    const x = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    const y = Math.max(0, Math.min(1, (e.clientY - rect.top) / rect.height));
    return { x, y };
  }

  canvas.addEventListener('mousemove', (e) => {
    const { x, y } = getCanvasCoords(e);
    sendMouseEvent(x, y, 255, (e.buttons & 1) !== 0);
  });

  canvas.addEventListener('mousedown', (e) => {
    canvas.focus();
    const { x, y } = getCanvasCoords(e);
    const btn = e.button === 1 ? 2 : (e.button === 2 ? 1 : 0);
    sendMouseEvent(x, y, btn, true);
    e.preventDefault();
  });

  canvas.addEventListener('mouseup', (e) => {
    const { x, y } = getCanvasCoords(e);
    const btn = e.button === 1 ? 2 : (e.button === 2 ? 1 : 0);
    sendMouseEvent(x, y, btn, false);
    e.preventDefault();
  });

  window.addEventListener('blur', () => {
    [16, 17, 18, 91].forEach((vk) => sendKeyEvent(vk, false));
  });

  canvas.addEventListener('contextmenu', (e) => e.preventDefault());

  canvas.addEventListener('wheel', (e) => {
    const { x, y } = getCanvasCoords(e);
    const delta = e.deltaY < 0 ? 120 : -120;
    sendMouseEvent(x, y, 255, false, delta);
    e.preventDefault();
  }, { passive: false });

  window.addEventListener('keydown', (e) => {
    if (document.activeElement === canvas) {
      sendKeyEvent(e.keyCode, true);
      if (e.keyCode === 9 || e.keyCode === 116 || e.keyCode === 122) e.preventDefault(); // Tab, F5, F11
    }
  });

  window.addEventListener('keyup', (e) => {
    if (document.activeElement === canvas) {
      sendKeyEvent(e.keyCode, false);
    }
  });

  // UI Buttons
  document.getElementById('btn-fullscreen').onclick = () => {
    if (!document.fullscreenElement) document.documentElement.requestFullscreen();
    else document.exitFullscreen();
  };

  document.getElementById('btn-refresh').onclick = () => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      const refBuf = new ArrayBuffer(6);
      new DataView(refBuf).setUint8(0, 10);
      new DataView(refBuf).setUint32(1, 1, true);
      new DataView(refBuf).setUint8(5, 1);
      ws.send(refBuf);
    }
  };

  document.getElementById('btn-display').onclick = () => {
    activeMonitorIndex = (monitorCount > 1) ? ((activeMonitorIndex + 1) % monitorCount) : 0;
    document.getElementById('btn-display').textContent = monitorCount > 1
      ? \`Screen \${activeMonitorIndex + 1}/\${monitorCount}\`
      : \`Screen \${activeMonitorIndex + 1}\`;
    if (ws && ws.readyState === WebSocket.OPEN) {
      const monBuf = new ArrayBuffer(5 + 4);
      const v = new DataView(monBuf);
      v.setUint8(0, 14); // SwitchMonitor
      v.setUint32(1, 4, true);
      v.setUint32(5, activeMonitorIndex, true);
      ws.send(monBuf);
    }
  };

  document.getElementById('btn-disconnect').onclick = () => {
    if (ws) ws.close();
  };

  btnSubmitPin.onclick = () => {
    authPin = pinInput.value.trim();
    if (authPin.length > 0) {
      authModal.style.display = 'none';
      if (ws && ws.readyState === WebSocket.OPEN) {
        sendConnectRequest(sessionId, authPin);
      } else {
        connect();
      }
    }
  };

  pinInput.addEventListener('keypress', (e) => {
    if (e.key === 'Enter') btnSubmitPin.click();
  });

  connect();
})();
</script>
</body>
</html>`);
        return;
    }

    if (parsedUrl.pathname === '/join') {
        const sessionId = parsedUrl.query.id;
        console.log(`[HTTP JOIN] Client from ${clientIp} requesting Session ID: '${sessionId}'`);
        const session = sessions.get(sessionId);
        if (!session) {
            console.log(`[HTTP JOIN ERROR] Invalid Session ID: '${sessionId}'`);
            res.writeHead(404);
            res.end(JSON.stringify({ error: 'Invalid Session ID' }));
            return;
        }
        console.log(`[HTTP JOIN SUCCESS] Host IP: ${session.hostIp}`);
        res.writeHead(200);
        res.end(JSON.stringify({ hostIp: session.hostIp }));
        return;
    }

    res.writeHead(404);
    res.end('Not found');
});

// --- WebSocket Servers (manual upgrade to support path routing) ---
const signalingWss = new WebSocket.Server({ noServer: true, maxPayload: 1024 * 1024 });
const relayWss     = new WebSocket.Server({ noServer: true, maxPayload: 16 * 1024 * 1024 });

// Route upgrades based on path
server.on('upgrade', (request, socket, head) => {
    const pathname = url.parse(request.url).pathname;
    if (pathname === '/relay') {
        relayWss.handleUpgrade(request, socket, head, (ws) => {
            relayWss.emit('connection', ws, request);
        });
    } else {
        signalingWss.handleUpgrade(request, socket, head, (ws) => {
            signalingWss.emit('connection', ws, request);
        });
    }
});

// --- Relay WebSocket: transparent binary proxy between host and client ---
relayWss.on('connection', (ws, req) => {
    ws.isAlive = true;
    ws.missedPings = 0;
    ws.on('pong', heartbeat);

    const parsedUrl = url.parse(req.url, true);
    const { role, session } = parsedUrl.query;

    if (!role || !session) {
        ws.close(1008, 'Missing role or session');
        return;
    }

    console.log(`[RELAY ${role.toUpperCase()}] Session: ${session}`);

    if (!relaySessions.has(session)) {
        relaySessions.set(session, { hostWs: null, clientWs: null });
    }
    const relay = relaySessions.get(session);

    if (role === 'host') {
        if (relay.hostWs && relay.hostWs !== ws) {
            try { relay.hostWs.terminate(); } catch (e) {}
        }
        relay.hostWs = ws;
        ws.on('message', (data) => {
            if (relay.clientWs && relay.clientWs.readyState === WebSocket.OPEN) {
                // Backpressure: drop video frames (type 3) if client buffer exceeds 1.5MB to prevent memory exhaustion
                const isVideoBurst = data.length > 0 && (data[0] === 3 || (data[0] === 17 && data.length > 4096));
                if (relay.clientWs.bufferedAmount > 128 * 1024 && isVideoBurst) {
                    return;
                }
                relay.clientWs.send(data, { binary: true });
            }
        });
        ws.on('close', () => {
            if (relaySessions.has(session)) {
                const s = relaySessions.get(session);
                if (s.hostWs === ws) {
                    s.hostWs = null; if (!s.clientWs) relaySessions.delete(session);
                }
            }
            console.log(`[RELAY HOST DISCONNECT] Session: ${session}`);
        });
    } else if (role === 'client') {
        if (relay.clientWs && relay.clientWs !== ws) {
            try { relay.clientWs.terminate(); } catch (e) {}
        }
        relay.clientWs = ws;
        ws.on('message', (data) => {
            if (relay.hostWs && relay.hostWs.readyState === WebSocket.OPEN) {
                relay.hostWs.send(data, { binary: true });
            }
        });
        ws.on('close', () => {
            if (relaySessions.has(session)) {
                const s = relaySessions.get(session);
                if (s.clientWs === ws) {
                    s.clientWs = null; if (!s.hostWs) relaySessions.delete(session);
                }
            }
            console.log(`[RELAY CLIENT DISCONNECT] Session: ${session}`);
        });
    }
});

// --- Signaling WebSocket: session handshake (existing logic) ---
function heartbeat() {
    this.isAlive = true;
    this.missedPings = 0;
}

signalingWss.on('connection', (ws, req) => {
    ws.isAlive = true;
    ws.missedPings = 0;
    ws.on('pong', heartbeat);

    const forwarded = req.headers['x-forwarded-for'];
    const clientIp = forwarded ? forwarded.split(',')[0] : req.socket.remoteAddress;
    console.log(`[WS CONNECT] New connection from ${clientIp}`);

    ws.on('message', (messageAsString) => {
        try {
            const data = JSON.parse(messageAsString);
            switch (data.type) {
                case 'register_host': {
                    const sessionId = uuidv4().substring(0, 8);
                    sessions.set(sessionId, { hostWs: ws, clientWs: null, hostIp: clientIp });
                    ws.sessionId = sessionId;
                    ws.role = 'host';
                    ws.send(JSON.stringify({ type: 'host_registered', sessionId }));
                    console.log(`[WS HOST REG] IP: ${clientIp} | ID: ${sessionId}`);
                    break;
                }
                case 'join_session': {
                    const { sessionId } = data;
                    const session = sessions.get(sessionId);
                    if (!session || (!session.hostWs && !session.hostIp)) {
                        ws.send(JSON.stringify({ type: 'error', message: 'Invalid Session' }));
                        return;
                    }
                    if (session.clientWs) {
                        ws.send(JSON.stringify({ type: 'error', message: 'Session active' }));
                        return;
                    }
                    session.clientWs = ws;
                    ws.sessionId = sessionId;
                    ws.role = 'client';
                    ws.send(JSON.stringify({ type: 'session_joined', hostIp: session.hostIp }));
                    if (session.hostWs) session.hostWs.send(JSON.stringify({ type: 'client_connected' }));
                    console.log(`[WS CLIENT JOIN] IP: ${clientIp} | ID: ${sessionId}`);
                    break;
                }
                case 'signal': {
                    const session = sessions.get(ws.sessionId);
                    if (!session) return;
                    const targetWs = (ws.role === 'host') ? session.clientWs : session.hostWs;
                    if (targetWs && targetWs.readyState === WebSocket.OPEN) {
                        targetWs.send(JSON.stringify({ type: 'signal', payload: data.payload }));
                    }
                    break;
                }
            }
        } catch (e) {
            console.error(`[WS ERROR] ${e.message}`);
        }
    });

    ws.on('close', () => {
        if (!ws.sessionId) return;
        const session = sessions.get(ws.sessionId);
        if (session) {
            const targetWs = (ws.role === 'host') ? session.clientWs : session.hostWs;
            if (targetWs && targetWs.readyState === WebSocket.OPEN) {
                targetWs.send(JSON.stringify({ type: 'peer_disconnected' }));
            }
            sessions.delete(ws.sessionId);
        }
    });
});

function pingClients(wss) {
    wss.clients.forEach((ws) => {
        ws.missedPings = (ws.missedPings || 0) + 1;
        if (ws.missedPings > 2) return ws.terminate();
        ws.ping();
    });
}

const interval = setInterval(() => {
    pingClients(signalingWss);
    pingClients(relayWss);
}, 25000);

signalingWss.on('close', () => clearInterval(interval));
relayWss.on('close', () => clearInterval(interval));

server.listen(port, () => {
    console.log(`[START] Gupt Signaling + Relay Server running on port ${port}`);
});
