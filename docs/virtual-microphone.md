# Virtual microphone investigation

## Linux

The Linux CLI can publish a PipeWire source fed by the live processing chain:

```bash
./build/clearmic-cli serve [pipewire-source-id]
```

The command opens the selected microphone (or PipeWire's default source), processes mono 48 kHz PCM16 in the PipeWire capture callback, and publishes the result as **ClearMic Virtual Microphone**. The process must remain running. Ctrl+C stops it. A bounded single-producer/single-consumer queue separates the capture and source callbacks; if the consumer falls behind, old samples are discarded to keep latency bounded. Source disconnection currently stops the service with an error; automatic reconnect and runtime metrics are not implemented.

## Windows

WASAPI capture enumeration does not create a microphone endpoint. A virtual capture endpoint usually requires an installed audio driver or a supported third-party virtual audio component. ClearMic has not installed a driver or exposed a virtual microphone. Driver signing, setup requirements, maintenance, and security implications must be evaluated before choosing a shipping strategy.

## Current status

Linux provides the source via the `serve` command. Windows still has no ClearMic virtual endpoint. PipeWire runtime verification with a physical microphone and consumer application remains outstanding on a native Linux desktop.
