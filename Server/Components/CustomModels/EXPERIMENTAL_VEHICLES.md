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

## Client compatibility

An unmodified SA-MP 0.3.DL client does **not** support the vehicle model
category introduced by this experiment. Such clients continue to receive only
the native 0.3.DL skin/object artwork records, and vehicles are streamed using
their base model IDs.

A client must explicitly opt in by sending RPC 186
(`ClientSupportsCustomVehicleModels`). The server replies with RPC 187
(`ServerSupportsCustomVehicleModels`) and then sends its model manifest,
including vehicle records. A client that does not implement the new category
must not send this capability RPC.

The server uses the following existing artwork RPCs for the transfer:

| RPC | Purpose |
| --- | --- |
| 179 | Model record; vehicle records use model type `3` |
| 181 | Request DFF by checksum |
| 182 | Request TXD by checksum |
| 183 | Download URL response |
| 184 | Client reports completion |
| 185 | Server confirms completion |
| 186 | Client advertises custom vehicle model support |
| 187 | Server acknowledges support |

For RPC 179, the manifest index (`poolID`) and total count (`Count`) are
shared by the model records sent to that client. The server builds a contiguous
manifest per client. Ordinary 0.3.DL clients receive skin/object records;
opted-in clients receive those records plus vehicle records.

Capability timing is handled in both cases. When RPC 186 arrives before client
initialization, the initial manifest includes all supported records. When it
arrives after the native 0.3.DL manifest, vehicle records are appended after the
existing native records. Later model registrations append a single new record
with the next pool index instead of retransmitting the full manifest. A client
must honor the supplied `poolID` and `Count`; it must not assume records are
grouped by model type or that the type-3 records always begin at index zero.

## Client implementation boundary

The server-side component only defines the opt-in protocol and the custom
vehicle model records. A compatible client must implement model type `3`,
download both assets, register the vehicle visual, and keep custom IDs valid
through vehicle creation and streaming.

Client-binary reconstruction, RenderWare ownership, `samp.dll` patch files,
and exact SA-MP executable validation are separate client-research artifacts;
they are not part of the open.mp source tree. Stock clients must never send
RPC 186 and continue to receive base vehicle IDs only.

## Test status

The server implementation and protocol are still experimental. CI builds can
verify server compilation for supported targets, but compilation alone does
not verify protocol compatibility or client rendering. No end-to-end test with
a client implementing model type `3` has been completed yet.

The first compatibility checks should confirm that:

- a stock 0.3.DL client receives no vehicle type-3 records and remains on the
  base vehicle model;
- a modified no-ASI 0.3.DL client completes the 186/187 handshake, downloads
  both assets, and replaces the correct vehicle artwork;
- stream-in/out, reconnect, respawn, passenger/driver sync, vehicle colors,
  components, and NPC vehicle interactions continue to work.

Do not enable this option on a production server until the client and server
have passed those checks.
