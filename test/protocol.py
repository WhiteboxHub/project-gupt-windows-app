"""
Gupt Remote Desktop Protocol Definition in Python
Matches project-gupt-windows-app/Shared/Protocol.h exactly
"""
import struct
from enum import IntEnum

class MessageType(IntEnum):
    ConnectRequest = 1
    ConnectResponse = 2
    FrameData = 3
    MouseEvent = 4
    KeyboardEvent = 5
    Heartbeat = 6
    Disconnect = 7
    ClipboardText = 8
    ClipboardImage = 9
    RefreshRequest = 10
    FrameAck = 11
    AudioData = 12

# Header: uint8_t type + uint32_t payloadSize (packed, little-endian)
HEADER_FORMAT = "<BI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

def serialize_message(msg_type: MessageType, payload: bytes = b"") -> bytes:
    header = struct.pack(HEADER_FORMAT, int(msg_type), len(payload))
    return header + payload

def deserialize_messages(data: bytes):
    offset = 0
    messages = []
    while offset + HEADER_SIZE <= len(data):
        m_type_raw, p_size = struct.unpack_from(HEADER_FORMAT, data, offset)
        offset += HEADER_SIZE
        if offset + p_size > len(data):
            break
        payload = data[offset : offset + p_size]
        offset += p_size
        try:
            m_type = MessageType(m_type_raw)
        except ValueError:
            m_type = m_type_raw
        messages.append((m_type, payload))
    return messages

def serialize_connect_request(session_id: str, token: str = "GUPT_LOCAL_TOKEN") -> bytes:
    session_bytes = session_id.encode('utf-8')[:31].ljust(32, b'\x00')
    token_bytes = token.encode('utf-8')[:63].ljust(64, b'\x00')
    payload = session_bytes + token_bytes
    return serialize_message(MessageType.ConnectRequest, payload)

def parse_connect_request(payload: bytes):
    if len(payload) < 96:
        raise ValueError(f"Payload too short for ConnectRequest: {len(payload)}")
    session_id = payload[:32].rstrip(b'\x00').decode('utf-8', errors='replace')
    token = payload[32:96].rstrip(b'\x00').decode('utf-8', errors='replace')
    return {"sessionId": session_id, "token": token}

def serialize_connect_response(accepted: bool, reason: str = "OK") -> bytes:
    acc_byte = b'\x01' if accepted else b'\x00'
    reason_bytes = reason.encode('utf-8')[:127].ljust(128, b'\x00')
    payload = acc_byte + reason_bytes
    return serialize_message(MessageType.ConnectResponse, payload)

def parse_connect_response(payload: bytes):
    if len(payload) < 129:
        raise ValueError(f"Payload too short for ConnectResponse: {len(payload)}")
    accepted = bool(payload[0])
    reason = payload[1:129].rstrip(b'\x00').decode('utf-8', errors='replace')
    return {"accepted": accepted, "reason": reason}

def serialize_mouse_event(x: float, y: float, button_id: int = 255, is_down: bool = False, wheel_delta: int = 0) -> bytes:
    # struct MouseEvent { float x; float y; uint8_t button; bool isDown; int wheelDelta; };
    payload = struct.pack("<ff??i", float(x), float(y), int(button_id), bool(is_down), int(wheel_delta))
    return serialize_message(MessageType.MouseEvent, payload)

def parse_mouse_event(payload: bytes):
    if len(payload) < 14:
        raise ValueError("Payload too short for MouseEvent")
    x, y, button, is_down, wheel_delta = struct.unpack("<ff??i", payload[:14])
    return {"x": x, "y": y, "button": button, "isDown": is_down, "wheelDelta": wheel_delta}

def serialize_keyboard_event(virtual_key: int, is_down: bool) -> bytes:
    # struct KeyboardEvent { uint16_t virtualKey; bool isDown; };
    payload = struct.pack("<HB", int(virtual_key), 1 if is_down else 0)
    return serialize_message(MessageType.KeyboardEvent, payload)

def parse_keyboard_event(payload: bytes):
    if len(payload) < 3:
        raise ValueError("Payload too short for KeyboardEvent")
    vk, is_down = struct.unpack("<HB", payload[:3])
    return {"virtualKey": vk, "isDown": bool(is_down)}

def serialize_frame(frame_num: int, total_w: int, total_h: int, update_w: int, update_h: int,
                    target_x: int, target_y: int, bpp: int, is_delta: bool, timestamp_ms: int,
                    jpeg_bytes: bytes) -> bytes:
    hdr = struct.pack("<IIIIIIII?Q", frame_num, total_w, total_h, update_w, update_h,
                      target_x, target_y, bpp, is_delta, timestamp_ms)
    payload = hdr + jpeg_bytes
    return serialize_message(MessageType.FrameData, payload)

def parse_frame(payload: bytes):
    hdr_size = struct.calcsize("<IIIIIIII?Q")
    if len(payload) < hdr_size:
        raise ValueError("Payload too short for FrameDataHeader")
    fn, tw, th, uw, uh, tx, ty, bpp, delta, ts = struct.unpack("<IIIIIIII?Q", payload[:hdr_size])
    return {
        "frameNumber": fn,
        "totalWidth": tw,
        "totalHeight": th,
        "updateWidth": uw,
        "updateHeight": uh,
        "targetX": tx,
        "targetY": ty,
        "bpp": bpp,
        "isDelta": delta,
        "timestampMs": ts,
        "jpegBytes": payload[hdr_size:]
    }

def serialize_audio(sample_rate: int, channels: int, bits_per_sample: int, pcm_bytes: bytes) -> bytes:
    hdr = struct.pack("<IHH I", sample_rate, channels, bits_per_sample, len(pcm_bytes))
    return serialize_message(MessageType.AudioData, hdr + pcm_bytes)

def parse_audio(payload: bytes):
    hdr_size = struct.calcsize("<IHHI")
    if len(payload) < hdr_size:
        raise ValueError("Payload too short for AudioDataHeader")
    sr, ch, bits, data_sz = struct.unpack("<IHHI", payload[:hdr_size])
    return {
        "sampleRate": sr,
        "channels": ch,
        "bitsPerSample": bits,
        "pcmData": payload[hdr_size:hdr_size + data_sz]
    }

def serialize_clipboard_text(text: str) -> bytes:
    return serialize_message(MessageType.ClipboardText, text.encode('utf-8'))

def serialize_frame_ack(frame_number: int) -> bytes:
    payload = struct.pack("<I", frame_number)
    return serialize_message(MessageType.FrameAck, payload)

def parse_frame_ack(payload: bytes):
    if len(payload) < 4:
        raise ValueError("Payload too short for FrameAck")
    return {"frameNumber": struct.unpack("<I", payload[:4])[0]}

