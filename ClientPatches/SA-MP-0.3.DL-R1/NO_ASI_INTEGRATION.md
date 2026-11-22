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

The missing part is model category `3` and vehicle-model registration.
The client already uses an extended model-info table for custom artwork IDs.
The GTA vehicle `SetClump` path is reusable, but the current implementation's
whole-object `memcpy` from a base vehicle is **not safe**: it duplicates
collision-model, RenderWare, texture-dictionary, and vehicle-structure state.
A correct implementation needs an independently initialized
`CVehicleModelInfo` instance and a reviewed list of scalar/configuration
fields to inherit from the base vehicle. The no-ASI approach remains viable,
but the current registration patch is blocked pending that lifecycle work.

Relevant technical references:

- Reconstructed client: `https://github.com/1therealcloud/samp`
- GTA vehicle model-info implementation:
  `https://github.com/gta-reversed/gta-reversed/blob/master/source/game_sa/Models/VehicleModelInfo.cpp`
- Vehicle model-info layout:
  `https://github.com/DK22Pac/plugin-sdk/blob/master/plugin_sa/game_sa/CVehicleModelInfo.h`
- GTA model-info vehicle allocation API:
  `https://github.com/DK22Pac/plugin-sdk/blob/master/plugin_sa/game_sa/CModelInfo.cpp`

## Client patch series

The repository contains two ordered patches targeting the pinned source SHA:

1. [0001-client-hardening.patch](0001-client-hardening.patch) fixes cache-hash parsing for the variable-width hexadecimal names used by the client, prevents invalid model records from leaking allocations, validates the 39-byte model-record and 63-byte vehicle-add payloads, adds vehicle-pool bounds checks, and invokes the existing waiting-list processor from the per-frame vehicle-pool update.
2. [0002-client-custom-vehicle-models.patch](0002-client-custom-vehicle-models.patch) adds the opt-in RPC 186/187 handshake, accepts model type 3 only after acknowledgement, includes type-3 DFF/TXD resources in the download manager, and queues vehicles until both assets are ready. It retains the original tuning payload for delayed vehicle creation and uses the base model only for keep-loaded bookkeeping. The registration routine now fails closed; it does not publish a custom `CVehicleModelInfo` until independent initialization and field inheritance are verified.

Both patches are experimental. Their unified-diff hunk sizes and source contexts have been checked in sequence against the pinned source files. This source-level replay check is not a substitute for running `git apply --check` in a full checkout, compiling the DL client, or testing it in-game. Neither patch has been applied to the external reference repository or compiled.

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

The complete RPC 179 payload is **39 bytes total**: a 4-byte pool index, a
4-byte record count, and a 31-byte model record. Validate the packet bit length,
index, count, type, base ID, and custom ID before allocating memory. The
vehicle-add RPC currently carries a 40-byte pool entry plus 23 bytes of tuning
data (63 bytes total); validate that payload before reading either structure.

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

The verified GTA SA 1.0 US `CVehicleModelInfo` layout is 0x308 bytes,
and vtable slot 16 is the vehicle-specific `SetClump`. However, **do not
clone the complete 0x308-byte base object**. The current experimental
`GameRegisterCustomVehicle` does exactly that and is not safe to run.

The class constructor initializes its own vehicle-structure pointer, color
state, animation index, and upgrade/remap arrays. Its virtual `Init` also
initializes the inherited `CBaseModelInfo` / `CClumpModelInfo` state, including
clearing the collision-model and RenderWare pointers and resetting model
flags. A replacement object must receive equivalent independent
initialization before any resources are attached. The class declaration and
virtual ordering establish `Init` at vtable slot 6 and vehicle `SetClump` at
slot 16 (17 total slots, including the virtual destructor). The GTA SA 1.0 US
reference header labels `0x4C5920` as the destructor, not the constructor; the
SA Plugin-SDK wrapper does not publish a constructor address. Do not call
`0x4C5920` as a constructor.

A possible construction route is to allocate a zeroed 0x308-byte object,
install only the vehicle vtable pointer, invoke virtual `Init` at slot 6, then
explicitly restore constructor-only defaults (`m_pVehicleStruct = nullptr`,
color count/flags zero, animation index -1, and upgrade/remap arrays filled
with -1). This is a source-derived candidate, not a verified target-binary
implementation: exact object offsets, ABI, target vtable, and all constructor
and base initialization side effects still require validation. Do not enable
this route until those checks and the base-vehicle field map are complete.

After independent initialization, copy only explicitly reviewed vehicle
configuration fields from the base model. Do not copy pointer-bearing fields
or ownership flags. The current header exposes a large opaque region, so the
field-by-field inheritance map is still incomplete. Only then should the DFF
be published through vtable slot 16. Vehicle `SetClump` allocates a new
`CVehicleStructure` and performs render-callback, frame-ID, pipeline, and
hierarchy setup.

The DFF reader must also reproduce the vehicle-specific context used by GTA's
native `CFileLoader::LoadClumpFile` path. Before `RpClumpStreamRead`, set the
active collision-plugin model to the custom model-info object and enable
`CVehicleModelInfo::UseCommonVehicleTexDicationary`. After the read, clear the
collision-plugin model and restore the previous texture-find callback. Do not
reuse `GameLoadPedDff` for this operation: that helper disables the collision
plugin stream callback by temporarily patching its entry point, so embedded
vehicle collision data would be skipped. The client patch adds a dedicated
`GameLoadVehicleDff` helper for this path. Its caller currently cannot reach
successful model registration because `GameRegisterCustomVehicle`
deliberately returns `FALSE` until object construction is safe.

Attach the downloaded TXD using the existing `GameAttachModelTxd` path. The
custom ID must resolve to a vehicle model-info instance whose handling,
vehicle type, wheel data, seat geometry, door count, and component metadata
originate from its base vehicle.

## 5. Lifecycle audit — current registration is blocked

The following findings were checked against the GTA reversed source files
listed below. They describe the GTA SA 1.0 US implementation; they do not
by themselves prove that every call site or address is identical in the target
SA-MP executable.

- `CVehicleModelInfo` is 0x308 bytes. Its constructor initializes
  `m_pVehicleStruct` to null, sets color/flag defaults, initializes the
  animation index, and fills upgrade/remap arrays with -1.
- Its virtual `Init` calls `CClumpModelInfo::Init`, then sets vehicle type,
  wheel model index, and bike steer angle. The inherited initialization
  clears `m_pColModel` and `m_pRwObject`, clears the TXD index, and resets
  base-model flags.
- `CVehicleModelInfo::SetClump` allocates a new `CVehicleStructure`, then
  calls `CClumpModelInfo::SetClump`. The latter installs the clump and calls
  `AddTexDictionaryRef()`. Therefore, do not add another TXD reference
  merely because `SetClump` succeeded.
- `CClumpModelInfo::DeleteRwObject` destroys the clump and removes its TXD
  reference. It calls `DeleteCollisionModel()` only when the
  `bOwnsCollisionModel` flag is set. Separately,
  `CBaseModelInfo::Shutdown` calls `DeleteCollisionModel()`; that routine
  deletes the collision model when `m_pColModel` is non-null and
  `bIsLod` is set.
- The collision-plugin stream reader associates the active model-info with
  the incoming collision chunk and calls `SetColModel(model, true)`. A
  shallow copy of a base model can therefore share a collision pointer and
  the related deletion state with the base model. This is a concrete
  double-free/use-after-free risk on shutdown, not merely a cosmetic
  metadata issue.
- The native vehicle-model store has 212 entries. Calling
  `CModelInfo::AddVehicleModel()` is not a general-purpose allocator for
  arbitrary custom IDs; the stock store is fixed-capacity. A separate
  allocation strategy must preserve constructor/Init and shutdown semantics.
- GTA reversed reports `UseCommonVehicleTexDicationary` at 0x4C75A0 and
  `StopUsingCommonVehicleTexDicationary` at 0x4C75C0. These addresses are
  corroborated by that repository's hook declarations, but still need
  verification against the exact target executable before shipping.

### Required changes before enabling vehicle registration

1. Keep registration fail-closed. Replace it only with a separately
   initialized instance after the vtable-slot-6 `Init` route, constructor-only
   defaults, and exact target ABI have been validated. Never restore the
   whole-object `memcpy(info, source, 0x308)`.
2. Establish the exact constructor/virtual-Init ABI for the target binary.
   Initialize upgrade and remap arrays to -1 and explicitly initialize every
   field not covered by virtual `Init`.
3. Produce a field-by-field inheritance map. At minimum, distinguish scalar
   vehicle configuration from pointers to RenderWare materials, clumps,
   collision models, TXD state, animation state, and vehicle structures.
4. Define collision ownership explicitly. The custom model must own only its
   own collision model. On every failure path, release a collision model
   created for the custom model without touching the base model's collision.
5. Make TXD registration transactional: check the TXD load result, track
   whether a slot was newly created or reused, and roll back only resources
   owned by this registration. Let `SetClump` perform its one TXD reference
   increment; pair it with the native deletion path.
6. Pending-vehicle cancellation is now included in the experimental patch:
   `CVehiclePool::CancelQueuedVehicle` removes matching waiting entries and
   clears their retained tuning data, and `ScrWorldVehicleRemove` invokes it
   before checking the live vehicle pool. This hunk still needs replay against
   a full checkout and compile/runtime testing.
7. Preserve and restore collision-plugin and common-vehicle-texture context
   on all normal return paths. Verify the stream callback and function ABI
   against the exact SA-MP 0.3.DL-R1 executable.

Primary source references (GTA SA 1.0 US reconstruction):

- [VehicleModelInfo.cpp](https://github.com/gta-reversed/gta-reversed/blob/56181bad17f7bb7bb63cf28ea6b18b97db0102e2/source/game_sa/Models/VehicleModelInfo.cpp)
- [VehicleModelInfo.h](https://github.com/gta-reversed/gta-reversed/blob/5f235cf42318044fc28c22692ad36a67a1697269/source/game_sa/Models/VehicleModelInfo.h)
- [ClumpModelInfo.cpp](https://github.com/gta-reversed/gta-reversed/blob/b266067488725f62fe91b2c7cad2ae3750dcb88b/source/game_sa/Models/ClumpModelInfo.cpp)
- [BaseModelInfo.cpp](https://github.com/gta-reversed/gta-reversed/blob/b9130a59447009672fff3cf161dddef6f3790d5f/source/game_sa/Models/BaseModelInfo.cpp)
- [ModelInfo.h](https://github.com/gta-reversed/gta-reversed/blob/ef1398c260112b7cb28b24e58819044b0cdcadd9/source/game_sa/Models/ModelInfo.h)
- [CollisionPlugin.cpp](https://github.com/gta-reversed/gta-reversed/blob/75dad5e24e5a79823ae7d815858c997bec0ad95f/source/game_sa/Plugins/CollisionPlugin/CollisionPlugin.cpp)

## 6. Vehicle RPC, streaming, and pending downloads

In `ScrWorldVehicleAdd` and `CVehiclePool`:

- Continue accepting normal vehicle IDs 400–611.
- Accept IDs 30001–32767 only when there is a matching loaded type-3 record.
- If the vehicle record is known but its DFF/TXD are still downloading, queue
  the vehicle and retry it from the vehicle-pool update loop.
- The reconstructed source has a `ProcessWaitingList` method, but it is not
  currently called from `CVehiclePool::Process`. The patch must invoke it from
  that per-frame update path; otherwise queued vehicles never retry.
- Add bounds checks for the vehicle pool ID before indexing the 2,000-entry
  vehicle arrays in `CVehiclePool::Create`.
- Reject unknown or failed custom IDs rather than passing them to GTA.
- Keep train-consist creation behavior unchanged.

In `CGame::CreateVehicle`, use the custom record's base model ID when indexing
`m_aKeepLoadedVehicleModels[model - 400]`. Validate that the resolved base is
400–611 before indexing. Pass the custom ID to the GTA vehicle creation
command so the model-info table resolves the custom visual. Never index the
212-element keep-loaded array with a custom ID.

## 7. Backward compatibility

A stock 0.3.DL client does not send RPC 186. The open.mp server must therefore
continue sending that client only native skin/object model records and base
vehicle IDs.

The modified client sends RPC 186 and receives RPC 187 before type-3 records.
If it does not receive the acknowledgement, it must not interpret type 3.
This keeps the extension opt-in and prevents stock clients from receiving an
unknown model category.

## 8. Test matrix

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

## 9. Current status

The server-side branch contains the experimental protocol and the latest
manifest, HTTP-endpoint, and download-state fixes. The client-side model type-3
implementation is drafted in the two patches above, but the vehicle registration
path is currently **blocked** because it shallow-copies a live
`CVehicleModelInfo`. It has not been applied to an external checkout, compiled,
or tested in-game. Treat this as an unverified experimental patch series, not
a release-ready client.

To test it in a separate checkout, first check out the exact baseline SHA above,
then run:

```sh
git apply --check 0001-client-hardening.patch
git apply 0001-client-hardening.patch
git apply --check 0002-client-custom-vehicle-models.patch
git apply 0002-client-custom-vehicle-models.patch
```

The commands above are instructions only; they have not been run here. Build
with the source repository's documented Visual Studio .NET 2003 RTM and
DirectX 9.0 SDK April 2005 toolchain, then run the test matrix below before
enabling the server option.
