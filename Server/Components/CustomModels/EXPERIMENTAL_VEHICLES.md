# Experimental custom vehicle models

This server-side feature is experimental. It is disabled unless
`artwork.experimental_vehicle_models` is set to `true`.

## Registration

Place the vehicle DFF and TXD files in the configured artwork models directory,
then add an entry to `artconfig.txt`:

```text
AddVehicleModel( baseModelId, customModelId, "vehicle.dff", "vehicle.txd" )
```

- `customModelId` must be between `30001` and `32767`.
- `baseModelId` must be a normal vehicle model ID from `400` through `611`.
- Base models `449`, `537`, `538`, `569`, and `570` are excluded in this prototype because their special rail vehicle/train behavior has not been implemented.
- Both files must exist and must be valid GTA San Andreas DFF/TXD assets.

The base model ID supplies gameplay characteristics such as handling, seats,
components, and model-specific server calculations. The custom ID identifies
the replacement artwork sent to capable clients.

## Asset integrity

Each registered file path is bound to the size and CRC32 recorded in the
model manifest, regardless of whether the server uses its local web server or
a CDN. Registering the same path again with different contents is rejected.

With the local artwork web server, the server rechecks a registered file's
path, size, and CRC32 before serving it and again after opening it. A mismatch
is answered with HTTP 409 instead of serving the changed file. Restore the
registered bytes or restart the server after updating the model registration.

When `artwork.cdn` is configured, the server cannot inspect or validate the
remote CDN response. The operator must publish the exact DFF/TXD bytes that
were used when the model was registered and keep those URLs immutable.

## Client compatibility

An unmodified SA-MP 0.3.DL client does **not** support the vehicle model
category introduced by this experiment. Such clients continue to receive only
the native 0.3.DL skin/object artwork records, and vehicles are streamed using
their base model IDs.

A client must explicitly opt in by sending RPC 221
(`ClientSupportsCustomVehicleModels`). The server replies with RPC 222
(`ServerSupportsCustomVehicleModels`) and then sends the custom vehicle
manifest. A client that does not implement the new category must not send
this capability RPC.

The custom vehicle extension deliberately uses a separate RPC block so it
does not collide with stock SA-MP actor RPCs or the native 0.3.DL artwork
download pipeline:

| RPC | Direction | Purpose |
| --- | --- | --- |
| 220 | Server → client | Vehicle model manifest; model type is `3` |
| 221 | Client → server | Advertise custom vehicle model support |
| 222 | Server → client | Acknowledge custom vehicle support |
| 223 | Client → server | Request vehicle DFF by checksum |
| 224 | Client → server | Request vehicle TXD by checksum |
| 225 | Server → client | Vehicle asset download URL |
| 226 | Client → server | Report vehicle asset completion |
| 227 | Server → client | Confirm vehicle asset completion |

The native SA-MP/0.3.DL artwork RPCs remain unchanged: 179 carries native
skin/object model records, 181/182 request their DFF/TXD files, 183 carries
their URLs, and 184/185 complete the native artwork transfer. The custom
vehicle client adapter does not intercept those IDs.

RPC 220 uses the same 39-byte model-record layout as the legacy artwork
manifest: `poolID:u32`, `Count:i32`, `type:u8`, `virtualWorld:i32`,
`baseId:i32`, `newId:i32`, DFF/TXD checksums and sizes, then
`timeOn:u8` and `timeOff:u8`. Its pool is independent from the native
0.3.DL skin/object pool. Vehicle indices begin at zero and `Count` is the
number of registered vehicle models advertised to that client.

Capability timing is handled in both cases. When RPC 221 arrives before client
initialization, the initial vehicle manifest is sent during client init. When
it arrives after client initialization, the server sends the vehicle manifest
immediately. Later vehicle registrations append one record with the next
vehicle pool index instead of retransmitting the full manifest. A client must
honor the supplied `poolID` and `Count`; it must not mix the vehicle pool
with the native skin/object artwork pool.

## Client implementation boundary

The server-side component only defines the opt-in protocol and the custom
vehicle model records. A compatible client must implement model type `3`,
download both assets, register the vehicle visual, and keep custom IDs valid
through vehicle creation and streaming.

Client-binary reconstruction, RenderWare ownership, `samp.dll` patch files,
and exact SA-MP executable validation are separate client-research artifacts;
they are not part of the open.mp source tree. Stock clients must never send
RPC 221 and continue to receive base vehicle IDs only.

## Test status

The server implementation and protocol are still experimental. CI builds can
verify server compilation for supported targets, but compilation alone does
not verify protocol compatibility or client rendering. No end-to-end test with
a client implementing model type `3` has been completed yet.

The first compatibility checks should confirm that:

- a stock 0.3.DL client receives no vehicle type-3 records and remains on the
  base vehicle model;
- a modified 0.3.DL client completes the 221/222 handshake, downloads
  both assets, and replaces the correct vehicle artwork;
- stream-in/out, reconnect, respawn, passenger/driver sync, vehicle colors,
  components, and NPC vehicle interactions continue to work.

Do not enable this option on a production server until the client and server
have passed those checks.
