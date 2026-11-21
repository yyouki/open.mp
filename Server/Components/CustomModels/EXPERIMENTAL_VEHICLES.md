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
shared by the model records sent to that client. The server therefore builds a
contiguous manifest per client: ordinary 0.3.DL clients receive skin/object
records, while opted-in clients receive those records plus vehicle records.

## No-ASI experiment

The planned no-ASI experiment does not mean an unmodified 0.3.DL executable
will work. The client itself needs an implementation of model type `3` and
vehicle-model registration in its GTA/RenderWare model-loading path. The
intended experiment is to integrate that support into a rebuilt or directly
modified 0.3.DL client binary rather than load a separate ASI plugin.

At minimum, the client-side implementation must:

1. Recognize model type `3` in RPC 179 and preserve the shared manifest
   indices.
2. Reuse the DFF/TXD download and checksum-validation path for the two assets.
3. Register the downloaded DFF/TXD as a vehicle model while retaining the
   selected base vehicle model's gameplay behavior.
4. Resolve the custom vehicle ID consistently during vehicle creation,
   streaming, replacement, and streaming-out.
5. Send RPC 186 only after all of the above paths are available.

## Test status

The server implementation and protocol are still experimental. This branch
has not yet been verified by a full server build or an end-to-end client test.
The first compatibility checks should confirm that:

- a stock 0.3.DL client receives no vehicle type-3 records and remains on the
  base vehicle model;
- a modified no-ASI 0.3.DL client completes the 186/187 handshake, downloads
  both assets, and replaces the correct vehicle artwork;
- stream-in/out, reconnect, respawn, passenger/driver sync, vehicle colors,
  components, and NPC vehicle interactions continue to work.

Do not enable this option on a production server until the client and server
have passed those checks.
