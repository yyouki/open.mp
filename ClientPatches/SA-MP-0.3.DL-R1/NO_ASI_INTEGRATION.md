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
- Send it after the client has received its game initialization and the
  RakNet connection is active. Mark the capability as advertised only when
  RakNet accepts the RPC send, avoiding a stale success flag if the send call
  fails. The server also supports a late capability message by appending
  vehicle records to the already received native manifest.
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

The constructor and `Init` are not interchangeable. The source shows
that the constructor sets `m_pVehicleStruct = nullptr`, initializes the color
variation count and vehicle flags to zero, sets the animation-block index to
-1, and fills the 18 upgrade and 4 remap entries with -1. Virtual `Init`
does **not** perform those vehicle-specific steps; it initializes the base
model state and sets vehicle type to IGNORE, wheel model index to -1, and bike
steer angle to 999.99. `SetClump` then allocates a fresh
`CVehicleStructure`, so the structure pointer must be null before that call.

The header also exposes an opaque `field_60[464]` region and 32 dirt-material
pointers. Their semantics and lifecycle are not sufficiently mapped to copy
from a base vehicle. The candidate remains a zeroed 0x308-byte allocation,
vehicle-vtable installation, virtual `Init` at slot 6, and explicit
constructor-only defaults before copying only reviewed scalar configuration.
This is still a source-derived hypothesis, not a verified target-binary
implementation: exact offsets, ABI, target vtable, base initialization side
effects, and the opaque region must be audited before enabling it. The
registration routine therefore remains fail-closed.

After independent initialization, copy only explicitly reviewed vehicle
configuration fields from the base model. The currently readable vehicle
configuration members before the opaque region include plate material/text,
plate type, game name, vehicle type, front/rear wheel sizes, wheel model
index, handling ID, door count, vehicle class, flags, wheel-upgrade class,
frequency, extra-component rules, and bike steer angle. **Vehicle type must
be validated and initialized before calling `SetClump`:** the native method
indexes `ms_vehicleDescs[m_nVehicleType]` and dispatches renderer setup using
that value. The descriptor table has 12 entries, and the supported
`eVehicleType` values are exactly 0–11 (automobile through trailer);
`VEHICLE_TYPE_IGNORE` is -1 and must not reach `SetClump`. The handling ID
is also an unchecked array index: GTA reversed declares
`m_aVehicleHandling` as an array of 210 entries, so the safe index range is
0–209. Validate both values against the exact target executable before
publishing the model. These numeric bounds are source-derived from GTA SA
1.0 US and still require target-binary confirmation. In-range values must
also be semantically compatible with the custom DFF; in particular, the
plate-material pointer must not be inherited as an owned pointer without
a separate ownership decision. Do not copy the 464-byte opaque region, dirt
material pointers, base/clump pointers, collision pointers, TXD state, or
vehicle-structure pointer. The color arrays, color-variation state, upgrade
and remap arrays, and animation-block union need explicit per-field
initialization or validated copying rules. Only then should the DFF be
published through vtable slot 16. Vehicle `SetClump` allocates a new
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
- `SetClump` indexes the 12-entry `ms_vehicleDescs` table using
  `m_nVehicleType`; valid values are 0–11. `VEHICLE_TYPE_IGNORE` is -1
  and must not reach this call. Vehicle component setup also indexes the
  210-entry `m_aVehicleHandling` table using `m_nHandlingId`; validate
  handling IDs to 0–209 before exposing the model.
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
- [eVehicleType.h](https://github.com/gta-reversed/gta-reversed/blob/master/source/game_sa/Enums/eVehicleType.h)
- [cHandlingDataMgr.h](https://github.com/gta-reversed/gta-reversed/blob/55d57efa19a8501fdc7772b54067f821d65668f3/source/game_sa/cHandlingDataMgr.h)

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


### 9. Vehicle-structure and dirt-material ownership audit

A further source pass resolves important parts of the previously opaque lifecycle.
The GTA SA 1.0 US reference implementation shows:

- `CVehicleModelInfo::SetClump` allocates `CVehicleStructure` with
  `new CVehicleStructure()` before processing the clump. This is not an
  ordinary heap allocation: the nested class overloads `operator new` and
  `operator delete` to use the global `m_pInfoPool` pool at `0xB4E680`.
  The pool is created late in `CVehicleModelInfo::SetupCommonData`
  (`new CPool<CVehicleStructure>(50, "VehicleStruct")`), after vehicle colour,
  upgrade, environment-map, and common vehicle-TXD setup. Therefore, calling `SetClump` for custom models is
  unsafe until the target's pool has been initialized and its capacity/
  exhaustion behavior is understood. The reference pool is sized for 50
  structures; that is not evidence that 50 extra structures are available
  after native models have consumed pool entries.
- The structure constructor initializes all 15 dummy positions to zero,
  all 18 upgrade parent-component IDs to -1, all six extra-atomic pointers to
  null, the extra count to zero, and the damageable-component mask to zero.
  Do not shallow-copy a `CVehicleStructure` from the base model.
- The structure destructor iterates only the first `m_nNumExtras` entries,
  destroys each stored extra `RpAtomic`, and destroys its associated
  `RwFrame`. Thus those extras are owned resources. A duplicated structure
  pointer or duplicated extra pointers would cause double destruction.
- `CVehicleModelInfo::DeleteRwObject` deletes `m_pVehicleStruct`, clears
  the pointer, and then delegates to `CClumpModelInfo::DeleteRwObject`.
  The custom model's shutdown path must reach this virtual cleanup exactly
  once while the vehicle-structure pool remains alive. A partial registration
  failure after `SetClump` must use the same cleanup path; simply freeing the
  model-info allocation is insufficient.
- `SetClump` also invokes `PreprocessHierarchy`, which populates the
  vehicle structure from the custom clump. Its resulting extra atomics and
  frames are subsequently owned by the structure destructor. This makes
  successful hierarchy preprocessing and its rollback path part of the
  registration transaction.
- The 32 `m_apDirtMaterials` entries are material pointers discovered from
  the model's own clump by the reference implementation's optional
  `FindDirtMaterials` helper. The helper first clears the entire array to
  null, then stores pointers to matching materials found while traversing
  clump atomics and the extra atomics removed from the clump during
  `PreprocessHierarchy`. These are borrowed pointers into RenderWare-owned clump
  material data, not separately allocated materials; do not copy the base
  vehicle's entries and do not individually destroy them. Clearing them
  before loading and invalidating them with clump teardown avoids stale
  references. Note that this material-discovery helper is marked NOTSA in
  the reconstruction and may not execute in the retail target; the target's
  original dirt-material behavior must be checked separately.

Consequences for the no-ASI implementation:

1. Keep `GameRegisterCustomVehicle` fail-closed. The pool-backed structure
   allocation is an additional prerequisite beyond object initialization.
2. Before any custom `SetClump`, verify that the exact target initializes
   `CVehicleStructure::m_pInfoPool`, determine current pool occupancy and
   exhaustion behavior, and prove that custom entries cannot exceed capacity.
3. On successful registration, register the model-info object with a
   destruction route that invokes `CVehicleModelInfo::DeleteRwObject`
   exactly once before freeing the object storage. Do not call both
   `DeleteRwObject` and an equivalent manual clump/structure destructor.
4. On failure after `SetClump`, unwind the custom model through the native
   virtual deletion path while the structure pool and TXD system are valid;
   then release only the TXD slot/reference and collision data owned by that
   custom registration.
5. Never copy `m_pVehicleStruct` or `m_apDirtMaterials` from the base
   model. Let the custom clump's own hierarchy populate its structure and
   its own dirt-material scan populate any supported material cache.

Source reference:
- [VehicleModelInfo.cpp — structure allocation, destruction, and dirt materials](https://github.com/gta-reversed/gta-reversed/blob/56181bad17f7bb7bb63cf28ea6b18b97db0102e2/source/game_sa/Models/VehicleModelInfo.cpp)
- [VehicleModelInfo.h — pool-backed structure and model layout](https://github.com/gta-reversed/gta-reversed/blob/5f235cf42318044fc28c22692ad36a67a1697269/source/game_sa/Models/VehicleModelInfo.h)


### 10. Pool exhaustion, initialization order, and model-store separation

The follow-up audit establishes the following additional constraints from the
GTA SA 1.0 US reference implementation:

- `CVehicleModelInfo::SetupCommonData` is called from the first IPL-loading
  initialization path, after object data initialization and before
  `CStreaming::Init2`. It creates `CVehicleStructure::m_pInfoPool` only
  after vehicle colours, upgrades, environment maps, and the common
  `vehicle.txd` have been initialized. A custom model must not call
  `SetClump` before this stage has completed.
- The pool has 50 slots, but `CModelInfo` separately owns a fixed
  `CStore<CVehicleModelInfo, 212>` for vehicle model-info objects. The 212
  model-info capacity and the 50 vehicle-structure capacity are different
  resources. Increasing or bypassing the 212-entry store does not increase
  the structure pool.
- `CModelInfo::ShutDown` calls `Shutdown` on each registered vehicle model
  info and resets the vehicle-store count. It does not, in the inspected
  implementation, show a corresponding `m_pInfoPool` destruction call.
  The exact retail target's global shutdown sequence and pool lifetime remain
  unverified; custom cleanup must not assume the pool is safe to use after
  model/streaming shutdown.
- In the inspected `CPool::New` implementation, allocation searches for a
  free slot and returns null if none exists. The failure branch can also
  trigger a debug break when the pool is not configured to tolerate
  allocation failure. `CVehicleStructure::operator new` forwards directly
  to this pool allocator. This is a concrete exhaustion path, not a normal
  heap fallback. The exact retail executable's allocator behavior must still
  be confirmed before relying on this reconstructed implementation.
- The pool is used by loaded vehicle clumps, not one permanent structure per
  model-info object: `SetClump` allocates a structure, while
  `CVehicleModelInfo::DeleteRwObject` deletes it when the clump is removed.
  Consequently, a model-info object may exist while its structure slot is
  free, and streaming several additional custom clumps can exhaust the
  shared 50-slot pool even when the custom model-info table has room.
- The native model-info store has capacity 212. `CModelInfo::AddVehicleModel`
  appends to that store, calls `Init`, and publishes the pointer under the
  requested model ID. It is therefore not an extension allocator for IDs
  outside the native store, and it must not be called for each custom ID
  without proving available store capacity and preserving shutdown iteration.

Implementation requirements:

1. Gate custom clump registration on completed common vehicle initialization.
2. Treat 50 as a hard shared pool capacity until exact-target testing proves
   otherwise. Do not reserve all remaining slots blindly: native streaming
   also needs them.
3. Before attempting allocation, use a target-verified free-slot check or a
   controlled allocator wrapper. A pre-check alone is not sufficient if
   another thread or callback can allocate between the check and
   `SetClump`; serialize the operation with the native model-loading path.
4. If the pool is full, reject/defer the custom clump without invoking
   `SetClump`. Do not allow a null structure pointer to reach
   `PreprocessHierarchy`, `CreateInstance`, or vehicle rendering.
5. Keep the custom model-info registry separate from
   `CModelInfo::ms_vehicleModelInfoStore` unless a verified store extension
   also updates its capacity, iteration, shutdown, and pointer-registration
   invariants.
6. Confirm the target's shutdown order before releasing any custom model
   whose cleanup requires `m_pInfoPool`. Destruction of the pool must occur
   only after all custom and native vehicle structures have been returned.

Source references:
- [FileLoader.cpp — common vehicle setup before streaming initialization](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/FileLoader.cpp)
- [VehicleModelInfo.cpp — pool construction and SetClump/DeleteRwObject lifecycle](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/Models/VehicleModelInfo.cpp)
- [Pool.h — allocation failure and pool capacity behavior](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/Core/Pool.h)
- [ModelInfo.cpp — fixed vehicle model-info store and shutdown iteration](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/Models/ModelInfo.cpp)
- [ModelInfo.h — native vehicle model-info store capacity of 212](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/Models/ModelInfo.h)
