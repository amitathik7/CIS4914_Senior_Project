"""How the gateway treats request bodies: bounded in size and in total time, and a refusal is answered as reliably as a success.

The real server on an ephemeral loopback port. Every request here is refused or fails before any replay would start, so no
executable is needed. Run from python/strategy_lab:  .venv\\Scripts\\python.exe -m unittest discover -s tests
"""

import http.client
import json
import socket
import threading
import time
import unittest
from unittest import mock

from strategy_lab_ui import bridge, gateway

HEAD = b"POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n"


class RequestBodies(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = gateway.make_server(gateway.Gateway(bridge.find_runner), 0)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def call(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def converse(self, request: bytes, wait: float = 8.0) -> bytes:
        """Send raw bytes and read until the gateway closes the connection (or `wait` seconds pass)."""
        with socket.create_connection(("127.0.0.1", self.port), timeout=wait) as sock:
            sock.sendall(request)
            reply = b""
            try:
                while chunk := sock.recv(65536):
                    reply += chunk
            except (socket.timeout, ConnectionError):
                pass
            return reply

    def test_a_refusal_raised_while_reading_the_body_is_answered_as_reliably(self):
        # The content-type refusal is raised by the body reader itself, before a byte of the body is read.
        for attempt in range(25):
            status, response = self.call("POST", "/lab/v1/check", body=b"x" * 300_000, headers={"Content-Type": "text/plain"})
            self.assertEqual((status, json.loads(response)["error"]["code"]), (400, "bad_request"), f"attempt {attempt}")
        status, response = self.call("POST", "/lab/v1/check", body=b"x" * (gateway.MAX_BODY_BYTES + 1024), headers={"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(response)["error"]["code"]), (413, "input_too_large"))

    def test_a_body_that_never_completes_is_given_up_on_not_waited_for_forever(self):
        with mock.patch.object(gateway, "BODY_READ_S", 0.4):
            started = time.monotonic()
            reply = self.converse(HEAD % (b"/lab/v1/check", 1000) + b"{}")          # 998 bytes short, and the client just waits
        self.assertTrue(reply.startswith(b"HTTP/1.0 400"), reply[:80])
        self.assertIn(b"did not arrive completely and in time", reply)
        self.assertLess(time.monotonic() - started, 5)

    def test_a_body_cut_short_is_never_processed_as_if_it_were_complete(self):
        # "{}" is valid JSON, so a truncated body must be refused for being truncated, not read as an (empty) request.
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
            sock.sendall(HEAD % (b"/lab/v1/check", 1000) + b"{}")
            sock.shutdown(socket.SHUT_WR)                                              # the client is done sending, 998 bytes short
            reply = b""
            while chunk := sock.recv(4096):
                reply += chunk
        self.assertTrue(reply.startswith(b"HTTP/1.0 400"), reply[:80])
        self.assertIn(b"did not arrive completely and in time", reply)

    def test_a_client_that_hangs_up_mid_body_is_not_an_error_of_the_gateway(self):
        with mock.patch("sys.stderr"):
            with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
                sock.sendall(HEAD % (b"/lab/v1/check", 1000) + b"{}")
            time.sleep(0.3)                                                            # the handler sees EOF and answers into the void
        self.assertEqual(self.call("GET", "/lab/v1/status")[0], 200)                    # and the server carries on

    def test_a_body_trickled_in_cannot_hold_a_refusal_open(self):
        # One byte at a time never finishes 100,000 bytes; the time allowed is a total, not per read.
        with mock.patch.object(gateway, "DRAIN_S", 0.5):
            with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
                sock.sendall(HEAD % (b"/lab/v1/nope", 100_000))
                started, reply = time.monotonic(), b""
                while not reply and time.monotonic() - started < 4:
                    try:
                        sock.sendall(b"x")
                        sock.settimeout(0.1)
                        reply = sock.recv(4096)
                    except socket.timeout:
                        pass
                    except OSError:
                        break
        self.assertTrue(reply.startswith(b"HTTP/1.0 404"), reply[:80])
        self.assertLess(time.monotonic() - started, 3)

    def test_a_body_that_was_read_is_not_waited_for_again_before_the_answer(self):
        with mock.patch.object(gateway, "DRAIN_S", 3.0):
            started = time.monotonic()
            status, response = self.call("POST", "/lab/v1/check", body=b"{}", headers={"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(response)["error"]["code"]), (400, "bad_request"))
        self.assertLess(time.monotonic() - started, 1.5)

    def test_one_request_per_connection_and_a_body_is_never_read_as_the_next_request(self):
        smuggled = b"GET /lab/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
        reply = self.converse(HEAD % (b"/lab/v1/nope", len(smuggled)) + smuggled)
        self.assertEqual(reply.count(b"HTTP/1."), 1, reply)                            # one answer, then the server closed the connection
        self.assertTrue(reply.startswith(b"HTTP/1.0 404"))
        self.assertIn(b"Connection: close", reply)


if __name__ == "__main__":
    unittest.main()
