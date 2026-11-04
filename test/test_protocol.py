"""
Unit tests for Gupt remote desktop protocol serialization & deserialization.
"""
import unittest
from protocol import (
    MessageType,
    serialize_message,
    deserialize_messages,
    serialize_connect_request,
    parse_connect_request,
    serialize_connect_response,
    parse_connect_response,
    serialize_mouse_event,
    parse_mouse_event,
    serialize_keyboard_event,
    parse_keyboard_event,
    serialize_frame,
    parse_frame,
    serialize_audio,
    parse_audio,
    serialize_clipboard_text
)

class TestProtocol(unittest.TestCase):

    def test_message_header_and_stream(self):
        msg1 = serialize_message(MessageType.Heartbeat, b"\x01")
        msg2 = serialize_clipboard_text("Hello Gupt!")
        combined = msg1 + msg2

        deserialized = deserialize_messages(combined)
        self.assertEqual(len(deserialized), 2)
        self.assertEqual(deserialized[0][0], MessageType.Heartbeat)
        self.assertEqual(deserialized[0][1], b"\x01")
        self.assertEqual(deserialized[1][0], MessageType.ClipboardText)
        self.assertEqual(deserialized[1][1].decode('utf-8'), "Hello Gupt!")

    def test_connect_request(self):
        pkt = serialize_connect_request("abc12345", "MY_TOKEN_XYZ")
        msgs = deserialize_messages(pkt)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(msgs[0][0], MessageType.ConnectRequest)
        parsed = parse_connect_request(msgs[0][1])
        self.assertEqual(parsed["sessionId"], "abc12345")
        self.assertEqual(parsed["token"], "MY_TOKEN_XYZ")

    def test_connect_response(self):
        pkt_ok = serialize_connect_response(True, "OK")
        msgs_ok = deserialize_messages(pkt_ok)
        self.assertEqual(len(msgs_ok), 1)
        parsed_ok = parse_connect_response(msgs_ok[0][1])
        self.assertTrue(parsed_ok["accepted"])
        self.assertEqual(parsed_ok["reason"], "OK")

        pkt_denied = serialize_connect_response(False, "User rejected access")
        msgs_denied = deserialize_messages(pkt_denied)
        parsed_denied = parse_connect_response(msgs_denied[0][1])
        self.assertFalse(parsed_denied["accepted"])
        self.assertEqual(parsed_denied["reason"], "User rejected access")

    def test_mouse_event(self):
        pkt = serialize_mouse_event(0.55, 0.88, button_id=1, is_down=True, wheel_delta=-120)
        msgs = deserialize_messages(pkt)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(msgs[0][0], MessageType.MouseEvent)
        parsed = parse_mouse_event(msgs[0][1])
        self.assertAlmostEqual(parsed["x"], 0.55, places=3)
        self.assertAlmostEqual(parsed["y"], 0.88, places=3)
        self.assertEqual(parsed["button"], 1)
        self.assertTrue(parsed["isDown"])
        self.assertEqual(parsed["wheelDelta"], -120)

    def test_keyboard_event(self):
        # VK_A = 0x41
        pkt = serialize_keyboard_event(0x41, True)
        msgs = deserialize_messages(pkt)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(msgs[0][0], MessageType.KeyboardEvent)
        parsed = parse_keyboard_event(msgs[0][1])
        self.assertEqual(parsed["virtualKey"], 0x41)
        self.assertTrue(parsed["isDown"])

    def test_frame_serialization(self):
        dummy_jpeg = b"\xFF\xD8\xFF\xE0\x00\x10JFIF" + (b"\x00" * 100) + b"\xFF\xD9"
        pkt = serialize_frame(
            frame_num=42, total_w=1920, total_h=1080, update_w=1920, update_h=1080,
            target_x=0, target_y=0, bpp=32, is_delta=False, timestamp_ms=1700000000,
            jpeg_bytes=dummy_jpeg
        )
        msgs = deserialize_messages(pkt)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(msgs[0][0], MessageType.FrameData)
        parsed = parse_frame(msgs[0][1])
        self.assertEqual(parsed["frameNumber"], 42)
        self.assertEqual(parsed["totalWidth"], 1920)
        self.assertEqual(parsed["totalHeight"], 1080)
        self.assertEqual(parsed["jpegBytes"], dummy_jpeg)

    def test_audio_serialization(self):
        dummy_pcm = b"\x00\x10" * 480 # 480 samples of 16-bit audio
        pkt = serialize_audio(48000, 2, 16, dummy_pcm)
        msgs = deserialize_messages(pkt)
        self.assertEqual(len(msgs), 1)
        self.assertEqual(msgs[0][0], MessageType.AudioData)
        parsed = parse_audio(msgs[0][1])
        self.assertEqual(parsed["sampleRate"], 48000)
        self.assertEqual(parsed["channels"], 2)
        self.assertEqual(parsed["bitsPerSample"], 16)
        self.assertEqual(parsed["pcmData"], dummy_pcm)

if __name__ == '__main__':
    unittest.main()
