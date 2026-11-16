"""
Pure Python RFC 6455 WebSocket Client (Stdlib only)
Supports WSS (SSL) and WS binary and text framing.
Zero third-party dependencies.
"""
import socket
import ssl
import os
import base64
import hashlib
import struct

class WebSocketClient:
    def __init__(self, host: str, port: int, path: str, secure: bool = True, timeout: float = 10.0):
        self.host = host
        self.port = port
        self.path = path
        self.secure = secure
        self.timeout = timeout
        self.sock = None
        self.connected = False

    def connect(self):
        raw_sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        if self.secure:
            ctx = ssl.create_default_context()
            self.sock = ctx.wrap_socket(raw_sock, server_hostname=self.host)
        else:
            self.sock = raw_sock

        sec_key = base64.b64encode(os.urandom(16)).decode('ascii')
        req = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {sec_key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode('ascii'))

        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("Server closed connection during handshake")
            resp += chunk

        status_line = resp.split(b"\r\n")[0].decode('latin1')
        if "101" not in status_line:
            raise ConnectionError(f"Handshake failed: {status_line}")

        self.connected = True
        self.sock.settimeout(self.timeout)

    def send_binary(self, data: bytes):
        if not self.connected:
            raise ConnectionError("Not connected")
        # Opcode 0x02 = binary, masked client-side
        length = len(data)
        mask_key = os.urandom(4)
        header = bytearray()
        header.append(0x82) # FIN + binary

        if length <= 125:
            header.append(0x80 | length)
        elif length <= 65535:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))

        header.extend(mask_key)
        # Apply mask
        masked_payload = bytearray(length)
        for i in range(length):
            masked_payload[i] = data[i] ^ mask_key[i % 4]

        self.sock.sendall(bytes(header) + bytes(masked_payload))

    def recv_frame(self, timeout: float = None) -> bytes:
        if not self.connected:
            return b""
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            head = self._recv_exact(2)
            if not head:
                return b""
            b1, b2 = head[0], head[1]
            opcode = b1 & 0x0F
            is_masked = bool(b2 & 0x80)
            length = b2 & 0x7F

            if length == 126:
                length = struct.unpack("!H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exact(8))[0]

            mask = self._recv_exact(4) if is_masked else None
            payload = self._recv_exact(length)

            if is_masked and mask:
                unmasked = bytearray(length)
                for i in range(length):
                    unmasked[i] = payload[i] ^ mask[i % 4]
                payload = bytes(unmasked)

            if opcode == 0x08: # Close
                self.close()
                return b""
            elif opcode == 0x09: # Ping
                # Reply with pong
                self._send_pong(payload)
                return self.recv_frame(timeout)
            return payload
        except (socket.timeout, TimeoutError):
            return b""
        except Exception:
            self.close()
            return b""

    def _send_pong(self, data: bytes):
        mask_key = os.urandom(4)
        hdr = bytearray([0x8A, 0x80 | len(data)]) + bytearray(mask_key)
        masked = bytearray(data[i] ^ mask_key[i % 4] for i in range(len(data)))
        self.sock.sendall(bytes(hdr) + bytes(masked))

    def _recv_exact(self, num_bytes: int) -> bytes:
        buf = bytearray()
        while len(buf) < num_bytes:
            chunk = self.sock.recv(num_bytes - len(buf))
            if not chunk:
                break
            buf.extend(chunk)
        return bytes(buf)

    def close(self):
        self.connected = False
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
