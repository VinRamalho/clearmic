# Hardware compatibility

## Ubuntu 24.04+ / PipeWire

- The Linux PipeWire backend and live `record-test` path compile in Ubuntu 24.04 CI and in a local Ubuntu 24.04 WSL environment.
- The WSL environment has no connected PipeWire microphone source. Enumeration reports no input device; the capture command rejects this state instead of producing a misleading silent recording. No physical device audio was captured and no hardware compatibility is claimed from this run.
- The Linux backend now maps a PipeWire Bluetooth address to BlueZ's reported battery percentage when available. WSL does not expose a BlueZ system bus or connected Bluetooth microphone, so this telemetry path has not been validated against physical hardware here.
- On a desktop session with PipeWire, run `clearmic-cli devices` and `clearmic-cli record-test 5 original.wav processed.wav [device-id]` to validate an actual source. The current WSL environment cannot perform this step.

## Development machine: Windows 11

### Current Windows audio endpoints

- Input endpoint observed: `Microfone (Realtek(R) Audio)`.
- Output endpoint observed: `Altofalantes (Realtek(R) Audio)`.
- The WF-C710N appears as a Bluetooth PnP device and as `WF-C710N Hands-Free` / `WF-C710N` MEDIA functions under one Windows device container. It does not currently appear as an active `AudioEndpoint` capture device, so ClearMic cannot select it through WASAPI at this time.
- The CLI's supplemental Bluetooth inventory now lists the WF-C710N hands-free function as connected and `selectable: no`; the separate Realtek AudioEndpoint remains the only selectable microphone in this run.
- A native Windows `record-test 3` invocation selected the Realtek default endpoint, completed WASAPI capture, and created valid 48 kHz mono WAVs with 144,000 frames each.
- The captured 3-second sample was all zero (peak 0, RMS 0), so this run proves endpoint opening, buffer flow, and WAV output but does not verify an audible microphone signal or enhancement quality. The microphone may have been muted or no sound was present during the test.
- A repeat 3-second capture on 2026-10-03 with the current Windows cross-build again completed successfully and wrote both local A/B WAVs (288,044 bytes each, 48 kHz mono PCM16, 144,000 samples). Both original and processed files had peak 0, RMS 0, and no nonzero or clipped samples. This reconfirms the WASAPI capture/file path only; no audible input or processing quality was measured.
- A diagnostics-enabled repeat reported a 4,800-frame WASAPI capture buffer. `IAudioClient::GetStreamLatency` returned zero, so ClearMic reports stream latency as unavailable rather than displaying zero milliseconds. The shared DSP processed the three-second sample in 81.7 ms wall time; this measures offline A/B compute time, not real-time callback or end-to-end latency. The audio samples remained silent.
- Repeat `clearmic-cli record-test 5 original.wav processed.wav [device-id]` while speaking into the device, then compare both files. Omit the ID to use the Windows default microphone.
- The Windows panel now enumerates playback endpoints, saves an explicitly selected output, and exposes a start/stop live WASAPI processing route to that endpoint. The implementation uses 48 kHz mono PCM16 with shared-mode conversion, the Windows engine-selected event period, and a bounded queue. The Windows CI and MinGW cross-builds verify compilation, but the panel route has not yet been exercised on the Windows host with a compatible virtual cable.
- No virtual-audio cable driver was found or installed for this validation. The observed Realtek playback endpoint is a physical speaker and was not used for live routing, to avoid acoustic feedback. Therefore the Windows processed microphone is not yet verified in a separate consumer application.

### Sony WF-C710N

- Present as a Bluetooth PnP device. Its AVRCP service IDs report vendor `054C` and product `0F8B`.
- Windows exposes WF-C710N Hands-Free and media PnP functions, but no active WASAPI `AudioEndpoint` capture endpoint was listed during inspection.
- A GATT Battery Service (`0x180F`) was present for a different paired BLE device (`5FA9BC30D18D`), not for the WF-C710N. This does not establish battery telemetry for the headset.
- Windows `root/wmi:BatteryStatus` currently reports only `ACPI\\PNP0C0A\\1_0` (the system battery). No battery WMI instance was associated with the WF-C710N device container, so its charge and charging state remain unknown.

### USB wireless device

- Windows reports a connected composite USB device with bus description `Wireless Device`, VID `3151`, PID `3020`, revision `0002`.
- It exposes `MI_00` and `MI_01` interfaces. PnP classifies both as HID interfaces; no USB Audio interface or corresponding microphone endpoint is currently visible.
- PnP shows an active composite receiver (`USB\\VID_3151&PID_3020`, revision `0002`) with `MI_00` and `MI_01`; the `MI_01` children include mouse (`UP:0001/U:0002`), system control (`UP:0001/U:0080`), consumer control (`UP:000C/U:0001`), and vendor-defined (`UP:FF00/U:000E`) HID collections. They share container `{999EC788-C3EC-5F0C-B470-BA9DC2EEC329}`.
- The collections have no PnP problem code, and Windows exposes only generic `DEVPKEY_Device_PowerData`; that is OS power-management metadata, not a device battery percentage or charging report.
- Read-only `HidD_GetPreparsedData` / `HidP_GetCaps` inspection of the vendor collection found a 32-byte input report and a 32-byte output report, both report ID `0xBA` with 31 eight-bit vendor values; no feature reports are declared. A GET_INPUT_REPORT returned 32 zero bytes in three reads spaced five seconds apart, and a 10-second passive overlapped input read received no reports. The declared output report was not sent. No battery semantics or changing telemetry were observed. Transmitter/receiver battery availability remains **unknown** because the wireless transmitter was not confirmed active during this inspection; no telemetry is shown or inferred.

Battery and transmitter/receiver values for the WF-C710N and USB wireless device remain unknown. The USB investigation includes parsed HID capabilities and read-only input reports, but the transmitter was not confirmed active and the vendor report semantics remain unknown. Sony headset capture, charging status, actual audio quality, and virtual-microphone routing have not been tested with actual sound or an application consumer.
