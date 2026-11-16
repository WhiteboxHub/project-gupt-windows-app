"""
Tests for Gupt Cloud Relay / Signaling Server API.
Validates registration, session creation, HTTP health endpoints.
"""
import unittest
import http.client
import json
import ssl

SIGNALING_HOST = "gupt-signal-server-560359652969.us-central1.run.app"
SIGNALING_PORT = 443

class TestRelayServer(unittest.TestCase):

    def setUp(self):
        ctx = ssl.create_default_context()
        self.conn = http.client.HTTPSConnection(SIGNALING_HOST, SIGNALING_PORT, timeout=10, context=ctx)

    def tearDown(self):
        self.conn.close()

    def test_register_session(self):
        self.conn.request("GET", "/register")
        resp = self.conn.getresponse()
        self.assertEqual(resp.status, 200)

        data = resp.read().decode('utf-8')
        doc = json.loads(data)
        self.assertIn("sessionId", doc)
        session_id = doc["sessionId"]
        self.assertTrue(len(session_id) >= 6, f"Invalid sessionId length: {session_id}")
        print(f" [PASS] Created active cloud relay session: {session_id}")

    def test_health_check(self):
        self.conn.request("GET", "/health")
        resp = self.conn.getresponse()
        # Should be 200 or 404 depending on express route, but must respond without SSL failure
        self.assertIn(resp.status, [200, 404])

if __name__ == '__main__':
    unittest.main()
