const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');

const port = process.env.PORT || 8081;
const wss = new WebSocket.Server({ port });

// Store sessions: sessionId -> { hostWs, clientWs }
const sessions = new Map();

wss.on('connection', (ws) => {
    ws.on('message', (messageAsString) => {
        try {
            const data = JSON.parse(messageAsString);

            switch (data.type) {
                // Host registers itself to get a session ID
                case 'register_host': {
                    const sessionId = uuidv4().substring(0, 8); // Short ID for users
                    sessions.set(sessionId, { hostWs: ws, clientWs: null });
                    ws.sessionId = sessionId;
                    ws.role = 'host';
                    
                    ws.send(JSON.stringify({
                        type: 'host_registered',
                        sessionId: sessionId
                    }));
                    console.log(`[HOST REGISTERED] Session ID: ${sessionId}`);
                    break;
                }

                // Client attempts to join an existing session
                case 'join_session': {
                    const { sessionId } = data;
                    const session = sessions.get(sessionId);

                    if (session && session.hostWs) {
                        if (session.clientWs) {
                            ws.send(JSON.stringify({ type: 'error', message: 'Session already active.' }));
                            return;
                        }
                        
                        session.clientWs = ws;
                        ws.sessionId = sessionId;
                        ws.role = 'client';

                        // Notify both parties that connection is established (P2P handshaking can now occur)
                        ws.send(JSON.stringify({ type: 'session_joined' }));
                        session.hostWs.send(JSON.stringify({ type: 'client_connected' }));
                        
                        console.log(`[CLIENT JOINED] Session ID: ${sessionId}`);
                    } else {
                        ws.send(JSON.stringify({ type: 'error', message: 'Invalid Session ID.' }));
                    }
                    break;
                }

                // Forward signaling (WebRTC ICE candidates / SDP)
                case 'signal': {
                    const session = sessions.get(ws.sessionId);
                    if (!session) return;

                    // Relay signal to the other peer
                    const targetWs = (ws.role === 'host') ? session.clientWs : session.hostWs;
                    if (targetWs && targetWs.readyState === WebSocket.OPEN) {
                        targetWs.send(JSON.stringify({
                            type: 'signal',
                            payload: data.payload
                        }));
                    }
                    break;
                }

                default:
                    console.warn('Unknown message type:', data.type);
            }
        } catch (e) {
            console.error('Failed to parse message:', e.message);
        }
    });

    ws.on('close', () => {
        if (ws.sessionId) {
            console.log(`[DISCONNECT] ${ws.role} left session ${ws.sessionId}`);
            const session = sessions.get(ws.sessionId);
            
            if (session) {
                // Notify the other party
                const targetWs = (ws.role === 'host') ? session.clientWs : session.hostWs;
                if (targetWs && targetWs.readyState === WebSocket.OPEN) {
                    targetWs.send(JSON.stringify({ type: 'peer_disconnected' }));
                }

                // Clean up session
                sessions.delete(ws.sessionId);
            }
        }
    });
});

console.log(`Gupt Signaling Server running on ws://localhost:${port}`);
