# Online play on the Vita: what the PC port has, and what porting it would take

An analysis (2026-11-13), nothing built yet. The Vita port has no online or ad-hoc play (the CHANGELOG's known
issues); the PC port it comes from, [z3xox/Tenkaichi3Decomp](https://github.com/z3xox/Tenkaichi3Decomp), has had it
since its release 0.1.8. Its working log is that repository's `docs/netplay_notes.md`.

## Where the two ports part

The Vita port branched from the PC port at `dd6b20c` (2026-10-06). The PC port's netcode began the next day
(`6a65ae7`, 2026-10-07: "two copies in lockstep over UDP"); by 2026-11-13 its `main` had 644 commits more than that
point, the Vita port 70 of its own (the renderer, `port/vita/`). None of the PC port's online code is in this
repository.

## How the PC port plays online

- **Code**: `port/src/gs/net.c` (the session, lockstep and rollback), `net_lobby.c`, `net_match.c`, `net_relay.c`,
  `net_chat.c`, `net_sign.c`, `net_update.c`: about 11,000 lines, plain BSD sockets over UDP, SDL3 for threads and
  timers.
- **Matchmaking**: lobbies with a six-character code and no port forwarding, through a Cloudflare Worker
  (`port/matchmaking/worker.js`) and a relay; up to eight places in a lobby (two play, six watch), Quick Match,
  chat. "Host by Address" / "Join by Address" work without the server.
- **What is exchanged**: the players' input only, the per-fighter record the game's own replays keep
  (`{buttons, stickX, stickY}` per input-taking frame). Each machine runs the whole fight itself, so both must
  compute it **bit for bit** the same. The PC port can: the game's arithmetic is its own soft-float code, the PS2's
  results exactly, on any processor. A fight checksum (`BT3_HASH`) compares the two machines as they go.
- **Rollback** (optional, the lobby's owner chooses the frames and the input delay): the state is saved every frame
  and, when the other player's input arrives and differs from what was assumed, the game goes back and runs the
  frames again silently. On a PC a silent frame costs 1 to 4 ms (`netplay_notes.md`, "What a silently re-run frame
  costs"). The 32-bit Linux build has no rollback.
- **Versions**: both players need the same release (0.1.39 does not play against 0.1.38.x).

## What it would take on the Vita

1. **The code base.** Playing against the PC needs the same game code and the same netcode version: the Vita port
   would first have to take in the PC port's 644 commits since 2026-10-06, which also touch what the Vita renderer
   is built on (`port/src/gs/`). Vita against Vita only needs the netcode itself.
2. **The arithmetic.** The Vita port runs the game's floats on the hardware, with the PS2's rules where they differ
   (`__port_fdiv`, `port/vita/README.md`); a self-test at start finds no difference from the PC's exact sums on a
   sample of 1,000 per operation (`vita: float self-test` in the log). A sample is not a proof: between two Vitas
   (the same program) it does not matter, against a PC it could drift a fight apart. The safe way against a PC is
   the PC's soft-float code on the Vita too, about twice the hardware's time per operation (the same self-test: add
   283 against 145 ns).
3. **Rollback.** The Vita's game thread is busy about 10-12 ms of a fight's 33 ms frame; a silent frame there is
   estimated (not measured) at ten times the PC's, so rolling back 2-4 frames would not fit in a frame. Delay-based
   lockstep (the input sent a few frames ahead, as the PC port without rollback) would.
4. **The network.** The Vita has BSD sockets (SceNet); ad-hoc between two Vitas has its own API (SceNetAdhoc). The
   PC port's lobbies use its author's server: using it from the Vita needs the author's agreement and the same
   protocol; a server of our own, or the address-only and ad-hoc ways, need none.

## The ways to go

| Way | What it needs | Size |
|---|---|---|
| Vita against Vita (local network, ad-hoc, or by address) | the PC port's lockstep netcode, without rollback; the same Vita release on both | Medium |
| Vita against PC | first the 644 commits; then the netcode; the soft-float arithmetic or proof that the hardware's matches | Large |
| Only the fixes and features of the 644 commits, no online | a sorted list of them, taken one by one | Variable |

Before any of these: ask the Vita port's maintainer (arielxflores), whose CHANGELOG lists online as not implemented
yet, whether it is being worked on.
