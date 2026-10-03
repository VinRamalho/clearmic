# Audio pipeline

The intended shared pipeline is capture → noise suppression/voice enhancement → optional dynamics → output. Processing components will be selected based on supported formats and measured audio quality; WebRTC APM and RNNoise will not be stacked by default.

True acoustic echo cancellation needs a synchronized playback reference. Microphone-only filtering cannot be presented as AEC. Room reverberation reduction is distinct from echo cancellation and must be described according to the implemented algorithm.

The current code only discovers input devices. It does not capture or alter audio. Recording, real-time processing, latency reporting, and output routing remain unimplemented.
