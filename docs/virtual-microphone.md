# Virtual microphone investigation

## Linux

The Linux CLI can publish a PipeWire source fed by the live processing chain:

```bash
./build/clearmic-cli serve [pipewire-source-id] [natural|meeting|strong] [processing switches]
```

Processing switches include `--noise-suppression=on|off`, `--noise-gate=on|off`, `--automatic-gain=on|off`, and `--compressor=on|off`.

The command opens the selected microphone (or PipeWire's default source), processes mono 48 kHz PCM16 in the PipeWire capture callback, and publishes the result as **ClearMic Virtual Microphone**. The process must remain running. Ctrl+C stops it. A bounded single-producer/single-consumer queue separates the capture and source callbacks; if the consumer falls behind, old samples are discarded to keep latency bounded. The service reports processed duration, overruns, and underruns to stderr, and publishes RMS meter values to stdout for the GTK panel. If the service exits unexpectedly, the panel retries with a bounded backoff; Stop cancels retries. The optional tray mode keeps the panel available after its window closes.

## Windows

WASAPI capture enumeration does not create a microphone endpoint. WASAPI loopback captures rendered system audio, so it is useful as a possible AEC reference but cannot make ClearMic's processed microphone selectable by other applications ([Microsoft loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)).

The current Windows panel discovers active endpoints and supports local A/B recording/playback, but it does not publish processed PCM as a Windows microphone. Microsoft's [SYSVAD sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/sysvad-virtual-audio-device-driver-sample/) demonstrates a WDM virtual audio device driver. A user-mode APO can process streams in the Windows audio engine, but it is packaged and registered with an audio driver and does not independently create the virtual capture endpoint ([APO architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-processing-object-architecture)).

Any public Windows implementation therefore needs a virtual capture endpoint (or a licensed, redistributable third-party endpoint), a documented installation and update path, and a signed driver package where a kernel driver is used ([driver signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing), [public release signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/signing-a-driver-for-public-release)). The repository has not selected a driver source or redistribution license. ClearMic has not installed a driver or exposed a Windows virtual microphone; the MSI must not imply otherwise.

### Candidate review (2026-10)

The [Virtual Audio Driver by MikeTheTech](https://github.com/VirtualDrivers/Virtual-Audio-Driver) currently advertises Windows 10/11 virtual speaker and microphone endpoints. Its repository lists its own code under MIT and SYSVAD-derived portions under MS-PL in its [third-party notices](https://github.com/VirtualDrivers/Virtual-Audio-Driver/blob/main/THIRD_PARTY_NOTICES.md), which makes redistribution legally plausible subject to preserving those notices. It is not a production-ready ClearMic dependency today: the project warns that it is beta, its documented installation is a manual Device Manager flow, and the driver project's README says named-pipe/shared-memory integration for application developers is a separately quoted custom build. Its published instructions also mention test-signing mode for test-signed packages. ClearMic has not verified a production-signed package, installation/removal automation, a supported PCM injection API, or end-to-end latency against this driver. These gaps rule it out as a bundled solution at this stage; no driver source or binaries have been copied into this repository.

The [Microsoft SYSVAD sample](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad) remains a reference for WDM/WaveRT endpoint implementation, not a drop-in ClearMic output: it demonstrates a virtual device and sample topologies, but ClearMic would still have to implement and maintain the audio data bridge, package/sign the driver, and pass Windows driver validation. An APO alone also does not create a capture endpoint.

Next evaluation must establish, against a real Windows 11 machine: a redistributable production-signed package; silent or clearly consented installer deployment and reliable uninstall/upgrade; a documented user-mode PCM transport API; clean-driver behavior with Secure Boot and Memory Integrity enabled; and measured capture-to-virtual-mic latency and recovery on endpoint restart. Do not enable test signing, disable platform security, or distribute a driver until those criteria have a supported solution.

### Implementation paths to evaluate

- Adapt the Microsoft SYSVAD sample and maintain a ClearMic virtual capture driver. This offers control over the endpoint and format negotiation, but creates ongoing kernel-driver maintenance, signing, release, and Windows-version compatibility work.
- Integrate a third-party virtual audio driver only after confirming its license permits redistribution, installation, updates, and the intended commercial/open-source use. A technical sample or source-available driver is not automatically safe to bundle.
- Keep a user-mode APO as a possible later effect path if ClearMic needs to process audio in the Windows system pipeline. An APO alone does not provide the virtual microphone endpoint.

The next Windows architecture milestone is to compare candidate driver implementations and their redistribution/signing requirements, then build a minimal endpoint prototype and verify that a separate capture application can enumerate and receive audio from it. Until a candidate and its licensing path are established, the project will not bundle a driver or present the Windows MSI as providing a virtual microphone.

## Current status

Linux provides the source via the `serve` command. Windows still has no ClearMic virtual endpoint. PipeWire runtime verification with a physical microphone and consumer application remains outstanding on a native Linux desktop.
