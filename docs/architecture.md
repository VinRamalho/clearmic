# ClearMic architecture

ClearMic uses a C++20 core shared by Windows and Linux. The common `AudioDevice` model keeps operating-system identifiers and optional capabilities separate from platform discovery. Missing device or battery telemetry is represented as unknown (`std::optional`), never as a fabricated value.

`IAudioDeviceManager` is the platform boundary. Windows enumerates active capture endpoints through Core Audio/WASAPI. Linux enumerates PipeWire `Audio/Source` nodes. The initial CLI presents this data without a desktop shell, keeping the first runtime milestone useful while the audio engine and product UI are built later.

The processing engine will consume and produce platform-independent PCM frames. Capture/output callbacks must not perform file I/O or blocking UI work. A real virtual microphone requires routing processed frames back into a platform audio source; it is separate from device discovery.

## Current scope

- Implemented: shared device and optional battery capability model, native input-device enumeration backends, CLI, and core model test.
- The CLI can apply Xiph RNNoise to explicit 48 kHz PCM16 WAV files. A shared stateful processing chain now handles arbitrary PCM16 callback sizes with preset-backed gate, gain, compression, and AGC controls. This DSP core is not yet connected to live microphone capture.
- Windows backend reads the default Windows audio mix format as device capability information. Battery and device-change monitoring are not implemented.
- Linux backend requires PipeWire development headers (`libpipewire-0.3-dev`). Linux build/runtime validation requires a PipeWire system.
- Linux has a bounded PipeWire `record-test` path that captures a selected/default source, runs the shared processor, and writes original and processed WAVs on explicit request. It is not yet a long-running capture/output service. Windows live capture, A/B playback UI, virtual microphone, UI, persistent settings, and installers remain future work.
