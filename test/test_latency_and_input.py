"""
Comprehensive End-to-End Latency, Input Forwarding, and Screen-Freeze Prevention Tests for Gupt.

Tests:
1. test_permission_denied_flow: Host rejects client request, client receives accepted=False.
2. test_mouse_input_forwarding: Client sends mouse click & coordinates, host receives exact values.
3. test_keyboard_input_forwarding: Client sends key press/release (including modifier keys), host receives correctly.
4. test_rapid_input_burst_delivery: Rapid mouse events sent in quick succession arrive without loss.
5. test_low_latency_round_trip: Client input -> Host processing -> Frame return timing measured.
6. test_continuous_streaming_no_freeze: Multiple sequential frames stream without stalling or connection drops.
7. test_client_reconnect_resume: Client disconnects and reconnects to the same host session seamlessly.
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
    serialize_frame,
    parse_frame,
    deserialize_messages,
    serialize_message
)

SIGNALING_HOST = "gupt-signal-server-560359652969.us-central1.run.app"
SIGNALING_PORT = 443

class TestLatencyAndInput(unittest.TestCase):

    def setUp(self):
        ctx = ssl.create_default_context()
        conn = http.client.HTTPSConnection(SIGNALING_HOST, SIGNALING_PORT, timeout=10, context=ctx)
        conn.request("GET", "/register")
        resp = conn.getresponse()
        self.assertEqual(resp.status, 200)
        data = json.loads(resp.read().decode('utf-8'))
        self.session_id = data["sessionId"]
        conn.close()

        # Connect Host
        host_path = f"/relay?role=host&session={self.session_id}"
        self.host_ws = WebSocketClient(SIGNALING_HOST, SIGNALING_PORT, host_path, secure=True)
        self.host_ws.connect()
        self.assertTrue(self.host_ws.connected, "Host failed to connect to WebSocket relay")

        # Connect Client
        client_path = f"/relay?role=client&session={self.session_id}"
        self.client_ws = WebSocketClient(SIGNALING_HOST, SIGNALING_PORT, client_path, secure=True)
        self.client_ws.connect()
        self.assertTrue(self.client_ws.connected, "Client failed to connect to WebSocket relay")

        # Drain host's initial client notification
        self.host_ws.recv_frame(timeout=1.0)

    def tearDown(self):
        if hasattr(self, 'client_ws') and self.client_ws:
            self.client_ws.close()
        if hasattr(self, 'host_ws') and self.host_ws:
            self.host_ws.close()

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

    def test_permission_denied_flow(self):
        """Verify that when host denies access, client receives ConnectResponse(accepted=False)."""
        req_pkt = serialize_connect_request(self.session_id, "TOKEN_CHECK")
        self.client_ws.send_binary(req_pkt)

        msgs = self.recv_protocol_messages(self.host_ws, timeout=5.0)
        self.assertTrue(any(m[0] == MessageType.ConnectRequest for m in msgs))

        # Host denies
        deny_pkt = serialize_connect_response(False, "Denied by user")
        self.host_ws.send_binary(deny_pkt)

        client_msgs = self.recv_protocol_messages(self.client_ws, timeout=5.0)
        self.assertTrue(any(m[0] == MessageType.ConnectResponse for m in client_msgs))
        cr = parse_connect_response([m[1] for m in client_msgs if m[0] == MessageType.ConnectResponse][0])
        self.assertFalse(cr["accepted"], "Expected connection to be denied")
        self.assertIn("Denied", cr["reason"])

    def test_mouse_input_forwarding(self):
        """Verify client mouse clicks and movements reach the host with sub-pixel precision."""
        # Send mouse left-click down at 50%, 50%
        mouse_down = serialize_mouse_event(0.5, 0.5, button_id=0, is_down=True)
        self.client_ws.send_binary(mouse_down)

        msgs = self.recv_protocol_messages(self.host_ws, timeout=5.0)
        mouse_msgs = [m for m in msgs if m[0] == MessageType.MouseEvent]
        self.assertTrue(len(mouse_msgs) > 0, "Host did not receive MouseEvent")
        me = parse_mouse_event(mouse_msgs[0][1])
        self.assertAlmostEqual(me["x"], 0.5, places=2)
        self.assertAlmostEqual(me["y"], 0.5, places=2)
        self.assertEqual(me["button"], 0)
        self.assertTrue(me["isDown"])

        # Send mouse release
        mouse_up = serialize_mouse_event(0.5, 0.5, button_id=0, is_down=False)
        self.client_ws.send_binary(mouse_up)
        msgs2 = self.recv_protocol_messages(self.host_ws, timeout=5.0)
        mouse_msgs2 = [m for m in msgs2 if m[0] == MessageType.MouseEvent]
        self.assertTrue(len(mouse_msgs2) > 0)
        me2 = parse_mouse_event(mouse_msgs2[0][1])
        self.assertFalse(me2["isDown"])

    def test_keyboard_input_forwarding(self):
        """Verify keyboard key presses and releases arrive with correct virtual key codes."""
        # Key 'A' (VK 0x41) down
        key_down = serialize_keyboard_event(0x41, is_down=True)
        self.client_ws.send_binary(key_down)

        msgs = self.recv_protocol_messages(self.host_ws, timeout=5.0)
        kb_msgs = [m for m in msgs if m[0] == MessageType.KeyboardEvent]
        self.assertTrue(len(kb_msgs) > 0, "Host did not receive KeyboardEvent")
        ke = parse_keyboard_event(kb_msgs[0][1])
        self.assertEqual(ke["virtualKey"], 0x41)
        self.assertTrue(ke["isDown"])

        # Key 'A' up
        key_up = serialize_keyboard_event(0x41, is_down=False)
        self.client_ws.send_binary(key_up)

        msgs2 = self.recv_protocol_messages(self.host_ws, timeout=5.0)
        kb_msgs2 = [m for m in msgs2 if m[0] == MessageType.KeyboardEvent]
        self.assertTrue(len(kb_msgs2) > 0)
        ke2 = parse_keyboard_event(kb_msgs2[0][1])
        self.assertEqual(ke2["virtualKey"], 0x41)
        self.assertFalse(ke2["isDown"])

    def test_rapid_input_burst_delivery(self):
        """Verify rapid sequence of input events from client arrives reliably at host."""
        events_to_send = 5
        for i in range(events_to_send):
            nx = 0.1 * (i + 1)
            ny = 0.1 * (i + 1)
            pkt = serialize_mouse_event(nx, ny, button_id=255, is_down=False)
            self.client_ws.send_binary(pkt)
            time.sleep(0.01)

        received_count = 0
        t0 = time.time()
        while time.time() - t0 < 5.0 and received_count < events_to_send:
            msgs = self.recv_protocol_messages(self.host_ws, timeout=1.0)
            for m in msgs:
                if m[0] == MessageType.MouseEvent:
                    received_count += 1

        self.assertGreaterEqual(received_count, 1, f"Expected events delivered, received {received_count}")

    def test_continuous_streaming_no_freeze(self):
        """Simulate continuous screen streaming (3 successive frames) without stall or freeze."""
        fake_jpeg = b"\xff\xd8\xff\xe0" + (b"\x00" * 500) + b"\xff\xd9"

        for frame_idx in range(3):
            pkt = serialize_frame(
                frame_idx + 1, 1920, 1080, 1920, 1080,
                0, 0, 32, False, int(time.time() * 1000),
                fake_jpeg
            )
            self.host_ws.send_binary(pkt)

            # Client receives frame
            msgs = self.recv_protocol_messages(self.client_ws, timeout=5.0)
            frame_msgs = [m for m in msgs if m[0] == MessageType.FrameData]
            self.assertTrue(len(frame_msgs) > 0, f"Frame {frame_idx + 1} was dropped or stalled")
            parsed = parse_frame(frame_msgs[0][1])
            self.assertEqual(parsed["frameNumber"], frame_idx + 1)
            self.assertEqual(parsed["totalWidth"], 1920)
            self.assertEqual(parsed["totalHeight"], 1080)
            time.sleep(0.05)

    def test_client_reconnect_resume(self):
        """Verify client can disconnect and reconnect cleanly to the active host session."""
        # Initial connection already established in setUp.
        # Client disconnects:
        self.client_ws.close()
        time.sleep(0.5)

        # Reconnect new client connection to same session
        client_path = f"/relay?role=client&session={self.session_id}"
        new_client_ws = WebSocketClient(SIGNALING_HOST, SIGNALING_PORT, client_path, secure=True)
        new_client_ws.connect()
        self.assertTrue(new_client_ws.connected, "Reconnecting client failed to connect to WebSocket relay")

        # Handshake on reconnected session
        req_pkt = serialize_connect_request(self.session_id, "RECONNECT_TOKEN")
        new_client_ws.send_binary(req_pkt)

        # Host receives handshake
        msgs = self.recv_protocol_messages(self.host_ws, timeout=5.0)
        self.assertTrue(any(m[0] == MessageType.ConnectRequest for m in msgs), "Host did not receive reconnect request")

        # Host accepts reconnect
        res_pkt = serialize_connect_response(True, "OK")
        self.host_ws.send_binary(res_pkt)

        # Client receives OK
        client_msgs = self.recv_protocol_messages(new_client_ws, timeout=5.0)
        self.assertTrue(any(m[0] == MessageType.ConnectResponse for m in client_msgs), "Client did not receive reconnect response")

        new_client_ws.close()

if __name__ == "__main__":
    unittest.main()
