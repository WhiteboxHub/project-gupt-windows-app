import assert from "assert";
import {
  MessageType,
  decodeMessages,
  encodeConnectRequest,
  encodeConnectResponse,
  encodeMessage,
  encodeMouseMove,
  encodeKeyboard,
  encodeClipboardText,
} from "../CrossPlatform/apps/gupt-desktop/src/protocol.js";

// Mirrors the macOS Host approval & streaming state machine in CrossPlatform/apps/gupt-desktop/src/main.js
class MacHostLifecycleHarness {
  constructor() {
    this.hostApproved = false;
    this.hostApprovalPending = false;
    this.hostActive = false;
    this.relayAuthenticated = false;
    this.approvalModalVisible = false;
    this.sentPackets = [];
    this.injectedMouse = 0;
    this.injectedKeyboard = 0;
    this.receivedClipboard = 0;
    this.framesSent = 0;
  }

  onWsOpen(sessionId, hostSecret) {
    this.relayAuthenticated = false;
    this.sentPackets.push(JSON.stringify({ type: "relay_auth", role: "host", sessionId, hostSecret }));
    // Must NOT start streaming on WS open
  }

  onRelayMessage(data) {
    if (typeof data === "string") {
      const parsed = JSON.parse(data);
      if (parsed.type === "relay_auth_ok") {
        this.relayAuthenticated = true;
      }
      return;
    }

    for (const msg of decodeMessages(data)) {
      if (msg.type === MessageType.ConnectRequest) {
        if (this.hostApproved || this.hostApprovalPending) {
          this.sentPackets.push(encodeConnectResponse(false, "Busy"));
          continue;
        }
        this.hostApprovalPending = true;
        this.approvalModalVisible = true;
      } else if (msg.type === MessageType.Disconnect) {
        this.hostApproved = false;
        this.hostApprovalPending = false;
        this.hostActive = false;
        this.approvalModalVisible = false;
      } else if (!this.hostApproved) {
        continue;
      } else if (msg.type === MessageType.MouseEvent) {
        this.injectedMouse += 1;
      } else if (msg.type === MessageType.KeyboardEvent) {
        this.injectedKeyboard += 1;
      } else if (msg.type === MessageType.ClipboardText) {
        this.receivedClipboard += 1;
      }
    }
  }

  resolveApproval(approved) {
    this.hostApprovalPending = false;
    this.approvalModalVisible = false;
    if (approved) {
      this.hostApproved = true;
      this.sentPackets.push(encodeConnectResponse(true, "OK"));
      this.hostActive = true;
    } else {
      this.hostApproved = false;
      this.hostActive = false;
      this.sentPackets.push(encodeConnectResponse(false, "Rejected by host"));
    }
  }

  onRelayClose() {
    this.relayAuthenticated = false;
    this.hostApproved = false;
    this.hostApprovalPending = false;
    this.hostActive = false;
    this.approvalModalVisible = false;
  }

  tickStreamFrame() {
    if (this.hostActive && this.hostApproved) {
      this.framesSent += 1;
    }
  }
}

console.log("[TEST] Running macOS Host Lifecycle & Input Gate unit tests...");

const h = new MacHostLifecycleHarness();

// 1. Opening WS sends relay_auth and does NOT start streaming
h.onWsOpen("a1b2c3d4", "secret_xyz");
h.onRelayMessage(JSON.stringify({ type: "relay_auth_ok", role: "host", sessionId: "a1b2c3d4" }));
assert.strictEqual(h.relayAuthenticated, true);
assert.strictEqual(h.hostActive, false);
assert.strictEqual(h.hostApproved, false);
h.tickStreamFrame();
assert.strictEqual(h.framesSent, 0);

// 2. Unauthorized input before ConnectRequest is rejected
h.onRelayMessage(encodeMouseMove(0.5, 0.5));
h.onRelayMessage(encodeKeyboard(65, true));
h.onRelayMessage(encodeClipboardText("evil"));
assert.strictEqual(h.injectedMouse, 0);
assert.strictEqual(h.injectedKeyboard, 0);
assert.strictEqual(h.receivedClipboard, 0);

// 3. ConnectRequest opens approval modal without starting streaming
h.onRelayMessage(encodeConnectRequest("a1b2c3d4", "123456"));
assert.strictEqual(h.hostApprovalPending, true);
assert.strictEqual(h.approvalModalVisible, true);
assert.strictEqual(h.hostApproved, false);
assert.strictEqual(h.hostActive, false);

// 4. Rejecting does not start streaming and blocks input
h.resolveApproval(false);
assert.strictEqual(h.hostApproved, false);
assert.strictEqual(h.hostActive, false);
h.tickStreamFrame();
assert.strictEqual(h.framesSent, 0);
h.onRelayMessage(encodeMouseMove(0.5, 0.5));
assert.strictEqual(h.injectedMouse, 0);

// 5. Accepting starts streaming and allows input
h.onRelayMessage(encodeConnectRequest("a1b2c3d4", "123456"));
h.resolveApproval(true);
assert.strictEqual(h.hostApproved, true);
assert.strictEqual(h.hostActive, true);
h.tickStreamFrame();
assert.strictEqual(h.framesSent, 1);
h.onRelayMessage(encodeMouseMove(0.5, 0.5));
h.onRelayMessage(encodeKeyboard(65, true));
h.onRelayMessage(encodeClipboardText("hello"));
assert.strictEqual(h.injectedMouse, 1);
assert.strictEqual(h.injectedKeyboard, 1);
assert.strictEqual(h.receivedClipboard, 1);

// 6. Disconnect stops streaming and blocks subsequent input
h.onRelayMessage(encodeMessage(MessageType.Disconnect));
assert.strictEqual(h.hostApproved, false);
assert.strictEqual(h.hostActive, false);
h.tickStreamFrame();
assert.strictEqual(h.framesSent, 1);
h.onRelayMessage(encodeMouseMove(0.2, 0.2));
assert.strictEqual(h.injectedMouse, 1);

// 7. Reconnect starts with approval reset
h.onRelayMessage(encodeConnectRequest("a1b2c3d4", "123456"));
h.resolveApproval(true);
assert.strictEqual(h.hostApproved, true);
h.onRelayClose();
assert.strictEqual(h.hostApproved, false);
assert.strictEqual(h.hostActive, false);
assert.strictEqual(h.relayAuthenticated, false);

console.log("[SUCCESS] All macOS Host Lifecycle & Input Gate tests passed!");

