# SPDX-License-Identifier: MIT
"""Stands up the OBS script's server and replays a scripted capture session into it.

Used by scripts/cross-language-check.sh. Everything here goes through the script's own Service,
ObsBridge and server, so what is exercised is the shipping code path rather than a re-creation
of it.
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "obs-script"))

import obs_nvidia_notify as script  # noqa: E402


def main():
    endpoint = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

    service = script.Service()
    # Full paths on, so the {path} and {folder} placeholders are exercised too.
    service.bridge.settings["send_full_paths"] = True
    service.bridge.state["obs_version"] = "30.2.3"
    service.bridge.state["current_scene"] = "Gameplay"

    if not service.start(endpoint):
        sys.stderr.write("producer: could not listen on %s\n" % endpoint)
        return 1
    sys.stderr.write("producer: listening on %s\n" % endpoint)

    # A plausible session, in the order these actually happen.
    timeline = [
        (1.5, script.EV_REPLAY_STARTED, {"replay_seconds": 30}),
        (0.4, script.EV_RECORDING_STARTED, {}),
        (0.4, script.EV_RECORDING_PAUSED, {}),
        (0.4, script.EV_RECORDING_RESUMED, {}),
        (0.4, script.EV_REPLAY_SAVED, {"path": "C:\\clips\\Replay 2026-09-20.mkv",
                                       "replay_seconds": 30}),
        (0.4, script.EV_STREAM_STARTED, {"service": "Twitch"}),
        (0.4, script.EV_STREAM_RECONNECTING, {}),
        (0.4, script.EV_STREAM_RECONNECTED, {}),
        (0.4, script.EV_WARNING, {"detail": "Encoder overloaded: frames are being skipped"}),
        (0.4, script.EV_RECORDING_STOPPED, {}),
        (0.2, script.EV_RECORDING_SAVED, {"path": "C:\\clips\\2026-09-20 21-14-03.mkv"}),
        (0.4, script.EV_STREAM_STOPPED, {}),
        (0.4, script.EV_REPLAY_STOPPED, {}),
    ]

    deadline = time.time() + seconds
    for delay, kind, extra in timeline:
        time.sleep(delay)
        if time.time() > deadline:
            break
        service.submit(kind, extra)
        sys.stderr.write("producer: sent %s\n" % kind)

    while time.time() < deadline:
        time.sleep(0.1)
    service.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
