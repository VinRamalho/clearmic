# ClearMic

**Real-time microphone enhancement for Windows and Linux.**

ClearMic is an open-source, cross-platform desktop application designed to improve microphone voice quality in real time.

> **Development status:** the C++20 device model, Windows WASAPI and Linux PipeWire audio backends, shared stateful DSP chain, and CI builds for Windows and Ubuntu 24.04 are in place. Both platforms provide CLI `record-test` A/B capture. Linux has a GTK panel to discover/select a PipeWire microphone, persist settings, start/stop the `ClearMic Virtual Microphone` service, view live input/output RMS meters, record and compare samples, refresh devices while idle, recover from service failures, and optionally run in the system tray. Windows has a native panel for WASAPI input and playback endpoint selection, persistent processing controls, A/B capture/playback, and start/stop continuous processing into a selected playback endpoint. Using that stream as a microphone still requires a separately installed compatible virtual audio cable; the MSI does not install a driver, and native cable routing remains unverified. Linux reads Bluetooth battery percentages from BlueZ when available; Windows and the tested USB receiver do not currently provide battery telemetry. The CPack/WiX MSI passes CI installation, launch, and removal checks. Native audio quality and latency validation remain incomplete. The Ubuntu `.deb` includes the GUI. This is an early development project, not a finished product.

It currently targets background noise and inconsistent microphone levels while preserving a natural-sounding voice. Acoustic echo cancellation and room reverberation reduction are not implemented.

The product goal is simple:

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

ClearMic provides a local processing layer between the physical microphone and applications. Today, the continuous virtual-microphone route is implemented on Linux through PipeWire. Windows supports device selection and explicit A/B capture/playback; its continuous virtual-microphone route is still under development.

```text
Physical Microphone
        │
        ▼
┌─────────────────────────┐
│        ClearMic         │
│                         │
│   Noise Suppression     │
│   Input Gain            │
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

On Linux, the processed microphone can be selected like any other PipeWire source in voice applications, browsers, games, recording software, and communication tools while ClearMic is running.

---

## ✨ Capabilities and planned work

### Audio enhancement

- [x] Linux real-time noise suppression
- [ ] Windows real-time noise suppression
- [ ] Acoustic echo cancellation (requires synchronized playback reference)
- [ ] Room reverberation reduction
- [x] Automatic gain control
- [x] Noise gate
- [x] Dynamic range compression
- [ ] Voice activity detection
- [x] Configurable processing pipeline and presets

### Microphone management

- Automatic input device discovery
- Microphone selection
- Input and output level monitoring
- Device information
- Sample rate and channel detection
- Device connection/disconnection handling

### A/B microphone testing

ClearMic provides an explicit way to compare the microphone before and after processing.

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

The desktop panels record five-second A/B samples using the selected profile and controls, then play either recording locally. Listening with real hardware is still needed to evaluate voice quality.

### Virtual microphone

The Linux service exposes processed audio as:

```text
ClearMic Virtual Microphone
```

On Linux, applications such as Discord, Microsoft Teams, Google Meet, Zoom, OBS, browsers, games, and other voice applications can select the PipeWire source while the ClearMic service is running. On Windows, select a virtual cable's playback endpoint in ClearMic and its paired recording endpoint in the target application. ClearMic does not bundle that Windows driver; native endpoint pairing and audio delivery still need hardware validation.

Echo cancellation requires a playback reference, and reverberation reduction is not implemented yet. The current live chain provides RNNoise suppression, input gain, gate, compressor, and automatic gain control; it does not claim to remove acoustic echo or room reverberation.

---

## 🖥️ Platforms

ClearMic targets:

| Platform | Audio Backend | Status |
| --- | --- | --- |
| Ubuntu / Linux | PipeWire | 🚧 GTK desktop panel, device monitoring, presets/controls, A/B capture/playback, live service, and virtual source; native hardware quality validation pending |
| Windows 11 | WASAPI | 🚧 Native desktop panel, device monitoring, saved DSP controls, A/B capture/playback, and CI-verified MSI lifecycle; continuous virtual microphone pending |

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

The current processor uses RNNoise for noise suppression. The stateful chain also provides input gain, a noise gate, compression, and RMS-based automatic gain control.

The Linux live pipeline uses the shared stateful processor chain. Windows currently processes captured A/B samples after recording; it does not yet run a continuous stream into a virtual endpoint. WebRTC Audio Processing Module has not been integrated.

The Linux live pipeline is:

```text
Audio Capture
     │
     ▼
Noise Suppression
     │
     ▼
Input Gain and Noise Gate
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

The Windows panel shows the selected WASAPI device ID, mix sample rate, and channel count; after an A/B capture it also shows the actual capture buffer size, nonzero WASAPI stream latency when reported, and offline DSP wall time for the sample. A missing or zero latency value remains unavailable. Processing wall time is not per-callback processing latency, and end-to-end latency for a Windows virtual-microphone route remains unavailable. The Linux service reports processed duration and underrun/overrun counters; callback and capture latency instrumentation remains open. ClearMic does not claim fixed measurements such as:

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

- RNNoise for offline WAV processing and explicit short A/B capture tests at 48 kHz PCM16
- Shared stateful DSP chain with Natural, Meeting, and Strong Noise Reduction presets
- Optional input gain, noise gate, compressor, and RMS-based automatic gain control in that chain
- WebRTC Audio Processing and additional DSP remain under evaluation for live capture and playback-reference AEC

### Desktop UI

Linux uses a GTK 3 desktop panel and Windows uses a native Win32 panel. Windows can route live processed audio to an explicitly selected playback endpoint, with a separately installed virtual cable required to expose the audio as a microphone.

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
```

The project structure continues to evolve with the implementation.

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
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev libgtk-3-dev
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

Capture a five-second microphone test and save original and processed audio for listening comparison:

```bash
./build/clearmic-cli record-test 5 original.wav processed.wav
```

The optional final argument selects a discovered device ID. Without it, the platform default microphone is used. Linux requires an available PipeWire source; Windows uses WASAPI. Capture is limited to 30 seconds and audio is processed locally after recording.

On Linux, run the continuous processor and expose its output to desktop applications as a virtual PipeWire microphone:

```bash
./build/clearmic-cli serve [pipewire-source-id]
```

Omit the optional source ID to use PipeWire's default microphone. Keep the process running while selecting **ClearMic Virtual Microphone** in the target application; press Ctrl+C to stop. The Linux desktop panel can manage the service, select a device, and adjust processing controls; see [Virtual microphone](docs/virtual-microphone.md) for details.

Offline noise suppression for an existing WAV file:

```bash
./build/clearmic-cli process original.wav processed.wav
```

The current processor accepts PCM 16-bit WAV at 48 kHz with one or two channels. It rejects other sample rates and encodings rather than silently passing audio through.

Example output format:

```text
ClearMic input devices
* USB Microphone
    ID: <platform device ID>
    Sample rate: 48000 Hz
    Channels: 1
```

### Windows

The native ClearMic desktop panel discovers active WASAPI microphones, remembers the selected device and DSP settings, and records/plays a five-second original/processed comparison. The CLI also provides bounded WASAPI microphone capture.

The supported development build uses CMake and Visual Studio / MSVC.

`record-test` captures up to 30 seconds and writes original/processed WAV files. The recorded sample is converted to 48 kHz mono PCM and processed after capture ends. The Windows panel applies the selected Natural/Meeting/Strong profile and individual controls to its A/B capture. Input/output level bars show the last recorded sample. The Windows live route does not yet publish live meter values or provide its own virtual-microphone endpoint.

Build from a Visual Studio Developer PowerShell with:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\clearmic-cli.exe devices
```

---

## 📦 Distribution

The eventual goal is to distribute ClearMic as a normal desktop application.

### Windows

The Windows workflow builds a CPack/WiX MSI containing the desktop and CLI executables and verifies installation, installed files, desktop launch, and removal. The desktop can continuously process WASAPI microphone input and render it to a selected playback endpoint. To use that stream as a microphone, install a compatible virtual audio cable separately, select its playback endpoint in ClearMic, and select its paired recording endpoint in the voice application. The MSI does not include or install a virtual-audio driver; Windows virtual-microphone packaging and native compatibility validation remain open. A public release also needs a project license.

### Ubuntu

```text
clearmic_<version>_amd64.deb
```

Install with:

```bash
sudo apt install ./clearmic_<version>_amd64.deb
```

Build the Ubuntu `.deb` from an Ubuntu 24.04 environment using `packaging/linux/build-deb.sh`. The package includes `clearmic-cli`, the GTK control panel, a man page, and a desktop launcher. The GUI can start/stop real-time routing and select the **ClearMic Virtual Microphone** in other apps. Install `libpipewire-0.3-dev`, `libgtk-3-dev`, and `libgstreamer1.0-dev` to build from source; end users receive GTK and GStreamer runtime dependencies through the package.

---

## 🗺️ Roadmap

### Phase 1 — Foundation

- [x] Project architecture
- [x] CMake build system (Windows and Ubuntu CI)
- [x] Common audio/device abstractions
- [x] Linux / PipeWire device discovery
- [x] Windows / WASAPI device discovery
- [x] CLI device enumeration
- [x] Ubuntu command-line `.deb` packaging
- [x] Initial GTK Linux desktop window with microphone selection and service start/stop

### Phase 2 — Recording

- [x] Bounded microphone capture for explicit CLI A/B tests (PipeWire and WASAPI)
- [x] PCM WAV recording
- [x] Original vs. processed recording files
- [x] Basic audio diagnostics (processed duration and Linux service underrun/overrun counters)

### Phase 3 — Audio Processing

- [x] Offline RNNoise processing for supported WAV files
- [x] Stateful DSP core with RNNoise noise suppression
- [x] Automatic gain control, noise gate, and compression stages in the core
- [x] Processing presets
- [ ] Echo processing
- [ ] Reverberation reduction

### Phase 4 — Real-Time Processing

- [x] Linux PipeWire capture-to-processing pipeline
- [x] Bounded capture/virtual-source audio queue
- [x] Basic processed-duration and buffer underrun/overrun counters
- [x] Windows real-time capture/process/render pipeline (virtual cable required)
- [ ] Latency measurements

### Phase 5 — Virtual Microphone

- [x] PipeWire virtual microphone source (Linux development command)
- [ ] Bundled Windows virtual microphone endpoint and driver installation
- [ ] Application compatibility testing

### Phase 6 — Desktop Application

- [x] Desktop UI (GTK on Linux and native Win32 on Windows)
- [x] Device selector
- [x] Input/output level meters (Linux live stream and last A/B sample on Windows)
- [x] Processing controls
- [x] Audio profiles
- [x] A/B recording test

### Phase 7 — Distribution

- [x] Windows CPack/WiX installer with CI install, launch, and removal validation
- [x] Ubuntu command-line `.deb`
- [x] GitHub Actions builds (Windows and Ubuntu; MSI lifecycle check passes)
- [x] Automated tests (core and CLI validation; see CI for platform coverage)
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
│ Compressor                       [ ON ]  │
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
