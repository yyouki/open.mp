# Windows x86 ASI client adapter

This adapter connects the shared custom-vehicle protocol core to the upstream
`samp-client-sdk` network hook host. It is a Windows x86 ASI plugin, not a
standalone SA-MP client.

## Supported network profiles

The pinned upstream host recognizes these SA-MP builds under GTA: San Andreas
1.0 US. All six expose RakClient packet/RPC hooks; only four expose a verified
direct native game-state reader:

| Client build | Status in this adapter |
| --- | --- |
| SA-MP 0.3.7 R1 | Network/download adapter enabled; native game-state gate available |
| SA-MP 0.3.7 R2 | Network/download adapter enabled through packet/RPC hooks; no native game-state profile |
| SA-MP 0.3.7 R3-1 | Network/download adapter enabled; native game-state gate available |
| SA-MP 0.3.7 R4-2 | Network/download adapter enabled through packet/RPC hooks; no native game-state profile |
| SA-MP 0.3.7 R5-1 | Network/download adapter enabled; native game-state gate available |
| SA-MP 0.3.DL R1 | Network/download adapter enabled; native artwork channel remains untouched |
| Other builds | Not enabled by this adapter |

The adapter checks the detected client version before installing hooks. Unknown
and unsupported versions fail closed. For R2 and R4-2, connection negotiation starts
when incoming RPC 139 (InitGame) is observed and stops on SA-MP RakNet disconnect or
connection-failure packets. Since those profiles expose networking hooks but no
verified native game-state reader, load the plugin before joining a server; if it is
loaded after joining, reconnect before expecting custom vehicle negotiation.

The upstream SDK is pinned to commit
`c658b939a3d5b5be512b5b27ea869995bc37b44d`; its host must be installed separately
as `samp_client_sdk.asi`.

## What it implements

- Starts the custom vehicle capability negotiation (RPC 221).
- Hooks the isolated incoming extension RPCs 220, 222, 225, and 227. It consumes extension messages and
  forwards unrelated messages to the original client.
- Uses the portable shared session state machine for validation and sequencing.
- Sends vehicle DFF/TXD requests (RPC 223/224) and completion notifications
  (RPC 226, asset type plus CRC32 checksum) through the SDK's game-thread RPC
  queue. The server validates the pending asset and acknowledges it with RPC 227.
- Downloads DFF/TXD files on a worker thread over HTTP/HTTPS.
- Validates exact size and CRC32, then writes files to
  `%LOCALAPPDATA%\\open.mp\\custom-vehicle` using deterministic names and a
  temporary file before exposing the cache entry.
- Retries failed transfers with bounded attempts before asking the server for a
  fresh URL.
- Exposes copied vehicle model metadata through plugin exports for the native
  model-loader milestone.
- Starts RPC 221 after the server's InitGame RPC (139). R1/R3-1/R5-1/DL R1
  additionally wait for verified game state 14 and can detect an already-active
  connection when loaded late. R2/R4-2 rely on packet/RPC lifecycle events because
  the pinned SDK has no verified native game-state reader for those builds.

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
`custom-vehicle-windows-x86-asi.zip`. Download the artifact from the successful
workflow run, extract `custom_vehicle_windows.asi`, and follow the install steps
above. The artifact contains only the network/download adapter; it does not add
the native GTA/RenderWare rendering backend.

## Install

1. Install a compatible ASI loader and the upstream `samp_client_sdk.asi`
   host into the GTA: San Andreas 1.0 US directory.
2. Copy `custom_vehicle_windows.asi` into that directory.
3. Start one of the supported SA-MP builds (0.3.7 R1, R2, R3-1, R4-2, R5-1,
   or 0.3.DL R1) and connect to an open.mp server with experimental custom vehicle
   models enabled. For R2 and R4-2, the plugin must be loaded before connecting.

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

The custom vehicle extension uses a separate RPC block (220-227). Native
SA-MP/0.3.DL artwork messages (179 and 181-189) are forwarded unchanged to the
original client, so native DL skin/object downloads can run alongside custom
vehicle asset transfers without sharing request/URL/ACK state.
