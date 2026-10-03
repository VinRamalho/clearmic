# Battery monitoring

The shared device model reserves optional aggregate, transmitter, and receiver battery information. An empty value means telemetry is unavailable or has not been investigated; it never means 0%.

No battery telemetry backend is implemented in the current foundation. The development machine exposes a Realtek microphone endpoint, but endpoint enumeration alone does not establish Bluetooth or wireless-receiver battery availability. Per-device mechanisms will be investigated and documented against hardware before the UI reports a value.
