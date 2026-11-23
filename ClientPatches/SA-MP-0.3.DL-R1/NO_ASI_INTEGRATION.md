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

### 11. Streaming removal and loaded-vehicle bookkeeping audit

The GTA SA 1.0 US reference implementation adds another integration requirement independent of model-info construction:

- CStreaming::RemoveModel(modelId) resolves the model through CModelInfo::GetModelInfo(modelId) and calls the model-info object's virtual DeleteRwObject(). For a vehicle model, it then calls CStreaming::RemoveCarModel(modelId). A custom model-info pointer must remain published and valid for the entire period in which the streaming table can report the custom DFF as loaded. Removing the pointer from the custom lookup table before streaming removal runs would make cleanup unsafe.
- CVehicleModelInfo::DeleteRwObject() releases its pool-backed CVehicleStructure and delegates to CClumpModelInfo::DeleteRwObject(), which destroys the clump, releases the TXD reference, and conditionally releases an owned collision model. Streaming removal is the normal place where this must happen. A custom registry must not separately destroy those resources and then allow CStreaming::RemoveModel to invoke virtual cleanup a second time.
- On successful DFF load, CStreaming calls CStreaming::AddToLoadedVehiclesList(modelId) for vehicle model-info types. That routine obtains the vehicle model-info pointer, resets its usage count, adds the ID to a population loaded-car group, and adds it to ms_vehiclesLoaded. On removal, RemoveCarModel removes the ID from those groups and the loaded-vehicle list, and removes specific driver-model associations. The custom-ID path must verify that all these collections, their ID widths, and adjacent group/zone lookup code safely accept IDs 30001–32767.
- CStreaming::RemoveModel uses GetModelType(modelId) to select DFF/TXD/etc. handling before resolving the model-info object. Adding a custom model-info pointer alone is insufficient: streaming metadata for the custom ID must consistently identify it as a DFF and carry valid loaded, requested, and in-progress state. A mismatch can skip cleanup or route the ID into the wrong resource-removal path.

Implementation requirements:

1. Keep the custom model-info pointer registered until the streaming entry is no longer loaded or being read, and until native virtual cleanup has completed. Unpublish it only after streaming state is safely detached.
2. Ensure the custom model ID is represented consistently in both custom model-info lookup and streaming metadata lookup. Test load failure, retry, stream-out, stream-in, and reconnect transitions.
3. Audit CStreaming::AddToLoadedVehiclesList, RemoveCarModel, population loaded-car groups, zone/group membership checks, and specific-driver bookkeeping with IDs 30001–32767. Do not assume the loaded lists prove every adjacent population array is extension-safe.
4. Make cleanup idempotent at the registry boundary: one streaming removal must invoke vehicle virtual cleanup once; registry teardown must not race or duplicate that operation.
5. Keep registration fail-closed until the exact target binary confirms custom streaming-table mapping, model-info pointer lifetime, and vehicle population bookkeeping.

Source references (GTA SA 1.0 US reconstruction):

- [Streaming.cpp — RemoveModel and loaded-vehicle bookkeeping](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/Streaming.cpp)
- [ClumpModelInfo.cpp — Shutdown and DeleteRwObject](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/Models/ClumpModelInfo.cpp)
- [VehicleModelInfo.cpp — vehicle virtual cleanup](https://github.com/gta-reversed/gta-reversed/blob/9b86a2e5431952c19a2bc60a0503441bd8f6f4d1/source/game_sa/Models/VehicleModelInfo.cpp)

### 12. Loaded-car group capacity and model-ID representation

The GTA SA 1.0 US reconstruction exposes a concrete limit in the native vehicle-population bookkeeping:

- `CLoadedCarGroup` is exactly 0x2E bytes and stores 23 model IDs in `int16 m_models[23]`. The sentinel is `MODEL_INVALID` (-1). Its `AddMember()` appends only while a sentinel slot remains; when all 23 slots are occupied it logs an out-of-memory message and does not add the model. This is a fixed-capacity list, not an expandable container.
- `CStreaming::AddToLoadedVehiclesList()` always inserts a loaded vehicle into `ms_vehiclesLoaded` and either `m_AppropriateLoadedCars` or `m_InAppropriateLoadedCars`, unless it is recognized as a boat. All of these are `CLoadedCarGroup` instances with the same 23-entry capacity. Therefore, 24 or more simultaneously loaded custom vehicles can exceed a native bookkeeping group's capacity. Streaming may still load the DFF, but the overflowed model is absent from that loaded-car group; downstream selection/reclassification and cleanup assumptions must be audited rather than treating list insertion as guaranteed.
- `CLoadedCarGroup` stores signed 16-bit IDs. The current custom range 30001–32767 is representable as positive `int16`; IDs above 32767 would become negative and collide with the sentinel/invalid-value domain. Keep the upper bound at 32767 unless every native container and comparison is redesigned. The public `eModelID` enum itself is `int32`, but that does not widen this storage.
- The ten `m_LoadedGangCars` groups also have 23 entries each. `AddToLoadedVehiclesList()` adds a model to a gang group only if its ID appears in that gang's `CPopulation::m_CarGroups` source list. Ordinary custom IDs absent from stock `CARGRP.DAT` will not be added there merely because their visual base model is a gang vehicle.
- The source reconstruction's `AddMember()` implementation does not visibly perform the duplicate check described in the header comment; it simply finds the first sentinel and writes the ID. Do not rely on duplicate suppression without verifying the exact retail implementation.

Consequences and required safeguards:

1. The current 30001–32767 range is compatible with the signed 16-bit field, but the 23-entry capacity is a separate, much smaller limit.
2. Establish a hard cap or explicit policy for custom vehicles simultaneously represented in native loaded-car groups. If that cannot be guaranteed, patch the relevant native bookkeeping paths with capacity-aware extension storage instead of silently relying on the 23-slot arrays.
3. Audit all consumers of `ms_vehiclesLoaded`, `m_AppropriateLoadedCars`, `m_InAppropriateLoadedCars`, `m_LoadedBoats`, and `m_LoadedGangCars`, especially reclassification, least-used selection, and stream-out selection, for behavior when a model was not inserted because a group was full.
4. Do not add custom IDs to stock `m_CarGroups` arrays as a shortcut: each source car group is also a fixed 23-entry `int16` array, and its contents drive zone, cheat, and gang classification.
5. Keep native registration fail-closed until the exact target binary's group layout, `AddMember()` behavior, and every consumer are verified.

Source references (GTA SA 1.0 US reconstruction):

- [LoadedCarGroup.h — 23 signed 16-bit slots and 0x2E layout](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/LoadedCarGroup.h)
- [LoadedCarGroup.cpp — AddMember capacity behavior](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/LoadedCarGroup.cpp)
- [Streaming.cpp — loaded vehicle insertion/removal](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/Streaming.cpp)
- [Streaming.h — ms_vehiclesLoaded storage](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/Streaming.h)
- [Population.h — loaded-car groups and CARGRP arrays](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/Population.h)

### 13. Streaming eviction depends on the bounded loaded-car groups

A follow-up consumer audit shows that the 23-entry limit is not merely a population display/selection limitation:

- `CStreaming::RemoveLoadedVehicle()` chooses eviction candidates from `m_InAppropriateLoadedCars`, `m_LoadedBoats`, and a sorted copy of `m_AppropriateLoadedCars`. It does not scan the global streaming table for every loaded vehicle model.
- `CStreaming::RemoveModel()` is called by this eviction path, and the vehicle bookkeeping is removed from the same bounded groups. A custom vehicle that failed to enter its group because all 23 slots were occupied can therefore remain loaded but is not discoverable by this normal vehicle-eviction candidate scan.
- The streaming load decision compares `ms_vehiclesLoaded.CountMembers()` against `desiredNumVehiclesLoaded` and may call `RemoveLoadedVehicle()` to make room. Since `ms_vehiclesLoaded` is also a 23-entry `CLoadedCarGroup`, its count saturates at 23. If an insertion is dropped, the count no longer represents the actual number of loaded vehicle DFFs. This can undermine the native loaded-vehicle target and eviction policy.
- `ReclassifyLoadedCars()` copies entries from the two 23-entry groups into fresh 23-entry groups. If the aggregate entries exceed the destination capacity, the reclassification pass can also drop entries. It does not independently recover missing vehicles from streaming metadata.

This establishes a concrete correctness risk: custom vehicle registration cannot simply rely on the native `AddToLoadedVehiclesList()` returning true. That function returns true after calling `AddMember()` even though `AddMember()` can fail to insert because the fixed group is full. The loader can therefore believe bookkeeping succeeded when the ID was not recorded.

Required design decision before enabling custom registration:

1. Either modify the loaded-car bookkeeping and eviction logic to use extension-capable storage that includes custom IDs, or reserve native group slots with a provable global cap that accounts for already-loaded stock vehicles and all groups.
2. If using extension-capable storage, update every producer and consumer together: `AddToLoadedVehiclesList`, `RemoveCarModel`, `RemoveLoadedVehicle`, `ReclassifyLoadedCars`, count/target calculations, and any population selection that consumes these groups. Do not patch insertion alone.
3. Add a regression test where more than 23 eligible vehicle IDs are simultaneously loaded, including stock and custom vehicles, then verify each can be selected for eviction, removed exactly once, and loaded again.
4. Keep the current client integration fail-closed. The 23-entry finding is source-derived from a GTA SA 1.0 US reconstruction; validate structure sizes, function behavior, and addresses against the exact target executable before implementing binary hooks.

Additional source reference:

- [Streaming.cpp — RemoveLoadedVehicle, load-count decision, and ReclassifyLoadedCars](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/Streaming.cpp)

### 14. Remaining loaded-car-group consumers and extension design

The follow-up audit of `LoadedCarGroup.cpp`, `Population.cpp`,
`CarGenerator.cpp`, and `CarCtrl.cpp` finds additional consumers that
must be considered if custom IDs are inserted into native loaded-car groups:

- `CLoadedCarGroup::SortBasedOnUsage()` orders entries by reading
  `CModelInfo::GetVehicleModelInfo(modelId)->m_nTimesUsed`. A custom ID
  must resolve through the vehicle-model-info lookup for every entry, not
  merely through the streaming table. A null or stale custom pointer can
  crash eviction sorting.
- `CLoadedCarGroup::PickRandomCar()` reads vehicle-model metadata including
  frequency and filters candidates using model references and vehicle
  classification. If custom IDs are exposed to this API, their model-info
  fields and reference counts must satisfy the same invariants as native
  vehicles.
- `CPopulation::PickRiotRoadBlockCar()` iterates the appropriate and
  inappropriate loaded groups and reads each model's vehicle class. This is
  another direct consumer of model-info lookup for IDs in those groups.
- `CCarGenerator` chooses a random model from
  `m_AppropriateLoadedCars`, then reads its vehicle model-info and bounding
  box before creating a parked vehicle. Custom IDs must therefore have valid
  bounding-box and vehicle metadata if they are ever made eligible for this
  path.
- `CPopulation::PickGangCar()` reads one of the ten loaded gang groups.
  `AddToLoadedVehiclesList()` populates those groups only when the model ID
  is present in the corresponding static `m_CarGroups` source array. Merely
  inheriting a gang vehicle's base model metadata does not make a custom ID a
  member of that gang's population group.
- `CCarCtrl::ReInit()` clears the native loaded gang, appropriate,
  inappropriate, and boat groups. Any separate extension registry must have
  an explicit reset/rebuild lifecycle at the same transition; otherwise it
  can retain stale IDs after the native groups have been cleared.

#### Recommended integration boundary

Do not widen `CLoadedCarGroup` in-place without a complete executable-level
layout and call-site audit. Its fixed 0x2E layout is embedded in global
storage, and changing its size would shift or overwrite adjacent game data.

Prefer a separate custom-loaded-vehicle registry with these properties:

1. Store custom model IDs in a dynamically sized or explicitly bounded
   container whose capacity is independent of the native 23-entry groups.
   Keep native stock groups unchanged.
2. Treat the registry as authoritative for custom loaded IDs. Add an ID only
   after the custom DFF has completed loading and native vehicle bookkeeping
   has accepted the model-info pointer. Remove it on every stream-out,
   failed-load rollback, model replacement, reconnect, and global reinit path.
3. Extend the loaded-vehicle count used by streaming load-target decisions
   with the number of custom entries, rather than trying to stuff custom IDs
   into the 23-slot `ms_vehiclesLoaded` group. Avoid double-counting if a
   future design intentionally allows a custom ID into a native group.
4. Extend eviction to inspect custom entries in addition to the native
   appropriate/inappropriate/boat groups. Apply the same candidate predicate
   (loaded, zero references, not mission/game required), and remove a selected
   custom model through the normal streaming removal route so virtual
   `DeleteRwObject`, TXD release, collision cleanup, and vehicle bookkeeping
   still run once.
5. Keep the registry synchronized from the streaming lifecycle, not from the
   network vehicle pool. A network vehicle instance can be deleted while its
   model remains cached, and a model can be streamed out while its network
   record remains known.
6. Do not add custom IDs to the gang, boat, or random-population groups in the
   first implementation. That avoids introducing custom IDs into
   `PickRandomCar`, `PickGangCar`, `PickRiotRoadBlockCar`, and
   `CCarGenerator` until their model metadata, classification, weighting,
   bounding boxes, and population membership have a separate design.
7. If custom vehicles must participate in ambient population later, add an
   explicit opt-in population mapping and audit every consumer above,
   including group capacity, duplicate behavior, model-info lookup, and
   reinitialization. Do not infer population membership from the base model.

#### Required regression coverage

- Load more than 23 total vehicle models while mixing stock and custom IDs;
  verify the streaming count reflects the real total and repeated load
  requests do not bypass eviction.
- Make every unreferenced custom model eligible in turn; verify eviction
  finds it, calls native cleanup once, removes it from the extension registry,
  and permits a later reload.
- Keep a custom model referenced by an entity or mission/game-required flag;
  verify eviction skips it.
- Reclassify native stock groups repeatedly while custom models are loaded;
  verify custom registry membership remains stable and no custom ID is lost
  or duplicated.
- Run `CCarCtrl::ReInit()`, reconnect, failed DFF load, and TXD rollback;
  verify the extension registry is cleared or rebuilt without stale pointers.
- If ambient population is later enabled, separately test random-car
  selection, parked-car generators, riot roadblocks, gang selection, and
  boat exclusion.

These findings are derived from the GTA SA 1.0 US reconstruction at commit
`81ee79f984f0d049a8475d7badb7279a0049d78c`. They are design constraints,
not confirmation that the same addresses, layouts, or ABI are present in
SA-MP 0.3.DL-R1. Exact-target validation is still mandatory.

Additional source references (GTA SA 1.0 US reconstruction):

- [LoadedCarGroup.cpp — sorting and random selection](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/LoadedCarGroup.cpp)
- [Population.cpp — riot roadblock and gang model selection](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/Population.cpp)
- [CarGenerator.cpp — parked-car model selection](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/CarGenerator.cpp)
- [CarCtrl.cpp — population loaded-group reinitialization](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/CarCtrl.cpp)

### 15. Extended model-info lookup is not extended streaming metadata

The pinned reconstructed SA-MP source already contains a general model-info
table extension for the existing 0.3.DL custom skin/object pipeline. This is
useful groundwork for vehicle IDs, but it must not be mistaken for a complete
extension of GTA's model system.

#### What the existing table extension actually provides

- `g_gameModelInfoTable` points to a 32,768-entry pointer bank beginning at
  `g_gameCustomModelInfo`. The adjacent prefix buffer and enable flag are
  separate allocations: the 32,767-DWORD prefix buffer ends exactly where the
  pointer bank begins, and the enable flag follows the pointer bank.
- `GameInitializeCustomModelInfo()` clears the extension storage, copies the
  original 20,000 model-info pointers from GTA address `0x00A9B0C8` into the
  beginning of the extended bank, patches 707 recorded model-info table
  references, and installs a hook at GTA address `0x004087EA`.
- `GamePatchModelInfoReferences()` rewrites the table operand only when the
  expected opcode matches one of the recorded US/EU sites. It does not prove
  that every model-info table access in the executable has been found. The
  patched-reference list is therefore a finite set of known call sites, not a
  universal guarantee that every engine subsystem uses the extended bank.
- The hook installed at `0x004087EA` rejects negative EBP model IDs and
  resumes the native table-index calculation for nonnegative IDs. It does
  not itself establish a safe upper bound. The model-info bank has 32,768
  entries, so the intended custom-ID ceiling of 32,767 is consistent with
  that allocation, but all callers must still validate their IDs.
- `GameGetModelInfo()` switches to `g_gameModelInfoTable[model]` whenever
  `g_gameUseCustomModelInfo` is set. In the inspected baseline this direct
  array access has no lower- or upper-bound check. `GameHasModelInfo()`
  simply calls it, so it inherits that risk. Any new custom-vehicle path must
  reject IDs outside 0–32767 before calling this helper; a hardened helper
  should also enforce that range centrally.
- The reconstructed `CGame::IsModelAvailable()` only invokes the native
  scripting availability command for IDs 0–20000 and returns TRUE for other
  values. Therefore, its success result is not proof that a custom ID has a
  registered model-info object, a loaded DFF, or valid streaming state.

#### What it does not provide

The extended pointer bank does not automatically resize or remap:

- GTA's per-model streaming metadata (model type, requested/loaded flags,
  streaming offsets, resource sizes, and in-progress state);
- the native vehicle model-info object store or its shutdown iteration;
- collision-plugin registration/ownership state;
- loaded-vehicle population groups, eviction lists, or vehicle structure pool;
- other model-indexed arrays, caches, zone/group membership tables, or
  code paths that retain direct pointers to the original
  `0x00A9B0C8` bank.

The 707 patched references cover only the sites recorded by the
reconstructed SA-MP source. Before relying on the table for custom vehicles,
the remaining direct references to the original model-info bank must be
enumerated from the exact target executable and classified as either safe
because they use a patched operand, irrelevant to custom vehicle IDs, or
requiring an additional hook. The reference list itself is not evidence that
the streaming metadata arrays are extended.

#### Required lookup and metadata contract

1. Enforce `0 <= modelId <= 32767` before every access to the extended
   model-info bank. Reject negative IDs and IDs at or above 32768 before
   pointer arithmetic or table indexing.
2. Keep the stock model-info entries 0–19999 initialized from the original
   bank. Do not assume entries 20000–32767 are valid merely because the
   pointer bank has capacity; each custom entry must be published only after
   its model-info allocation and initialization succeed.
3. Audit the 707 patched sites against both US and EU executable variants.
   Record opcode mismatches rather than silently treating a skipped patch as
   success. Validate that every rewritten operand points to the same extended
   bank and that no unpatched direct table access is reachable for custom
   vehicle operations.
4. Independently extend or hook streaming metadata for custom IDs. A valid
   model-info pointer must never be used as a substitute for a DFF model type,
   loaded state, request state, or streaming lifecycle record.
5. Make model-info lookup failure explicit before any field dereference.
   Several reconstructed helpers intentionally retain native unchecked
   dereferences (for example collision and reference-count accessors); do not
   feed them an invalid or unpublished custom ID.
6. Test boundary IDs 19999, 20000, 30000, 30001, 32767, 32768, -1, and the
   largest signed/unsigned values accepted by each RPC parser. Verify no
   out-of-range access occurs and that stock lookup behavior is unchanged.
7. Do not enable vehicle registration until exact-target disassembly confirms
   the table bank addresses, patched call sites, hook ABI, streaming metadata
   layout, and cleanup paths for the target SA-MP 0.3.DL-R1 binary.

Source references (reconstructed SA-MP baseline
`118956c2e6c67527395dce47f3e655fad4352a12`):

- [custommodels.cpp — model-info bank initialization and reference patching](https://github.com/1therealcloud/samp/blob/118956c2e6c67527395dce47f3e655fad4352a12/source/game/custommodels.cpp)
- [util.cpp — GameGetModelInfo direct extended-table lookup](https://github.com/1therealcloud/samp/blob/118956c2e6c67527395dce47f3e655fad4352a12/source/game/util.cpp)
- [hooks.cpp — model-info lookup hook](https://github.com/1therealcloud/samp/blob/118956c2e6c67527395dce47f3e655fad4352a12/source/game/hooks.cpp)
- [game.h — extended model-info storage declarations](https://github.com/1therealcloud/samp/blob/118956c2e6c67527395dce47f3e655fad4352a12/source/game/game.h)
- [game.cpp — model availability and lookup wrappers](https://github.com/1therealcloud/samp/blob/118956c2e6c67527395dce47f3e655fad4352a12/source/game/game.cpp)


## 16. GTA streaming metadata is a separate, smaller index domain

The reconstructed GTA SA 1.0 US streaming implementation has a separate
metadata table from the SA-MP custom model-info pointer bank described above.
The two tables must not be conflated.

At the pinned `gta-reversed` source revision
`81ee79f984f0d049a8475d7badb7279a0049d78c`:

- `CStreaming::ms_aInfoForModel` is declared as
  `std::array<CStreamingInfo, 26316>`, at address `0x8E4CC0`.
- `CStreamingInfo` is `0x14` bytes. The array therefore occupies
  `26316 * 0x14 = 0x51030` bytes in this reconstructed layout.
- Resource IDs are partitioned, not a generic 32K model namespace:
  DFF 0–19999; TXD 20000–24999; COL 25000–25254; IPL 25255–25510;
  DAT 25511–25574; IFP 25575–25754; RRR 25755–26229; SCM 26230–26311.
  IDs 26312–26315 are four internal loaded/requested-list sentinel entries.
- `CStreaming::Init2()` initializes all 26,316 records and sets
  `CStreamingInfo::ms_pArrayBase` to entry zero. It then links the sentinel
  records into the loaded and requested lists.
- Inline `GetInfo(modelId)` only asserts that the ID is non-negative before
  indexing `ms_aInfoForModel[modelId]`. `IsModelLoaded(model)` indexes it
  without a range check. `RequestModel()`, `RequestFile()`,
  `ConvertBufferToObject()`, `RemoveModel()`, and
  `IsObjectInCdImage()` obtain metadata through this unchecked accessor.
- Streaming operations also derive resource type from the fixed ID ranges,
  traverse requested/loaded lists, use 16-bit CD directory links, and consult
  DFF model-info/TXD metadata. Extending only the array would not make a
  custom ID a valid DFF resource or supply its archive position, size, IMG
  identity, dependency records, or lifecycle semantics.
- `CStreaming::AddToLoadedVehiclesList()` adds loaded vehicle IDs to fixed
  population groups and the native loaded-vehicle group. Its success return
  does not prove that every fixed-size group accepted the member. The earlier
  sections on group capacity and custom loaded-vehicle bookkeeping still apply.

Consequences for the current custom vehicle range:

1. IDs 30001–32767 are within the SA-MP model-info pointer-bank index domain,
   but outside `ms_aInfoForModel[0..26315]`. Passing one directly to
   `CStreaming::GetInfo`, `RequestModel`, `RemoveModel`, or another
   accessor that indexes this array is an out-of-bounds access.
2. Those IDs also do not belong to any declared native DFF/TXD/COL/IPL/DAT/
   IFP/RRR/SCM resource range. Treating a custom vehicle ID as a native DFF
   resource without an explicit translation/registration layer is invalid.
3. The 26,316-entry table is embedded at a fixed game address, followed by
   other global state. Writing beyond it is not a safe way to extend it.
   Replacing it requires a target-specific audit of every direct reference,
   `CStreamingInfo::ms_pArrayBase`, list sentinel pointers, serialization
   loops, model-type dispatch, CD directory links, and all consumers of
   `GetInfo`. A replacement table alone is insufficient.
4. The current custom DFF/TXD download path must own its own loading state,
   request state, resource bytes, dependencies, and cleanup, or use a fully
   audited adapter that maps custom IDs onto valid native resource records.
   Do not pass custom IDs directly into native streaming APIs as a shortcut.
5. Keep custom registration fail-closed. Before enabling it, validate all
   streaming call sites and data layouts against the exact SA-MP 0.3.DL R1
   client executable, not only this reconstructed GTA SA baseline.

Required regression boundaries for this layer: 19999, 20000, 24999, 25000,
26311, 26312, 26315, 26316, 30000, 30001, 32767, 32768, and -1. Test both
valid stock resource behavior and rejection/translation of custom IDs.

Source references (reconstructed GTA SA 1.0 US baseline
`81ee79f984f0d049a8475d7badb7279a0049d78c`):

- [Streaming.h — resource ID partitions, metadata array, and unchecked accessors](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/Streaming.h)
- [Streaming.cpp — request, conversion, removal, initialization, and directory handling](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/Streaming.cpp)
- [StreamingInfo.h — CStreamingInfo layout and state fields](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/StreamingInfo.h)
- [constants.h — resource category counts](https://github.com/gta-reversed/gta-reversed/blob/81ee79f984f0d049a8475d7badb7279a0049d78c/source/game_sa/constants.h)


## 17. Custom DFF/TXD download lifecycle and path-safety audit

The type-3 vehicle record currently reuses the existing custom-model download
worker and cache layout. This is distinct from GTA's native IMG streaming
metadata and does not make a custom model ID valid for `CStreaming`.

Source baseline: `1therealcloud/samp` commit
`118956c2e6c67527395dce47f3e655fad4352a12`, principally
`source/game/custommodels.cpp`.

Observed lifecycle in the experimental patch:

1. Type-3 records are accepted only after the server capability acknowledgement.
   The parser restricts the model ID to 30001–32767 and base vehicle ID to
   400–611, excluding several special models. Duplicate type-3 model IDs are
   rejected by `AddRecord()`.
2. The 94-byte wire record is copied into a 96-byte zeroed allocation. The
   extra two bytes are used for private registration state at offset
   `0x5e`; the wire record size itself is unchanged.
3. `MarkResourceRequests()` includes type-3 records in the existing download
   request path. `LoadVehicleEntry()` waits until both record state bytes
   `entry[0]` and `entry[1]` are set, then calls `LoadVehicleModel()`.
4. `LoadVehicleModel()` locates DFF and TXD files first in the server cache
   directory and then in the local cache directory. It passes the paths and
   TXD hash-derived name to `GameRegisterCustomVehicle()`.
5. The proposed vehicle DFF reader opens a RenderWare file stream, checks for
   the clump chunk, sets collision-plugin model context, enables the common
   vehicle texture dictionary, reads the clump, clears both contexts, and
   closes the stream.
6. The actual registration function intentionally returns `FALSE`. Therefore
   the vehicle path cannot currently publish a model-info pointer or reach a
   successful `CUSTOM_VEHICLE_READY` state. This is the required fail-closed
   behavior until vehicle model-info ownership and streaming integration are
   solved.

Bounded path construction added to the experimental patch:

- `LoadVehicleModel()` now builds both DFF and TXD paths through
  `GameBuildCustomAssetPath()` instead of unbounded `sprintf()`. The helper
  rejects null/empty inputs, uses the target-era `_snprintf()` interface,
  rejects a negative or capacity-reaching return value, and explicitly writes
  the terminating NUL on both success and truncation.
- The 264-byte destination is therefore never passed to `GameFileExists()`
  after a truncated format. If the primary path cannot be represented or its
  file is absent, the loader tries the fallback root using the same checked
  helper; it never tests a truncated path.
- This helper only bounds and formats the generated filename. It does not
  canonicalize or authorize arbitrary cache roots; the two roots are read from
  the existing fixed client cache fields. Their initialization, termination,
  and trust boundary still require exact-target validation.
- The older ped/object loaders retain their pre-existing path construction
  and have not been changed by this patch. Their audit should remain separate
  to avoid broadening behavior without regression coverage.
- This change is source-level only. The VS2003 CRT behavior, include
  availability, unified-patch application, and runtime boundary cases still
  require verification in the target toolchain.

Lifecycle/rollback gaps that remain unproven:

- The proposed DFF reader restores collision-plugin and common vehicle-TXD
  context on its ordinary return paths, but the exact target ABI, function
  addresses, and global-context behavior have not been validated against the
  0.3.DL R1 executable.
- `GameRegisterCustomVehicle()` must be transactional. A failed TXD attach,
  DFF parse, collision-plugin read, or vehicle-structure initialization must
  release every resource acquired so far, undo TXD references, destroy any
  partial RenderWare object, clear the model-info slot, and leave the record
  retryable or explicitly failed according to a defined policy.
- The current `LoadVehicleEntry()` state byte `entry[0x5e]` records success
  or permanent failure. Do not mark a transient failure permanent until the
  registration routine distinguishes corrupt assets from temporary resource
  exhaustion and guarantees rollback.
- The custom cache's file-exists checks are not content validation. DFF/TXD
  parsing must validate stream chunk boundaries and reject malformed or
  truncated assets without publishing partially initialized model state.
- The custom asset loader must remain separate from native
  `CStreaming::RequestModel`, `GetInfo`, and `RemoveModel` for IDs
  30001–32767 unless a complete, bounded translation layer is implemented.

Required tests before enabling this path:

- Cache root lengths at the shortest supported value, maximum accepted value,
  and one byte beyond the accepted maximum.
- DFF and TXD present in primary cache, fallback cache, both caches, and
  neither cache.
- Truncated files, missing clump chunk, malformed RenderWare chunks, and
  repeated load attempts after failure.
- Failure injection after TXD attachment, collision context setup, clump
  read, and vehicle-structure initialization; verify no leaked references,
  allocations, or stale model-info pointers.
- Two distinct custom vehicles sharing one TXD hash; verify TXD reference
  accounting and independent model-info/clump ownership.
- Disconnect, reconnect, model-record replacement, and client shutdown while
  downloads are pending or assets are loaded.


## 18. CVehicleModelInfo construction and SetClump transaction boundary

The reconstructed GTA SA 1.0 US source was rechecked at
`gta-reversed/gta-reversed` commit
`81ee79f984f0d049a8475d7badb7279a0049d78c`, specifically
`source/game_sa/Models/VehicleModelInfo.cpp`,
`VehicleModelInfo.h`, and `BaseModelInfo.cpp`.

Confirmed class lifecycle:

- `CVehicleModelInfo` has a real C++ constructor. It first constructs
  `CClumpModelInfo`, then initializes `m_pVehicleStruct = nullptr`,
  color-variation count and flags to zero, animation-block index to -1, and
  all 18 upgrade plus 4 remap indices to -1. The exact constructor entry point
  for the 0.3.DL R1 executable has not been established. The known
  `0x4C5920` address is the destructor, not a constructor.
- Calling virtual `Init()` is not equivalent to construction. It invokes
  `CClumpModelInfo::Init()`, which invokes `CBaseModelInfo::Init()`, and
  then sets vehicle type to IGNORE, wheel model index to -1, and bike steer
  angle to 999.99. It does not establish every constructor-only vehicle
  default.
- `CBaseModelInfo::Init()` resets refcount, collision-model pointer,
  TXD index, 2D-effect state, object-info index, draw distance, RW object
  pointer, flags, and selected base flags. It is a state initializer, not a
  replacement for the C++ constructor/vtable and base-subobject construction.
- `CClumpModelInfo::SetClump()` publishes the clump pointer, updates 2D
  effect accounting, registers visibility-plugin model info, adds a TXD
  reference, optionally adds an animation-block reference, and runs lighting
  setup. It has no success return value and is not a transactional API.
- `CVehicleModelInfo::SetClump()` allocates `CVehicleStructure` first, then
  calls the clump base setter and performs vehicle callback setup, frame-ID
  assignment, render-pipeline setup, hierarchy preprocessing, material
  reduction, plate setup, and dirt-material discovery. Several operations
  mutate the clump and global RenderWare state. A failure partway through
  cannot be handled by merely clearing the model-info pointer.
- The vehicle structure allocation uses the fixed pool at `0xB4E680`.
  Its constructor initializes dummy positions, upgrade parent IDs, extras,
  and component-damage mask. Its destructor destroys owned extra atomics and
  their frames. Pool readiness/capacity must be checked before invoking the
  vehicle setter; do not assume ordinary heap allocation or that pool
  exhaustion is recoverable inside `SetClump()`.
- `DeleteRwObject()` deletes the vehicle structure, then delegates to
  `CClumpModelInfo::DeleteRwObject()`. That base cleanup destroys the clump,
  removes the TXD reference, removes an animation-block reference when
  present, and deletes the collision model only when the ownership flag says
  it owns it. The separate base `Shutdown()` path also invokes collision
  cleanup and resets base metadata.

Registration prerequisites now established:

1. Do not construct the model by zeroing `0x308` bytes, copying a base
   vehicle, assigning only a vtable, or calling `Init()` on unconstructed
   storage. Obtain and verify the exact target constructor/destructor ABI and
   allocator strategy first. If no safe constructor entry point is available,
   a small target-compiled placement-construction shim is required; calling
   an unverified address is not acceptable.
2. Initialize metadata before `SetClump()`: a valid vehicle type in
   `[0, 11]`, handling ID in `[0, 209]`, valid TXD slot, and any required
   vehicle class/door/wheel/color fields. The IGNORE type (-1) is invalid for
   `SetClump()`, because it indexes `ms_vehicleDescs[m_nVehicleType]`.
3. Validate the vehicle structure pool is initialized and has a free slot,
   and ensure all vehicle descriptor tables and RenderWare pipeline globals
   are initialized. A type/handling range check alone does not prove the
   chosen metadata is semantically compatible with the DFF.
4. Parse and validate the clump and collision data into privately owned
   staging resources. Validate required hierarchy/frame names and all
   metadata-dependent assumptions before calling the void `SetClump()`.
5. Treat `SetClump()` as the publication boundary. Only after all preflight
   checks pass may it attach the clump and TXD reference. If any callback or
   preprocessing operation can fail or raise an access violation, the current
   native setter cannot provide rollback; implement a reviewed staged/manual
   equivalent or keep registration disabled.
6. Publish the custom model-info pointer in the custom ID bank only after
   model-info construction, clump setup, collision ownership, and TXD
   reference accounting have completed. On removal, unpublish first to stop
   new lookups, then release owned resources exactly once and clear the slot.

This confirms why the current stub must continue returning `FALSE`. The
reconstructed source establishes class semantics but does not establish the
exact 0.3.DL R1 constructor entry point, allocator ABI, pool lifecycle, or
safe rollback behavior. No registration code was enabled by this audit.

Current decision: do not enable custom vehicle registration or the vehicle DFF
reader yet. The source patch now contains a bounded path builder, but it has not
been compiled or tested in the target toolchain. Next design transactional
model-info/TXD/clump ownership and a streaming-independent custom asset
lifecycle, then validate the path helper and failure paths on the exact client.
