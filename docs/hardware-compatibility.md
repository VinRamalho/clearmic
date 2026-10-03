# Hardware compatibility

## Ubuntu 24.04+ / PipeWire

- The Linux PipeWire backend and live `record-test` path compile in Ubuntu 24.04 CI and in a local Ubuntu 24.04 WSL environment.
- The WSL environment has no connected PipeWire microphone source. Enumeration reports no input device; the capture command rejects this state instead of producing a misleading silent recording. No physical device audio was captured and no hardware compatibility is claimed from this run.
- On a desktop session with PipeWire, run `clearmic-cli devices` and `clearmic-cli record-test 5 original.wav processed.wav [device-id]` to validate an actual source. The current WSL environment cannot perform this step.

## Development machine: Windows 11

### Current Windows audio endpoints

- Input endpoint observed: `Microfone (Realtek(R) Audio)`.
- Output endpoint observed: `Altofalantes (Realtek(R) Audio)`.
- No Sony or USB wireless-microphone input endpoint appeared in the current AudioEndpoint list.
- A WASAPI bounded `record-test` implementation now captures from an active endpoint and creates A/B files, but this machine has not yet been used for an explicit hardware capture test. The available Realtek microphone may be used for that validation.
- Validate it with `clearmic-cli record-test 5 original.wav processed.wav [device-id]`; omit the ID to use the Windows default microphone. Actual capture remains unverified on the available hardware.

### Sony WF-C710N

- Present as a Bluetooth PnP device. Its AVRCP service IDs report vendor `054C` and product `0F8B`.
- No WF-C710N capture endpoint was exposed in the current Windows AudioEndpoint list.
- A GATT Battery Service (`0x180F`) was present for a different paired BLE device (`5FA9BC30D18D`), not for the WF-C710N. This does not establish battery telemetry for the headset.

### USB wireless device

- Windows reports a connected composite USB device with bus description `Wireless Device`, VID `3151`, PID `3020`, revision `0002`.
- It exposes `MI_00` and `MI_01` interfaces. PnP classifies both as HID interfaces; no USB Audio interface or corresponding microphone endpoint is currently visible.
- The MI_01 HID child list includes consumer-control, system-control, mouse, and vendor-defined usage page `0xFF00`, usage `0x000E`.
- The raw HID report descriptor and feature reports have not yet been decoded or queried. Battery availability therefore remains **unknown**; no telemetry is shown or inferred.

Battery and transmitter/receiver values remain unknown. The current inventory is passive PnP evidence only; hardware capture, charging status, report telemetry, and virtual-microphone routing have not been tested.
