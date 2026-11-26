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
| SA-MP 0.3.DL R1 | Windows x86 research baseline | Custom-vehicle extension adapter; do not assume native DL vehicle support |
| Other SA-MP revisions | Not yet supported by this client | Separate verified network profile and regression test |
| open.mp server with a legacy SA-MP client | Server-side path exists | Client adapter must negotiate the experimental RPC extension |
| Distinct open.mp client binary | Not yet verified | Identify binary/version and add a dedicated adapter profile |
| Android GTA SA multiplayer client | Not yet supported | Android-native library/loader, ARM ABI profiles, and Android game-engine integration |
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

SA-MP 0.3.DL has an additional protocol-integration gap: its native artwork
pipeline also uses RPC 181/182/183/184/185. The current adapter can identify
custom transfers by the active checksum request, but it does not yet coordinate
or multiplex native DL transfers that may be in flight at the same time.
Consequently, DL R1 is a research target only; it is not qualified for
simultaneous native artwork and custom vehicle downloads.

## Shared core

The first implementation is in [Shared](Shared/). It currently provides:

- Decoder and validation for the 39-byte RPC 179 model manifest.
- Decoder for RPC 183 download URLs, including transport marker 6,
  `ModelDownloadType` values `DFF=1` and `TXD=2`, checksum, and
  length-prefixed URL.
- Encoder for checksum requests RPC 181 (DFF) and RPC 182 (TXD).
- Portable CRC32 and exact size/checksum verification for downloaded assets.
- A local DFF/TXD cache keyed by asset type and checksum. Writes use a `.part`
  file, verify the persisted bytes, and then rename to the final cache path.
  Existing entries are revalidated; paths resolving outside the cache root are rejected.
- A session state machine for RPC 186/187 capability negotiation, vehicle
  manifest collection, sequential asset requests, URL matching, file
  verification, RPC 184 finish notifications, and RPC 185 acknowledgements.
- A platform-neutral RPC adapter that sends the capability handshake, consumes
  experimental vehicle manifests, forwards native skin/object manifests, issues
  DFF/TXD checksum requests, and drives the download completion handshake.
- Serialized access to the session state so the RPC callback and the asynchronous
  download worker cannot mutate the asset queue concurrently.
- URL ownership checks: a malformed URL for an active custom transfer is
  consumed rather than exposed to the stock 0.3.DL artwork parser; unrelated
  native URL traffic is forwarded.
- Explicit connection teardown and retry handling for queued or interrupted
  asset transfers.
- A versioned C ABI in `include/custom_vehicle/bridge.h` for hosts written in
  C, Rust, or JNI-backed Android code. The C boundary exposes opaque adapter
  handles, RPC/download callbacks, and copied model snapshots without exposing
  C++ STL types. CI also compiles this header as C.

The adapter depends on two platform interfaces: `RpcTransport` sends raw
RakNet RPC payloads, and `AssetDownloadSink` copies a callback-scoped URL and
schedules download work off the network callback thread. The sink must not
re-enter the adapter from `beginDownload`, because it is called while the
session lock is held. No concrete HTTP client, platform hook, or native game
model loader is included yet; the cache is an API that the platform downloader
must call before reporting a completed asset.

RPC 179 currently encodes: `poolId:u32`, `modelCount:i32`,
`type:u8`, `virtualWorld:i32`, `baseId:i32`, `newId:i32`,
`dffChecksum:u32`, `txdChecksum:u32`, `dffSize:u32`,
`txdSize:u32`, `timeOn:u8`, and `timeOff:u8`.
The wire payload is 39 bytes. Fixed-width integers are decoded as little-endian
values; negative counts, invalid pool indices, unsupported base IDs, and
out-of-range custom IDs are rejected.

The RPC IDs above belong to this experimental open.mp extension. They are not a
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
macOS, and cross-compiles the portable core for Android `arm64-v8a`,
`armeabi-v7a`, `x86`, and `x86_64`. These jobs validate only the portable
library; they do not certify a multiplayer game client or its native model
backend on those targets.

## Milestones

1. **Shared protocol core** — manifest and download RPC codecs, session flow,
   model validation, asset integrity, and portable unit tests.
2. **Windows x86 ASI adapter** — capability negotiation, RPC hooks, HTTP
   download/cache, and exact client-version gating.
3. **GTA native backend** — safe DFF/TXD registration, model streaming, model
   lookup, vehicle construction, and cleanup on a verified executable profile.
4. **Additional SA-MP revisions and distinct open.mp clients** — add profiles
   only after network and native behavior are independently verified.
5. **Android** — a separate native client adapter for supported Android game
   versions and ARM ABIs; it cannot load a Windows `.asi`.
6. **Other architectures and operating systems** — only where a compatible GTA
   SA game client and plugin/runtime interface exist.

A milestone is not complete merely because the shared library compiles. End-to-end
support requires an actual client build, in-game loading tests, spawn/streaming
tests, and lifecycle/cleanup tests.
