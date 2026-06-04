# CLAUDE.md — gc_shim

Native C++ (32-bit) in-process shim, forked from
[mikkokko/csgo_gc](https://github.com/mikkokko/csgo_gc). It hooks
`ISteamGameCoordinator` on both the CS:GO client and `srcds` and intercepts Valve
GC traffic.

**Upstream csgo_gc answers everything locally and explicitly does not do
matchmaking** ("can't be implemented without a centralized server"). Our fork
adds exactly that centralized server by introducing an **`EdgeTransport`** at the
`SharedGC` seam: a whitelist of GC messages (matchmaking, ranks, SO cache, stats)
is forwarded over TLS to the `gamecoordinator` backend instead of being answered
locally. Econ/inventory/store stay local for v1.

> Protocol pinned to CS:GO **1573 / 1.38.7.9**. See `docs/EDGE_TRANSPORT.md`.

## Where things are

```
csgo_gc/              the shim itself (forked)
  steam_hook.*        hooks ISteamGameCoordinator (the interception point)
  gc_shared.*         SharedGC event queue — THE SEAM for EdgeTransport
  gc_client.*         ClientGC: client-side GC handling
  gc_server.*         ServerGC: dedicated-server GC handling
  networking_*        Steam P2P transport between client and server
protobufs/            CURRENT committed Valve protos (.proto + generated .pb.*)
third_party/gc_contracts  submodule: the wire (pinned by commit)
docs/                 design docs (EDGE_TRANSPORT.md ...)
```

## Hard rules

- The **`SharedGC` event boundary** (`gc_shared.h`) is the integration seam. Add
  the EdgeTransport there — do not scatter networking through the GC handlers.
- The wire is owned by `gc_contracts`. From Phase 1, generate C++ from the
  submodule (`buf generate ... --template buf.gen.cpp.yaml`) into `gen/` and link
  it into the 32-bit CMake target. The legacy `protobufs/` dir is replaced by that
  generated code — until the migration lands, do not diverge the two.
- 32-bit only (the game is 32-bit). Keep deps injectable-DLL-friendly: the
  transport is length-prefixed protobuf over TLS, **not gRPC**.
- Client version on the wire MUST be 1573; the backend rejects mismatches.
- Commits: no AI co-author trailer. Design docs go in `docs/`.

## Build

See `README.md` (upstream). 32-bit: `cmake -A Win32 -B build` (Windows),
`-m32` flags on Linux. The EdgeTransport + generated contracts join the build in
Phase 1.
