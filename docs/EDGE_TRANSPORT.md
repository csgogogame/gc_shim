# EdgeTransport — design

How this fork turns the standalone csgo_gc emulator into the client half of a
real, centralized GC. The goal is a **minimal, well-contained seam**: upstream
csgo_gc keeps working for econ/inventory locally, while matchmaking-class
messages are forwarded to the `gamecoordinator` backend.

## The seam

Intercepted GC traffic already funnels through one place: the `SharedGC` event
queue (`csgo_gc/gc_shared.h`). `steam_hook.cpp` hooks
`ISteamGameCoordinator::SendMessage/RetrieveMessage`; messages become events that
`ClientGC`/`ServerGC` service on a worker thread.

`EdgeTransport` plugs in **at that boundary**, not inside individual handlers. A
router classifies each outbound GC message by type:

- **Local** (econ, inventory, store, cases, graffiti…) → existing csgo_gc handling.
- **Remote** (matchmaking, ranks, SO cache, round stats…) → serialized into an
  `EdgeEnvelope` and sent to the backend; responses come back as envelopes and are
  injected into the game exactly as the local path injects them today.

The whitelist of remote message types is the contract surface and lives next to
the router.

## Transport

Length-prefixed protobuf over **TCP + TLS** (see
`gc_contracts/docs/WIRE_PROTOCOL.md`). Chosen over gRPC because this is a 32-bit
injected DLL where heavy deps are painful; the shim already links Crypto++. One
`EdgeFrame` per record:

```
[uint32 LE length][ csgogo.edge.EdgeFrame ]
```

Connection lifecycle: TLS → `EdgeHello` (role, version=1573, steamID, Steam auth
ticket) → `EdgeHelloResponse` → `EdgeEnvelope` traffic both ways → `EdgePing`
keepalive.

## Client vs dedicated

- **Client mode**: forwards `MatchmakingClient2GCHello`, `MatchmakingStart/Stop`,
  store/rank reads that must be authoritative, and receives `GC2ClientHello`,
  `GC2ClientUpdate`, `GC2ClientReserve`, `ClientGCRankUpdate`.
- **Dedicated mode**: registers to the backend, receives `GC2ServerReserve`,
  reports `MatchmakingServerRoundStats` / match end. The dedicated shim is baked
  into the `selfhosted_srcds` image.

## Build integration (Phase 1)

1. Add `gc_contracts` as a submodule (done in Phase 0).
2. `buf generate` C++ from it into `gen/`; add to the 32-bit CMake target.
3. Retire `protobufs/` in favor of the generated code (single source of truth).
4. Implement `EdgeTransport` (TLS client + framing) and the message router; wire
   it into `SharedGC`.

## Compatibility

The framing + `EdgeFrame` shape are the hard boundary shared with the Go backend.
Any change is a protocol bump coordinated in a single `gc_contracts` submodule
bump on both sides.
