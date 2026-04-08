const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');
const http = require('http');
const url = require('url');

const port = process.env.PORT || 8081;

// Store active sessions: sessionId -> { hostWs, clientWs, hostIp }
const sessions = new Map();

// --- HTTP Server for native C++ clients (Easy fetch) ---
const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url, true);
    
    // CORS headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Content-Type', 'application/json');

    const clientIp = req.socket.remoteAddress;

    if (parsedUrl.pathname === '/register') {
        let hIp = clientIp.includes('127.0.0.1') || clientIp.includes('::1') ? '172.31.96.1' : clientIp;
        
        const sessionId = uuidv4().substring(0, 8);
        sessions.set(sessionId, { hostWs: null, clientWs: null, hostIp: hIp });
        console.log(`[HTTP REGISTER] Remote: ${clientIp} -> Adjusted Host IP: ${hIp} | ID: ${sessionId}`);
        res.writeHead(200);
        res.end(JSON.stringify({ sessionId }));
        return;
    }

    if (parsedUrl.pathname === '/join') {
        const sessionId = parsedUrl.query.id;
        console.log(`[HTTP JOIN] Client from ${clientIp} requesting Session ID: '${sessionId}'`);
        const session = sessions.get(sessionId);
        if (!session) {
            console.log(`[HTTP JOIN ERROR] Invalid Session ID provided: '${sessionId}'`);
            res.writeHead(404);
            res.end(JSON.stringify({ error: 'Invalid Session ID' }));
            return;
        }
        console.log(`[HTTP JOIN SUCCESS] Handing over Host IP: ${session.hostIp}`);
        res.writeHead(200);
        res.end(JSON.stringify({ hostIp: session.hostIp }));
        return;
    }

    res.writeHead(404);
    res.end('Not found');
});

// --- WebSocket Server for WebRTC (attached to HTTP server) ---
const wss = new WebSocket.Server({ server });

function heartbeat() {
    this.isAlive = true;
}

wss.on('connection', (ws, req) => {
    ws.isAlive = true;
    ws.on('pong', heartbeat);

    const clientIp = req.socket.remoteAddress;
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
                    ws.send(JSON.stringify({ type: 'host_registered', sessionId: sessionId }));
                    console.log(`[WS HOST REG] Session IP: ${clientIp} | ID: ${sessionId}`);
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
                    console.log(`[WS CLIENT JOIN] Session IP: ${clientIp} | ID: ${sessionId}`);
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

const interval = setInterval(() => {
    wss.clients.forEach((ws) => {
        if (ws.isAlive === false) return ws.terminate();
        ws.isAlive = false;
        ws.ping();
    });
}, 30000);

wss.on('close', () => clearInterval(interval));

server.listen(port, () => {
    console.log(`[START] Gupt Sub-Signal Server running on http://localhost:${port}`);
    console.log(`[START] Gupt WebSocket Signaling Server running on ws://localhost:${port}`);
});
