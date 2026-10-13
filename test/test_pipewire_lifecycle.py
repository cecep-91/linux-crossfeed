"""Failure-path checks with fake routing commands and an unreachable server.

Never contacts the user's sound server: both runtime directory and remote name
are isolated; pactl/pw-link are replaced by logging stubs in a temporary PATH.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(__file__).resolve().parents[1] / "bin/crossfeed"
for existing in (False, True):
    with tempfile.TemporaryDirectory(prefix="crossfeed-routing-") as directory:
        root = Path(directory)
        commands = root / "commands"
        commands.mkdir()
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        stub = f"#!{sys.executable}\n" + '''import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["CROSSFEED_TEST_LOG"], "a") as f:
    f.write(json.dumps([name] + args) + "\\n")
if name == "pactl":
    if args == ["list", "short", "modules"] and os.environ["CROSSFEED_TEST_EXISTING"] == "1":
        print("42\\tmodule-null-sink\\tsink_name=crossfeed")
    elif args and args[0] == "load-module":
        print("42")
'''
        for name in ("pactl", "pw-link"):
            file = commands / name
            file.write_text(stub)
            file.chmod(0o755)
        env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                   XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root),
                   PIPEWIRE_RUNTIME_DIR=str(runtime), PIPEWIRE_REMOTE="unreachable-test-server",
                   CROSSFEED_TEST_LOG=str(root / "commands.jsonl"),
                   CROSSFEED_TEST_EXISTING="1" if existing else "0")
        result = subprocess.run([str(binary), "run", "--backend", "pipewire", "--target", "test-output"],
                                env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode != 0, result.stdout + result.stderr
        calls = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
        assert not any(call[1:2] == ["set-default-sink"] for call in calls), calls
        assert not any(call[1:2] == ["move-sink-input"] for call in calls), calls
        unloaded = [call for call in calls if call[1:2] == ["unload-module"]]
        assert len(unloaded) == (0 if existing else 1), calls
print("PASS: failed native startup leaves default/streams untouched and unloads only its own sink, exactly once.")
