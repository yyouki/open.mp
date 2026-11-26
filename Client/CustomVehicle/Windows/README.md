# Windows x86 ASI client adapter

This adapter connects the shared custom-vehicle protocol core to the upstream
`samp-client-sdk` network hook host. It is a Windows x86 ASI plugin, not a
standalone SA-MP client.

## Supported network profiles

The pinned upstream host verifies direct SA-MP bridges for GTA: San Andreas
1.0 US with these client builds:

| Client build | Status in this adapter |
| --- | --- |
| SA-MP 0.3.7 R1 | Network/download adapter enabled |
| SA-MP 0.3.7 R3-1 | Network/download adapter enabled |
| SA-MP 0.3.7 R5-1 | Network/download adapter enabled |
| SA-MP 0.3.DL R1 | Deliberately disabled pending native artwork/custom-download multiplexing |
| SA-MP R2 / R4-2 and other builds | Not enabled by this adapter |

The adapter checks the detected client version before installing RPC hooks. Unknown
and unsupported versions fail closed. The upstream SDK is pinned to commit
`c658b939a3d5b5be512b5b27ea869995bc37b44d`; its host must be installed separately
as `samp_client_sdk.asi`.

## What it implements

- Starts the custom vehicle capability negotiation (RPC 186).
- Hooks incoming RPC 179, 183, 185, and 187. It consumes extension messages and
  forwards unrelated messages to the original client.
- Uses the portable shared session state machine for validation and sequencing.
- Sends checksum requests and completion notifications through the SDK's
  game-thread RPC queue.
- Downloads DFF/TXD files on a worker thread over HTTP/HTTPS.
- Validates exact size and CRC32, then writes files to
  `%LOCALAPPDATA%\\open.mp\\custom-vehicle` using deterministic names and a
  temporary file before exposing the cache entry.
- Retries failed transfers with bounded attempts before asking the server for a
  fresh URL.
- Exposes copied vehicle model metadata through plugin exports for the native
  model-loader milestone.

## Build

Requirements: Visual Studio 2022 C++ tools, CMake, stable Rust with target
`i686-pc-windows-msvc`, and network access to the pinned GitHub SDK and crates.

From the repository root in PowerShell:

```powershell
cmake -S Client/CustomVehicle/Shared -B build/custom-vehicle-windows-x86 -A Win32 -DBUILD_TESTING=OFF
cmake --build build/custom-vehicle-windows-x86 --config Release

$env:CUSTOM_VEHICLE_CORE_LIB_DIR = "$PWD/build/custom-vehicle-windows-x86/Release"
cargo build --manifest-path Client/CustomVehicle/Windows/Cargo.toml --target i686-pc-windows-msvc --release

Copy-Item target/i686-pc-windows-msvc/release/custom_vehicle_windows.dll custom_vehicle_windows.asi
```

The dedicated GitHub Actions workflow builds the same x86 artifact and uploads
it as a workflow artifact.

## Install

1. Install a compatible ASI loader and the upstream `samp_client_sdk.asi`
   host into the GTA: San Andreas 1.0 US directory.
2. Copy `custom_vehicle_windows.asi` into that directory.
3. Start one of the supported SA-MP 0.3.7 builds and connect to an open.mp server
   with experimental custom vehicle models enabled.

The plugin exports `CustomVehicleWindows_Shutdown`. A plugin manager must call
it from a worker thread and wait for success before unloading the ASI. It must
not be called from `DllMain`, a network callback, or the game thread.

## Remaining native-game work

This milestone handles RPC negotiation and verified asset delivery only. It does
**not** yet register DFF/TXD data in RenderWare, allocate custom
`CVehicleModelInfo` records, map custom model IDs into the game streaming
system, or construct/render custom vehicles. Those operations require exact
GTA executable layouts and verified ownership/cleanup contracts. No guessed
addresses are used.

SA-MP 0.3.DL R1 remains disabled because its built-in artwork downloader also
uses RPC 181/182/183/184/185. The current adapter does not yet arbitrate native
DL artwork requests and experimental vehicle asset requests concurrently.
