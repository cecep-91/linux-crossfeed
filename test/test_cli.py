#!/usr/bin/env python3
"""Exercise listening CLI commands against an isolated fake engine, never live audio."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading

BINARY = Path(__file__).resolve().parents[1] / "bin" / "crossfeed"

with tempfile.TemporaryDirectory(prefix="crossfeed-cli-") as directory:
    root = Path(directory)
    env = dict(os.environ, XDG_CONFIG_HOME=str(root), XDG_RUNTIME_DIR=str(root))
    config = root / "pipewire-crossfeed"
    config.mkdir()
    (config / "presets.crossfeed").write_text(
        'CROSSFEED_PRESETS 1\n"Meier custom" -14 900 123 1600 -2 3500 0 -4\n'
    )

    def run(*arguments):
        return subprocess.run([str(BINARY), *arguments], env=env, text=True, capture_output=True, timeout=5)

    listed = run("presets")
    assert listed.returncode == 0 and "Meier custom" in listed.stdout
    selected = run("preset", "Meier custom")
    assert selected.returncode == 0, selected.stderr
    state = json.loads((config / "state.json").read_text())
    assert state["level_db"] == -14 and state["preamp_db"] == -4
    assert state["advanced_effects"] is False
    assert run("set", "--preamp", "nan").returncode != 0
    assert run("ab", "store", "c").returncode != 0
    assert run("match", "invalid").returncode != 0

    cases = [
        (("ab", "store", "a"), "AB STORE A"),
        (("ab", "store", "b"), "AB STORE B"),
        (("ab", "a"), "AB A"),
        (("ab", "b"), "AB B"),
        (("ab", "dry"), "AB DRY"),
        (("meters", "reset"), "METERS RESET"),
    ]
    requests = []
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(root / "crossfeed.sock"))
        server.listen()
        server.settimeout(5)

        def serve():
            for _ in cases:
                connection, _ = server.accept()
                with connection:
                    requests.append(connection.recv(1024).decode().strip())
                    connection.sendall(b'{"status":"running"}\n')

        worker = threading.Thread(target=serve)
        worker.start()
        for arguments, expected in cases:
            result = run(*arguments)
            assert result.returncode == 0, result.stderr
        worker.join(timeout=5)
        assert not worker.is_alive()
        assert requests == [expected for _, expected in cases]

print("PASS: saved-preset CLI recall, persistence, invalid input, and all listening commands use isolated IPC.")
