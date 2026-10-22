# Crossfeed 2.7.0 — Real-Time Lock-Free DSP Triple-Buffering & PipeWire Optimization

- **Real-Time Lock-Free DSP Parameters**:
  - Implemented tri-slot parameter ownership model (writer, exchange, reader).
  - Atomic exchange transfers parameter snapshots with acquire/release ordering, guaranteeing zero allocations, zero lock contention, and true real-time safety during parameter updates in the audio thread.
- **PipeWire Link Management Loop Optimization**:
  - Optimized link manager polling and routing recovery loop, eliminating redundant CPU wakeups while audio streams are active.
  - Added automated native routing lifecycle regression testing (`test/pipewire_routing_driver.cpp`, `test/test_pipewire_routing.py`).
- **Hardened IPC Socket Security**:
  - Added peer credential verification (`SO_PEERCRED` on Linux) to restrict control socket communication strictly to the owning user UID.
  - Implemented bounded socket read/write timeouts and robust malformed command handling.
- **Extended Test & Characterization Suites**:
  - Comprehensive characterization test coverage for filter transitions, sub-sample delay interpolation, 8–192 kHz Nyquist limits, and mono/side impulse alignment.
  - Added headless UI test verification (`xvfb-run -a make test-gui`).
- **Graceful Self-Updater Shutdown**:
  - Refined `scripts/update.sh` to cleanly stop running background engine instances before rebuilding and smoothly restart them after installation.

---

# Crossfeed 2.6.1 — GUI switch lever state synchronization fix

- Fixes `GtkSwitch` toggling and sliding behavior on GNOME and KDE desktops (master bypass switch and spatial effects switch).
- Properly handles `state-set` gesture signals to confirm animation completion and synchronize slider handle state.
- Prevents UI status polling from interrupting active switch drag and click animations.
- Enforces `GTK_ALIGN_CENTER` widget alignment on switch controls to eliminate trough stretching under themes like Orchis and Breeze.

---

# Crossfeed 2.6.0 — optional listening comparisons and saved presets

- Original audio follows the processed path's active output trim: −2 dB trim applies −2 dB to both. The shared preamp also applies to both. Pure mode bypasses the trim stage on both paths. No automatic loudness measurement or calibration is performed.
- The whole Hear the difference card is hidden by default. Enable **Menu → Show comparison tools** to reveal comparison buttons, A/B slots, output volume, and meters. This GUI preference is saved separately from engine state; preview mode never persists it.
- A/B session slots, original/crossfeed choices with a clear selected state, and guided setup.
- Custom preset library: save, rename, duplicate, delete, import/export, and exact-name CLI recall. Files are validated and saved atomically; file-management actions live in the More menu.
- Independent −24 to +12 dB preamp, input/output sample-peak meters, held peaks, and clip indicators. The response graph includes preamp and active trim in bypass.
- CLI commands: `ab store a|b`, `ab a|b|dry`, `meters reset`, and `set --preamp`.
- Repository URLs follow the `linux-crossfeed` rename. Configuration paths and release archive names remain compatible. Release packaging includes nested screenshot assets.

Copying trim gives both paths the same output-gain setting; it does not equalize perceived loudness or the energy changes caused by filtering and blending. Existing processed preset voicing is retained. Ordinary bypass and trim changes are smoothed; unsupported sample rates still use unscaled fallback until prepared settings arrive.

Verification covers shared trim at 8–192 kHz, Pure/Advanced modes, planar/interleaved agreement, graph response, A/B recall, GUI preferences, preset storage, telemetry concurrency, and callback allocation/lock checks.

---

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
