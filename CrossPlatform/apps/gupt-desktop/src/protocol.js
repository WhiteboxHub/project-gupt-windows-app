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
});

const HEADER_SIZE = 5;
const FRAME_HEADER_SIZE = 41;

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
