# VCV Doom port review

Reviewed 2026-07-12. Scope: `VcvDoom`, its Rack UI integration, and the
vendored Doom changes that form the thread/audio/video boundary. This is a
static review; it did not run a WAD in Rack.

## Verdict

The original review identified release-blocking data races at the Doom/Rack
boundary. The corresponding safety changes have now been applied: mixer/MIDI
mutation is excluded from Rack's non-blocking reader, control/state exchange
uses atomics, the WAD transition is no longer re-entrant, and engine status,
input overflow, save requests, WAD validation, and MIDI callback work are
hardened. Runtime stress testing in a correctly matched Rack toolchain is
still required before release.

This document preserves the pre-fix findings and remediation rationale below.

## Findings

| Priority | Finding | Impact / evidence | Recommended fix |
|---|---|---|---|
| P0 | Mixer state is concurrently mutated by the Doom thread and Rack's real-time thread without synchronization. | `process()` dereferences `chan.data`, reads/writes `pos` and writes `active` ([`src/VcvDoom.cpp:187`](src/VcvDoom.cpp:187)). At the same time `I_StartSound()` clears the channel, may `realloc()` its buffer, copies data, and republishes it ([`src/doom/i_sound.c:111`](src/doom/i_sound.c:111)); shutdown frees the same buffers ([`src/doom/i_sound.c:47`](src/doom/i_sound.c:47)). `volatile` fields are not atomic and do not make the pointer/length publication safe. A reallocation or shutdown can leave the audio thread reading freed memory. | Redesign the boundary. Have Doom publish fixed-size SFX commands to a lock-free SPSC queue, and let Rack's audio thread exclusively own voices, playback positions and preallocated sample storage. Do not use a mutex or allocation in `process()`. |
| P0 | MIDI iterator/file state is owned by both threads. | Rack advances and restarts `g_active_midi_iter` in `process()` ([`src/VcvDoom.cpp:236`](src/VcvDoom.cpp:236)), while Doom can stop/play a song and free/replace that iterator ([`src/doom/i_sound.c:249`](src/doom/i_sound.c:249)). This is a direct use-after-free race; the many `volatile` music globals do not solve it. | Make one thread the sole iterator owner. Prefer decoding/queueing timestamped MIDI events in the Doom thread and passing immutable events to Rack with a bounded lock-free queue, or transfer ownership only at an acknowledged safe point. |
| P0 | The WAD transition guard is re-entrant and releases its own exclusion early. | `loadWad()` constructs `DoomEngineTransition` then calls `stopDoomEngine()`, which constructs a second guard ([`src/VcvDoom.cpp:393`](src/VcvDoom.cpp:393), [`src/VcvDoom.cpp:81`](src/VcvDoom.cpp:81)). The inner destructor sets `gDoomEngineTransitioning` false before the outer load operation creates the replacement thread. DSP can therefore re-enter during WAD replacement, despite the outer guard still being alive. | Split `stopDoomEngine()` into a no-lock `stopDoomEngineLocked()` helper and an outer wrapper, or make the transition state reference-counted/serialized by a real control-thread mutex. Keep the DSP gate asserted for the entire stop/start transaction. |
| P1 | Nearly all other cross-thread game state is still data-racy. | Audio writes CV values and reads health/frag/engine/music state ([`src/VcvDoom.cpp:142`](src/VcvDoom.cpp:142)); Doom reads/writes these globals declared only `volatile` ([`src/doom/i_video.c:55`](src/doom/i_video.c:55)). UI writes warp/cheat requests while Doom reads and clears them; it also reads `gamemode`/`gamemission` while building menus ([`src/VcvDoomWidget.cpp:523`](src/VcvDoomWidget.cpp:523)). In C/C++, unsynchronized accesses are undefined behavior, not merely stale reads. Frag events can also be lost when both sides store zero/one. | Use atomics for small latest-value controls/status, with explicit acquire/release semantics; use a queue for commands/events that must not be lost. Publish a UI snapshot from the engine instead of reading Doom globals from the UI thread. |
| P1 | The audio callback can do unbounded MIDI work in one sample. | `while (g_music_ticks >= g_next_event_tick)` has no per-call limit ([`src/VcvDoom.cpp:241`](src/VcvDoom.cpp:241)). A dense MIDI track, a long scheduling gap, or a malformed/zero-delta stream can process an arbitrary number of events and scan voices repeatedly in a real-time callback. | Bound events per sample/block and retain backlog, or pre-schedule into a bounded audio-thread queue. Define an overload policy (coalesce/drop with a counter) rather than risking an xrun. |
| P1 | Save/load invokes Doom state directly from the UI thread and blocks the UI. | `triggerExplicitSave()` calls `G_SaveGame()` outside the Doom thread then polls disk for up to 500 ms ([`src/VcvDoom.cpp:560`](src/VcvDoom.cpp:560)). `sendsave`/save globals are non-atomic. Load writes the save then restarts the engine ([`src/VcvDoom.cpp:593`](src/VcvDoom.cpp:593)). | Route save/load through the engine command queue, acknowledge completion asynchronously, and update the UI state on completion. Avoid polling/sleeping on Rack's UI thread. |
| P2 | Input event queue silently corrupts its full/empty state under burst input. | The 64-entry ring advances `eventhead` without checking whether it catches `eventtail` ([`src/doom/d_event.c:37`](src/doom/d_event.c:37)). On a full wrap, head equals tail and the consumer treats the queue as empty; otherwise old unread events are overwritten. Mouse/key bursts can therefore cause lost releases or stuck game input. | Reserve one entry and drop/coalesce explicitly, or use an SPSC bounded queue with overflow accounting. Coalesce mouse-motion events before enqueueing. |
| P2 | Patch save serialization is inefficient and has weak validation. | The save is hex encoded, doubling its size in every patch ([`src/VcvDoom.cpp:522`](src/VcvDoom.cpp:522)). `decodeHex()` accepts invalid nibbles as zero and ignores an odd trailing byte ([`src/VcvDoom.cpp:535`](src/VcvDoom.cpp:535)), then writes the result to disk. | Use base64 or a separate compressed save asset with a hash/version. Reject odd-length or non-hex input and impose a maximum decoded size before allocating/writing. |
| P2 | Video handoff is safe but can stall the UI and does avoidable copying. | Doom holds a pthread mutex while converting all 64,000 pixels ([`src/doom/i_video.c:95`](src/doom/i_video.c:95)); the UI takes the same mutex to copy 256 KiB before a texture update ([`src/VcvDoomWidget.cpp:407`](src/VcvDoomWidget.cpp:407)). The source is converted then copied to a snapshot, then uploaded: roughly 9 MB/s copied at 35 fps, plus conversion and GPU upload. | This is acceptable at current resolution, but use double-buffered frame slots with atomic sequence numbers if UI hitching appears. Keep OpenGL/NanoVG calls exclusively on the UI thread, as currently done. |
| P2 | SFX output has no headroom and can hard-clip. | Up to 16 channels are summed and clipped to +/-1 before the Rack 5 V conversion ([`src/VcvDoom.cpp:217`](src/VcvDoom.cpp:217), [`src/VcvDoom.cpp:229`](src/VcvDoom.cpp:229)). This will create harsh distortion on dense sounds. | Apply a calibrated master gain and a soft limiter, or preserve sufficient headroom. Add tests for peak levels with 16 active channels. |
| P3 | WAD validation checks only the magic word. | Any readable file starting `IWAD` or `PWAD` is accepted ([`src/VcvDoom.cpp:375`](src/VcvDoom.cpp:375)); malformed directory offsets/counts are left to the engine. | Parse and bounds-check the header/directory before launching, and report a precise error. This is robustness rather than a substitute for engine hardening. |

## Latency assessment

* CV game control is sampled by Rack at audio rate but consumed by Doom's
  35 Hz tic loop. Excluding OS scheduling, this is **0–28.6 ms**, averaging
  **14.3 ms**, before the next rendered/game response. That is reasonable for
  Doom control but should be documented; it is not sample-accurate CV.
* A newly created SFX is heard on the next Rack sample after it is published,
  but publication originates at a Doom tic. Practical trigger-to-audio latency
  is consequently dominated by the same 0–28.6 ms game-tic phase. The current
  race makes the actual result non-deterministic.
* The MIDI/CV sequencer is sample-clocked once an iterator is active, which is
  a good design goal. Its unsafe ownership handoff and unbounded event loop
  must be corrected before its timing claims are reliable.
* The code linearly resamples 11 kHz-era Doom samples. At low Rack sample
  rates or 2x pitch it can exceed Nyquist and alias; use a band-limited path or
  constrain playback for low sample-rate engines if audio quality matters.

## Performance and Rack integration notes

* The steady-state 16-channel scan and 16-voice maintenance are modest, but
  they run for every audio sample. The larger concern is real-time safety:
  the callback must never wait, allocate, free, or traverse an unbounded event
  burst. The current design violates this through shared mutable state, even
  though it does not itself call allocation APIs in `process()`.
* The existing `DoomDspAccess` reader counter helps prevent deletion during an
  individual `process()` call. It does not synchronize with the continuously
  running Doom engine, so it cannot protect mixer/MIDI data. Fixing the nested
  transition bug is necessary but not sufficient.
* `I_CopyTargetRGBA()`'s mutex correctly protects the framebuffer. Retain that
  protection or replace it with a proven double-buffer protocol; do not read
  the Doom framebuffer directly from NanoVG.
* Only one module can own the global Doom engine. The secondary-instance UI
  makes this visible, but the global architecture also means patch load order
  changes which module becomes functional. Consider a single service/module
  model or persist the owner decision explicitly.

## Recommended remediation order

1. Replace all direct Doom-thread/audio-thread mixer and MIDI sharing with
   bounded lock-free command/event queues and audio-thread-owned playback
   objects. Add an audio-thread assertion/test that no allocation or lock is
   reachable from `process()`.
2. Make lifecycle transitions single-owner and non-reentrant; then test rapid
   WAD swaps, module deletion, and Rack/plugin unload under AddressSanitizer
   and ThreadSanitizer.
3. Replace remaining `volatile` cross-thread values with atomics or queues;
   publish a coherent UI snapshot.
4. Move save/load to asynchronous engine commands; harden save decoding and
   WAD validation.
5. Add stress tests: repeated song changes while audio is running, dense MIDI,
   16 simultaneous SFX, keyboard/mouse event floods, WAD swapping, and 8–96
   kHz Rack sample rates.

## Verification performed

* Inspected the complete Rack module/UI implementation and the modified Doom
  video, sound, event, game, lifecycle, and timer paths.
* `git diff --check` reported no whitespace errors before this report was
  added.
* `make -j2` compiled the sources but failed during linking because this shell
  combines a Linux linker with a Windows/MSYS Rack SDK/library (`nvgRGBA`, Rack,
  and C++ runtime symbols unresolved, followed by an ld assertion). This is an
  environment/toolchain mismatch, so no runtime Rack/WAD test was possible.
