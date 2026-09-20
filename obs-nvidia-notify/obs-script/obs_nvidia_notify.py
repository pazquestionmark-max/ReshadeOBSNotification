# SPDX-License-Identifier: MIT
"""OBS Notify -- publishes OBS capture events to the in-game ReShade overlay.

Load this from OBS: Tools -> Scripts -> +, then pick this file.

Design notes worth knowing before changing anything here:

* **Python 3.6.8.** That is what OBS ships against on Windows, so nothing newer is used: no
  dataclasses, no f-string ``=``, no walrus, no ``list[str]`` annotations. It runs on anything
  from 3.6 up.

* **Standard library only.** No pywin32, no pip install. The Windows named pipe is created
  through ``ctypes`` against kernel32 and advapi32 directly, which is the difference between
  "load the script" and "first install these packages into the exact Python OBS is linked
  against".

* **This process is the server.** OBS runs for a whole session; games come and go. So OBS
  listens and each game's overlay connects to it, with the reconnect logic on the overlay side.
  Several games can attach at once, each getting its own pipe instance.

* **OBS's event callback never blocks.** Frontend events are put on an unbounded-but-drained
  dispatch queue and handled on our own thread; per-client write queues are bounded and drop
  the oldest on overflow, then force a resynchronisation. A game that has stopped reading
  cannot stall OBS's UI thread.

* **Nothing reaches the network.** No socket is opened on Windows at all; POSIX uses an
  AF_UNIX socket, which is a file, not a port.
"""

import json
import os
import platform
import sys
import threading
import time

try:
    import queue
except ImportError:  # pragma: no cover - Python 2 is not supported, but fail clearly
    raise RuntimeError("OBS Notify requires Python 3.6 or newer")

# Importable without OBS so the protocol and transport can be exercised by the test suite.
try:
    import obspython as obs
except ImportError:  # pragma: no cover - exercised only outside OBS
    obs = None

SCRIPT_VERSION = "1.0.0"
PROTOCOL_VERSION = 1
MAX_MESSAGE_BYTES = 65536
HEARTBEAT_INTERVAL_S = 2.0

# Matches obsn::proto on the overlay side. Changing either without the other is what the
# version fields in the handshake exist to catch.
MSG_HELLO = "hello"
MSG_STATE = "state_snapshot"
MSG_EVENT = "event"
MSG_HEARTBEAT = "heartbeat"
MSG_ERROR = "error"
MSG_CLIENT_HELLO = "client_hello"
MSG_REQUEST_SNAPSHOT = "request_snapshot"
MSG_PING = "ping"
MSG_PONG = "pong"

IS_WINDOWS = os.name == "nt"


def now_ms():
    """Wall-clock milliseconds, matching the overlay's own clock."""
    return int(time.time() * 1000)


def file_name_of(path):
    """The file name alone, splitting on both separators.

    Not os.path.basename: that splits on the host's separator only, and OBS records to Windows
    paths. A privacy guarantee that works on Windows and leaks the whole path everywhere else is
    not a guarantee. Matches obsn::file_name_of on the overlay side.
    """
    cut = max(path.rfind("/"), path.rfind("\\"))
    return path[cut + 1:] if cut >= 0 else path


def log(message):
    """Prints to OBS's script log, or to stderr when running outside OBS."""
    line = "[obs-notify] " + str(message)
    if obs is not None:
        print(line)
    else:  # pragma: no cover
        sys.stderr.write(line + "\n")


# --------------------------------------------------------------------------------------------
# Protocol
# --------------------------------------------------------------------------------------------

class Protocol(object):
    """Encodes and decodes the newline-delimited JSON envelope.

    Kept as a class with no instance state beyond the sequence counter so the tests can drive it
    without OBS, and so the one place that decides what a message looks like is findable.
    """

    def __init__(self):
        self._seq = 0
        self._lock = threading.Lock()

    def next_seq(self):
        with self._lock:
            self._seq += 1
            return self._seq

    def encode(self, msg_type, data=None):
        """Returns one complete wire line, newline included.

        Messages are compact-encoded and ASCII-escaped. The ASCII escaping is deliberate: the
        overlay validates UTF-8 on the way in, and a scene name with an unpaired surrogate --
        which Windows file systems permit -- would otherwise produce a line it has to reject.
        """
        envelope = {
            "v": PROTOCOL_VERSION,
            "seq": self.next_seq(),
            "ts": now_ms(),
            "type": msg_type,
        }
        if data is not None:
            envelope["data"] = data
        line = json.dumps(envelope, separators=(",", ":"), ensure_ascii=True)
        if len(line) + 1 > MAX_MESSAGE_BYTES:
            # Truncating the payload would produce a message the overlay parses and then
            # believes. Replacing it with an error it can report is the honest failure.
            envelope.pop("data", None)
            envelope["type"] = MSG_ERROR
            envelope["data"] = {
                "code": "message_too_large",
                "message": "a %s message exceeded %d bytes and was dropped"
                % (msg_type, MAX_MESSAGE_BYTES),
            }
            line = json.dumps(envelope, separators=(",", ":"), ensure_ascii=True)
        return line + "\n"

    @staticmethod
    def decode(line):
        """Returns (ok, type, data, error). Never raises."""
        if len(line) > MAX_MESSAGE_BYTES:
            return False, None, None, "message exceeds the size limit"
        try:
            doc = json.loads(line)
        except ValueError as exc:
            return False, None, None, "not JSON: %s" % exc
        if not isinstance(doc, dict):
            return False, None, None, "top-level value is not an object"
        version = doc.get("v", PROTOCOL_VERSION)
        if not isinstance(version, int) or version != PROTOCOL_VERSION:
            return False, None, None, "unsupported protocol version %r" % (version,)
        msg_type = doc.get("type")
        if not isinstance(msg_type, str):
            return False, None, None, "missing 'type'"
        data = doc.get("data")
        if data is not None and not isinstance(data, dict):
            return False, msg_type, None, "'data' is not an object"
        return True, msg_type, data or {}, None


# --------------------------------------------------------------------------------------------
# Transport: a server that several overlays can attach to at once
# --------------------------------------------------------------------------------------------

class ClientSession(object):
    """One connected overlay.

    Owns a bounded outbound queue and the thread that drains it. The bound is the whole point:
    a game that stops reading -- minimised, hung, mid-shader-compile -- must cost this script a
    few dropped events and nothing else.
    """

    def __init__(self, session_id, queue_depth):
        self.id = session_id
        self.queue = queue.Queue(maxsize=queue_depth)
        self.description = "client %d" % session_id
        self.process = ""
        self.connected_ms = now_ms()
        self.messages_sent = 0
        self.messages_dropped = 0
        self.needs_snapshot = False
        self.alive = True

    def enqueue(self, line):
        """Never blocks. Returns False when the line was dropped."""
        try:
            self.queue.put_nowait(line)
            return True
        except queue.Full:
            # Drop the oldest and take its place: the newest message is the one that describes
            # the world as it is now. The client is then flagged for a fresh snapshot, so what
            # it lost is repaired rather than left as a gap it cannot know about.
            try:
                self.queue.get_nowait()
            except queue.Empty:
                pass
            self.messages_dropped += 1
            self.needs_snapshot = True
            try:
                self.queue.put_nowait(line)
                return True
            except queue.Full:
                return False


class BaseServer(object):
    """Shared bookkeeping for both transports.

    The platform-specific parts are exactly three: creating the listener, accepting one
    connection, and reading/writing a connection. Everything else -- sessions, broadcast,
    heartbeat, statistics -- lives here so the two implementations cannot drift in behaviour.
    """

    def __init__(self, endpoint, on_message, max_clients=8, queue_depth=256):
        self.endpoint = endpoint
        self.on_message = on_message
        self.max_clients = max_clients
        self.queue_depth = queue_depth
        self.protocol = Protocol()

        self._sessions = []
        self._sessions_lock = threading.Lock()
        self._next_id = 1
        self._running = False
        self._threads = []
        self._error = ""

        self.connections_accepted = 0
        self.connections_rejected = 0

    # --- to be provided by the platform implementation ---
    def _listen(self):
        raise NotImplementedError

    def _accept(self):
        """Returns an opaque connection object, or None on shutdown or error."""
        raise NotImplementedError

    def _send(self, conn, data):
        raise NotImplementedError

    def _recv(self, conn, size):
        raise NotImplementedError

    def _close(self, conn):
        raise NotImplementedError

    def _unblock_accept(self):
        raise NotImplementedError

    # --- lifecycle ---
    def start(self):
        if self._running:
            return True
        try:
            self._listen()
        except Exception as exc:  # noqa: BLE001 - reported to the user, never raised into OBS
            self._error = str(exc)
            log("could not listen on %s: %s" % (self.endpoint, exc))
            return False
        self._running = True
        self._error = ""
        self._spawn(self._accept_loop, "obsn-accept")
        self._spawn(self._heartbeat_loop, "obsn-heartbeat")
        log("listening on %s" % self.endpoint)
        return True

    def stop(self):
        if not self._running:
            return
        self._running = False
        try:
            self._unblock_accept()
        except Exception:  # noqa: BLE001 - shutdown is best effort by definition
            pass
        with self._sessions_lock:
            sessions = list(self._sessions)
            self._sessions = []
        for session, conn in sessions:
            session.alive = False
            # A sentinel rather than a flag check alone: the writer may be parked in
            # Queue.get(), and this is what wakes it.
            try:
                session.queue.put_nowait(None)
            except queue.Full:
                pass
            try:
                self._close(conn)
            except Exception:  # noqa: BLE001
                pass
        for thread in self._threads:
            thread.join(timeout=2.0)
        self._threads = []
        log("stopped listening")

    def running(self):
        return self._running

    def error(self):
        return self._error

    def _spawn(self, target, name):
        thread = threading.Thread(target=target, name=name)
        thread.daemon = True
        thread.start()
        self._threads.append(thread)
        return thread

    # --- sessions ---
    def client_count(self):
        with self._sessions_lock:
            return len(self._sessions)

    def describe_clients(self):
        with self._sessions_lock:
            return [
                "%s  sent %d, dropped %d"
                % (s.description, s.messages_sent, s.messages_dropped)
                for s, _ in self._sessions
            ]

    def broadcast(self, msg_type, data=None):
        """Queues one message to every attached overlay. Safe from any thread, never blocks."""
        line = self.protocol.encode(msg_type, data)
        with self._sessions_lock:
            sessions = list(self._sessions)
        for session, _ in sessions:
            session.enqueue(line)

    def send_to(self, session, msg_type, data=None):
        session.enqueue(self.protocol.encode(msg_type, data))

    def _accept_loop(self):
        while self._running:
            conn = self._accept()
            if conn is None:
                continue
            if not self._running:
                self._close(conn)
                return
            if self.client_count() >= self.max_clients:
                # A cap rather than unbounded growth: eight games at once is already
                # implausible, and a peer that reconnects in a loop must not be able to spawn
                # threads without limit.
                self.connections_rejected += 1
                log("refusing a connection: already at %d clients" % self.max_clients)
                self._close(conn)
                continue

            session = ClientSession(self._next_id, self.queue_depth)
            self._next_id += 1
            self.connections_accepted += 1
            with self._sessions_lock:
                self._sessions.append((session, conn))
            self._spawn(lambda s=session, c=conn: self._write_loop(s, c),
                        "obsn-write-%d" % session.id)
            self._spawn(lambda s=session, c=conn: self._read_loop(s, c),
                        "obsn-read-%d" % session.id)

    def _drop(self, session, conn, why):
        session.alive = False
        with self._sessions_lock:
            self._sessions = [(s, c) for s, c in self._sessions if s is not session]
        try:
            session.queue.put_nowait(None)
        except queue.Full:
            pass
        try:
            self._close(conn)
        except Exception:  # noqa: BLE001
            pass
        log("%s disconnected (%s)" % (session.description, why))

    def _write_loop(self, session, conn):
        while self._running and session.alive:
            try:
                line = session.queue.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                return
            try:
                self._send(conn, line.encode("utf-8"))
            except Exception as exc:  # noqa: BLE001 - a dead peer is ordinary, not exceptional
                self._drop(session, conn, "write failed: %s" % exc)
                return
            session.messages_sent += 1

    def _read_loop(self, session, conn):
        pending = b""
        while self._running and session.alive:
            try:
                chunk = self._recv(conn, 4096)
            except Exception as exc:  # noqa: BLE001
                self._drop(session, conn, "read failed: %s" % exc)
                return
            if not chunk:
                self._drop(session, conn, "closed by the overlay")
                return
            pending += chunk
            if len(pending) > MAX_MESSAGE_BYTES:
                # A peer that sends a megabyte with no newline must not be able to grow this
                # buffer to a megabyte before we notice.
                self._drop(session, conn, "over-long line")
                return
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                if not raw.strip():
                    continue
                try:
                    text = raw.decode("utf-8")
                except UnicodeDecodeError:
                    self._drop(session, conn, "not UTF-8")
                    return
                ok, msg_type, data, error = Protocol.decode(text)
                if not ok:
                    log("%s sent an undecodable message: %s" % (session.description, error))
                    continue
                try:
                    self.on_message(self, session, msg_type, data)
                except Exception as exc:  # noqa: BLE001 - a handler bug must not kill the link
                    log("error handling %s: %s" % (msg_type, exc))

    def _heartbeat_loop(self):
        while self._running:
            time.sleep(HEARTBEAT_INTERVAL_S)
            if not self._running:
                return
            self.broadcast(MSG_HEARTBEAT)


class UnixSocketServer(BaseServer):
    """AF_UNIX listener, used on Linux and macOS.

    This is also what makes the whole pipeline testable in CI on Linux against a real kernel
    transport rather than a mock.
    """

    def __init__(self, *args, **kwargs):
        BaseServer.__init__(self, *args, **kwargs)
        self._socket = None

    def _listen(self):
        import socket

        # A stale socket file from a crashed run must not block startup.
        try:
            os.unlink(self.endpoint)
        except OSError:
            pass
        directory = os.path.dirname(self.endpoint)
        if directory and not os.path.isdir(directory):
            os.makedirs(directory)
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.bind(self.endpoint)
        # Owner only. The overlay runs as the same user, so nothing else needs to reach it.
        os.chmod(self.endpoint, 0o600)
        self._socket.listen(8)
        self._socket.settimeout(0.5)

    def _accept(self):
        import socket

        try:
            conn, _ = self._socket.accept()
        except socket.timeout:
            return None
        except OSError:
            return None
        conn.settimeout(None)
        return conn

    def _send(self, conn, data):
        conn.sendall(data)

    def _recv(self, conn, size):
        return conn.recv(size)

    def _close(self, conn):
        try:
            conn.close()
        except OSError:
            pass

    def _unblock_accept(self):
        # accept() already polls on a 0.5s timeout, so it needs no wakeup: closing the listener
        # and letting the loop notice `_running` is enough, and avoids racing a half-accepted
        # connection.
        if self._socket is not None:
            try:
                self._socket.close()
            except OSError:
                pass
            self._socket = None
        try:
            os.unlink(self.endpoint)
        except OSError:
            pass


class WindowsPipeServer(BaseServer):
    """Named pipe listener, built on ctypes so no third-party package is needed.

    One pipe instance is created per accepted connection, which is how several games attach to
    the same name at once. The accept thread always keeps exactly one instance listening.
    """

    # kernel32 constants, spelled out rather than imported from a package we do not have.
    PIPE_ACCESS_DUPLEX = 0x00000003
    FILE_FLAG_FIRST_PIPE_INSTANCE = 0x00080000
    PIPE_TYPE_BYTE = 0x00000000
    PIPE_READMODE_BYTE = 0x00000000
    PIPE_WAIT = 0x00000000
    PIPE_REJECT_REMOTE_CLIENTS = 0x00000008
    PIPE_UNLIMITED_INSTANCES = 255
    INVALID_HANDLE_VALUE = -1
    ERROR_PIPE_CONNECTED = 535
    ERROR_BROKEN_PIPE = 109
    ERROR_NO_DATA = 232
    ERROR_OPERATION_ABORTED = 995
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3

    # Only this user and SYSTEM. `P` blocks inherited ACEs, so a permissive ACL somewhere up the
    # namespace cannot widen this; OW grants to whoever owns the object, which is the creating
    # user. A remote client is refused outright by the pipe mode flag as well.
    SDDL = "D:P(A;;GA;;;SY)(A;;GA;;;OW)"

    def __init__(self, *args, **kwargs):
        BaseServer.__init__(self, *args, **kwargs)
        self._k32 = None
        self._pending = None
        self._security = None
        self._first_instance = True

    def _load(self):
        import ctypes
        from ctypes import wintypes

        if self._k32 is not None:
            return
        self._ctypes = ctypes
        self._wintypes = wintypes
        self._k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._advapi = ctypes.WinDLL("advapi32", use_last_error=True)

        # Prototypes are declared explicitly. Letting ctypes guess means a HANDLE is truncated
        # to 32 bits on a 64-bit build, which fails in a way that looks like a permissions
        # problem rather than the type error it is.
        self._k32.CreateNamedPipeW.restype = wintypes.HANDLE
        self._k32.CreateNamedPipeW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
            wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        ]
        self._k32.ConnectNamedPipe.restype = wintypes.BOOL
        self._k32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
        self._k32.DisconnectNamedPipe.restype = wintypes.BOOL
        self._k32.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
        self._k32.CloseHandle.restype = wintypes.BOOL
        self._k32.CloseHandle.argtypes = [wintypes.HANDLE]
        self._k32.CancelIoEx.restype = wintypes.BOOL
        self._k32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
        self._k32.ReadFile.restype = wintypes.BOOL
        self._k32.ReadFile.argtypes = [
            wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
        ]
        self._k32.WriteFile.restype = wintypes.BOOL
        self._k32.WriteFile.argtypes = [
            wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
        ]
        self._k32.CreateFileW.restype = wintypes.HANDLE
        self._k32.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
        ]
        self._k32.LocalFree.restype = ctypes.c_void_p
        self._k32.LocalFree.argtypes = [ctypes.c_void_p]

        convert = self._advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
        convert.restype = wintypes.BOOL
        convert.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(wintypes.DWORD),
        ]

    def _build_security(self):
        """Builds a SECURITY_ATTRIBUTES granting only this user and SYSTEM.

        Returning None on failure is deliberate and is *not* silent: the pipe is then created
        with the default DACL, which is still restricted to the creating user's logon session by
        Windows itself. The log line says which one is in force.
        """
        import ctypes
        from ctypes import wintypes

        class SECURITY_ATTRIBUTES(ctypes.Structure):
            _fields_ = [
                ("nLength", wintypes.DWORD),
                ("lpSecurityDescriptor", ctypes.c_void_p),
                ("bInheritHandle", wintypes.BOOL),
            ]

        descriptor = ctypes.c_void_p()
        size = wintypes.DWORD()
        ok = self._advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            self.SDDL, 1, ctypes.byref(descriptor), ctypes.byref(size)
        )
        if not ok:
            log("could not build the pipe's security descriptor (error %d); falling back to "
                "the default, which Windows still restricts to this logon session"
                % ctypes.get_last_error())
            return None
        attributes = SECURITY_ATTRIBUTES()
        attributes.nLength = ctypes.sizeof(SECURITY_ATTRIBUTES)
        attributes.lpSecurityDescriptor = descriptor
        attributes.bInheritHandle = False
        # The descriptor is kept alive by holding a reference to it alongside the structure;
        # LocalFree happens in stop().
        self._descriptor = descriptor
        return attributes

    def _create_instance(self):
        import ctypes

        flags = self.PIPE_ACCESS_DUPLEX
        if self._first_instance:
            # Refuses to start if something already owns this name, rather than silently
            # becoming a second server nobody will reach.
            flags |= self.FILE_FLAG_FIRST_PIPE_INSTANCE

        handle = self._k32.CreateNamedPipeW(
            self.endpoint,
            flags,
            self.PIPE_TYPE_BYTE | self.PIPE_READMODE_BYTE | self.PIPE_WAIT
            | self.PIPE_REJECT_REMOTE_CLIENTS,
            self.PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            ctypes.byref(self._security) if self._security is not None else None,
        )
        if handle == self.INVALID_HANDLE_VALUE or handle is None:
            raise OSError("CreateNamedPipeW failed (error %d)" % ctypes.get_last_error())
        self._first_instance = False
        return handle

    def _listen(self):
        self._load()
        self._security = self._build_security()
        # Created here rather than in the accept loop so a name that is already taken is
        # reported by start() instead of being discovered on a background thread.
        self._pending = self._create_instance()

    def _accept(self):
        import ctypes

        if self._pending is None:
            try:
                self._pending = self._create_instance()
            except OSError as exc:
                log("could not create another pipe instance: %s" % exc)
                time.sleep(0.5)
                return None

        handle = self._pending
        ok = self._k32.ConnectNamedPipe(handle, None)
        error = ctypes.get_last_error()
        # A client that connected in the window between CreateNamedPipe and ConnectNamedPipe is
        # reported as an error that means success. Treating it as a failure is a connection
        # silently lost every time the timing is unlucky.
        if not ok and error != self.ERROR_PIPE_CONNECTED:
            self._pending = None
            self._k32.CloseHandle(handle)
            if self._running and error != self.ERROR_OPERATION_ABORTED:
                log("ConnectNamedPipe failed (error %d)" % error)
            return None

        self._pending = None
        return handle

    def _send(self, conn, data):
        import ctypes
        from ctypes import wintypes

        written = wintypes.DWORD(0)
        buffer = ctypes.create_string_buffer(data)
        ok = self._k32.WriteFile(conn, buffer, len(data), ctypes.byref(written), None)
        if not ok:
            raise OSError("WriteFile failed (error %d)" % ctypes.get_last_error())
        if written.value != len(data):
            raise OSError("short write: %d of %d bytes" % (written.value, len(data)))

    def _recv(self, conn, size):
        import ctypes
        from ctypes import wintypes

        buffer = ctypes.create_string_buffer(size)
        read = wintypes.DWORD(0)
        ok = self._k32.ReadFile(conn, buffer, size, ctypes.byref(read), None)
        if not ok:
            error = ctypes.get_last_error()
            if error in (self.ERROR_BROKEN_PIPE, self.ERROR_NO_DATA,
                         self.ERROR_OPERATION_ABORTED):
                return b""   # an ordinary disconnect, not a fault
            raise OSError("ReadFile failed (error %d)" % error)
        return buffer.raw[: read.value]

    def _close(self, conn):
        # CancelIoEx first, so a thread parked in ReadFile on this handle is released before the
        # handle goes away. Closing a handle out from under a blocked read is the thing that
        # produces the hangs this avoids.
        try:
            self._k32.CancelIoEx(conn, None)
        except Exception:  # noqa: BLE001
            pass
        self._k32.DisconnectNamedPipe(conn)
        self._k32.CloseHandle(conn)

    def _unblock_accept(self):
        import ctypes

        # Connecting to our own pipe releases the accept thread from ConnectNamedPipe. It is the
        # documented way to do this without switching the whole server to overlapped I/O.
        handle = self._k32.CreateFileW(
            self.endpoint, self.GENERIC_READ | self.GENERIC_WRITE, 0, None,
            self.OPEN_EXISTING, 0, None
        )
        if handle != self.INVALID_HANDLE_VALUE and handle is not None:
            self._k32.CloseHandle(handle)
        if self._pending is not None:
            self._k32.CancelIoEx(self._pending, None)
            self._k32.CloseHandle(self._pending)
            self._pending = None
        if getattr(self, "_descriptor", None) is not None:
            self._k32.LocalFree(self._descriptor)
            self._descriptor = None
        self._security = None


def default_endpoint(name=""):
    """The endpoint both ends default to. Must match obsn::default_endpoint on the overlay side."""
    if IS_WINDOWS:
        if name.startswith("\\\\"):
            return name
        if not name:
            # The SID keeps two users on one machine -- fast user switching, a shared PC -- from
            # colliding on a single pipe name.
            sid = _current_user_sid()
            name = "obsn.v1." + sid if sid else "obsn.v1"
        return "\\\\.\\pipe\\" + name
    if name.startswith("/"):
        return name
    directory = os.environ.get("XDG_RUNTIME_DIR") or os.environ.get("TMPDIR") or "/tmp"
    return os.path.join(directory, (name or "obsn.v1") + ".sock")


def _current_user_sid():
    """This process's user SID as a string, or "" when it cannot be determined."""
    if not IS_WINDOWS:
        return ""
    try:
        import ctypes
        from ctypes import wintypes

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        advapi = ctypes.WinDLL("advapi32", use_last_error=True)

        TOKEN_QUERY = 0x0008
        TokenUser = 1

        token = wintypes.HANDLE()
        if not advapi.OpenProcessToken(k32.GetCurrentProcess(), TOKEN_QUERY,
                                       ctypes.byref(token)):
            return ""
        try:
            size = wintypes.DWORD(0)
            advapi.GetTokenInformation(token, TokenUser, None, 0, ctypes.byref(size))
            buffer = ctypes.create_string_buffer(size.value)
            if not advapi.GetTokenInformation(token, TokenUser, buffer, size,
                                              ctypes.byref(size)):
                return ""

            class SID_AND_ATTRIBUTES(ctypes.Structure):
                _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", wintypes.DWORD)]

            user = ctypes.cast(buffer, ctypes.POINTER(SID_AND_ATTRIBUTES)).contents
            text = ctypes.c_wchar_p()
            convert = advapi.ConvertSidToStringSidW
            convert.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_wchar_p)]
            if not convert(user.Sid, ctypes.byref(text)):
                return ""
            value = text.value or ""
            k32.LocalFree(text)
            return value
        finally:
            k32.CloseHandle(token)
    except Exception:  # noqa: BLE001 - a missing SID costs a shared name, not a failure
        return ""


# --------------------------------------------------------------------------------------------
# OBS bridge: what is true, and what just happened
# --------------------------------------------------------------------------------------------

# Event kinds, matching obsn::EventKind's wire names.
EV_RECORDING_STARTING = "recording.starting"
EV_RECORDING_STARTED = "recording.started"
EV_RECORDING_STOPPING = "recording.stopping"
EV_RECORDING_STOPPED = "recording.stopped"
EV_RECORDING_PAUSED = "recording.paused"
EV_RECORDING_RESUMED = "recording.resumed"
EV_RECORDING_SAVED = "recording.saved"
EV_REPLAY_STARTING = "replay.starting"
EV_REPLAY_STARTED = "replay.started"
EV_REPLAY_STOPPED = "replay.stopped"
EV_REPLAY_SAVED = "replay.saved"
EV_STREAM_STARTING = "stream.starting"
EV_STREAM_STARTED = "stream.started"
EV_STREAM_STOPPING = "stream.stopping"
EV_STREAM_STOPPED = "stream.stopped"
EV_STREAM_RECONNECTING = "stream.reconnecting"
EV_STREAM_RECONNECTED = "stream.reconnected"
EV_VIRTUALCAM_STARTED = "virtualcam.started"
EV_VIRTUALCAM_STOPPED = "virtualcam.stopped"
EV_SCENE_CHANGED = "scene.changed"
EV_PROFILE_CHANGED = "profile.changed"
EV_WARNING = "warning"

# Which switch in the script's settings governs each event. Grouping them by output is what
# makes "don't tell the overlay about my streams" one checkbox rather than six.
GROUP_OF = {
    EV_RECORDING_STARTING: "recording", EV_RECORDING_STARTED: "recording",
    EV_RECORDING_STOPPING: "recording", EV_RECORDING_STOPPED: "recording",
    EV_RECORDING_PAUSED: "recording", EV_RECORDING_RESUMED: "recording",
    EV_RECORDING_SAVED: "recording",
    EV_REPLAY_STARTING: "replay", EV_REPLAY_STARTED: "replay",
    EV_REPLAY_STOPPED: "replay", EV_REPLAY_SAVED: "replay",
    EV_STREAM_STARTING: "streaming", EV_STREAM_STARTED: "streaming",
    EV_STREAM_STOPPING: "streaming", EV_STREAM_STOPPED: "streaming",
    EV_STREAM_RECONNECTING: "streaming", EV_STREAM_RECONNECTED: "streaming",
    EV_VIRTUALCAM_STARTED: "virtualcam", EV_VIRTUALCAM_STOPPED: "virtualcam",
    EV_SCENE_CHANGED: "scenes", EV_PROFILE_CHANGED: "scenes",
    EV_WARNING: "warnings",
}


class ObsBridge(object):
    """Keeps the session state and turns frontend events into protocol messages.

    Split from the transport so it can be driven by the test suite with no pipe and no OBS: the
    state machine is the part with rules worth checking, and it is pure bookkeeping.
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.state = self._empty_state()
        self.settings = {
            "recording": True,
            "replay": True,
            "streaming": True,
            "virtualcam": True,
            "scenes": False,
            "warnings": True,
            "send_full_paths": False,
            "send_file_sizes": True,
        }

    @staticmethod
    def _empty_state():
        return {
            "obs_version": "",
            "script_version": SCRIPT_VERSION,
            "profile": "",
            "scene_collection": "",
            "current_scene": "",
            "recording": {"state": "idle", "started_ms": 0, "paused_ms": 0,
                          "paused_since_ms": 0, "path": ""},
            "replay": {"state": "idle", "started_ms": 0, "last_saved_path": "",
                       "last_saved_ms": 0, "duration_s": 0},
            "stream": {"state": "idle", "started_ms": 0, "service": "",
                       "reconnect_attempt": 0},
            "virtual_cam": {"state": "idle", "started_ms": 0},
            "stats": {},
        }

    def snapshot(self):
        """A deep-enough copy to hand to the encoder without holding the lock while it runs."""
        with self.lock:
            return json.loads(json.dumps(self.state))

    def wants(self, kind):
        return self.settings.get(GROUP_OF.get(kind, "warnings"), True)

    def sanitise(self, event):
        """Applies the privacy settings to one event, in place.

        A recording path contains the user's account name, so what crosses the pipe is the file
        name alone unless they ask for the whole thing. The overlay's `{file}` placeholder works
        either way; only `{path}` and `{folder}` need the rest.
        """
        path = event.get("path")
        if path and not self.settings.get("send_full_paths", False):
            event["path"] = file_name_of(path)
        if not self.settings.get("send_file_sizes", True):
            event.pop("size_bytes", None)
        return event

    # --- state transitions ---
    def apply(self, kind, at, extra=None):
        """Folds one event into the state. Returns the event dict to send, or None to drop it."""
        extra = extra or {}
        event = {"kind": kind, "ts": at}
        event.update(extra)

        with self.lock:
            recording = self.state["recording"]
            replay = self.state["replay"]
            stream = self.state["stream"]
            camera = self.state["virtual_cam"]

            if kind == EV_RECORDING_STARTING:
                recording["state"] = "starting"
            elif kind == EV_RECORDING_STARTED:
                recording["state"] = "active"
                recording["started_ms"] = at
                # Reset rather than accumulate: paused time belongs to one recording, and
                # carrying it into the next makes every subsequent timer wrong.
                recording["paused_ms"] = 0
                recording["paused_since_ms"] = 0
            elif kind == EV_RECORDING_PAUSED:
                recording["state"] = "paused"
                recording["paused_since_ms"] = at
            elif kind == EV_RECORDING_RESUMED:
                if recording["paused_since_ms"]:
                    recording["paused_ms"] += max(0, at - recording["paused_since_ms"])
                recording["paused_since_ms"] = 0
                recording["state"] = "active"
            elif kind == EV_RECORDING_STOPPING:
                recording["state"] = "stopping"
            elif kind == EV_RECORDING_STOPPED:
                # The length is computed here, where the start time is still known, rather than
                # being left for the overlay to infer from a state that is about to be cleared.
                if recording["started_ms"]:
                    paused = recording["paused_ms"]
                    if recording["paused_since_ms"]:
                        paused += max(0, at - recording["paused_since_ms"])
                    event.setdefault("duration_ms",
                                     max(0, at - recording["started_ms"] - paused))
                recording["state"] = "idle"
                recording["started_ms"] = 0
                recording["paused_ms"] = 0
                recording["paused_since_ms"] = 0
                recording["path"] = ""
            elif kind == EV_RECORDING_SAVED:
                if event.get("path"):
                    recording["path"] = event["path"]

            elif kind == EV_REPLAY_STARTING:
                replay["state"] = "starting"
            elif kind == EV_REPLAY_STARTED:
                replay["state"] = "active"
                replay["started_ms"] = at
            elif kind == EV_REPLAY_STOPPED:
                replay["state"] = "idle"
                replay["started_ms"] = 0
            elif kind == EV_REPLAY_SAVED:
                replay["last_saved_path"] = event.get("path", "")
                replay["last_saved_ms"] = at

            elif kind == EV_STREAM_STARTING:
                stream["state"] = "starting"
            elif kind == EV_STREAM_STARTED:
                stream["state"] = "active"
                stream["started_ms"] = at
                stream["reconnect_attempt"] = 0
            elif kind == EV_STREAM_STOPPING:
                stream["state"] = "stopping"
            elif kind == EV_STREAM_STOPPED:
                if stream["started_ms"]:
                    event.setdefault("duration_ms", max(0, at - stream["started_ms"]))
                stream["state"] = "idle"
                stream["started_ms"] = 0
                stream["reconnect_attempt"] = 0
            elif kind == EV_STREAM_RECONNECTING:
                stream["state"] = "reconnecting"
                stream["reconnect_attempt"] += 1
                event.setdefault("attempt", stream["reconnect_attempt"])
            elif kind == EV_STREAM_RECONNECTED:
                stream["state"] = "active"
                stream["reconnect_attempt"] = 0

            elif kind == EV_VIRTUALCAM_STARTED:
                camera["state"] = "active"
                camera["started_ms"] = at
            elif kind == EV_VIRTUALCAM_STOPPED:
                camera["state"] = "idle"
                camera["started_ms"] = 0

            elif kind == EV_SCENE_CHANGED:
                event.setdefault("previous_scene", self.state["current_scene"])
                self.state["current_scene"] = event.get("scene", "")
            elif kind == EV_PROFILE_CHANGED:
                self.state["profile"] = event.get("profile", "")

        return event


# --------------------------------------------------------------------------------------------
# The running service: dispatcher thread, health polling, OBS frontend hooks
# --------------------------------------------------------------------------------------------

class Service(object):
    """Ties the bridge to the server and owns the one thread that does real work.

    OBS's frontend callback runs on its UI thread. Everything that could take measurable time --
    stat'ing a just-written file, serialising a snapshot, writing to a pipe -- happens on the
    dispatcher instead, so that callback does a dictionary build and a queue put and returns.
    """

    def __init__(self):
        self.bridge = ObsBridge()
        self.server = None
        self.endpoint = ""
        self._dispatch = queue.Queue()
        self._dispatch_thread = None
        self._running = False

    def start(self, endpoint, max_clients=8):
        self.stop()
        self.endpoint = endpoint
        factory = WindowsPipeServer if IS_WINDOWS else UnixSocketServer
        self.server = factory(endpoint, self._on_client_message, max_clients=max_clients)
        if not self.server.start():
            self.server = None
            return False
        self._running = True
        self._dispatch_thread = threading.Thread(target=self._dispatch_loop, name="obsn-dispatch")
        self._dispatch_thread.daemon = True
        self._dispatch_thread.start()
        return True

    def stop(self):
        if not self._running:
            return
        self._running = False
        self._dispatch.put(None)
        if self._dispatch_thread is not None:
            self._dispatch_thread.join(timeout=2.0)
            self._dispatch_thread = None
        if self.server is not None:
            self.server.stop()
            self.server = None

    def running(self):
        return self._running

    def submit(self, kind, extra=None):
        """Called from OBS's thread. Never blocks, never touches the pipe."""
        if not self._running:
            return
        if not self.bridge.wants(kind):
            return
        self._dispatch.put((kind, now_ms(), extra or {}))

    def _dispatch_loop(self):
        while self._running:
            try:
                item = self._dispatch.get(timeout=0.5)
            except queue.Empty:
                continue
            if item is None:
                return
            kind, at, extra = item
            try:
                self._handle(kind, at, extra)
            except Exception as exc:  # noqa: BLE001 - one bad event must not end the session
                log("error dispatching %s: %s" % (kind, exc))

    def _handle(self, kind, at, extra):
        # The file size is read here, off OBS's thread, because a recording on a slow or
        # network drive can make a stat call take long enough to be felt in the UI.
        path = extra.get("path")
        if path and self.bridge.settings.get("send_file_sizes", True):
            size = _file_size_when_settled(path)
            if size is not None:
                extra["size_bytes"] = size

        event = self.bridge.apply(kind, at, extra)
        if event is None:
            return
        event = self.bridge.sanitise(event)
        if self.server is not None:
            self.server.broadcast(MSG_EVENT, event)
            self._serve_pending_snapshots()

    def _serve_pending_snapshots(self):
        """Sends a snapshot to any client whose queue overflowed, repairing what it lost."""
        if self.server is None:
            return
        with self.server._sessions_lock:            # noqa: SLF001 - same module, one owner
            sessions = list(self.server._sessions)  # noqa: SLF001
        for session, _ in sessions:
            if session.needs_snapshot:
                session.needs_snapshot = False
                self.server.send_to(session, MSG_STATE, self.bridge.snapshot())

    def _on_client_message(self, server, session, msg_type, data):
        """Runs on that client's read thread."""
        if msg_type == MSG_CLIENT_HELLO:
            process = str(data.get("process", ""))[:120]
            session.process = process
            session.description = "%s (client %d)" % (process or "overlay", session.id)
            log("%s attached" % session.description)
            server.send_to(session, MSG_HELLO, {
                "protocol_min": PROTOCOL_VERSION,
                "protocol_max": PROTOCOL_VERSION,
                "script_version": SCRIPT_VERSION,
                "obs_version": self.bridge.state.get("obs_version", ""),
                "platform": platform.system(),
                "capabilities": _capabilities(),
            })
            server.send_to(session, MSG_STATE, self.bridge.snapshot())
        elif msg_type == MSG_REQUEST_SNAPSHOT:
            server.send_to(session, MSG_STATE, self.bridge.snapshot())
        elif msg_type == MSG_PING:
            server.send_to(session, MSG_PONG)
        else:
            log("%s sent an unexpected %s" % (session.description, msg_type))


def _file_size_when_settled(path, attempts=4, delay=0.08):
    """Size of a file OBS has just finished writing, or None.

    OBS reports the path at the moment it closes the file, but a muxer can still be flushing
    when the event arrives, so an immediate stat sometimes returns a size that is short by a few
    megabytes. Waiting for two identical readings costs a fraction of a second on a thread
    nothing is waiting on, and is the difference between "1.4 GB" and a number that is wrong.
    """
    previous = None
    for _ in range(attempts):
        try:
            size = os.path.getsize(path)
        except OSError:
            return None
        if size == previous and size > 0:
            return size
        previous = size
        time.sleep(delay)
    return previous


def _capabilities():
    """What this script can actually report, given the OBS build it is running in.

    Advertised rather than assumed: an older OBS without the virtual camera events simply does
    not list them, and the overlay's diagnostics then say so instead of leaving the user
    wondering why one category never fires.
    """
    caps = ["recording", "replay_buffer", "streaming"]
    if obs is None:
        return caps
    if hasattr(obs, "OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED"):
        caps.append("virtualcam")
    if hasattr(obs, "OBS_FRONTEND_EVENT_RECORDING_PAUSED"):
        caps.append("recording_pause")
    if hasattr(obs, "OBS_FRONTEND_EVENT_RECORDING_FILE_CHANGED"):
        caps.append("file_split")
    caps.append("warnings")
    return caps


# --------------------------------------------------------------------------------------------
# OBS API glue
# --------------------------------------------------------------------------------------------

_service = Service()
_settings_ref = None
_health = {"dropped": 0, "warned_dropped": False, "warned_disk": False}


def _obs_event(name):
    """An OBS frontend event constant, or None when this build does not have it.

    Every constant is looked up this way. OBS has added events over the years and a script that
    reads them as attributes at import time simply fails to load on an older build, which is a
    much worse outcome than one missing category.
    """
    return getattr(obs, name, None) if obs is not None else None


def _current_scene_name():
    if obs is None:
        return ""
    source = obs.obs_frontend_get_current_scene()
    if source is None:
        return ""
    try:
        return obs.obs_source_get_name(source) or ""
    finally:
        obs.obs_source_release(source)


def _streaming_service_name():
    if obs is None:
        return ""
    try:
        service = obs.obs_frontend_get_streaming_service()
        if service is None:
            return ""
        # The display name is what OBS shows the user ("Twitch"); the internal name is not.
        name = obs.obs_service_get_display_name(service)
        return name or ""
    except Exception:  # noqa: BLE001 - absent on some builds; a missing name is not a failure
        return ""


def _last_recording_path():
    if obs is None:
        return ""
    try:
        return obs.obs_frontend_get_last_recording() or ""
    except Exception:  # noqa: BLE001
        return ""


def _last_replay_path():
    if obs is None:
        return ""
    try:
        return obs.obs_frontend_get_last_replay() or ""
    except Exception:  # noqa: BLE001
        return ""


def _replay_buffer_seconds():
    """The configured buffer length, or 0 when it cannot be read.

    Zero means "we do not know" and the overlay omits the figure, rather than showing a
    0-second buffer that would be a confident wrong answer.
    """
    if obs is None:
        return 0
    try:
        config = obs.obs_frontend_get_profile_config()
        if config is None:
            return 0
        mode = obs.config_get_string(config, "Output", "Mode") or "Simple"
        if mode == "Advanced":
            return int(obs.config_get_uint(config, "AdvOut", "RecRBTime") or 0)
        return int(obs.config_get_uint(config, "SimpleOutput", "RecRBTime") or 0)
    except Exception:  # noqa: BLE001
        return 0


def _refresh_static_state():
    """Fills in the parts of the state that only change when the user changes them."""
    if obs is None:
        return
    with _service.bridge.lock:
        state = _service.bridge.state
        try:
            state["obs_version"] = obs.obs_get_version_string() or ""
        except Exception:  # noqa: BLE001
            state["obs_version"] = ""
        try:
            state["profile"] = obs.obs_frontend_get_current_profile() or ""
            state["scene_collection"] = obs.obs_frontend_get_current_scene_collection() or ""
        except Exception:  # noqa: BLE001
            pass
        state["replay"]["duration_s"] = _replay_buffer_seconds()
        state["stream"]["service"] = _streaming_service_name()
    state_scene = _current_scene_name()
    with _service.bridge.lock:
        _service.bridge.state["current_scene"] = state_scene


def _frontend_event(event):
    """OBS's frontend callback. Runs on the UI thread, so it does as little as possible."""
    mapping = _frontend_event_map()
    kind = mapping.get(event)
    if kind is None:
        return

    extra = {}
    if kind == EV_RECORDING_STARTED:
        _service.bridge.state["recording"]["path"] = ""
    elif kind == EV_RECORDING_STOPPED:
        # Two events from one: the recording ended, and a file was written. They are separate
        # because a split writes a file without the recording ending, and because a user who
        # only wants to be told where the file went should be able to say so.
        path = _last_recording_path()
        _service.submit(EV_RECORDING_STOPPED, {})
        if path:
            _service.submit(EV_RECORDING_SAVED, {"path": path})
        return
    elif kind == EV_RECORDING_SAVED:
        # A file split. The *previous* file is the one that was just written.
        path = _last_recording_path()
        if not path:
            return
        extra["path"] = path
    elif kind == EV_REPLAY_SAVED:
        extra["path"] = _last_replay_path()
        extra["replay_seconds"] = _replay_buffer_seconds()
    elif kind == EV_REPLAY_STARTED:
        extra["replay_seconds"] = _replay_buffer_seconds()
    elif kind in (EV_STREAM_STARTED, EV_STREAM_STARTING):
        extra["service"] = _streaming_service_name()
    elif kind == EV_SCENE_CHANGED:
        extra["scene"] = _current_scene_name()
    elif kind == EV_PROFILE_CHANGED:
        try:
            extra["profile"] = obs.obs_frontend_get_current_profile() or ""
        except Exception:  # noqa: BLE001
            pass
        _refresh_static_state()

    _service.submit(kind, extra)


_EVENT_MAP_CACHE = None


def _frontend_event_map():
    """Builds the OBS-constant to event-kind map once, skipping constants this build lacks."""
    global _EVENT_MAP_CACHE
    if _EVENT_MAP_CACHE is not None:
        return _EVENT_MAP_CACHE
    pairs = [
        ("OBS_FRONTEND_EVENT_RECORDING_STARTING", EV_RECORDING_STARTING),
        ("OBS_FRONTEND_EVENT_RECORDING_STARTED", EV_RECORDING_STARTED),
        ("OBS_FRONTEND_EVENT_RECORDING_STOPPING", EV_RECORDING_STOPPING),
        ("OBS_FRONTEND_EVENT_RECORDING_STOPPED", EV_RECORDING_STOPPED),
        ("OBS_FRONTEND_EVENT_RECORDING_PAUSED", EV_RECORDING_PAUSED),
        ("OBS_FRONTEND_EVENT_RECORDING_UNPAUSED", EV_RECORDING_RESUMED),
        ("OBS_FRONTEND_EVENT_RECORDING_FILE_CHANGED", EV_RECORDING_SAVED),
        ("OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING", EV_REPLAY_STARTING),
        ("OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED", EV_REPLAY_STARTED),
        ("OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED", EV_REPLAY_STOPPED),
        ("OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED", EV_REPLAY_SAVED),
        ("OBS_FRONTEND_EVENT_STREAMING_STARTING", EV_STREAM_STARTING),
        ("OBS_FRONTEND_EVENT_STREAMING_STARTED", EV_STREAM_STARTED),
        ("OBS_FRONTEND_EVENT_STREAMING_STOPPING", EV_STREAM_STOPPING),
        ("OBS_FRONTEND_EVENT_STREAMING_STOPPED", EV_STREAM_STOPPED),
        ("OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED", EV_VIRTUALCAM_STARTED),
        ("OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED", EV_VIRTUALCAM_STOPPED),
        ("OBS_FRONTEND_EVENT_SCENE_CHANGED", EV_SCENE_CHANGED),
        ("OBS_FRONTEND_EVENT_PROFILE_CHANGED", EV_PROFILE_CHANGED),
    ]
    mapping = {}
    for name, kind in pairs:
        value = _obs_event(name)
        if value is not None:
            mapping[value] = kind
    _EVENT_MAP_CACHE = mapping
    return mapping


def _poll_health():
    """Runs on an OBS timer. Raises a warning toast for things still worth fixing.

    Only transitions are reported: a stream that is dropping frames says so once, not twice a
    second for the next hour. The flag clears when the condition does, so a second bad patch
    later in the session is reported again.
    """
    if obs is None or not _service.running():
        return
    if not _service.bridge.settings.get("warnings", True):
        return

    try:
        output = obs.obs_frontend_get_streaming_output()
    except Exception:  # noqa: BLE001
        output = None
    if output is not None:
        try:
            dropped = obs.obs_output_get_frames_dropped(output)
            total = obs.obs_output_get_total_frames(output)
            if total > 0:
                percent = 100.0 * float(dropped) / float(total)
                recent = dropped - _health["dropped"]
                _health["dropped"] = dropped
                if percent >= 5.0 and recent > 0 and not _health["warned_dropped"]:
                    _health["warned_dropped"] = True
                    _service.submit(EV_WARNING, {
                        "detail": "Dropping frames: %.1f%% lost to the network" % percent,
                    })
                elif percent < 2.0:
                    _health["warned_dropped"] = False
        except Exception:  # noqa: BLE001
            pass
        finally:
            try:
                obs.obs_output_release(output)
            except Exception:  # noqa: BLE001
                pass

    # Disk space is read with shutil rather than an OBS call, because the OBS helper is not
    # exposed to scripts on every build and this one is in the standard library everywhere.
    path = _recording_directory()
    if path:
        try:
            import shutil

            free_mb = shutil.disk_usage(path).free // (1024 * 1024)
            with _service.bridge.lock:
                _service.bridge.state["stats"]["free_disk_mb"] = int(free_mb)
            recording = _service.bridge.state["recording"]["state"] in ("active", "paused")
            if recording and free_mb < 2048 and not _health["warned_disk"]:
                _health["warned_disk"] = True
                _service.submit(EV_WARNING, {
                    "detail": "Low disk space: %d MB left where recordings are saved" % free_mb,
                })
            elif free_mb > 4096:
                _health["warned_disk"] = False
        except Exception:  # noqa: BLE001
            pass


def _recording_directory():
    if obs is None:
        return ""
    try:
        config = obs.obs_frontend_get_profile_config()
        if config is None:
            return ""
        mode = obs.config_get_string(config, "Output", "Mode") or "Simple"
        if mode == "Advanced":
            return obs.config_get_string(config, "AdvOut", "RecFilePath") or ""
        return obs.config_get_string(config, "SimpleOutput", "FilePath") or ""
    except Exception:  # noqa: BLE001
        return ""


# --- OBS script entry points ----------------------------------------------------------------

def script_description():
    return (
        "<b>OBS Notify</b><br/>"
        "Sends recording, replay buffer and streaming events to the OBS Notifications "
        "ReShade add-on, so they appear inside the game.<br/><br/>"
        "This script decides <i>what</i> is sent. How the notifications look -- colours, "
        "position, wording, animation -- is configured in game, in ReShade's menu under "
        "<i>OBS Notifications</i>.<br/><br/>"
        "Nothing here opens a network connection."
    )


def script_defaults(settings):
    obs.obs_data_set_default_string(settings, "pipe_name", "")
    obs.obs_data_set_default_bool(settings, "recording", True)
    obs.obs_data_set_default_bool(settings, "replay", True)
    obs.obs_data_set_default_bool(settings, "streaming", True)
    obs.obs_data_set_default_bool(settings, "virtualcam", True)
    obs.obs_data_set_default_bool(settings, "scenes", False)
    obs.obs_data_set_default_bool(settings, "warnings", True)
    obs.obs_data_set_default_bool(settings, "send_full_paths", False)
    obs.obs_data_set_default_bool(settings, "send_file_sizes", True)
    obs.obs_data_set_default_int(settings, "max_clients", 8)


def script_properties():
    props = obs.obs_properties_create()

    obs.obs_properties_add_text(
        props, "pipe_name",
        "Pipe name (leave empty for the default)", obs.OBS_TEXT_DEFAULT)

    events = obs.obs_properties_create()
    obs.obs_properties_add_bool(events, "recording", "Recording started, stopped, paused, saved")
    obs.obs_properties_add_bool(events, "replay", "Replay buffer on, off, saved")
    obs.obs_properties_add_bool(events, "streaming", "Stream started, stopped, reconnecting")
    obs.obs_properties_add_bool(events, "virtualcam", "Virtual camera on and off")
    obs.obs_properties_add_bool(events, "scenes", "Scene and profile changes")
    obs.obs_properties_add_bool(events, "warnings", "Warnings (dropped frames, low disk space)")
    obs.obs_properties_add_group(props, "events_group", "Send these events",
                                 obs.OBS_GROUP_NORMAL, events)

    privacy = obs.obs_properties_create()
    obs.obs_properties_add_bool(
        privacy, "send_full_paths",
        "Send the full path, not just the file name")
    obs.obs_properties_add_bool(privacy, "send_file_sizes", "Send file sizes")
    obs.obs_properties_add_group(
        props, "privacy_group",
        "What leaves OBS  (a full path contains your account name)",
        obs.OBS_GROUP_NORMAL, privacy)

    obs.obs_properties_add_int(props, "max_clients", "Most games attached at once", 1, 32, 1)

    obs.obs_properties_add_button(props, "test_button", "Send a test notification",
                                  _on_test_button)
    obs.obs_properties_add_button(props, "restart_button", "Restart the connection",
                                  _on_restart_button)
    obs.obs_properties_add_button(props, "status_button", "Write status to the script log",
                                  _on_status_button)
    return props


def _on_test_button(props, prop):
    (void_props, void_prop) = (props, prop)
    if not _service.running():
        log("not listening, so there is nothing to send a test to")
        return False
    # Sent as a real event through the real path, so a test that arrives proves the whole chain
    # rather than just this function.
    _service.submit(EV_REPLAY_SAVED, {
        "path": "Test notification.mkv",
        "replay_seconds": 30,
    })
    log("test notification sent to %d attached overlay(s)" % _service.server.client_count())
    return False


def _on_restart_button(props, prop):
    (void_props, void_prop) = (props, prop)
    _restart()
    return False


def _on_status_button(props, prop):
    (void_props, void_prop) = (props, prop)
    if _service.server is None:
        log("not listening")
        return False
    log("listening on %s" % _service.endpoint)
    log("%d attached, %d accepted, %d refused"
        % (_service.server.client_count(), _service.server.connections_accepted,
           _service.server.connections_rejected))
    for line in _service.server.describe_clients():
        log("  " + line)
    return False


def script_update(settings):
    global _settings_ref
    _settings_ref = settings

    previous = _service.bridge.settings.copy()
    for key in ("recording", "replay", "streaming", "virtualcam", "scenes", "warnings",
                "send_full_paths", "send_file_sizes"):
        _service.bridge.settings[key] = obs.obs_data_get_bool(settings, key)

    endpoint = default_endpoint(obs.obs_data_get_string(settings, "pipe_name") or "")
    max_clients = int(obs.obs_data_get_int(settings, "max_clients") or 8)

    # Only the endpoint and the client cap need a restart; the rest are read per event, so
    # toggling a category does not drop every attached overlay.
    if endpoint != _service.endpoint or not _service.running():
        _restart(endpoint, max_clients)
    elif previous != _service.bridge.settings:
        log("settings updated")


def _restart(endpoint=None, max_clients=8):
    if endpoint is None:
        endpoint = _service.endpoint or default_endpoint("")
    _service.stop()
    _refresh_static_state()
    if _service.start(endpoint, max_clients=max_clients):
        log("OBS Notify %s ready" % SCRIPT_VERSION)
    else:
        log("OBS Notify could not start: %s"
            % (_service.server.error() if _service.server else "see the messages above"))


def script_load(settings):
    global _settings_ref
    _settings_ref = settings
    _refresh_static_state()
    obs.obs_frontend_add_event_callback(_frontend_event)
    # Two seconds is often enough to catch a disk filling or a stream degrading while there is
    # still time to act, and rare enough to cost nothing measurable.
    obs.timer_add(_poll_health, 2000)
    script_update(settings)


def script_unload():
    try:
        obs.timer_remove(_poll_health)
    except Exception:  # noqa: BLE001
        pass
    try:
        obs.obs_frontend_remove_event_callback(_frontend_event)
    except Exception:  # noqa: BLE001
        pass
    _service.stop()
