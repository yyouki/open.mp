# Custom vehicle client

This directory contains the client-side implementation track for the experimental
custom vehicle extension in open.mp. The server component remains in
`Server/Components/CustomModels`; the client code is intentionally isolated.

## Architecture

The client is split into two layers:

- **Shared core**: platform-neutral C++17 protocol parsing, model-ID validation,
  and asset integrity checks. It must not include GTA, SA-MP, Windows, or Android
  headers.
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

## Shared core

The first shared implementation is in [Shared](Shared/). It decodes the existing
39-byte RPC 179 model-manifest record, validates the experimental vehicle ID
range (30001-32767) and supported base vehicle IDs (400-611, excluding 449,
537, 538, 569, and 570), and verifies downloaded asset size plus CRC32.

Build and run its tests:

```sh
cmake -S Client/CustomVehicle/Shared -B build/custom-vehicle-client
cmake --build build/custom-vehicle-client
ctest --test-dir build/custom-vehicle-client --output-on-failure
```

## Protocol facts

The current experimental server extension uses these RPC identifiers:

| RPC | Direction | Purpose |
| --- | --- | --- |
| 179 | Server to client | Custom model manifest record |
| 181 | Client to server | Request DFF download URL by checksum |
| 182 | Client to server | Request TXD download URL by checksum |
| 183 | Server to client | Download URL and asset metadata |
| 184 | Client to server | Finish-download notification |
| 185 | Server to client | Download-completed acknowledgement |
| 186 | Client to server | Custom vehicle capability opt-in |
| 187 | Server to client | Capability acknowledgement |

RPC 179 currently encodes: `poolId:u32`, `modelCount:i32`,
`type:u8`, `virtualWorld:i32`, `baseId:i32`, `newId:i32`,
`dffChecksum:u32`, `txdChecksum:u32`, `dffSize:u32`,
`txdSize:u32`, `timeOn:u8`, and `timeOff:u8`.
The wire payload is 39 bytes. The shared decoder reads fixed-width integers
as little-endian values and validates the pool index before publishing a record.

The RPC IDs above belong to this experimental open.mp extension. They are not a
claim that stock SA-MP clients understand custom vehicles or these extension
messages.

## Milestones

1. **Shared protocol core** — manifest decoding, model validation, asset integrity,
   and portable unit tests.
2. **Windows x86 ASI adapter** — capability negotiation, RPC handlers, asset
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
