# Third-party notices

## RNNoise

ClearMic downloads and statically links the pinned RNNoise source and its model at configure time. Copyright and license terms for RNNoise and the model are included in the upstream `COPYING` file, which ClearMic installs with both packages at `share/doc/clearmic/licenses/rnnoise/COPYING`.

## Platform libraries

Linux packages declare PipeWire, GTK 3, and GStreamer as system runtime dependencies rather than bundling them. Windows builds use operating-system APIs. Each dependency remains subject to its own license; this file does not relicense those components.
