# ClearMic architecture

ClearMic uses a C++20 core shared by Windows and Linux. The common `AudioDevice` model keeps operating-system identifiers and optional capabilities separate from platform discovery. Missing device or battery telemetry is represented as unknown (`std::optional`), never as a fabricated value.

`IAudioDeviceManager` is the platform boundary. Windows enumerates active capture endpoints through Core Audio/WASAPI. Linux enumerates PipeWire `Audio/Source` nodes. The CLI presents this data without a desktop shell. Linux also provides a persistent PipeWire command that connects an input stream, the shared DSP chain, and a virtual `Audio/Source` stream.

The processing engine consumes and produces platform-independent PCM frames. Capture/output callbacks must not perform file I/O, logging, or blocking UI work. The Linux service uses a bounded single-producer/single-consumer PCM ring between PipeWire capture and virtual-source callbacks. Windows live routing and a desktop shell remain future work.

## Current scope

- Implemented: shared device and optional battery capability model, native input-device enumeration backends, CLI, Linux continuous PipeWire-to-virtual-source routing, and core model test.
- The CLI can apply Xiph RNNoise to explicit 48 kHz PCM16 WAV files. A shared stateful processing chain handles arbitrary PCM16 callback sizes with preset-backed gate, gain, compression, and AGC controls. The Linux PipeWire service uses the chain for continuous microphone enhancement.
- Windows backend reads the default Windows audio mix format as device capability information. Battery and device-change monitoring are not implemented.
- Linux builds require PipeWire, GTK 3, and GStreamer development packages. Linux runtime uses PipeWire for microphone routing and GStreamer for local A/B playback.
- Linux PipeWire and Windows WASAPI have bounded `record-test` paths that capture a selected/default source and save original/processed WAVs after explicit user invocation. The Linux GTK panel provides device/preset selection, persistent selection, service controls, RMS meters, and a five-second A/B record/playback flow. The sample is processed after capture. Windows continuous audio routing, graphical UI, battery telemetry, and installers remain future work.
