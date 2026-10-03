# Release artifacts

Every successful push or pull-request build produces short-lived Windows MSI and Ubuntu `.deb` artifacts for validation. A version tag creates a GitHub draft release after both platform jobs pass. The draft contains exactly one MSI, one Ubuntu package, and a `SHA256SUMS.txt` manifest for those files.

## Prepare a draft

1. Confirm the version in `project(ClearMic VERSION ...)` in the root `CMakeLists.txt`.
2. Create and push a matching version tag, for example `v0.1.0`.
3. Wait for the **Build and test** workflow. The release job runs only after Windows and Ubuntu build, test, and packaging jobs succeed.
4. Review the generated GitHub draft release and its installer assets and checksums. Publish it manually when the release is ready.

The Windows artifact is a WiX MSI and the Linux artifact is an amd64 `.deb`. The workflow does not sign the Windows installer or publish the draft automatically. Windows virtual-microphone driver support is not included in the MSI; see [Virtual microphone](virtual-microphone.md) for the current driver limitation.
