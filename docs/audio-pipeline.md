# Audio pipeline

## Implemented offline processor

The CLI currently reads integer PCM16 WAV, runs Xiph RNNoise, and writes a new PCM16 WAV. It supports mono and stereo files at 48 kHz. Stereo channels use independent RNNoise state. Unsupported sample rates and encodings fail with an error. The input stays local, and processing is only invoked by an explicit `clearmic-cli process` command.

The model and source revision are pinned and SHA-256 verified during CMake configuration. CMake stores them under the build directory and copies the upstream `COPYING` text into `build/licenses/rnnoise`. An initial configure therefore needs internet access unless the archives are already cached.

The RNNoise frame API processes 480 samples per channel at 48 kHz, a 10 ms frame. Its one-frame processing delay is preserved and flushed for offline files so output duration matches input duration.

## Not implemented yet

The intended live pipeline is capture → noise suppression/voice enhancement → optional dynamics → output. Live capture, real-time processing, noise gate, compression, AGC, configuration controls, and latency reporting do not exist yet. WebRTC APM and RNNoise will not be stacked by default; each additional processor needs a demonstrated role.

True acoustic echo cancellation needs a synchronized playback reference. Microphone-only filtering cannot be presented as AEC. Room reverberation reduction is distinct from echo cancellation and must be described according to the implemented algorithm.
