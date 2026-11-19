export const MessageType = Object.freeze({
  ConnectRequest: 1,
  ConnectResponse: 2,
  FrameData: 3,
  MouseEvent: 4,
  KeyboardEvent: 5,
  Heartbeat: 6,
  Disconnect: 7,
  ClipboardText: 8,
  ClipboardImage: 9,
  RefreshRequest: 10,
  FrameAck: 11,
  AudioData: 12,
  P2PCandidate: 13,
  SwitchMonitor: 14,
  SecureDesktopNotification: 15,
  KeyExchange: 16,
  EncryptedPayload: 17,
  MonitorInfo: 18,
  WebRtcOffer: 19,
  WebRtcAnswer: 20,
  WebRtcCandidate: 21,
});

const HEADER_SIZE = 5;
const FRAME_HEADER_SIZE = 41;
const WEBRTC_SDP_HEADER_SIZE = 52;
const WEBRTC_CANDIDATE_HEADER_SIZE = 120;

export function encodeMessage(type, payload = new Uint8Array()) {
  const out = new Uint8Array(HEADER_SIZE + payload.byteLength);
  const view = new DataView(out.buffer);
  view.setUint8(0, type);
  view.setUint32(1, payload.byteLength, true);
  out.set(payload, HEADER_SIZE);
  return out;
}

export function encodeConnectRequest(sessionId, token = "GUPT_TAURI_CLIENT") {
  const payload = new Uint8Array(96);
  writeAscii(payload, 0, 32, sessionId);
  writeAscii(payload, 32, 64, token);
  return encodeMessage(MessageType.ConnectRequest, payload);
}

export function encodeConnectResponse(accepted, reason = "OK") {
  const payload = new Uint8Array(129);
  const view = new DataView(payload.buffer);
  view.setUint8(0, accepted ? 1 : 0);
  writeAscii(payload, 1, 128, reason);
  return encodeMessage(MessageType.ConnectResponse, payload);
}

export function encodeFrameAck(frameNumber) {
  const payload = new Uint8Array(4);
  new DataView(payload.buffer).setUint32(0, frameNumber, true);
  return encodeMessage(MessageType.FrameAck, payload);
}

export function encodeMouseMove(normalizedX, normalizedY) {
  return encodeMouseEvent(normalizedX, normalizedY, 255, false, 0);
}

export function encodeMouseButton(normalizedX, normalizedY, buttonId, isDown) {
  return encodeMouseEvent(normalizedX, normalizedY, buttonId, isDown, 0);
}

export function encodeMouseWheel(normalizedX, normalizedY, delta) {
  return encodeMouseEvent(normalizedX, normalizedY, 255, false, delta);
}

export function encodeKeyboard(virtualKey, isDown) {
  const payload = new Uint8Array(3);
  const view = new DataView(payload.buffer);
  view.setUint16(0, virtualKey, true);
  view.setUint8(2, isDown ? 1 : 0);
  return encodeMessage(MessageType.KeyboardEvent, payload);
}

const CLIPBOARD_TEXT_PREFIX = "GUPT_CLIPBOARD_TEXT_V1\n";

export function encodeClipboardText(text) {
  // Pure UTF-8 payload matching native Windows and Mac Protocol
  return encodeMessage(MessageType.ClipboardText, new TextEncoder().encode(String(text ?? "")));
}

export function decodeClipboardText(payload) {
  let body = new TextDecoder().decode(payload);
  if (body.startsWith(CLIPBOARD_TEXT_PREFIX)) {
    const rest = body.slice(CLIPBOARD_TEXT_PREFIX.length);
    const separator = rest.indexOf("\n");
    if (separator !== -1) {
      body = rest.slice(separator + 1);
    }
  }
  return {
    text: body,
    transferMode: "auto",
  };
}

export function decodeMessages(buffer) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  const messages = [];
  let offset = 0;
  while (offset + HEADER_SIZE <= bytes.byteLength) {
    const view = new DataView(bytes.buffer, bytes.byteOffset + offset, HEADER_SIZE);
    const type = view.getUint8(0);
    const payloadSize = view.getUint32(1, true);
    const end = offset + HEADER_SIZE + payloadSize;
    if (end > bytes.byteLength) break;
    messages.push({ type, payload: bytes.slice(offset + HEADER_SIZE, end) });
    offset = end;
  }
  return messages;
}

export function decodeFrame(payload) {
  if (payload.byteLength < FRAME_HEADER_SIZE) return null;
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    frameNumber: view.getUint32(0, true),
    totalWidth: view.getUint32(4, true),
    totalHeight: view.getUint32(8, true),
    updateWidth: view.getUint32(12, true),
    updateHeight: view.getUint32(16, true),
    targetX: view.getUint32(20, true),
    targetY: view.getUint32(24, true),
    bpp: view.getUint32(28, true),
    isDelta: view.getUint8(32) !== 0,
    timestampMs: Number(view.getBigUint64(33, true)),
    jpeg: payload.slice(FRAME_HEADER_SIZE),
  };
}

function encodeMouseEvent(normalizedX, normalizedY, buttonId, isDown, wheelDelta) {
  const payload = new Uint8Array(14);
  const view = new DataView(payload.buffer);
  view.setFloat32(0, clamp01(normalizedX), true);
  view.setFloat32(4, clamp01(normalizedY), true);
  view.setUint8(8, buttonId);
  view.setUint8(9, isDown ? 1 : 0);
  view.setInt32(10, wheelDelta, true);
  return encodeMessage(MessageType.MouseEvent, payload);
}

function writeAscii(bytes, offset, length, value) {
  const text = String(value ?? "");
  const max = Math.max(0, length - 1);
  for (let i = 0; i < Math.min(max, text.length); i += 1) {
    bytes[offset + i] = text.charCodeAt(i) & 0x7f;
  }
  bytes[offset + Math.min(max, text.length)] = 0;
}

function clamp01(value) {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(1, value));
}

function readAscii(bytes, offset, length) {
  let end = offset;
  const limit = offset + length;
  while (end < limit && bytes[end] !== 0) {
    end++;
  }
  return new TextDecoder().decode(bytes.subarray(offset, end));
}

export function encodeWebRtcOffer(sessionId, role, sdp) {
  return encodeWebRtcSdp(MessageType.WebRtcOffer, sessionId, role, sdp);
}

export function encodeWebRtcAnswer(sessionId, role, sdp) {
  return encodeWebRtcSdp(MessageType.WebRtcAnswer, sessionId, role, sdp);
}

function encodeWebRtcSdp(type, sessionId, role, sdp) {
  const sdpBytes = new TextEncoder().encode(String(sdp ?? ""));
  if (sdpBytes.byteLength > 65536) throw new Error("SDP string exceeds 64KB limit");
  const payload = new Uint8Array(WEBRTC_SDP_HEADER_SIZE + sdpBytes.byteLength);
  writeAscii(payload, 0, 32, sessionId);
  writeAscii(payload, 32, 16, role);
  new DataView(payload.buffer).setUint32(48, sdpBytes.byteLength, true);
  payload.set(sdpBytes, WEBRTC_SDP_HEADER_SIZE);
  return encodeMessage(type, payload);
}

export function decodeWebRtcSdp(payload) {
  if (!payload || payload.byteLength < WEBRTC_SDP_HEADER_SIZE) return null;
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const sdpLen = view.getUint32(48, true);
  if (sdpLen > 65536 || WEBRTC_SDP_HEADER_SIZE + sdpLen > payload.byteLength) return null;

  const sessionId = readAscii(payload, 0, 32);
  const role = readAscii(payload, 32, 16);
  const sdp = new TextDecoder().decode(payload.subarray(WEBRTC_SDP_HEADER_SIZE, WEBRTC_SDP_HEADER_SIZE + sdpLen));
  return { sessionId, role, sdp };
}

export function encodeWebRtcCandidate(sessionId, role, sdpMid, sdpMLineIndex, ufrag, candidate) {
  const candBytes = new TextEncoder().encode(String(candidate ?? ""));
  if (candBytes.byteLength > 8192) throw new Error("Candidate string exceeds 8KB limit");
  const payload = new Uint8Array(WEBRTC_CANDIDATE_HEADER_SIZE + candBytes.byteLength);
  writeAscii(payload, 0, 32, sessionId);
  writeAscii(payload, 32, 16, role);
  writeAscii(payload, 48, 32, sdpMid ?? "");
  const view = new DataView(payload.buffer);
  view.setInt32(80, typeof sdpMLineIndex === "number" ? sdpMLineIndex : -1, true);
  writeAscii(payload, 84, 32, ufrag ?? "");
  view.setUint32(116, candBytes.byteLength, true);
  payload.set(candBytes, WEBRTC_CANDIDATE_HEADER_SIZE);
  return encodeMessage(MessageType.WebRtcCandidate, payload);
}

export function decodeWebRtcCandidate(payload) {
  if (!payload || payload.byteLength < WEBRTC_CANDIDATE_HEADER_SIZE) return null;
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const candLen = view.getUint32(116, true);
  if (candLen > 8192 || WEBRTC_CANDIDATE_HEADER_SIZE + candLen > payload.byteLength) return null;

  const sessionId = readAscii(payload, 0, 32);
  const role = readAscii(payload, 32, 16);
  const sdpMid = readAscii(payload, 48, 32);
  const sdpMLineIndex = view.getInt32(80, true);
  const ufrag = readAscii(payload, 84, 32);
  const candidate = new TextDecoder().decode(payload.subarray(WEBRTC_CANDIDATE_HEADER_SIZE, WEBRTC_CANDIDATE_HEADER_SIZE + candLen));
  return { sessionId, role, sdpMid, sdpMLineIndex, ufrag, candidate };
}
