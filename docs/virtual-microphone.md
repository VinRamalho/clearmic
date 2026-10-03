# Virtual microphone investigation

## Linux

The Linux CLI can publish a PipeWire source fed by the live processing chain:

```bash
./build/clearmic-cli serve [pipewire-source-id] [natural|meeting|strong] [processing switches]
```

Processing switches include `--noise-suppression=on|off`, `--noise-gate=on|off`, `--automatic-gain=on|off`, and `--compressor=on|off`.

The command opens the selected microphone (or PipeWire's default source), processes mono 48 kHz PCM16 in the PipeWire capture callback, and publishes the result as **ClearMic Virtual Microphone**. The process must remain running. Ctrl+C stops it. A bounded single-producer/single-consumer queue separates the capture and source callbacks; if the consumer falls behind, old samples are discarded to keep latency bounded. The service reports processed duration, overruns, and underruns to stderr, and publishes RMS meter values to stdout for the GTK panel. If the service exits unexpectedly, the panel retries with a bounded backoff; Stop cancels retries. The optional tray mode keeps the panel available after its window closes.

## Windows

WASAPI capture enumeration does not create a microphone endpoint. The current Windows panel discovers active endpoints and supports local A/B recording/playback, but it does not publish processed PCM as a Windows microphone. Microsoft's [SYSVAD sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/sysvad-virtual-audio-device-driver-sample/) demonstrates a WDM virtual audio device driver. Microsoft documents that 64-bit kernel drivers must be signed and that public driver distribution uses its Hardware Developer Center signing process ([driver signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing), [public release signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/signing-a-driver-for-public-release)). ClearMic has not installed a driver or exposed a virtual microphone; the MSI must not imply otherwise. The app still needs a maintainable virtual endpoint strategy and signed-driver release path.

## Current status

Linux provides the source via the `serve` command. Windows still has no ClearMic virtual endpoint. PipeWire runtime verification with a physical microphone and consumer application remains outstanding on a native Linux desktop.
