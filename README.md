# ClearMic

**Real-time microphone enhancement for Windows and Linux.**

ClearMic is an open-source, cross-platform desktop application designed to improve microphone voice quality in real time.

> **Development status:** the C++20 device model, Windows WASAPI and Linux PipeWire input enumeration backends, RNNoise WAV processing, and shared stateful DSP chain build on Windows and Ubuntu 24.04 in CI. The Linux CLI can explicitly capture up to 30 seconds from PipeWire, process it, and write original/processed WAV files for manual comparison. The DSP chain includes Natural, Meeting, and Strong Noise Reduction presets with noise suppression, optional gain, gate, compression, and AGC. Windows live capture is not connected. The desktop UI, battery telemetry, virtual microphone, persistent settings, and installers remain unimplemented. This is an early development project, not an installable product.

It aims to reduce background noise, echo, room reverberation, and inconsistent microphone levels while preserving a natural-sounding voice.

The long-term goal is simple:

> Turn any microphone into a cleaner, enhanced virtual microphone that can be used by any application.

ClearMic is designed to work with Bluetooth headsets, USB microphones, built-in laptop microphones, wireless microphones, and other audio input devices exposed by the operating system.

---

## 🎯 Why ClearMic?

Many microphones — especially Bluetooth headsets — provide acceptable audio playback but noticeably worse microphone quality.

Common issues include:

- Background noise
- Room echo and reverberation
- Poor voice clarity
- Low or inconsistent microphone volume
- Playback audio leaking into the microphone
- Aggressive or poor-quality built-in processing
- Bluetooth headset microphone limitations

ClearMic provides a local processing layer between the physical microphone and applications.

```text
Physical Microphone
        │
        ▼
┌─────────────────────────┐
│        ClearMic         │
│                         │
│   Noise Suppression     │
│   Echo Reduction        │
│   Reverb Reduction      │
│   Noise Gate            │
│   Compressor            │
│   Automatic Gain        │
└────────────┬────────────┘
             │
             ▼
   ClearMic Virtual Mic
             │
      ┌──────┼──────┬──────┐
      ▼      ▼      ▼      ▼
   Discord  Meet   Teams   Zoom
```

The processed microphone can eventually be selected like any other microphone in voice applications, browsers, games, recording software, and communication tools.

---

## ✨ Planned Features

### Audio enhancement

- Real-time noise suppression
- Echo reduction
- Room reverberation reduction
- Automatic gain control
- Noise gate
- Dynamic range compression
- Voice activity detection
- Configurable processing pipeline

### Microphone management

- Automatic input device discovery
- Microphone selection
- Input and output level monitoring
- Device information
- Sample rate and channel detection
- Device connection/disconnection handling

### A/B microphone testing

ClearMic will provide an easy way to compare the microphone before and after processing.

```text
Original microphone
        │
        ├──► original.wav
        │
        ▼
    ClearMic DSP
        │
        └──► processed.wav
```

This makes it possible to objectively evaluate whether a processing configuration actually improves voice quality.

### Virtual microphone

The long-term goal is to expose processed audio as:

```text
ClearMic Virtual Microphone
```

Applications such as Discord, Microsoft Teams, Google Meet, Zoom, OBS, browsers, games, and other voice applications will be able to use it as a regular microphone.

---

## 🖥️ Platforms

ClearMic targets:

| Platform | Audio Backend | Status |
| --- | --- | --- |
| Ubuntu / Linux | PipeWire | 🚧 Device enumeration builds in CI; runtime validation pending |
| Windows 11 | WASAPI | 🚧 Device enumeration builds in CI; runtime validation pending |

Additional Linux distributions using PipeWire may work in the future.

---

## 🏗️ Architecture

ClearMic separates platform-specific audio I/O from the audio processing engine.

```text
                     ClearMic
                        │
              ┌─────────┴─────────┐
              │                   │
          Windows               Linux
              │                   │
           WASAPI              PipeWire
              │                   │
              └─────────┬─────────┘
                        │
                        ▼
                ┌───────────────┐
                │  Audio Engine │
                │     C++       │
                └───────┬───────┘
                        │
                ┌───────▼────────┐
                │ DSP Processing │
                │                │
                │ Noise          │
                │ Echo           │
                │ Reverb         │
                │ Gain           │
                │ Gate           │
                │ Compression    │
                └───────┬────────┘
                        │
                        ▼
                 Virtual Input
```

The core audio processing code is designed to remain platform-independent.

Platform-specific implementations are isolated behind common interfaces.

---

## 🧠 Audio Processing

ClearMic is being designed around proven audio-processing techniques rather than proprietary cloud processing.

The current offline processor uses:

- RNNoise

WebRTC Audio Processing Module has not been integrated. A live processing pipeline and configurable processors remain future work.

A possible pipeline looks like:

```text
Audio Capture
     │
     ▼
Noise Suppression
     │
     ▼
Echo / Reverb Processing
     │
     ▼
Noise Gate
     │
     ▼
Compressor
     │
     ▼
Automatic Gain
     │
     ▼
Audio Output
```

Low latency and natural voice reproduction are primary design goals.

---

## 🔊 Echo vs. Reverberation

ClearMic distinguishes between different problems that are often simply described as "echo."

They can include:

- **Acoustic echo** — speaker output being captured again by the microphone
- **Room reverberation** — reflections from walls and the surrounding environment
- **Feedback** — audio being routed back into the input
- **Bluetooth limitations** — reduced microphone quality caused by Bluetooth audio profiles
- **Operating-system processing** — additional processing applied by the audio stack

True Acoustic Echo Cancellation (AEC) generally requires access to the playback/reference audio signal.

ClearMic will avoid presenting microphone-only filtering as true AEC when no reference signal is available.

---

## ⚡ Performance

Real-time audio software must be fast.

ClearMic is designed with the following principles:

- Low-latency audio capture
- Minimal allocations in audio callbacks
- No filesystem access in real-time audio threads
- No heavy logging in critical audio paths
- Predictable buffer handling
- Detection of underruns and overruns
- Measurable processing latency

Future diagnostic tools will expose metrics such as:

```text
Sample Rate:        48000 Hz
Channels:           1
Buffer Size:        480 samples

Capture Latency:    10 ms
Processing:          3 ms
Estimated Total:    18 ms
```

---

## 🔒 Privacy

ClearMic is designed to process audio **locally on your computer**.

By default, ClearMic does not require:

- Cloud audio processing
- User accounts
- Audio uploads
- External APIs
- Internet connectivity

Microphone audio should never leave the computer as part of the normal processing pipeline.

Recordings are only created when explicitly requested by the user.

---

## 🛠️ Technology

The current architecture targets:

### Audio engine

- C++20+
- CMake
- PipeWire on Linux
- WASAPI on Windows

### Audio processing

- RNNoise for explicit offline 48 kHz PCM16 WAV processing
- WebRTC Audio Processing and additional DSP remain under evaluation for live capture and playback-reference AEC

### Desktop UI

Planned:

- React
- TypeScript
- Lightweight native desktop shell

The project intentionally avoids committing to Electron unless there is a compelling technical reason to use it.

## 📚 Developer documentation

- [Architecture](docs/architecture.md)
- [Audio pipeline](docs/audio-pipeline.md)
- [Battery monitoring](docs/battery-monitoring.md)
- [Virtual microphone](docs/virtual-microphone.md)
- [Hardware compatibility](docs/hardware-compatibility.md)

---

## 📁 Project Structure

The project is expected to evolve roughly around this structure:

```text
clearmic/
├── audio-engine/
│   ├── core/
│   ├── processing/
│   ├── platform/
│   │   ├── linux/
│   │   └── windows/
│   └── tests/
│
├── desktop/
│   ├── ui/
│   └── native/
│
├── docs/
│   ├── architecture.md
│   ├── audio-pipeline.md
│   └── virtual-microphone.md
│
├── scripts/
│
├── CMakeLists.txt
├── README.md
└── LICENSE
```

The structure may change as the architecture evolves.

---

## 🚀 Building

> ClearMic is currently under active development. Build instructions may change.

### Ubuntu

Target environment:

- Ubuntu 24.04+
- PipeWire
- C++20 compatible compiler
- CMake

Install the current build dependencies with:

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev
```

The intended development workflow is:

```bash
git clone https://github.com/VinRamalho/clearmic.git
cd clearmic

cmake -S . -B build
cmake --build build
```

The first CMake configure downloads the pinned RNNoise source and model archives and verifies their SHA-256 hashes. An internet connection is required unless those archives are already cached in the build directory.

Once the CLI is available:

```bash
./build/clearmic-cli devices
```

Offline noise suppression for an existing WAV file:

```bash
./build/clearmic-cli process original.wav processed.wav
```

The current processor accepts PCM 16-bit WAV at 48 kHz with one or two channels. It rejects other sample rates and encodings rather than silently passing audio through.

Example expected output:

```text
ClearMic Audio Devices

[0] Built-in Audio Analog Stereo
[1] Bluetooth Headset
[2] USB Wireless Microphone

Default input: Bluetooth Headset
```

### Windows

Windows 11 support will use WASAPI.

The goal is to support a similarly simple CMake workflow using Visual Studio / MSVC.

The current Windows work is limited to device discovery; capture, processing, and virtual-microphone support remain unimplemented.

The current Windows device-enumeration target uses CMake and a Visual Studio C++ toolchain. Build from a Visual Studio Developer PowerShell with:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\clearmic-cli.exe devices
```

---

## 📦 Distribution

The eventual goal is to distribute ClearMic as a normal desktop application.

### Windows

```text
ClearMic-Setup.exe
```

No development environment should be required.

### Ubuntu

```text
clearmic_<version>_amd64.deb
```

Install with:

```bash
sudo apt install ./clearmic_<version>_amd64.deb
```

After installation, ClearMic should be available from the desktop application menu.

---

## 🗺️ Roadmap

### Phase 1 — Foundation

- [x] Project architecture
- [x] CMake build system (Windows and Ubuntu CI)
- [x] Common audio/device abstractions
- [ ] Linux / PipeWire device discovery
- [ ] Windows / WASAPI device discovery
- [ ] CLI device enumeration

### Phase 2 — Recording

- [ ] Microphone capture
- [ ] PCM WAV recording
- [ ] Original vs. processed recording
- [ ] Basic audio diagnostics

### Phase 3 — Audio Processing

- [x] Offline RNNoise processing for supported WAV files
- [ ] Live noise suppression
- [ ] Automatic gain control
- [ ] Noise gate
- [ ] Compression
- [ ] Echo processing
- [ ] Reverberation reduction
- [ ] Processing presets

### Phase 4 — Real-Time Processing

- [ ] Real-time processing pipeline
- [ ] Latency measurements
- [ ] Buffer monitoring
- [ ] Underrun / overrun diagnostics

### Phase 5 — Virtual Microphone

- [ ] PipeWire virtual microphone
- [ ] Windows virtual microphone architecture
- [ ] Application compatibility testing

### Phase 6 — Desktop Application

- [ ] Desktop UI
- [ ] Device selector
- [ ] Input/output level meters
- [ ] Processing controls
- [ ] Audio profiles
- [ ] A/B recording test

### Phase 7 — Distribution

- [ ] Windows installer
- [ ] Ubuntu `.deb`
- [ ] GitHub Actions builds
- [ ] Automated tests
- [ ] Release pipeline

---

## 🎚️ Future UI Concept

```text
┌──────────────────────────────────────────┐
│ ClearMic                                 │
├──────────────────────────────────────────┤
│                                          │
│ Microphone                               │
│ [ Bluetooth Headset                  ▼ ] │
│                                          │
│ Input                                    │
│ ███████████████░░░░░                     │
│                                          │
│ Enhance Microphone                [ ON ]  │
│                                          │
│ Noise Suppression                       │
│ Low ───────────●──────────── High        │
│                                          │
│ Echo Reduction                   [ ON ]  │
│ Reverb Reduction                 [ ON ]  │
│ Automatic Gain                   [ ON ]  │
│ Noise Gate                       [ ON ]  │
│                                          │
│ Profile                                  │
│ [ Natural ] [ Meeting ] [ Strong ]       │
│                                          │
│          [ Record A/B Test ]              │
│                                          │
└──────────────────────────────────────────┘
```

---

## 🤝 Contributing

ClearMic is in the early stages of development.

Contributions, experiments, bug reports, DSP research, platform-specific improvements, and audio-quality comparisons will be welcome as the project matures.

When contributing audio-processing changes, prefer measurable improvements and A/B testing over subjective assumptions.

---

## 📄 License

The project is intended to be open source.

The final license will be defined before the first public release.

---

## Clear voice. Local processing. Any microphone.

**ClearMic**
