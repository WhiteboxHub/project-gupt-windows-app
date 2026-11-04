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
const signalingWss = new WebSocket.Server({ noServer: true });
const relayWss     = new WebSocket.Server({ noServer: true });

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
                if (relay.clientWs.bufferedAmount > 1536 * 1024 && data.length > 0 && data[0] === 3) {
                    return;
                }
                relay.clientWs.send(data, { binary: true });
            }
        });
        ws.on('close', () => {
            if (relaySessions.has(session)) {
                const s = relaySessions.get(session);
                if (s.hostWs === ws) {
                    s.hostWs = null;
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
                    s.clientWs = null;
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
