"""
End-to-End Test for Gupt Session over Cloud Relay.
Spawns Host and Client connections to Cloud Relay, performs protocol
handshake (ConnectRequest -> ConnectResponse), exchanges real input
events, audio data, frames, and measures live round-trip latency.
"""
import unittest
import http.client
import json
import ssl
import time
import sys
import os

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
if TEST_DIR not in sys.path:
    sys.path.insert(0, TEST_DIR)

from simple_ws import WebSocketClient
from protocol import (
    MessageType,
    serialize_connect_request,
    serialize_connect_response,
    parse_connect_request,
    parse_connect_response,
    serialize_mouse_event,
    parse_mouse_event,
    serialize_keyboard_event,
    parse_keyboard_event,
    serialize_audio,
    parse_audio,
    serialize_frame,
    parse_frame,
    deserialize_messages,
    serialize_message
)

SIGNALING_HOST = "gupt-signal-server-560359652969.us-central1.run.app"
SIGNALING_PORT = 443

class TestEndToEndRelaySession(unittest.TestCase):

    def setUp(self):
        # 1. Register a new real session on Cloud Relay
        ctx = ssl.create_default_context()
        conn = http.client.HTTPSConnection(SIGNALING_HOST, SIGNALING_PORT, timeout=10, context=ctx)
        conn.request("GET", "/register")
        resp = conn.getresponse()
        self.assertEqual(resp.status, 200)
        data = json.loads(resp.read().decode('utf-8'))
        self.session_id = data["sessionId"]
        conn.close()

        # 2. Connect Host to Relay
        host_path = f"/relay?role=host&session={self.session_id}"
        self.host_ws = WebSocketClient(SIGNALING_HOST, SIGNALING_PORT, host_path, secure=True)
        self.host_ws.connect()
        self.assertTrue(self.host_ws.connected, "Host failed to connect to WebSocket relay")

        # 3. Connect Client to Relay
        client_path = f"/relay?role=client&session={self.session_id}"
        self.client_ws = WebSocketClient(SIGNALING_HOST, SIGNALING_PORT, client_path, secure=True)
        self.client_ws.connect()
        self.assertTrue(self.client_ws.connected, "Client failed to connect to WebSocket relay")

        # Drain the initial connection notification sent to host by relay server
        initial = self.host_ws.recv_frame(timeout=1.0)
        # Note: initial is usually b'{"type":"client_relay_connected"}'

    def recv_protocol_messages(self, ws, timeout=5.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            raw = ws.recv_frame(timeout=1.0)
            if not raw:
                continue
            msgs = deserialize_messages(raw)
            if msgs:
                return msgs
        return []

    def tearDown(self):
        if self.client_ws:
            self.client_ws.close()
        if self.host_ws:
            self.host_ws.close()

    def test_handshake_and_permission_flow(self):
        """Simulates client sending ConnectRequest and host responding with ConnectResponse."""
        req_pkt = serialize_connect_request(self.session_id, "TOKEN_E2E_TEST")
        self.client_ws.send_binary(req_pkt)

        # Host receives ConnectRequest
        msgs = self.recv_protocol_messages(self.host_ws, timeout=5.0)
        self.assertTrue(any(m[0] == MessageType.ConnectRequest for m in msgs), f"Expected ConnectRequest, got: {[m[0] for m in msgs]}")
        req_data = parse_connect_request(msgs[0][1])
        self.assertEqual(req_data["sessionId"], self.session_id)

        # Host grants permission: sends ConnectResponse(accepted=True)
        res_pkt = serialize_connect_response(True, "OK")
        self.host_ws.send_binary(res_pkt)

        # Client receives ConnectResponse
        client_msgs = self.recv_protocol_messages(self.client_ws, timeout=5.0)
        self.assertTrue(any(m[0] == MessageType.ConnectResponse for m in client_msgs))
        res_data = parse_connect_response(client_msgs[0][1])
        self.assertTrue(res_data["accepted"])
        print(f" [PASS] Handshake & permission flow verified for session {self.session_id}")

    def test_round_trip_latency(self):
        """Measures ping-pong round trip latency over Cloud Relay."""
        latencies = []
        for i in range(5):
            t0 = time.perf_counter()
            # Send Heartbeat from client to host
            ping = serialize_message(MessageType.Heartbeat, bytes([i]))
            self.client_ws.send_binary(ping)

            # Host receives
            host_recv = self.host_ws.recv_frame(timeout=3.0)
            self.assertGreater(len(host_recv), 0, "Host timeout on ping")

            # Host echoes back
            self.host_ws.send_binary(host_recv)

            # Client receives echo
            client_recv = self.client_ws.recv_frame(timeout=3.0)
            self.assertGreater(len(client_recv), 0, "Client timeout on pong")
            rtt_ms = (time.perf_counter() - t0) * 1000.0
            latencies.append(rtt_ms)

        avg_latency = sum(latencies) / len(latencies)
        print(f" [PASS] Measured Cloud Relay Round-Trip Latency: min={min(latencies):.1f}ms, avg={avg_latency:.1f}ms, max={max(latencies):.1f}ms")
        self.assertLess(avg_latency, 1000.0, f"Average relay latency too high: {avg_latency:.1f}ms")

    def test_mouse_and_keyboard_streaming(self):
        """Verifies low-latency mouse and keyboard event transmission from client to host."""
        # 1. Mouse move + click
        mouse_pkt = serialize_mouse_event(0.42, 0.73, button_id=0, is_down=True, wheel_delta=0)
        self.client_ws.send_binary(mouse_pkt)

        msgs = self.recv_protocol_messages(self.host_ws, timeout=4.0)
        self.assertTrue(any(m[0] == MessageType.MouseEvent for m in msgs))
        m_ev = parse_mouse_event(msgs[0][1])
        self.assertAlmostEqual(m_ev["x"], 0.42, places=2)
        self.assertAlmostEqual(m_ev["y"], 0.73, places=2)
        self.assertTrue(m_ev["isDown"])

        # 2. Keyboard event
        kbd_pkt = serialize_keyboard_event(0x0D, True) # VK_RETURN
        self.client_ws.send_binary(kbd_pkt)

        msgs = self.recv_protocol_messages(self.host_ws, timeout=4.0)
        self.assertTrue(any(m[0] == MessageType.KeyboardEvent for m in msgs))
        k_ev = parse_keyboard_event(msgs[0][1])
        self.assertEqual(k_ev["virtualKey"], 0x0D)
        self.assertTrue(k_ev["isDown"])
        print(" [PASS] Mouse and Keyboard event streaming verified")

    def test_audio_streaming(self):
        """Verifies audio packet transmission from host to client."""
        # Host streams audio to client
        pcm_test_data = b"\x12\x34\x56\x78" * 240 # 960 bytes of PCM
        audio_pkt = serialize_audio(48000, 2, 16, pcm_test_data)
        self.host_ws.send_binary(audio_pkt)

        recv = self.client_ws.recv_frame(timeout=3.0)
        msgs = deserialize_messages(recv)
        self.assertTrue(any(m[0] == MessageType.AudioData for m in msgs))
        audio = parse_audio(msgs[0][1])
        self.assertEqual(audio["sampleRate"], 48000)
        self.assertEqual(audio["channels"], 2)
        self.assertEqual(audio["bitsPerSample"], 16)
        self.assertEqual(audio["pcmData"], pcm_test_data)
        print(" [PASS] High-fidelity PCM Audio streaming verified")

    def test_frame_streaming(self):
        """Verifies screen frame streaming from host to client."""
        fake_jpeg = b"\xFF\xD8\xFF" + (b"\xAA" * 2048) + b"\xFF\xD9"
        frame_pkt = serialize_frame(
            frame_num=101, total_w=1920, total_h=1080, update_w=1920, update_h=1080,
            target_x=0, target_y=0, bpp=32, is_delta=False, timestamp_ms=int(time.time() * 1000),
            jpeg_bytes=fake_jpeg
        )
        self.host_ws.send_binary(frame_pkt)

        msgs = self.recv_protocol_messages(self.client_ws, timeout=5.0)
        self.assertTrue(any(m[0] == MessageType.FrameData for m in msgs))
        f_data = parse_frame(msgs[0][1])
        self.assertEqual(f_data["frameNumber"], 101)
        self.assertEqual(f_data["totalWidth"], 1920)
        self.assertEqual(f_data["totalHeight"], 1080)
        self.assertEqual(f_data["jpegBytes"], fake_jpeg)
        print(" [PASS] Screen Frame streaming verified (1920x1080 full scan)")

if __name__ == '__main__':
    unittest.main()
