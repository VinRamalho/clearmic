# Audio pipeline

## Implemented offline processor

The CLI currently reads integer PCM16 WAV, runs Xiph RNNoise, and writes a new PCM16 WAV. It supports mono and stereo files at 48 kHz. Stereo channels use independent RNNoise state. Unsupported sample rates and encodings fail with an error. The input stays local, and processing is only invoked by an explicit `clearmic-cli process` command.

The model and source revision are pinned and SHA-256 verified during CMake configuration. CMake stores them under the build directory and copies the upstream `COPYING` text into `build/licenses/rnnoise`. An initial configure therefore needs internet access unless the archives are already cached.

The RNNoise frame API processes 480 samples per channel at 48 kHz, a 10 ms frame. Its one-frame processing delay is preserved and flushed for offline files so output duration matches input duration.

## Stateful live processor core

The shared `ProcessorChain` accepts arbitrary callback block sizes, carries partial 480-sample RNNoise frames across calls, and uses preallocated buffers. It adds input gain, an optional noise gate, a soft-knee compressor, and a smoothed RMS-based automatic gain stage. Natural, Meeting, and Strong Noise Reduction presets set actual processor options. Atomic controls let a UI thread update parameters without taking a mutex in the audio callback. Changing enhancement settings leaves the RNNoise state warm so toggling does not reinitialize the DSP in an audio callback.

RNNoise contributes one 10 ms algorithmic frame of latency. The chain returns silence for the first frame while initializing its pipeline, then maintains frame order across callbacks. It currently accepts interleaved PCM16 at 48 kHz. Linux PipeWire and Windows WASAPI have bounded capture-test commands that capture live source buffers, convert to the chain's PCM format, process the sample, and write original/processed recordings after capture completes. Linux also has a continuous PipeWire `serve` command that routes processed samples to a virtual source through a bounded ring buffer. The GTK panel selects and persists the source, preset, master enhancement state, noise suppression, noise gate, automatic gain, compressor, and input gain. The selected values apply to both its A/B capture and continuous service; controls are locked while either operation is active. During service use, the panel displays input/processed block RMS levels updated once per second. It records a five-second comparison on request and plays either WAV through the local GStreamer audio sink. The service reports processed audio duration and source underrun/capture overrun counts to stderr. Native desktop hardware validation remains open.

Run `clearmic-cli devices`, then `clearmic-cli record-test 5 original.wav processed.wav [device-id] [natural|meeting|strong] [processing options]`. The command accepts the same enhancement, noise suppression, gate, automatic gain, compressor, and input-gain switches as the Linux service; gain is limited to finite values from -12 to +12 dB. Linux verifies PipeWire reports a capture source and rejects stale IDs; Windows uses the selected WASAPI endpoint or Windows' default microphone. The command limits samples to 30 seconds, performs file writes after capture stops, and keeps all data local. Both desktop panels offer an explicit five-second recording and local playback controls for original and processed audio using their selected preset and controls.

## Not implemented yet

The Linux PipeWire service and virtual source are implemented. Windows has bounded WASAPI capture but no continuous processed output or virtual microphone. The current gate is a hard threshold, so its acoustic quality needs listening evaluation before product release. End-to-end latency and sustained CPU/memory usage still need measurement under desktop and hardware load.

True acoustic echo cancellation needs a synchronized playback reference. ClearMic does not yet capture that reference, so neither platform offers AEC. Room reverberation reduction is also not implemented; microphone-only noise suppression must not be presented as dereverberation. WebRTC APM remains a candidate only after its reverse-reference integration and measurable quality benefit are established; it should not be stacked with RNNoise without a demonstrated role.
