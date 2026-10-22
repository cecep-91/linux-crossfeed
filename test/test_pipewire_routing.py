"""Exercise native hotplug recovery using only a stateful fake audio graph.

The C++ driver calls the production routing code but never initializes PipeWire.
Both commands are stubs; runtime/config/server addresses are private as well.
"""
import fcntl
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager

DRIVER = Path(os.environ.get("CROSSFEED_ROUTING_DRIVER", Path(__file__).resolve().parents[1] / "bin/test_pipewire_routing"))
STUB = r'''import fcntl, json, os, sys
from pathlib import Path
root = Path(os.environ["CROSSFEED_ROUTING_FAKE"])
with (root / "lock").open("a") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    state = json.loads((root / "state.json").read_text())
    args = sys.argv[1:]
    command = Path(sys.argv[0]).name
    state["calls"].append([command] + args)
    outputs = ["crossfeed:monitor_FL", "crossfeed:monitor_FR", "crossfeed-dsp:out_FL", "crossfeed-dsp:out_FR", "player:out_FL", "player:out_FR"]
    inputs = ["crossfeed:playback_FL", "crossfeed:playback_FR", "crossfeed-dsp:in_FL", "crossfeed-dsp:in_FR"]
    for sink, ports in state["sinks"].items():
        inputs.extend(sink + ":" + port for port in ports)
    links = [link for link in state["links"] if link[0] in outputs and link[1] in inputs]
    code = 0
    if command == "pw-link":
        if args == ["-i"]:
            state["discoveries"] += 1
            if state.get("discovery_failure"): code = 1
            else: print("\n".join(inputs))
        elif args == ["-l"]:
            if state.get("graph_failure"): code = 1
            else:
                for port in sorted(set(outputs + inputs)):
                    print(port)
                    for source, target in links:
                        if source == port: print("  |-> " + target)
                        if target == port: print("  |<- " + source)
        elif args[:1] == ["-d"]:
            link = args[1:]
            if link in links: links.remove(link)
            else: code = 1
        elif len(args) == 2:
            if args[0] not in outputs or args[1] not in inputs: code = 1
            elif state.get("fail_destination") == args[1]:
                state.pop("fail_destination")
                code = 1
            elif args in links: code = 1 # Match EEXIST; healthy routes must be observed, not recreated.
            else: links.append(args)
        else: raise AssertionError(args)
    elif command == "pactl":
        if args == ["info"]: print("Default Sink: " + state["default"])
        elif args[:1] == ["set-default-sink"]: state["default"] = args[1]
        elif args == ["list", "short", "sinks"]:
            print("99\tcrossfeed\tfake\tfloat32le 2ch 48000Hz\tRUNNING")
            for index, sink in enumerate(state["sinks"]): print(str(index) + "\t" + sink + "\tfake\tfloat32le 2ch 48000Hz\tRUNNING")
        elif args == ["list", "short", "sink-inputs"]: pass
        elif args[:1] == ["move-sink-input"]: pass
        else: raise AssertionError(args)
    else: raise AssertionError(command)
    state["links"] = links
    (root / "state.json").write_text(json.dumps(state))
    sys.exit(code)
'''
STEREO = ["playback_FL", "playback_FR"]
SPEAKER = "analog.speaker"
DAC = "usb.dac"


class Graph:
    def __init__(self, root):
        self.root = root
        self.write(sinks={SPEAKER: STEREO, DAC: STEREO}, default=DAC, links=[], calls=[], discoveries=0)

    def write(self, **changes):
        with (self.root / "lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            path = self.root / "state.json"
            state = json.loads(path.read_text()) if path.exists() else {}
            state.update(changes)
            path.write_text(json.dumps(state))

    def read(self):
        with (self.root / "lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            return json.loads((self.root / "state.json").read_text())

    def linked(self, sink, ports=STEREO):
        links = self.read()["links"]
        return all(["crossfeed-dsp:out_" + channel, sink + ":" + port] in links
                   for channel, port in zip(("FL", "FR"), ports))


@contextmanager
def session(loop=False):
    with tempfile.TemporaryDirectory(prefix="crossfeed-hotplug-") as directory:
        root = Path(directory)
        graph = Graph(root)
        commands = root / "commands"
        commands.mkdir()
        for name in ("pactl", "pw-link"):
            path = commands / name
            path.write_text(f"#!{sys.executable}\n" + STUB)
            path.chmod(0o755)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                   XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime),
                   PIPEWIRE_REMOTE="unreachable-hotplug-test", PULSE_SERVER="unix:" + str(runtime / "no-pulse"),
                   XDG_CONFIG_HOME=str(root), CROSSFEED_ROUTING_FAKE=str(root))
        process = subprocess.Popen([str(DRIVER)] + (["--loop"] if loop else []), env=env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

        def command(value="STEP"):
            process.stdin.write(value + "\n")
            process.stdin.flush()
            assert select.select([process.stdout], [], [], 10)[0], "Routing step timed out"
            line = process.stdout.readline().rstrip("\n")
            assert line, f"Driver exited: {process.poll()}"
            return line.split("\t")

        try:
            yield graph, command
        finally:
            if process.poll() is None:
                process.stdin.write("QUIT\n")
                process.stdin.flush()
            assert process.wait(timeout=10) == 0


def ready(command, sink):
    result = command()
    assert result[1:3] == ["ready", sink], result


with session() as (graph, command):
    ready(command, DAC)
    assert graph.linked(DAC)
    for _ in range(16): ready(command, DAC)
    graph.write(sinks={SPEAKER: STEREO}, default=SPEAKER)
    ready(command, SPEAKER)
    # A remembered DAC wins over the temporary physical default on return.
    graph.write(sinks={SPEAKER: STEREO, DAC: STEREO}, default=SPEAKER)
    ready(command, DAC)
    assert graph.linked(DAC) and not graph.linked(SPEAKER)
    # Same sink name, new ports (e.g. a USB profile switch).
    ports = ["playback_AUX0", "playback_AUX1"]
    graph.write(sinks={SPEAKER: STEREO, DAC: ports})
    ready(command, DAC)
    assert graph.linked(DAC, ports)
    # Repair a single lost channel on an otherwise unchanged sink.
    state = graph.read()
    graph.write(links=[link for link in state["links"] if link != ["crossfeed-dsp:out_FR", DAC + ":playback_AUX1"]])
    ready(command, DAC)
    assert graph.linked(DAC, ports)
    # Query failures leave established routing intact and retry automatically.
    graph.write(discovery_failure=True)
    assert command()[1] == "reconnecting"
    assert graph.linked(DAC, ports)
    graph.write(discovery_failure=False, graph_failure=True)
    assert command()[1] == "reconnecting"
    graph.write(graph_failure=False)
    ready(command, DAC)
    assert command("TARGET crossfeed")[0] == "error"
    assert command("TARGET crossfeed-dsp")[0] == "error"
    # Prefer named stereo channels if a device also exposes auxiliary ports.
    graph.write(sinks={DAC: STEREO + ["playback_AUX0", "playback_AUX1"]})
    ready(command, DAC)
    assert graph.linked(DAC)
    assert not graph.linked(DAC, ["playback_AUX0", "playback_AUX1"])

with session() as (graph, command):
    assert command("TARGET " + DAC)[0] == "ok"
    ready(command, DAC)
    graph.write(sinks={SPEAKER: STEREO}, default=SPEAKER)
    assert command()[1] == "waiting"
    assert not graph.linked(SPEAKER) # Explicit selection never falls back to speakers.
    graph.write(sinks={SPEAKER: STEREO, DAC: ["playback_FL"]})
    assert command()[1] == "waiting" # Wait until both channels register.
    graph.write(sinks={SPEAKER: STEREO, DAC: STEREO})
    ready(command, DAC)
    assert graph.linked(DAC)
    # Manual target changes only publish; the worker applies them.
    previous = graph.read()["calls"]
    assert command("TARGET " + SPEAKER)[3] == SPEAKER
    assert graph.read()["calls"] == previous
    ready(command, SPEAKER)

with session() as (graph, command):
    graph.write(sinks={}, default="crossfeed")
    assert command()[1] == "waiting"
    assert not any(call[1:2] == ["set-default-sink"] for call in graph.read()["calls"])
    graph.write(sinks={DAC: STEREO}, fail_destination=DAC + ":playback_FR")
    assert command()[1] == "reconnecting"
    assert not graph.linked(DAC)
    assert not any(call[1:2] == ["set-default-sink"] for call in graph.read()["calls"])
    ready(command, DAC)
    assert graph.linked(DAC)

with session() as (graph, command):
    ready(command, DAC)
    graph.write(sinks={}, default="crossfeed")
    assert command()[1] == "waiting"
    graph.write(sinks={SPEAKER: STEREO, DAC: STEREO}, default=SPEAKER)
    ready(command, DAC)

with session() as (graph, command):
    graph.write(sinks={SPEAKER: STEREO}, default=SPEAKER)
    ready(command, SPEAKER)
    graph.write(sinks={SPEAKER: STEREO, DAC: STEREO}, fail_destination=DAC + ":playback_FR")
    result = command()
    assert result[1:3] == ["reconnecting", SPEAKER], result
    assert graph.linked(SPEAKER) and not graph.linked(DAC)
    ready(command, DAC)
    assert not graph.linked(SPEAKER)
    # A transient physical default is not a new explicit Crossfeed selection.
    graph.write(default=SPEAKER)
    ready(command, DAC)
    command("TARGET " + SPEAKER)
    for _ in range(4): ready(command, SPEAKER)
    command("TARGET auto")
    ready(command, DAC)

# Run the actual background loop beyond the old saturated stable counter.
# This specifically prevents a regression to polling only at stable_count % 4.
with session(loop=True) as (graph, command):
    deadline = time.monotonic() + 25
    while graph.read()["discoveries"] < 13:
        assert time.monotonic() < deadline, "Stable routing stopped discovering devices"
        time.sleep(.1)
    assert command("STATUS")[1:3] == ["ready", DAC]
    graph.write(sinks={SPEAKER: STEREO}, default=SPEAKER)
    deadline = time.monotonic() + 5
    while command("STATUS")[1:3] != ["ready", SPEAKER]:
        assert time.monotonic() < deadline, "Fallback did not recover"
        time.sleep(.1)
    graph.write(sinks={SPEAKER: STEREO, DAC: STEREO})
    deadline = time.monotonic() + 5
    while command("STATUS")[1:3] != ["ready", DAC]:
        assert time.monotonic() < deadline, ("DAC did not return after a stable session", graph.read())
        time.sleep(.1)
    assert graph.linked(DAC)
    command("STRESS") # Publish requests while the worker and status readers run.

print("PASS: native hotplug after stable sessions, DAC return, explicit targets, staged/profile replug, stereo repair, failed links/queries, waiting startup and concurrent requests; no audio server used.")
