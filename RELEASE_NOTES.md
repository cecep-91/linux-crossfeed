# Crossfeed 2.5.0

Crossfeed now has a refreshed GTK interface and an interactive frequency-response graph.

- Theme-aware panels, clearer controls, wrapping presets, and a compact layout for smaller windows. Checked with Adwaita and Breeze in light/dark variants, plus HighContrast.
- A mono/side response graph updates as you change presets, controls, Pure/Advanced mode, bypass, or sample rate. Hover for frequency and gain. It includes phase, fractional delay, and output trim; this is a calculated filter response, not a live spectrum analyzer.
- `crossfeed gui --preview` lets you explore the interface without starting or contacting the audio engine, changing routing, or saving settings.
- Fixed spatial-effects switch updates and kept controls available during bypass for A/B comparison.
- Status polling now notices changes in sample rate, buffer size, and backend.
- Per-user desktop launchers select the user-installed binary even when an older system package exists.

The existing DSP and preset voicing are unchanged in this release. Response-model tests compare the graph with processed audio impulses across all five presets, both modes, and 8–192 kHz. GTK interaction checks cover switches, presets, graph updates, compact layout, and stopped state. CI runs the GUI checks in light and dark themes alongside the DSP, IPC, real-time, routing, and sanitizer suites.

Packages: Debian/Ubuntu `.deb`, Fedora/RHEL `.rpm`, Void Linux `.xbps`, and a Linux x86_64 binary tarball. The GUI requires GTK3 and Ayatana AppIndicator; the KDE appearance uses the installed Breeze GTK theme.
