# Audio pipeline

## Implemented offline processor

The CLI currently reads integer PCM16 WAV, runs Xiph RNNoise, and writes a new PCM16 WAV. It supports mono and stereo files at 48 kHz. Stereo channels use independent RNNoise state. Unsupported sample rates and encodings fail with an error. The input stays local, and processing is only invoked by an explicit `clearmic-cli process` command.

The model and source revision are pinned and SHA-256 verified during CMake configuration. CMake stores them under the build directory and copies the upstream `COPYING` text into `build/licenses/rnnoise`. An initial configure therefore needs internet access unless the archives are already cached.

The RNNoise frame API processes 480 samples per channel at 48 kHz, a 10 ms frame. Its one-frame processing delay is preserved and flushed for offline files so output duration matches input duration.

## Stateful live processor core

The shared `ProcessorChain` accepts arbitrary callback block sizes, carries partial 480-sample RNNoise frames across calls, and uses preallocated buffers. It adds input gain, an optional noise gate, a soft-knee compressor, and a smoothed RMS-based automatic gain stage. Natural, Meeting, and Strong Noise Reduction presets set actual processor options. Atomic controls let a UI thread update parameters without taking a mutex in the audio callback. Changing enhancement settings leaves the RNNoise state warm so toggling does not reinitialize the DSP in an audio callback.

RNNoise contributes one 10 ms algorithmic frame of latency. The chain returns silence for the first frame while initializing its pipeline, then maintains frame order across callbacks. It currently accepts interleaved PCM16 at 48 kHz. This core is implemented, but neither Windows nor PipeWire live capture is connected to it yet.

## Not implemented yet

Platform live capture/output adapters, thread-safe UI control updates, level metering, and measured end-to-end latency remain unimplemented. WebRTC APM and RNNoise will not be stacked by default; each additional processor needs a demonstrated role. The current gate is a hard threshold and its acoustic quality needs listening evaluation before product release.

True acoustic echo cancellation needs a synchronized playback reference. Microphone-only filtering cannot be presented as AEC. Room reverberation reduction is distinct from echo cancellation and must be described according to the implemented algorithm.
