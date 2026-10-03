# Virtual microphone investigation

## Linux

PipeWire can expose virtual source nodes, but a usable ClearMic source must be fed processed PCM by the running application and survive session lifecycle changes. This is not implemented yet.

## Windows

WASAPI capture enumeration does not create a microphone endpoint. A virtual capture endpoint usually requires an installed audio driver or a supported third-party virtual audio component. ClearMic has not installed a driver or exposed a virtual microphone. Driver signing, setup requirements, maintenance, and security implications must be evaluated before choosing a shipping strategy.

## Current status

No platform currently provides ClearMic Virtual Microphone. Device discovery and virtual output are separate capabilities; compiling discovery code will not be treated as proof of routing.
