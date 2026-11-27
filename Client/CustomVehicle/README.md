# Custom vehicle client

This directory contains the client-side implementation track for the experimental
custom vehicle extension in open.mp. The server component remains in
`Server/Components/CustomModels`; the client code is intentionally isolated.

## Architecture

The client is split into two layers:

- **Shared core**: platform-neutral C++17 protocol parsing, session state,
  model-ID validation, and asset integrity checks. It must not include GTA,
  SA-MP, Windows, or Android headers.
- **Platform adapters**: client-specific networking, file storage, model loading,
  and vehicle-model registration. Each adapter is built and tested for its own
  client binary, operating system, and CPU ABI.

The shared core is not a game plugin by itself. A client adapter must still
negotiate the extension, receive model manifests, download and verify assets,
register DFF/TXD resources with the game, and make the custom model ID resolve
to the correct vehicle model at runtime.

## Compatibility policy

"All versions" is a target, not a claim about the current implementation.
Unknown client binaries must fail closed: no native hooks or guessed offsets are
installed for an unrecognized executable.

| Client target | Current status | Required implementation |
| --- | --- | --- |
| SA-MP 0.3.7 R1 / R3-1 / R5-1 | Windows x86 research baseline | Version-aware network adapter and verified GTA native backend |
| SA-MP 0.3.DL R1 | Windows x86 network/download adapter enabled | Separate vehicle RPC channel; native DL artwork remains untouched |
| Other SA-MP revisions | Not yet supported by this client | Separate verified network profile and regression test |
| open.mp server with a legacy SA-MP client | Server-side path exists | Client adapter must negotiate the experimental RPC extension |
| Distinct open.mp client binary | Not yet verified | Identify binary/version and add a dedicated adapter profile |
| Android GTA SA multiplayer client | JNI/native bridge builds for four ABIs; no game client is qualified yet | Verified client RPC hook, Android model loader, and game-engine integration |
| Windows x64 / Linux / macOS | Not yet supported as game clients | Separate runtime integration for each platform; the current ASI SDK is Windows x86 only |

The upstream `samp-client-sdk` project describes its ASI host as Windows x86
only. Its published direct client bridge covers SA-MP 0.3.7 R1, 0.3.7 R3-1,
0.3.7 R5-1, and 0.3.DL R1 on GTA: San Andreas 1.0 US. Other recognized builds
are limited to raw network observation unless their direct bridge is separately
verified. This makes it a possible first Windows adapter dependency, not an
all-version or Android solution.

The same upstream SDK currently marks GTA model streaming and native vehicle
creation as unsupported. Until those lifecycle and ownership contracts are
verified, this project must not call guessed `CStreaming`, vehicle constructors,
pool allocation, or world-insertion addresses. This is a hard blocker for
end-to-end rendering, not just a missing wrapper.

The extension deliberately does not reuse native SA-MP/0.3.DL artwork
RPC IDs. Native model/artwork messages stay on 179 and 181-189, while custom
vehicle manifests and downloads use open.mp extension RPCs 220-227. That keeps
actor operations and stock DL artwork traffic out of the custom transfer state
machine, including when both download channels are active at once.

## Shared core

The first implementation is in [Shared](Shared/). It currently provides:

- Decoder and validation for the 39-byte RPC 220 vehicle manifest.
- Decoder and validation for server RPC 164 vehicle stream-in payloads (63 bytes),
  including the 40-byte vehicle record, 14 mod slots, paintjob and body colours.
  The decoder accepts standard IDs 400-611 and extension IDs 30001-32767,
  rejects non-finite transform/health fields, and does not install a game hook.
- Decoder for RPC 225 vehicle download URLs, including transport marker 6,
  `ModelDownloadType` values `DFF=1` and `TXD=2`, checksum, and
  length-prefixed URL.
- Encoder for vehicle checksum requests RPC 223 (DFF) and RPC 224 (TXD).
- Portable CRC32 and exact size/checksum verification for downloaded assets.
- A local DFF/TXD cache keyed by asset type and checksum. Writes use a `.part`
  file, verify the persisted bytes, and then rename to the final cache path.
  Existing entries are revalidated; paths resolving outside the cache root are rejected.
- A session state machine for RPC 221/222 capability negotiation, vehicle
  manifest collection, sequential asset requests, URL matching, file
  verification, RPC 226 finish notifications, and RPC 227 acknowledgements.
- A platform-neutral RPC adapter that sends the capability handshake, consumes
  experimental vehicle manifests, forwards native skin/object manifests, issues
  DFF/TXD checksum requests, and drives the download completion handshake.
- Serialized access to the session state so the RPC callback and the asynchronous
  download worker cannot mutate the asset queue concurrently.
- The server keeps one pending vehicle asset request per opted-in player.
  A retry may request a new URL for the same type/checksum; a different asset
  advances only after RPC 227 acknowledges the matching completion. RPC 226
  carries five bytes: asset type (`u8`) plus CRC32 checksum (`u32`,
  little-endian), allowing the server to reject mismatched or unsolicited
  completions.
- RPC isolation: only extension IDs 220-227 are consumed; native RPC 179 and
  181-189 are forwarded to the original SA-MP/0.3.DL client unchanged.
- Explicit connection teardown and retry handling for queued or interrupted
  asset transfers.
- A C ABI decoder for server RPC 164 that resolves a custom model ID to its
  registered base vehicle and reports whether its DFF/TXD assets are ready. This
  is a platform hand-off contract only; native GTA vehicle construction/rendering
  remains a separate milestone.
- A versioned C ABI in `include/custom_vehicle/bridge.h` for hosts written in
  C, Rust, or JNI-backed Android code. The C boundary exposes opaque adapter
  handles, RPC/download callbacks, and copied model snapshots without exposing
  C++ STL types. CI also compiles this header as C.
- A bounded native model-slot registry that maps server IDs `30001-32767` to
  confirmed-vacant entries in GTA SA's original `0-19999` model-info table.
  Its default allocation range is `612-19999`; each slot is checked through
  a caller-supplied vacancy callback. It tracks live-vehicle leases and blocks
  slot reuse until a model is retired, all vehicles release their leases, and
  the native table entry is confirmed empty. C ABI wrappers keep C++ mutex,
  vector, and class internals outside the Rust/ASI ABI.
- A transactional native model lifecycle controller in
  `native_vehicle_model_backend.hpp`. It sequences model-info creation,
  native streaming request/poll, clump attachment, vehicle-use leases, and
  reverse cleanup. Native asset callbacks return `Pending`, `Ready`, or
  `Failed`; a pending request retains its slot and resumes on a later game tick
  without blocking. Failed activation rolls back completed stages; failed
  teardown leaves the slot retired and reserved for an explicit cleanup retry.
  The controller contains no GTA addresses or game pointers. A Windows backend
  must call it only from the verified game thread and supply the actual GTA
  operations through the stable C ABI.
- A fixed-capacity per-vehicle binding table connects each SA-MP vehicle-pool
  ID to its active custom model and native slot. Bind operations are idempotent
  for the same vehicle/model, reject a different model while an old binding is
  active, and return a generation token. Release requires the exact copied
  24-byte binding record, so stale cleanup after vehicle-pool ID reuse cannot
  release a newer vehicle's model lease. The C ABI is tested from C99 and C++.
- The lifecycle C ABI passes a copied 44-byte model manifest to native callbacks
  and publishes named cleanup stages: Reserved, ModelInfoCreated,
  AssetsRequested, AssetsLoaded, and ClumpAttached. Callbacks execute
  synchronously while lifecycle state is locked and must not re-enter the
  controller; the destroy callback must be idempotent and report success only
  after all native pointers, streaming requests, clump resources and references
  have been cleared.
- Android JNI exposes the same RPC 164 decoder to Java hosts; the returned
  status distinguishes native vehicles, ready custom vehicles, and custom models
  whose assets are still pending. Float fields are passed as IEEE-754 bit patterns.
- An Android JNI wrapper in [Android](Android/) that links the shared adapter,
  verifies downloaded bytes, and stores DFF/TXD files in an app-private cache.
  It builds a `.so` for Android ARM64, ARMv7, x86, and x86_64. This is an
  integration bridge for a compatible multiplayer client, not client support
  by itself.

The adapter depends on two platform interfaces: `RpcTransport` sends raw
RakNet RPC payloads, and `AssetDownloadSink` copies a callback-scoped URL and
schedules download work off the network callback thread. The sink must not
re-enter the adapter from `beginDownload`, because it is called while the
session lock is held. No concrete HTTP client, platform hook, or native game
model loader is included yet; the cache is an API that the platform downloader
must call before reporting a completed asset.

RPC 220 currently encodes: `poolId:u32`, `modelCount:i32`,
`type:u8`, `virtualWorld:i32`, `baseId:i32`, `newId:i32`,
`dffChecksum:u32`, `txdChecksum:u32`, `dffSize:u32`,
`txdSize:u32`, `timeOn:u8`, and `timeOff:u8`.
The wire payload is 39 bytes. Fixed-width integers are decoded as little-endian
values; negative counts, invalid pool indices, unsupported base IDs, and
out-of-range custom IDs are rejected.

The RPC IDs 220-227 belong to this experimental open.mp extension. They are not a
claim that stock SA-MP clients understand custom vehicles or these extension
messages. The shared adapter can be connected to a verified client network hook,
but it is not itself an ASI plugin, Android plugin, HTTP downloader, file cache,
DFF/TXD loader, or in-game vehicle integration.

Build and run its tests:

```sh
cmake -S Client/CustomVehicle/Shared -B build/custom-vehicle-client -DCMAKE_BUILD_TYPE=Release
cmake --build build/custom-vehicle-client --config Release
ctest --test-dir build/custom-vehicle-client --build-config Release --output-on-failure
```

The dedicated CI workflow compiles/tests the host core on Windows, Linux, and
macOS, and cross-compiles the Android JNI shared library for `arm64-v8a`,
`armeabi-v7a`, `x86`, and `x86_64`. These jobs validate the portable
protocol, adapter, cache, C ABI and JNI build; they do not certify an Android
multiplayer game client or its native model backend.

## Native backend checkpoint

The slot registry and lifecycle controller are groundwork for the native
backend, not GTA registration itself. The controller only sequences callbacks;
it does not contain a concrete `CVehicleModelInfo` allocator, RenderWare DFF/TXD
loader, `SetClump` call, executable detour, or vehicle-pool mutation. The
Windows x86 ASI now intercepts the incoming stream-in/remove pair: RPC 164
stock vehicles (400-611) continue unchanged; a registered custom model is
validated, rewritten to its compatible base ID, and copied into a bounded
queue. Unknown custom IDs and queue failures are blocked so an invalid external
ID never reaches stock `CreateVehicle`. RPC 165 cancels an entry that has not
yet been popped. `CustomVehicleWindows_PopPendingVehicle` exposes copied
records for the game-thread processing stage.

The ASI also registers an observation-only game-thread callback through the
pinned `modkit-sdk` service when the exact GTA SA 1.0 US native profile is
available. It validates the host-issued callback scope, counts ticks, and samples
the pending queue length. The callback deliberately does not dequeue records or
mutate GTA state. Callback teardown uses the host's drain operation and retains
its context if in-flight work cannot be safely drained. If the native profile is
unavailable, networking and asset transfers remain enabled while this callback
stays disabled.

This is still only the network-to-game-thread handoff. The current code lets the
stock client create the compatible **base vehicle**, but it has no processing
consumer, native model-info allocator, DFF/TXD RenderWare loader, clump attachment,
or base-to-custom vehicle swap yet. Therefore the actual custom vehicle is not
rendered by this milestone.

The next native integration must be implemented as one guarded path:

1. Identify the exact GTA and SA-MP executable profile and validate all hook
   signatures before installing any native detour.
2. Connect the existing RPC 164/165 handoff queue to a verified GTA
   game-thread consumer. Preserve ordinary vehicles byte-for-byte, ensure
   removals cancel pending work, and verify the corresponding vehicle still
   exists before applying any native mutation. Bind each confirmed custom
   vehicle with its pool ID and generation token; release only after native
   entity teardown, never from an unverified or stale remove callback.
3. Avoid passing an external ID directly to the stock `CGame::CreateVehicle`
   wrapper: its `model - 400` keep-loaded array access is not range-checked.
   Construct the vehicle through a verified base-model path, then associate
   its native entity with a safely registered internal vehicle model slot.
4. For the pinned GTA SA 1.0 US profile, use a typed, heap-owned vehicle
   model-info strategy rather than appending beyond the fixed 212-record
   `CStore<CVehicleModelInfo>`: the MTA reference allocates a complete
   `CVehicleModelInfoSAInterface`, copies the `0x308` interface from a compatible
   base vehicle, resets references/RW pointers and sentinels, registers it in the
   model-info pointer table, and copies the base streaming archive metadata.
   This is design evidence only; validate the exact interface and slot for the
   selected executable before any write. The paired teardown must remove the
   model from streaming, balance references, reset detached car-part references,
   delete the correctly typed interface, clear the model-info pointer, and reset
   the streaming-info record. It must also release the embedded-collision context,
   TXD/animation references, vehicle visual structure, and RenderWare clump.
   Unwind every acquired resource on failure or stream-out.
5. Keep model-info slots, vehicle entities, RenderWare objects, TXD references,
   detached car parts, and streaming references synchronized through deletion
   and same-ID model replacement.

GTA SA's initial `CVehicleStructure` pool capacity is 50. The registry's
available model-info slot count does not increase this pool. The renderer
backend must manage the lifetime of loaded vehicle clumps and decide, from
verified allocator behavior, whether to reuse a bounded number of slots or
safely expand the structure pool. It must not assume that the model-info
table's free slots imply unlimited loaded vehicle models.

### Feature backlog after basic spawn

The supplied LunarVehiclesMP/LunarVisualsMP release notes are useful as a
reference for later compatibility work. The backlog is intentionally staged:
custom tuning parts and paintjobs; correct damage/repair and original-part
restoration; fast model replacement and old-resource cleanup; robust downloads,
retry, cache pruning and integrity-checked updates; then client-specific Android
texture handling, mod coexistence, crash diagnostics, custom engine sounds,
sirens and motorcycle rider animations. These are not implemented features or
current compatibility claims.

## Milestones

1. **Shared protocol core** — manifest and download RPC codecs, session flow,
   model validation, asset integrity, and portable unit tests.
2. **Windows x86 ASI adapter** — capability negotiation, RPC hooks, HTTP
   download/cache, and exact client-version gating.
3. **GTA native backend** — safe DFF/TXD registration, model streaming, model
   lookup, vehicle construction, and cleanup on a verified executable profile.
4. **Additional SA-MP revisions and distinct open.mp clients** — add profiles
   only after network and native behavior are independently verified.
5. **Android client integration** — connect the JNI bridge to a specific Android
   multiplayer client's verified RPC hook and native game/model backend; a
   Windows `.asi` cannot be loaded on Android.
6. **Other architectures and operating systems** — only where a compatible GTA
   SA game client and plugin/runtime interface exist.

A milestone is not complete merely because the shared library compiles. End-to-end
support requires an actual client build, in-game loading tests, spawn/streaming
tests, and lifecycle/cleanup tests.
