# SPDX-License-Identifier: MIT
"""Tests for the OBS script, run with unittest so there is nothing to install.

The script imports cleanly without obspython, which is what lets the protocol, the state
machine and the transport be exercised here rather than only inside OBS.
"""

import json
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import obs_nvidia_notify as script  # noqa: E402


class ProtocolTests(unittest.TestCase):
    def test_encode_decode_round_trip(self):
        protocol = script.Protocol()
        line = protocol.encode(script.MSG_EVENT, {"kind": "recording.started"})
        self.assertTrue(line.endswith("\n"))
        ok, msg_type, data, error = script.Protocol.decode(line.strip())
        self.assertTrue(ok, error)
        self.assertEqual(msg_type, script.MSG_EVENT)
        self.assertEqual(data["kind"], "recording.started")

    def test_sequence_numbers_increase(self):
        protocol = script.Protocol()
        first = json.loads(protocol.encode(script.MSG_HEARTBEAT))
        second = json.loads(protocol.encode(script.MSG_HEARTBEAT))
        self.assertEqual(second["seq"], first["seq"] + 1)

    def test_rubbish_is_rejected_without_raising(self):
        for bad in ["", "not json", "[1,2,3]", "{}", '{"v":99,"type":"event"}']:
            ok, _, _, error = script.Protocol.decode(bad)
            self.assertFalse(ok, bad)
            self.assertTrue(error)

    def test_an_over_long_message_becomes_an_error_not_a_truncation(self):
        # Truncating the payload would produce a message the overlay parses and then believes.
        protocol = script.Protocol()
        line = protocol.encode(script.MSG_EVENT, {"detail": "x" * (script.MAX_MESSAGE_BYTES * 2)})
        self.assertLessEqual(len(line), script.MAX_MESSAGE_BYTES + 1)
        doc = json.loads(line)
        self.assertEqual(doc["type"], script.MSG_ERROR)
        self.assertEqual(doc["data"]["code"], "message_too_large")

    def test_output_is_ascii_safe(self):
        # A scene name with an unpaired surrogate is legal on Windows and must not produce a
        # line the overlay has to reject as invalid UTF-8.
        protocol = script.Protocol()
        line = protocol.encode(script.MSG_EVENT, {"scene": "caf\u00e9 \ud800"})
        line.encode("ascii")   # raises if anything non-ASCII escaped


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.bridge = script.ObsBridge()

    def test_recording_duration_excludes_paused_time(self):
        self.bridge.apply(script.EV_RECORDING_STARTED, 1000)
        self.bridge.apply(script.EV_RECORDING_PAUSED, 3000)
        self.bridge.apply(script.EV_RECORDING_RESUMED, 5000)
        event = self.bridge.apply(script.EV_RECORDING_STOPPED, 11000)
        self.assertEqual(event["duration_ms"], 8000)

    def test_duration_is_computed_while_still_paused(self):
        self.bridge.apply(script.EV_RECORDING_STARTED, 1000)
        self.bridge.apply(script.EV_RECORDING_PAUSED, 3000)
        event = self.bridge.apply(script.EV_RECORDING_STOPPED, 11000)
        self.assertEqual(event["duration_ms"], 2000)

    def test_paused_time_does_not_carry_into_the_next_recording(self):
        self.bridge.apply(script.EV_RECORDING_STARTED, 0)
        self.bridge.apply(script.EV_RECORDING_PAUSED, 1000)
        self.bridge.apply(script.EV_RECORDING_RESUMED, 6000)
        self.bridge.apply(script.EV_RECORDING_STOPPED, 10000)
        self.bridge.apply(script.EV_RECORDING_STARTED, 20000)
        event = self.bridge.apply(script.EV_RECORDING_STOPPED, 25000)
        self.assertEqual(event["duration_ms"], 5000)

    def test_state_tracks_every_output(self):
        self.bridge.apply(script.EV_RECORDING_STARTED, 1000)
        self.bridge.apply(script.EV_REPLAY_STARTED, 1000)
        self.bridge.apply(script.EV_STREAM_STARTED, 1000)
        self.bridge.apply(script.EV_VIRTUALCAM_STARTED, 1000)
        state = self.bridge.snapshot()
        self.assertEqual(state["recording"]["state"], "active")
        self.assertEqual(state["replay"]["state"], "active")
        self.assertEqual(state["stream"]["state"], "active")
        self.assertEqual(state["virtual_cam"]["state"], "active")

        self.bridge.apply(script.EV_RECORDING_STOPPED, 2000)
        self.assertEqual(self.bridge.snapshot()["recording"]["state"], "idle")

    def test_reconnect_attempts_are_counted_and_reset(self):
        self.bridge.apply(script.EV_STREAM_STARTED, 0)
        first = self.bridge.apply(script.EV_STREAM_RECONNECTING, 1000)
        second = self.bridge.apply(script.EV_STREAM_RECONNECTING, 2000)
        self.assertEqual(first["attempt"], 1)
        self.assertEqual(second["attempt"], 2)
        self.bridge.apply(script.EV_STREAM_RECONNECTED, 3000)
        self.assertEqual(self.bridge.snapshot()["stream"]["reconnect_attempt"], 0)

    def test_scene_change_records_where_it_came_from(self):
        self.bridge.apply(script.EV_SCENE_CHANGED, 0, {"scene": "Starting soon"})
        event = self.bridge.apply(script.EV_SCENE_CHANGED, 1000, {"scene": "Gameplay"})
        self.assertEqual(event["previous_scene"], "Starting soon")
        self.assertEqual(self.bridge.snapshot()["current_scene"], "Gameplay")

    def test_full_paths_are_withheld_by_default(self):
        # A recording path contains the user's account name, so only the file name crosses the
        # pipe unless they ask otherwise.
        event = self.bridge.sanitise({"path": "C:\\Users\\Someone\\Videos\\clip.mkv"})
        self.assertEqual(event["path"], "clip.mkv")

        self.bridge.settings["send_full_paths"] = True
        event = self.bridge.sanitise({"path": "C:\\Users\\Someone\\Videos\\clip.mkv"})
        self.assertEqual(event["path"], "C:\\Users\\Someone\\Videos\\clip.mkv")

    def test_windows_paths_are_trimmed_on_any_host(self):
        # os.path.basename would leave a Windows path untouched on Linux, which is the whole
        # path leaking on the one platform nobody tests on.
        self.assertEqual(script.file_name_of("C:\\Users\\Someone\\Videos\\clip.mkv"),
                         "clip.mkv")
        self.assertEqual(script.file_name_of("/home/someone/Videos/clip.mkv"), "clip.mkv")
        self.assertEqual(script.file_name_of("clip.mkv"), "clip.mkv")
        self.assertEqual(script.file_name_of(""), "")

    def test_file_sizes_can_be_withheld(self):
        self.bridge.settings["send_file_sizes"] = False
        self.assertNotIn("size_bytes", self.bridge.sanitise({"size_bytes": 1024}))

    def test_category_switches_gate_whole_outputs(self):
        self.bridge.settings["streaming"] = False
        self.assertFalse(self.bridge.wants(script.EV_STREAM_STARTED))
        self.assertFalse(self.bridge.wants(script.EV_STREAM_RECONNECTING))
        self.assertTrue(self.bridge.wants(script.EV_RECORDING_STARTED))

    def test_every_event_kind_belongs_to_a_group(self):
        # An event with no group would be governed by no switch, so the user could not turn it
        # off and would not know why.
        kinds = [value for name, value in vars(script).items()
                 if name.startswith("EV_") and isinstance(value, str)]
        for kind in kinds:
            self.assertIn(kind, script.GROUP_OF, kind)


class ClientSessionTests(unittest.TestCase):
    def test_the_queue_is_bounded_and_drops_the_oldest(self):
        session = script.ClientSession(1, queue_depth=3)
        for i in range(10):
            session.enqueue("line %d\n" % i)
        self.assertLessEqual(session.queue.qsize(), 3)
        self.assertGreater(session.messages_dropped, 0)
        # Losing messages is repaired rather than left as a gap the client cannot know about.
        self.assertTrue(session.needs_snapshot)
        # What survives is the newest, which is what describes the world as it is now.
        remaining = []
        while not session.queue.empty():
            remaining.append(session.queue.get_nowait())
        self.assertEqual(remaining[-1], "line 9\n")


@unittest.skipIf(script.IS_WINDOWS, "the AF_UNIX server is the POSIX transport")
class UnixServerTests(unittest.TestCase):
    """Drives the real server over a real socket, as the overlay does."""

    def setUp(self):
        self.endpoint = os.path.join(
            os.environ.get("TMPDIR", "/tmp"), "obsn-test-%d.sock" % os.getpid())
        self.received = []
        self.server = script.UnixSocketServer(self.endpoint, self._on_message)
        self.assertTrue(self.server.start())

    def tearDown(self):
        self.server.stop()

    def _on_message(self, server, session, msg_type, data):
        self.received.append((msg_type, data))
        if msg_type == script.MSG_CLIENT_HELLO:
            server.send_to(session, script.MSG_HELLO, {"script_version": script.SCRIPT_VERSION})
        elif msg_type == script.MSG_PING:
            server.send_to(session, script.MSG_PONG)

    def _connect(self):
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.settimeout(3.0)
        conn.connect(self.endpoint)
        return conn

    def _read_line(self, conn, buffer):
        deadline = time.time() + 3.0
        while b"\n" not in buffer[0] and time.time() < deadline:
            buffer[0] += conn.recv(4096)
        line, _, buffer[0] = buffer[0].partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def test_handshake_and_broadcast(self):
        conn = self._connect()
        buffer = [b""]
        conn.sendall(script.Protocol().encode(
            script.MSG_CLIENT_HELLO, {"process": "game.exe"}).encode("utf-8"))

        hello = self._read_line(conn, buffer)
        self.assertEqual(hello["type"], script.MSG_HELLO)
        self.assertEqual(hello["data"]["script_version"], script.SCRIPT_VERSION)

        self.server.broadcast(script.MSG_EVENT, {"kind": "recording.started"})
        # Heartbeats share the stream, so the event is looked for rather than assumed to be next.
        deadline = time.time() + 3.0
        while time.time() < deadline:
            message = self._read_line(conn, buffer)
            if message["type"] == script.MSG_EVENT:
                self.assertEqual(message["data"]["kind"], "recording.started")
                break
        else:
            self.fail("the event never arrived")
        conn.close()

    def test_ping_is_answered(self):
        conn = self._connect()
        buffer = [b""]
        conn.sendall(script.Protocol().encode(script.MSG_PING).encode("utf-8"))
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if self._read_line(conn, buffer)["type"] == script.MSG_PONG:
                break
        else:
            self.fail("no pong")
        conn.close()

    def test_the_client_cap_is_enforced(self):
        self.server.max_clients = 1
        first = self._connect()
        time.sleep(0.3)
        second = self._connect()
        time.sleep(0.3)
        self.assertGreater(self.server.connections_rejected, 0)
        self.assertLessEqual(self.server.client_count(), 1)
        first.close()
        second.close()

    def test_a_disconnect_is_noticed_and_the_session_reaped(self):
        conn = self._connect()
        time.sleep(0.3)
        self.assertEqual(self.server.client_count(), 1)
        conn.close()
        deadline = time.time() + 3.0
        while self.server.client_count() > 0 and time.time() < deadline:
            time.sleep(0.05)
        self.assertEqual(self.server.client_count(), 0)


class ServiceTests(unittest.TestCase):
    @unittest.skipIf(script.IS_WINDOWS, "uses the POSIX transport")
    def test_events_flow_through_the_dispatcher_to_a_client(self):
        endpoint = os.path.join(os.environ.get("TMPDIR", "/tmp"),
                                "obsn-service-%d.sock" % os.getpid())
        service = script.Service()
        self.assertTrue(service.start(endpoint))
        try:
            conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            conn.settimeout(3.0)
            conn.connect(endpoint)
            conn.sendall(script.Protocol().encode(
                script.MSG_CLIENT_HELLO, {"process": "game.exe"}).encode("utf-8"))

            # Broadcast reaches the clients attached at that moment, so the accept has to
            # have completed first. A client that attaches later is caught up by a snapshot,
            # not by replayed events.
            deadline = time.time() + 3.0
            while service.server.client_count() == 0 and time.time() < deadline:
                time.sleep(0.02)
            self.assertEqual(service.server.client_count(), 1)

            service.submit(script.EV_RECORDING_STARTED, {})
            buffer = b""
            deadline = time.time() + 4.0
            seen = None
            while time.time() < deadline and seen is None:
                try:
                    buffer += conn.recv(4096)
                except socket.timeout:
                    break
                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    message = json.loads(line.decode("utf-8"))
                    if message["type"] == script.MSG_EVENT:
                        seen = message["data"]
                        break
            self.assertIsNotNone(seen, "the event never reached the client")
            self.assertEqual(seen["kind"], "recording.started")
            conn.close()
        finally:
            service.stop()

    def test_a_disabled_category_never_reaches_the_dispatcher(self):
        service = script.Service()
        service.bridge.settings["streaming"] = False
        service._running = True          # noqa: SLF001 - driving the gate without a transport
        service.submit(script.EV_STREAM_STARTED, {})
        service.submit(script.EV_RECORDING_STARTED, {})
        service._running = False         # noqa: SLF001
        queued = []
        while not service._dispatch.empty():   # noqa: SLF001
            queued.append(service._dispatch.get_nowait()[0])  # noqa: SLF001
        self.assertNotIn(script.EV_STREAM_STARTED, queued)
        self.assertIn(script.EV_RECORDING_STARTED, queued)


class EndpointTests(unittest.TestCase):
    def test_an_explicit_absolute_endpoint_is_used_as_given(self):
        if script.IS_WINDOWS:
            self.assertEqual(script.default_endpoint("\\\\.\\pipe\\custom"),
                             "\\\\.\\pipe\\custom")
        else:
            self.assertEqual(script.default_endpoint("/tmp/custom.sock"), "/tmp/custom.sock")

    def test_a_bare_name_is_placed_in_the_platform_namespace(self):
        endpoint = script.default_endpoint("mine")
        if script.IS_WINDOWS:
            self.assertTrue(endpoint.startswith("\\\\.\\pipe\\"))
        else:
            self.assertTrue(endpoint.endswith("mine.sock"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
