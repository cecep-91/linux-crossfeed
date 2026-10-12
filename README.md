<div align="center">
  <img src="data/crossfeed.svg" width="96" height="96" alt="Crossfeed Logo" />
  <h1>Crossfeed</h1>
  <p><strong>Ultra-Low-Latency Psychoacoustic Headphone Crossfeed &amp; Spatial Audio Processor for Linux</strong></p>
  <p>
    <a href="https://github.com/cecep-91/pipewire-crossfeed/releases"><img src="https://img.shields.io/github/v/release/cecep-91/pipewire-crossfeed?color=blue&label=Release" alt="Latest Release"/></a>
    <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B" alt="C++17"/>
    <img src="https://img.shields.io/badge/Audio-PipeWire%20%7C%20Pulse%20%7C%20ALSA-informational" alt="Audio Backend"/>
    <img src="https://img.shields.io/badge/DSP%20Latency-%3C3%C2%B5s-brightgreen" alt="DSP Latency"/>
    <img src="https://img.shields.io/badge/CPU-%3C0.05%25-brightgreen" alt="CPU Usage"/>
    <img src="https://img.shields.io/badge/Packaging-xbps%20%7C%20deb%20%7C%20rpm-orange" alt="Packages"/>
    <img src="https://img.shields.io/badge/License-MIT-green" alt="License"/>
  </p>
</div>

---

Crossfeed is a standalone, single-binary C++ audio engine designed to eliminate headphone fatigue and "in-head localization." It features an ultra-responsive native GTK3 graphical interface, full system tray integration, dynamic live parameter tuning, and universal compatibility across all Linux distributions and sound servers (PipeWire, PulseAudio, and ALSA).

---

## The Nerd Stuff: Psychoacoustics &amp; DSP Architecture

### 1. The Headphone Dilemma ("In-Head Localization")
When listening to loudspeakers in a room, your ears naturally receive acoustic cross-talk: sound from the left speaker travels across the room, diffracts around your skull, and arrives at your right ear slightly delayed, attenuated, and filtered. Your brain uses these subtle spatial cues to construct a wide, natural, externalized 3D soundstage.

In contrast, stereo headphones create **unnatural acoustic isolation**. Left-channel audio never reaches the right ear. On classic stereo recordings with hard-panned instruments (e.g., 1960s–70s rock, jazz, or classical mixes), this produces extreme binaural disparity, causing:
- **"Super-Stereo" Fatigue**: The sensation of sound originating inside the center of your skull rather than in front of you.
- **Auditory Exhaustion**: The cognitive strain of the brain struggling to place hard-panned sound sources in physical space.

Crossfeed rectifies this by routing an acoustically modeled, frequency-sculpted, time-delayed copy of each stereo channel to the contralateral (opposite) ear.

```
          [ Stereo Audio Input (L / R) ]
                        │
         ┌──────────────┴──────────────┐
         ▼                             ▼
   Direct Signal                 Contralateral Signal
         │                             │
   [ Low-Shelf Filter ]          [ Low-Pass Filter (Cross Crossover) ]
   (Direct ear compensation)           │
         │                       [ Acoustic Head Shadow Filter ]
         │                       (High-frequency skull absorption)
         │                             │
         │                       [ All-Pass Filter (APF) ]
         │                       (Comb-filtering phase alignment)
         │                             │
         │                       [ Fractional Delay Line ]
         │                       (Interaural Time Delay, 0–800 µs)
         │                             │
         │                       [ Interaural Level Difference (ILD) ]
         │                       (Opposite-channel attenuation)
         │                             │
         └──────────────┬──────────────┘
                        ▼
               [ Channel Summing ]
                        │
             [ Center Summing Trim ]
             (Master gain compensation: -6 dB to 0 dB)
                        │
                        ▼
             [ Headphone Output ]
```

---

### 2. Core Acoustic Elements

#### Interaural Level Difference (ILD)
The skull acts as an acoustic barrier, attenuating the opposite-ear signal. Crossfeed provides adjustable opposite-ear blend attenuation from **-30.0 dB to -6.0 dB** (default: **-10.0 dB**), mimicking varying virtual speaker angles.

#### Interaural Time Delay (ITD)
Sound travels at approximately $343\text{ m/s}$ in dry air. For an average human head width ($\sim 18\text{–}20\text{ cm}$), audio from a speaker angled at $30^\circ$ takes an extra $250\text{–}300\ \mu\text{s}$ to travel around the skull curvature to the opposite ear.
- **Engine Implementation**: A dedicated 2048-sample circular buffer with **sub-sample fractional linear interpolation**, allowing continuous jitter-free delay modulation between **$0\ \mu\text{s}$ and $800\ \mu\text{s}$** without comb stepping or zipper noise.

#### Phase Alignment & Comb-Filtering Prevention
When a delayed contralateral signal recombines with direct audio, destructive phase cancellation (comb filtering) can produce a hollow, thin sound and sap bass impact.
- **Engine Implementation**: A 2nd-order Audio EQ Cookbook **All-Pass Filter (APF)** with flat magnitude response ($200\text{–}4000\text{ Hz}$). Long omnidirectional bass wavelengths remain coherently in-phase to preserve punch, while higher frequencies rotate naturally to deliver spatial localization.

#### Acoustic Head Shadow
Human tissue and skull geometry do not absorb all frequencies equally. Low bass diffuses freely around obstacles, whereas treble ($>1.5\text{ kHz}$) is absorbed and shadowed.
- **Engine Implementation**: A 2nd-order low-pass head shadow filter ($1000\text{–}8000\text{ Hz}$, default: $3000\text{ Hz}$) that rolls off high treble on the cross-channel, keeping the stereo field airy while preventing phase blur.

#### Center Summing Trim Matrix
Summing Left and Right signals adds acoustic energy to center-panned mono elements (vocals, kick drums, bass), producing a $+1.5\text{ dB}$ to $+3.0\text{ dB}$ center boost that can cause $0\text{ dBFS}$ digital clipping overshoots.
- **Engine Implementation**: A master gain compensation stage ($-6.0\text{ dB}$ to $0.0\text{ dB}$, default: $-1.5\text{ dB}$) that prevents volume build-up and maintains transparent LUFS loudness parity with the bypassed state.

---

### 3. Dual-Mode Architecture

The engine features a dedicated toggle between two listening philosophies:

| Mode | Processing Pipeline | Best Suited For |
|---|---|---|
| **All Spatial Effects** | Full psychoacoustic model: ITD fractional delay line + Phase APF + Head Shadow absorption + Center Trim | Modern music, complex stereo mixes, binaural immersion, spatial staging |
| **Pure Crossfeed** | Classic analog low-shelf blend + low-pass filter only (zero delay, zero phase shift, 0 dB trim) | Vintage recordings, analog purists, minimalist low-latency setups |

---

### 4. Acoustic Emulation Presets

Inside the **`Not for me options :v`** expander, you'll find pre-tuned models calibrated to famous hardware circuits and acoustic benchmarks:

- **Jan Meier (Corda)**: $-9.5\text{ dB}$ blend, $650\text{ Hz}$ crossover, $280\ \mu\text{s}$ delay, $1500\text{ Hz}$ APF, $-1.5\text{ dB}$ trim, $3200\text{ Hz}$ shadow. Natural, fatigue-free presentation.
- **Chu Moy (HeadWize)**: $-6.0\text{ dB}$ blend, $700\text{ Hz}$ crossover, $260\ \mu\text{s}$ delay, $2000\text{ Hz}$ APF, $-2.0\text{ dB}$ trim, $2800\text{ Hz}$ shadow. Classic analog RC crossfeed recreation.
- **Bauer BS2B**: $-4.5\text{ dB}$ blend, $700\text{ Hz}$ crossover, $350\ \mu\text{s}$ delay, $1200\text{ Hz}$ APF, $-2.5\text{ dB}$ trim, $2500\text{ Hz}$ shadow. Higher cross-feed simulation for pronounced centering.
- **Siegfried Linkwitz**: $-7.0\text{ dB}$ blend, $1200\text{ Hz}$ crossover, $220\ \mu\text{s}$ delay, $1600\text{ Hz}$ APF, $-1.8\text{ dB}$ trim, $4000\text{ Hz}$ shadow. Wide-stage loudspeaker monitor emulation.
- **Natural Studio 30°**: $-8.0\text{ dB}$ blend, $850\text{ Hz}$ crossover, $250\ \mu\text{s}$ delay, $1800\text{ Hz}$ APF, $-1.5\text{ dB}$ trim, $3500\text{ Hz}$ shadow. Reference equilateral studio monitor geometry.

---

## Distro Packaging

Crossfeed is packaged as a single native binary containing the engine, GUI, CLI, and tray app. Packages can be generated using `make`:

```sh
# Build all packages (.xbps, .deb, and .rpm) into dist/
make pkg

# Or build individually:
make xbps   # Void Linux (.xbps)
make deb    # Debian, Ubuntu, Linux Mint (.deb)
make rpm    # Fedora, RHEL, openSUSE (.rpm)
```

Packages install `/usr/bin/crossfeed`, `/usr/share/applications/crossfeed.desktop`, the SVG application icon, and license metadata.

### Installing Packages

```sh
# Void Linux (.xbps)
xbps-rindex -a dist/*.xbps
sudo xbps-install -R dist crossfeed

# Debian / Ubuntu / Linux Mint (.deb)
sudo apt install ./dist/crossfeed_2.2.0-1_amd64.deb

# Fedora / RHEL / openSUSE (.rpm)
sudo dnf install ./dist/crossfeed-2.2.0-1.x86_64.rpm

# Fedora Silverblue / Kinoite / Bazzite / CoreOS (rpm-ostree)
rpm-ostree install ./dist/crossfeed-2.2.0-1.x86_64.rpm
```

> [!NOTE]
> **rpm-ostree & Immutable OS Compatibility**: Crossfeed RPM complies 100% with OSTree standards: all files install strictly under `/usr`, with zero scriptlets (`%pre`/`%post`) and zero mutable `/etc` or `/var` writes, enabling clean atomic package layering.

---

## Build from Source & Local Installation

### Prerequisites

| Distro | Dependencies |
|---|---|
| **Void Linux** | `sudo xbps-install -S base-devel pipewire-devel pulseaudio-devel alsa-lib-devel gtk+3-devel libayatana-appindicator-devel` |
| **Ubuntu / Debian** | `sudo apt install build-essential libpipewire-0.3-dev libpulse-dev libasound2-dev libgtk-3-dev libayatana-appindicator3-dev` |
| **Fedora** | `sudo dnf install gcc-c++ make pipewire-devel pulseaudio-libs-devel alsa-lib-devel gtk3-devel libayatana-appindicator-gtk3-devel` |
| **Arch Linux** | `sudo pacman -S --needed base-devel pipewire libpulse alsa-lib gtk3 libayatana-appindicator` |

### Compile & Install

```sh
git clone https://github.com/cecep-91/pipewire-crossfeed.git
cd pipewire-crossfeed
make -j$(nproc)
./install.sh
```

Or install/upgrade directly with a single command without cloning:
```sh
curl -fsSL https://raw.githubusercontent.com/cecep-91/pipewire-crossfeed/main/install.sh | bash
```

To uninstall cleanly:
```sh
./install.sh --uninstall
```

---

## Upgrading Crossfeed

Crossfeed makes upgrading seamless across all environments:

### 1. In-App One-Click Upgrade (GUI)
- Open Crossfeed and open the header menu **`☰`** (or right-click the system tray icon).
- Select **"Check for Updates..."**.
- When an update is detected, click **"Upgrade Now"**. Crossfeed automatically downloads the latest release, updates files, and restarts the audio engine with zero downtime.

### 2. Command Line (CLI)
```sh
# Check if an update is available:
crossfeed check-update

# Upgrade to the latest version:
crossfeed upgrade
```

### 3. Git Source Trees
If you cloned the repository locally:
```sh
make update
# or:
./install.sh --update
```

---

## Usage

### 1. Graphical Interface & System Tray

Launch Crossfeed from your application launcher or terminal:
```sh
crossfeed
```

- **Header Bar**: Master toggle switch and status indicator.
- **Scroll Protection**: Mouse wheel or touchpad scrolling over sliders will never accidentally alter values; the view scrolls naturally.
- **"Not for me options :v" Expander**:
  - **Acoustic Spatial Effects Switch**: Toggle between Pure Crossfeed and All Effects.
  - **Emulation Presets**: One-click switching between Jan Meier, Chu Moy, Bauer BS2B, Linkwitz, and Studio 30°.
  - **Sliders**: Acoustic Delay (ITD), Phase APF, Center Summing Trim, and Head Shadow.
- **System Tray**: Closing the window docks Crossfeed to the tray. Right-click the tray icon to toggle bypass, change presets, switch modes, or quit cleanly.

### 2. Command Line Interface

The same binary provides full CLI control:

```sh
# Service management
crossfeed start     # Start background engine
crossfeed status    # View active routing, latency, and parameters
crossfeed stop      # Stop engine and restore default routing
crossfeed restart   # Restart engine

# Real-time filter toggling
crossfeed toggle    # Toggle bypass (ideal for keyboard shortcuts)
crossfeed on        # Force enable
crossfeed off       # Force bypass

# Adjust parameters on the fly
crossfeed set --pure                                            # Pure Crossfeed mode
crossfeed set --effects                                         # All Spatial Effects mode
crossfeed set --level -12.0 --freq 650 --delay 280 --phase 1500 # Live parameter tuning

# Benchmark DSP throughput
crossfeed bench
```

---

## References & Scientific Literature

- **Dr. Jan Meier (Meier Audio / Corda)**: Research on natural headphone crossfeed filters, passive attenuation networks, and physiological listening fatigue. ([Meier Audio Crossfeed](https://www.meier-audio.de/crossfeed.html))
- **Chu Moy (HeadWize)**: Pioneering work on passive analog crossfeed circuits for headphone listening.
- **Boris Bauer (BS2B)**: Development of the Bauer Stereophonic-to-Binaural DSP model and research on stereophonic cross-talk reproduction. ([Bauer BS2B Project](https://bs2b.sourceforge.net/))
- **Siegfried Linkwitz**: Acoustic research on stereo imaging, head-diffraction compensation, and loudspeaker-to-headphone simulation.
- **Robert Bristow-Johnson**: *Audio EQ Cookbook* — mathematical foundation for second-order biquadratic filter coefficient calculation (Direct Form II Transposed).

---

## Shoutouts & Acknowledgments

- **Wim Taymans & the PipeWire Team**: For engineering PipeWire into the ultra-low-latency multimedia foundation for modern Linux.
- **Ayatana Indicators Community**: For maintaining the Ayatana AppIndicator protocol, bringing rock-solid tray icons to Wayland and X11 desktops alike.
- **EasyEffects & Linux Pro-Audio Developers**: For pushing Linux audio DSP forward and inspiring pristine multi-node compatibility.
- **Void Linux, Debian, Fedora & Arch Maintainers**: For outstanding packaging ecosystems and minimal developer tooling.

---

## License

Released under the [MIT License](LICENSE). Copyright © 2024–2026.
