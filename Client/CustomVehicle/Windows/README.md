# Windows x86 ASI client adapter

This adapter connects the shared custom-vehicle protocol core to the upstream
`samp-client-sdk` network hook host. The pinned host binary is embedded in the
distributed Windows x86 ASI and extracted/loaded automatically on first start.
It is not a standalone SA-MP client.

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
and unsupported versions fail closed. The pinned native GTA service is stricter:
it accepts only the exact 32-bit GTA SA 1.0 US executable with SHA-256
`a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26`
and image base `0x00400000`. The six network-hook profiles below do not expand
that native executable profile. Other GTA executables, 64-bit variants, Android,
and other operating systems do not have a native renderer implementation here. For R2 and R4-2, connection negotiation starts
when incoming RPC 139 (InitGame) is observed and stops on SA-MP RakNet disconnect or
connection-failure packets. Since those profiles expose networking hooks but no
verified native game-state reader, load the plugin before joining a server; if it is
loaded after joining, reconnect before expecting custom vehicle negotiation.

The upstream SDK host is pinned to commit
`c658b939a3d5b5be512b5b27ea869995bc37b44d` and is built from that exact source
revision in CI. The host payload is embedded into `custom_vehicle_windows.asi`;
at runtime it is written to
`%LOCALAPPDATA%\\open.mp\\custom-vehicle\\sdk-host\\samp_client_sdk.asi`
and loaded by Windows. An already-loaded compatible host is reused. The host
module remains loaded until process exit so other ASI plugins can safely share it.

This is a single-file installation/distribution, not a statically linked single
module: the SDK host is still a separate internal Windows module at runtime.
The upstream MIT license is included in `LICENSES/samp-client-sdk-MIT.txt`.

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
- Exposes `CustomVehicleWindows_DecodeStreamInVehicle` for native RPC-164
  dispatchers. It returns `-1` for malformed/unknown vehicles, `0` for a
  stock vehicle, `1` for a registered custom model with ready assets, or `2`
  while DFF/TXD assets are pending. The output uses the 76-byte
  `custom_vehicle_stream_in_vehicle_v1` C ABI layout declared in
  `Shared/include/custom_vehicle/bridge.h`.
- Exposes `CustomVehicleWindows_RewriteStreamInVehicle`, which validates the
  exact 63-byte RPC 164 payload and copies it while rewriting only the four-byte
  model ID to the manifest's compatible base ID. It returns the same ready vs.
  pending status and leaves output/metadata unchanged for stock or invalid
  packets. A native dispatcher must queue model activation and only forward the
  rewritten payload after the corresponding game-thread operation is ready.
- Exposes `CustomVehicleWindows_IsModelReady` to recheck that the registered
  model's DFF and TXD are locally verified and server-acknowledged before native
  loading. These exports do not perform RenderWare registration or vehicle
  creation; the incoming RPC 164 hook still forwards stock and custom RPC 164
  unchanged until the native activation/cleanup path is implemented.
- Starts RPC 221 after the server's InitGame RPC (139). R1/R3-1/R5-1/DL R1
  additionally wait for verified game state 14 and can detect an already-active
  connection when loaded late. R2/R4-2 rely on packet/RPC lifecycle events because
  the pinned SDK has no verified native game-state reader for those builds.

## Build

Requirements: Visual Studio 2022 C++ tools, CMake, Git, stable Rust with target
`i686-pc-windows-msvc`, and network access to the pinned GitHub repositories and crates.

From the repository root in PowerShell:

```powershell
# Fetch the exact SDK host source revision used by CI.
git clone https://github.com/Akionka/rak-samp.git build/custom-vehicle-sdk-host
git -C build/custom-vehicle-sdk-host checkout c658b939a3d5b5be512b5b27ea869995bc37b44d

# Build the x86 host payload before building the main ASI.
cargo build --manifest-path build/custom-vehicle-sdk-host/Cargo.toml --target i686-pc-windows-msvc --release --locked

cmake -S Client/CustomVehicle/Shared -B build/custom-vehicle-windows-x86 -A Win32 -DBUILD_TESTING=OFF
cmake --build build/custom-vehicle-windows-x86 --config Release

$env:CUSTOM_VEHICLE_CORE_LIB_DIR = "$PWD/build/custom-vehicle-windows-x86/Release"
$env:CUSTOM_VEHICLE_HOST_DLL_PATH = (Resolve-Path "build/custom-vehicle-sdk-host/target/i686-pc-windows-msvc/release/samp_client_sdk.dll").Path
cargo build --manifest-path Client/CustomVehicle/Windows/Cargo.toml --target i686-pc-windows-msvc --release

Copy-Item Client/CustomVehicle/Windows/target/i686-pc-windows-msvc/release/custom_vehicle_windows.dll custom_vehicle_windows.asi
```

The build script rejects an embedded host that is not a 32-bit x86 PE DLL.

The dedicated GitHub Actions workflow builds the same x86 artifact and uploads
`custom-vehicle-windows-x86-asi.zip`. Download the artifact from the successful
workflow run, extract `custom_vehicle_windows.asi`, and follow the install steps
above. The artifact contains only the network/download adapter; it does not add
the native GTA/RenderWare rendering backend.

## Install

1. Install a compatible ASI loader for GTA: San Andreas 1.0 US.
2. Copy only `custom_vehicle_windows.asi` into the GTA directory. The embedded
   SDK host is extracted and loaded automatically; no separate SDK-host ASI is
   required in the GTA directory.
3. Start one of the supported SA-MP builds (0.3.7 R1, R2, R3-1, R4-2, R5-1,
   or 0.3.DL R1) and connect to an open.mp server with experimental custom vehicle
   models enabled. For R2 and R4-2, the plugin must be loaded before connecting.

The plugin exports `CustomVehicleWindows_Shutdown`. A plugin manager must call
it from a worker thread and wait for success before unloading the ASI. It must
not be called from `DllMain`, a network callback, or the game thread.

## Native vehicle backend status

The Windows adapter now exposes a native-facing C header at
`Windows/include/custom_vehicle_windows.h`. Its RPC-164 decoder returns the
validated vehicle state and distinguishes stock vehicles, custom vehicles with
ready assets, and custom vehicles whose assets are pending. The matching
`custom_vehicle_stream_in_vehicle_v1` payload layout is asserted in C99,
C++17, and Rust. A separate 44-byte model-manifest ABI is also asserted.

`Windows/include/gta_sa_vehicle_model_info_layout.hpp` is a compile-only
layout reference for GTA: San Andreas 1.0 US. The Win32 CMake build checks:

- `CVehicleModelInfoLayout`: 0x308 bytes.
- `VehicleStructureLayout`: 0x314 bytes.
- The vehicle type, vehicle-structure pointer, dirt-material, colour, upgrade,
  remap-TXD, and animation-block offsets.

The layout is based on the pinned
[gta-reversed VehicleModelInfo.h](https://github.com/gta-reversed/gta-reversed/blob/85bce0be4bd3229eb5ef445d3a7ac2e611a994f4/source/game_sa/Models/VehicleModelInfo.h)
and
[VehicleModelInfo.cpp](https://github.com/gta-reversed/gta-reversed/blob/85bce0be4bd3229eb5ef445d3a7ac2e611a994f4/source/game_sa/Models/VehicleModelInfo.cpp).
It is evidence only: it provides no function addresses and is not used to
dereference live game memory. The native backend must still verify the loaded
GTA executable profile before using any game structure or function.

The shared core now also contains `VehicleModelSlotRegistry` and a stable C ABI
for it in `Shared/include/custom_vehicle/bridge.h`. The registry maps a server
model ID to a vacant internal GTA model-info slot, tracks live-vehicle leases,
and delays slot reuse until the model is retired, all vehicles release it, and
the native slot is empty. It does not allocate or populate model-info objects,
load RenderWare assets, or hook the game; a future Windows game-thread backend
must provide the vacancy callback and perform those native operations. The
callback executes while the registry lock is held and must not re-enter the
registry. It must inspect the game model table only from a verified safe thread.

### Native vehicle lifecycle C ABI

The shared core exposes an opaque `custom_vehicle_native_vehicle_model_backend`
through `Shared/include/custom_vehicle/bridge.h`. It coordinates model-info
creation, native asset streaming, vehicle clump attachment, vehicle-use leases,
retirement, and retryable teardown without exposing C++ classes or GTA pointers
across the C ABI.

The `request_or_poll_vehicle_assets` callback returns one of three named values:

- `CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_PENDING` (1): a GTA streaming request is
  pending. Return to the game loop; call
  `custom_vehicle_native_vehicle_model_backend_activate` again on a later
  game tick to poll.
- `CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_READY` (2): streaming is complete; the
  controller proceeds to the vehicle-specific clump-attachment callback.
- `CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_FAILED` (0): roll back all completed
  stages. The callback itself must not leave partial resources behind.

The controller distinguishes two pending states:
`CUSTOM_VEHICLE_NATIVE_MODEL_ASSETS_PENDING` (3) means the server manifest's
DFF/TXD files have not yet passed local integrity checks and server
acknowledgement. `CUSTOM_VEHICLE_NATIVE_MODEL_NATIVE_ASSETS_PENDING` (5)
means those files are ready but GTA's own streaming is still in progress.
Only `CUSTOM_VEHICLE_NATIVE_MODEL_ACTIVATED` (1) or
`CUSTOM_VEHICLE_NATIVE_MODEL_ALREADY_ACTIVE` (2) indicates that the custom
model may be assigned to a vehicle. A pending model remains registered but is
not active and must not be used for spawn/replacement.

Callbacks execute synchronously while lifecycle state is locked. They must not
re-enter the backend. Asset polling must not block the game thread. Retain one
vehicle-use lease for every live vehicle that refers to the custom model and
release it on vehicle removal. Retirement stops new leases; if old vehicles
remain, the API returns `CUSTOM_VEHICLE_NATIVE_MODEL_IN_USE` (-9) until all
leases are released.

The C ABI destroy functions refuse to discard live ownership state:
`custom_vehicle_native_vehicle_model_backend_destroy` returns 0 while any
model lifecycle entry remains, including entries awaiting cleanup. It returns
1 and destroys the controller only after every entry has been fully released.
The slot-registry destroy function follows the same rule for reserved slots.
Callers must stop concurrent API use before attempting destruction; a refused
destroy leaves the handle valid for cleanup retries.

The destroy callback receives the last fully completed resource stage:
`RESERVED=0`, `MODEL_INFO_CREATED=1`, `ASSETS_REQUESTED=2`,
`ASSETS_LOADED=3`, or `CLUMP_ATTACHED=4`. It must be idempotent and clear
all resources through that stage, including pending streaming requests,
model-info table entries, RenderWare clumps, `CVehicleStructure` allocations,
TXD/animation references, and related native pointers. It may report success
only after the native model-info slot is empty. Failed cleanup keeps the slot
retired and reserved for an explicit retry; the slot must never be reused early.

These are lifecycle contracts only. The Windows adapter has not yet supplied
the concrete GTA operations, connected them to `Gta::on_tick`, or connected
the controller to the incoming RPC 164 path. The hook still forwards RPC 164
unchanged, and this milestone does not spawn or render a custom vehicle.

### Reference lifecycle from MTA (design evidence only)

The pinned MTA source for GTA SA 1.0 US provides a useful comparison:

- `CModelInfoSA::MakeVehicleAutomobile` allocates a vehicle model-info record,
  copies the `0x308` vehicle interface from a compatible base vehicle, resets
  the reference count and RW/visual pointers, sets native sentinel fields, and
  copies the base model's streaming archive metadata to the new ID.
- `CModelInfoSA::DeallocateModel` removes the model from streaming, resets
  detached car-part references, deletes the vehicle interface, clears the
  model-info table entry, and resets the streaming-info record.
- `CRenderWareSA::ReplaceModel` releases the previous vehicle visual structure
  before calling the vehicle `SetClump` routine again. It also balances TXD
  and animation-block references after replacing the clump.

Pinned references:
[MakeVehicleAutomobile and DeallocateModel](https://github.com/multitheftauto/mtasa-blue/blob/002d0274c63740c512ae06daab67e0b6e770745a/Client/game_sa/CModelInfoSA.cpp),
[vehicle interface layouts](https://github.com/multitheftauto/mtasa-blue/blob/002d0274c63740c512ae06daab67e0b6e770745a/Client/game_sa/CModelInfoSA.h),
and
[vehicle clump replacement and pool cleanup](https://github.com/multitheftauto/mtasa-blue/blob/002d0274c63740c512ae06daab67e0b6e770745a/Client/game_sa/CRenderWareSA.cpp).
These are implementation patterns for the matching GTA build, not verified
addresses for every SA-MP client revision. They are not copied into the adapter
as native calls.

### Confirmed blockers for real vehicle spawning

The reverse-engineered SA-MP client source
[netrpc.cpp](https://github.com/1therealcloud/samp/blob/5dc7813555e0dd69e526511c56f20eee3f58eca6/source/net/netrpc.cpp)
shows that stock RPC 164 rejects model IDs outside 400-611. The same client
source's
[game.cpp](https://github.com/1therealcloud/samp/blob/5dc7813555e0dd69e526511c56f20eee3f58eca6/source/game/game.cpp)
indexes its keep-loaded vehicle table using `model - 400` without a range
check. A custom model ID cannot safely be passed through the stock vehicle
creation wrapper.

GTA's `CVehicleModelInfo::SetClump` performs vehicle-specific initialization:
it allocates a `CVehicleStructure`, calls the base clump loader, applies
vehicle atomic callbacks and frame IDs, selects render pipelines, preprocesses
the hierarchy, and performs vehicle material/plate setup. It is not equivalent
to assigning a generic RenderWare clump pointer. The paired
`DeleteRwObject` path must release that structure and the base clump resources.

The initial `CVehicleStructure` pool is allocated with a capacity of 50 in
`CVehicleModelInfo::SetupCommonData`. Every loaded vehicle clump uses a pool
entry until its RW object is deleted. The backend must therefore coordinate
stream-in/stream-out and resource release; it must not register an unlimited
number of permanently loaded vehicle clumps. The capacity is recorded in the
layout header as an initial GTA pool setting, not a universal limit for every
possible future backend.

The pinned SA-MP 0.3.DL custom-model extension sample
[custommodels.cpp](https://github.com/1therealcloud/samp/blob/5dc7813555e0dd69e526511c56f20eee3f58eca6/source/game/custommodels.cpp)
contains Ped and Object model-info registration paths, but no equivalent
vehicle registration function. Vehicle registration must be implemented
separately.

The remaining implementation needs a safe custom-RPC-164 interception path,
base-vehicle model-info cloning and initialization, TXD/DFF/animation ownership,
streaming lifecycle, vehicle-pool construction, and cleanup of references such
as detached car parts. None of these native operations is implemented yet, and
no guessed addresses are used.

The custom vehicle extension uses a separate RPC block (220-227). Native
SA-MP/0.3.DL artwork messages (179 and 181-189) are forwarded unchanged to the
original client, so native DL skin/object downloads can run alongside custom
vehicle asset transfers without sharing request/URL/ACK state.
