# SA-MP 0.3.DL-R1 — no-ASI custom vehicle model integration

This document tracks the client-side work for the experimental custom vehicle
model protocol implemented in the sibling open.mp server branch.

The intended delivery is a rebuilt `samp.dll` with custom vehicle support in
the client source. It is **not** an ASI plugin and does not require a player to
install an extra loader. An unmodified retail 0.3.DL client will continue to
ignore custom vehicle support and receive base vehicle IDs only.

## Source baseline and build constraints

Reference source repository:

- `https://github.com/1therealcloud/samp`
- Baseline observed during this investigation:
  `118956c2e6c67527395dce47f3e655fad4352a12`
- Target: SA-MP 0.3.DL-R1 client.
- The reference repository describes itself as a reimplementation reconstructed
  from the binary, with approximately 75% of the compiled SA-MP client code
  matching the reference binary. It reports the client as fully playable.
- Its documented build toolchain is Visual Studio .NET 2003 RTM and DirectX
  9.0 SDK April 2005; dependencies are pinned as Git submodules.

The repository belongs to another GitHub owner, so this project records the
integration work here rather than pushing changes into that external repository.
The client source has not been modified or compiled as part of this commit.

## Why no-ASI is feasible

The client source already contains the native 0.3.DL artwork pipeline:
RPC 179 records, RPC 181/182 resource requests, RPC 183 download URLs,
checksum-based DFF/TXD caching, and the download completion barrier.

The missing part is model category `3` and vehicle-model registration. The
client already uses an extended model-info table for custom artwork IDs. The
GTA San Andreas vehicle model-info class can be cloned from a supported base
vehicle, then its clump can be installed through its virtual `SetClump`
method. The vehicle implementation handles the vehicle-specific setup,
including render callbacks, frame IDs, component hierarchy, and wheel/extra
processing. This avoids writing a parallel vehicle renderer.

Relevant technical references:

- Reconstructed client: `https://github.com/1therealcloud/samp`
- GTA vehicle model-info implementation:
  `https://github.com/gta-reversed/gta-reversed/blob/master/source/game_sa/Models/VehicleModelInfo.cpp`
- Vehicle model-info layout:
  `https://github.com/DK22Pac/plugin-sdk/blob/master/plugin_sa/game_sa/CVehicleModelInfo.h`
- GTA model-info vehicle allocation API:
  `https://github.com/DK22Pac/plugin-sdk/blob/master/plugin_sa/game_sa/CModelInfo.cpp`

## Client source changes required

### 1. Capability negotiation

In `source/net/netrpc.cpp`:

- Add outbound RPC 186, `ClientSupportsCustomVehicleModels`.
- Send it once after the client has received its game initialization and the
  RakNet connection is active. Sending it earlier is preferable, but the
  server also supports a late capability message by appending vehicle records
  to the already received native manifest.
- Register inbound RPC 187,
  `ServerSupportsCustomVehicleModels`, and set a per-session acknowledgement
  flag.
- Reset both flags when the RPC registry is torn down.
- Do not accept model type 3 records until RPC 187 has been acknowledged.

### 2. Safe model record parsing

The RPC 179 wire record is 39 bytes after its 32-bit index and 32-bit count.
Validate the packet bit length, index, count, type, base ID, and custom ID
before allocating memory.

For `CCustomModels::AddRecord`:

- Reject null records and indices outside the negotiated manifest limit.
- Accept type 3 only for custom model IDs 30001–32767 and supported base
  vehicle IDs 400–611.
- Exclude 449, 537, 538, 569, and 570 in this first implementation.
- Preserve the existing base-ID validation for types 1 and 2.
- Check allocation and `SetRecord` results. Free the newly allocated record
  when insertion fails. The current reconstructed implementation allocates
  before validation and can leak on invalid base IDs.

### 3. Reuse the native download manager

Extend the following `CCustomModels` paths so type 3 participates in DFF/TXD
download and cache validation as a global model category:

- `RefreshCachedResources`
- `CountMissingResources`
- `MarkResourceRequests`
- `Process`

The resource file types remain DFF=1 and TXD=2. Model type 3 must not be
confused with the file type sent in RPC 181/182/183.

Add vehicle-record lookup and load methods:

- `IsVehicleEntry(customModelId)`
- `GetVehicleBaseModel(customModelId)`
- `LoadVehicleEntry(customModelId)`
- `LoadVehicleModel(baseId, customModelId, dffChecksum, txdChecksum)`

A vehicle entry is ready only after both files are cached and the GTA vehicle
model-info registration succeeds. Mark a failed model load so a bad DFF does
not cause an endless retry every frame.

### 4. Register the vehicle model with GTA

Use the base vehicle's `CVehicleModelInfo` as the template for the custom
model-info entry. The verified Plugin-SDK layout is 0x308 bytes. The vehicle
structure pointer is at offset 0x5C; the native vehicle `SetClump` implementation
creates its own vehicle structure before processing the replacement clump.

Load the custom DFF as an RW clump and publish it through the model-info
virtual method at vtable slot 16. For a `CVehicleModelInfo`, that dispatches
to its vehicle-specific `SetClump` implementation, which performs the vehicle
render and hierarchy setup. Attach the downloaded TXD using the existing
`GameAttachModelTxd` path.

Do not treat a vehicle DFF as a ped or object model. The custom ID must resolve
to a vehicle model-info instance whose handling, vehicle type, wheel data,
seat geometry, door count, and component metadata originate from its base
vehicle.

### 5. Vehicle RPC, streaming, and pending downloads

In `ScrWorldVehicleAdd` and `CVehiclePool`:

- Continue accepting normal vehicle IDs 400–611.
- Accept IDs 30001–32767 only when there is a matching loaded type-3 record.
- If the vehicle record is known but its DFF/TXD are still downloading, queue
  the vehicle and retry it from the vehicle-pool update loop.
- Reject unknown or failed custom IDs rather than passing them to GTA.
- Keep train-consist creation behavior unchanged.

In `CGame::CreateVehicle`, use the custom record's base model ID when indexing
`m_aKeepLoadedVehicleModels[model - 400]`. Pass the custom ID to the GTA
vehicle creation command so the model-info table resolves the custom visual.
Never index the 212-element keep-loaded array with a custom ID.

### 6. Backward compatibility

A stock 0.3.DL client does not send RPC 186. The open.mp server must therefore
continue sending that client only native skin/object model records and base
vehicle IDs.

The modified client sends RPC 186 and receives RPC 187 before type-3 records.
If it does not receive the acknowledgement, it must not interpret type 3.
This keeps the extension opt-in and prevents stock clients from receiving an
unknown model category.

## Test matrix

The following tests are required before enabling the feature on a public server:

1. Stock 0.3.DL client: no RPC 186; no type-3 records; normal base vehicles.
2. Modified no-ASI client: RPC 186/187 handshake succeeds.
3. DFF and TXD absent from cache: both are downloaded, CRC-checked, and cached.
4. DFF and TXD already cached: vehicle registration works without redownloading.
5. A valid custom vehicle streams in with its custom visual and base handling.
6. A vehicle arriving before its download completes is queued, then created once
   the model is registered.
7. Stream out/in and reconnect preserve the correct visual.
8. Driver, passenger, unoccupied, trailer, color, component, damage, and respawn
   updates remain valid.
9. Invalid base IDs, invalid custom IDs, truncated RPC 179 packets, and invalid
   DFF/TXD files are rejected without memory leaks or an endless queue.
10. Multiple custom models using the same base vehicle can be loaded together.

## Current status

The server-side branch contains the experimental protocol and the latest
manifest, HTTP-endpoint, and download-state fixes. The no-ASI route is technically
feasible from the source baseline above, but the client integration described
here is not yet implemented or compiled. This document is an implementation
specification, not a claim of successful client testing.
