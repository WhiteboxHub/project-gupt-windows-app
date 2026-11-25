const assert = require("assert");
const http = require("http");
const path = require("path");
const WebSocket = require(path.join(__dirname, "../SignalingServer/node_modules/ws"));
const { server, sessions, cleanupExpiredSessions } = require("../SignalingServer/server");

function httpGetJson(baseUrl, reqPath) {
  return new Promise((resolve, reject) => {
    http.get(`${baseUrl}${reqPath}`, (res) => {
      let body = "";
      res.on("data", (c) => (body += c));
      res.on("end", () => {
        try {
          resolve({ status: res.statusCode, json: JSON.parse(body) });
        } catch (e) {
          reject(e);
        }
      });
    }).on("error", reject);
  });
}

function connectWs(wsUrl) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl);
    ws.once("open", () => resolve(ws));
    ws.once("error", reject);
  });
}

function waitForClose(ws) {
  return new Promise((resolve) => {
    if (ws.readyState === WebSocket.CLOSED) return resolve({ code: 1006, reason: "" });
    ws.once("close", (code, reason) => resolve({ code, reason: reason.toString() }));
  });
}

function waitForMessage(ws) {
  return new Promise((resolve, reject) => {
    ws.once("message", (data, isBinary) => resolve({ data, isBinary }));
    ws.once("close", (code, reason) => reject(new Error(`Closed (${code}): ${reason.toString()}`)));
  });
}

async function run() {
  const loggedLines = [];
  const origLog = console.log;
  console.log = (...args) => {
    loggedLines.push(args.map(String).join(" "));
  };

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const addr = server.address();
  const baseUrl = `http://127.0.0.1:${addr.port}`;
  const wsBase = `ws://127.0.0.1:${addr.port}`;

  try {
    origLog("[TEST] Running Relay Authentication & Security integration tests...");

    // 1. Register returns sessionId, hostSecret, pairingPin
    const reg = await httpGetJson(baseUrl, "/register");
    assert.strictEqual(reg.status, 200);
    const { sessionId, hostSecret, pairingPin, expiresInMs } = reg.json;
    assert.match(sessionId, /^[0-9a-f]{8}$/);
    assert.match(hostSecret, /^[0-9a-f]{64}$/);
    assert.match(pairingPin, /^\d{6}$/);
    assert.ok(expiresInMs > 0);
    origLog("  -> /register generates 8-char sessionId, 256-bit hostSecret, 6-digit PIN: PASSED");

    // 2. Unknown session is rejected
    const wsUnknown = await connectWs(`${wsBase}/relay?role=host&session=deadbeef`);
    const closeUnknown = await waitForClose(wsUnknown);
    assert.strictEqual(closeUnknown.code, 1008);
    origLog("  -> Unknown session rejected (1008): PASSED");

    // 3. Binary message before auth is rejected
    const wsPreBin = await connectWs(`${wsBase}/relay?role=host&session=${sessionId}`);
    wsPreBin.send(Buffer.from([1, 0, 0, 0, 0]));
    const closePreBin = await waitForClose(wsPreBin);
    assert.strictEqual(closePreBin.code, 1008);
    origLog("  -> Binary message before relay_auth rejected: PASSED");

    // 4. Invalid Host credential is rejected
    const wsBadHost = await connectWs(`${wsBase}/relay?role=host&session=${sessionId}`);
    wsBadHost.send(JSON.stringify({ type: "relay_auth", role: "host", sessionId, hostSecret: "wrong_secret" }));
    const closeBadHost = await waitForClose(wsBadHost);
    assert.strictEqual(closeBadHost.code, 1008);
    origLog("  -> Invalid Host credential rejected: PASSED");

    // 5. Valid Host authenticates and unauthorized peer CANNOT replace it
    const wsHost = await connectWs(`${wsBase}/relay?role=host&session=${sessionId}`);
    wsHost.send(JSON.stringify({ type: "relay_auth", role: "host", sessionId, hostSecret }));
    const hostAuthAck = await waitForMessage(wsHost);
    assert.strictEqual(hostAuthAck.isBinary, false);
    assert.strictEqual(JSON.parse(hostAuthAck.data.toString()).type, "relay_auth_ok");

    const wsHijackHost = await connectWs(`${wsBase}/relay?role=host&session=${sessionId}`);
    wsHijackHost.send(JSON.stringify({ type: "relay_auth", role: "host", sessionId, hostSecret: "attacker_secret" }));
    const closeHijack = await waitForClose(wsHijackHost);
    assert.strictEqual(closeHijack.code, 1008);
    assert.strictEqual(wsHost.readyState, WebSocket.OPEN);
    origLog("  -> Unauthorized peer cannot replace authenticated Host: PASSED");

    // 6. Invalid Controller PIN is rejected and cannot replace valid Controller
    const wsBadClient = await connectWs(`${wsBase}/relay?role=client&session=${sessionId}`);
    wsBadClient.send(JSON.stringify({ type: "relay_auth", role: "client", sessionId, pin: "000000" }));
    const closeBadClient = await waitForClose(wsBadClient);
    assert.strictEqual(closeBadClient.code, 1008);

    const wsClient = await connectWs(`${wsBase}/relay?role=client&session=${sessionId}`);
    wsClient.send(JSON.stringify({ type: "relay_auth", role: "client", sessionId, pin: pairingPin }));
    const clientAuthAck = await waitForMessage(wsClient);
    assert.strictEqual(JSON.parse(clientAuthAck.data.toString()).type, "relay_auth_ok");
    origLog("  -> Valid Host & Controller authenticate & invalid PIN rejected: PASSED");

    // 7. Binary forwarding works after both authenticate
    const testFrame = Buffer.from([19, 4, 0, 0, 0, 1, 2, 3, 4]);
    const hostRecvP = waitForMessage(wsHost);
    wsClient.send(testFrame);
    const forwarded = await hostRecvP;
    assert.strictEqual(forwarded.isBinary, true);
    assert.deepStrictEqual(Buffer.from(forwarded.data), testFrame);
    origLog("  -> Binary forwarding works after mutual auth: PASSED");

    // 8. Repeated invalid attempts trigger rate-limit lockout
    const reg2 = await httpGetJson(baseUrl, "/register");
    const s2 = reg2.json.sessionId;
    for (let i = 0; i < 5; i++) {
      const w = await connectWs(`${wsBase}/relay?role=client&session=${s2}`);
      w.send(JSON.stringify({ type: "relay_auth", role: "client", sessionId: s2, pin: "111111" }));
      await waitForClose(w);
    }
    const wsLocked = await connectWs(`${wsBase}/relay?role=client&session=${s2}`);
    const closeLocked = await waitForClose(wsLocked);
    assert.strictEqual(closeLocked.code, 1008);
    assert.match(closeLocked.reason, /Too many failed/);
    origLog("  -> Repeated invalid attempts trigger lockout: PASSED");

    // 9. Expired session is rejected and cleaned up
    const reg3 = await httpGetJson(baseUrl, "/register");
    const s3 = reg3.json.sessionId;
    sessions.get(s3).expiresAt = Date.now() - 1000;
    const wsExp = await connectWs(`${wsBase}/relay?role=host&session=${s3}`);
    const closeExp = await waitForClose(wsExp);
    assert.strictEqual(closeExp.code, 1008);
    assert.strictEqual(sessions.has(s3), false);

    const reg4 = await httpGetJson(baseUrl, "/register");
    sessions.get(reg4.json.sessionId).expiresAt = Date.now() - 1000;
    cleanupExpiredSessions();
    assert.strictEqual(sessions.has(reg4.json.sessionId), false);
    origLog("  -> Expired sessions rejected and cleaned up: PASSED");

    // 10. Secrets never appear in server logs
    const allLogs = loggedLines.join("\n");
    assert.strictEqual(allLogs.includes(hostSecret), false, "hostSecret leaked in logs!");
    assert.strictEqual(allLogs.includes(pairingPin), false, "pairingPin leaked in logs!");
    origLog("  -> No hostSecret or pairingPin leaked in server logs: PASSED");

    wsHost.close();
    wsClient.close();
    origLog("[SUCCESS] All Relay Auth & Security tests passed!");
  } finally {
    console.log = origLog;
    server.close();
  }
}

run().catch((err) => {
  console.error("TEST FAILED:", err);
  process.exit(1);
});

