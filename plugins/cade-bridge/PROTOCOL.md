# Cade ⇄ VPX PlatformService protocol

VPX (the cade-bridge plugin) **hosts** a gRPC `PlatformService` on `0.0.0.0:50052`.
cade is the **client**: it dials in and opens the bidirectional `PlatformEventFlow`
stream. Events flow VPX→cade (device hits, status, manifest, config ACKs); commands
flow cade→VPX (device actuation, config updates, stream control / heartbeats).

## Stream contract

On `PlatformEventFlow` open, the server (VPX) sends, in order:

1. `PlatformEvent.status` = `STATE_CONNECTED`
2. `PlatformEvent.manifest` (the device manifest, if a table is loaded)
3. `PlatformEvent.device_event` … (table_ready + state snapshot, then live events)

The server responds to `PlatformConfigUpdate` commands with a `PlatformEvent.config_ack`.

## Threading model (server side)

The server uses **one reader thread and one dedicated writer thread** on the same
`ServerReaderWriter` (gRPC permits one concurrent `Read` + one concurrent `Write`):

- **writerThread** — the only thread that calls `stream->Write()`. Waits on a
  condition variable and drains the outbound queue the instant an event is
  enqueued. Outbound delivery does **not** depend on inbound traffic.
- **readerThread** — the only thread that calls `stream->Read()`; feeds inbound
  commands to a dispatch queue.
- **dispatchThread** — applies inbound commands via the VPX API; never touches
  the stream.

> History: an earlier single-thread design flushed outbound events only *around*
> `stream->Read()`. Because cade only heartbeats at stream-open and during
> config-ACK waits (not during steady-state play), the reader stayed blocked in
> `Read()` and queued events were never written (`reads=2 writes=0`). The
> dedicated writer removes that coupling.

## Keepalive

| Side | Setting | Value |
|------|---------|-------|
| cade client | keepalive `Time` | 6 min |
| cade client | keepalive `Timeout` | 20 s |
| cade client | `PermitWithoutStream` | true |
| VPX server | `GRPC_ARG_KEEPALIVE_TIME_MS` | 5 min |
| VPX server | `GRPC_ARG_KEEPALIVE_TIMEOUT_MS` | 10 s |

The server pings *less* aggressively than the client's `MinTime` default (5 min)
to avoid `ENHANCE_YOUR_CALM` teardown. Both use 1 MB initial windows.

## Proposed (optional) cade client change

With the dedicated server writer, cade **no longer needs** the 50 ms heartbeat to
pump outbound events — that mechanism (`SendConfigUpdate`'s ticker in
`platform_client.go`) is now only required to bound config-ACK latency and can stay.

Two optional, low-risk client improvements:

1. **Liveness heartbeat:** send a single `StreamControl{ACTION_HEARTBEAT}` on a slow
   ticker (e.g. every 2–5 s) for the whole stream lifetime. Detects a half-open
   connection far sooner than the 6-min keepalive, with negligible cost. This is
   defense-in-depth, not a correctness requirement.

2. **Do not reconnect across table changes.** Once VPX hosts the server at app
   scope (persistent across table load/unload), the stream stays open between
   tables; cade receives `table_stopped` / `table_ready` device events instead of
   stream EOF. The driver should treat those as table-state transitions and keep
   the existing stream, reconnecting only on actual transport errors.

No proto changes are required for either.
