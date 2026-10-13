"""Embed measured C++ impulse responses into the offline walkthrough."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
result = subprocess.run([str(root / "bin/render_response")], check=True, capture_output=True, text=True)
data = json.loads(result.stdout)
assert len(data["presets"]) == 5
for preset in data["presets"]:
    assert len(preset["modes"]) == 2
    for mode in preset["modes"]:
        assert len(mode["points"]) == 161
template = (root / "docs/walkthrough.template.html").read_text()
assert template.count("@@RESPONSE_DATA@@") == 1
encoded = json.dumps(data, separators=(",", ":"), allow_nan=False).replace("</", "<\\/")
output = root / "docs/walkthrough.html"
output.write_text(template.replace("@@RESPONSE_DATA@@", encoded))
print(f"Generated {output}: five presets, pure/advanced mono and side responses from the compiled DSP.")
