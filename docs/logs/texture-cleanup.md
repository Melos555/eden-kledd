# Texture cleanup (garbage collector) in Eden Kledd

Main log for this thread is in Melos's claude.ai project (`logs/`); this repo copy was started by
a Claude Code session that could not read the project. Merge it there.

## 2026-11-10: how the cleanup settings actually behave in v2 golden (code read, verified)

Read in `TextureCache<P>::RunGarbageCollector` (`src/video_core/texture_cache/texture_cache.h`) at
`936c9238a9` (kledd-prime4-v2-golden = main):

- **"[Kledd] Texture cleanup: keep for (seconds)"** (`kledd_gc_seconds`, Melos's setting): the box
  is hidden since the Nov 3 D preset (`e0ad0fa5e2`), and the code ignores the setting: it uses a
  fixed `5U`. The keep time then depends on texture memory: **5 s** when used >= critical,
  **10 s** when used >= expected, **20 s** otherwise. Earlier builds (e.g. #77) still had the box,
  where Melos could type any value.
- **"[Kledd] Gentle texture cleanup"** (`kledd_gentle_gc`, Melos's): visible, but currently has
  **no effect**: its frame counts are set first and then always overwritten by the seconds-based
  values above (because the keep time is never 0).
- **"[Kledd] GPU BC7 encoding"** (`kledd_gpu_bc7`): hidden and unused since `ba1d61aba1`; the choice
  is now "BC7 on GPU" / "BC7 on CPU" in ASTC Recompression. (The v2 golden commit `cec02ef840`
  set its default to on; that does nothing.)
- **"[Kledd] Resumable texture cleanup (John)"**: only changes how the sweep walks the list, not
  how long textures are kept.

**Interpretation (not verified):** black for a moment after scanning for a while may be textures
outliving the keep time (5-20 s depending on memory) while the scan view hides the room. In
Melos's Nov 10 long-frame logs texture memory sat around the "expected" line (~2.2-2.7 GB vs
2268 MiB), i.e. mostly the **10 s** case, not 5 s as first assumed.

**Next (not done, needs Melos's go):** bring the keep-seconds box back and make the code read it,
and decide whether Gentle cleanup should do anything.

## 2026-11-11: John's resumable cleanup, Melos's verdict "legit good"

- **Melos:** "John's Garbage Collector? Legit good."
- **Recordings (build #136, kledd-prime4-v1-golden-shot-probe 68db0e6; delta off, chunk Normal,
  ASTC recompression 0, shot probe on in some runs):**
  - 11:53, resumable **on**: 38 s, avg 81.6, 1% low 12.7, 8 hitches > 100 ms, worst 354 ms.
  - 11:55, resumable **on**: 61 s, avg 85.6, 1% low 18.9, 4 hitches, worst 214 ms.
  - 12:01, resumable **off**: 100 s, avg 80.0, 1% low 14.6, 12 hitches, worst 328 ms.
- **Interpretation:** per minute, on = ~7.3 hitches (both runs together), off = ~7.2; the best
  single run had it on. Not worse by the numbers, better by feel. Short runs, routes not identical.
- **Note:** v0.3 (main) locks resumable cleanup **off** (`d6b2ccc460`). The lab branch
  (kledd-prime4-v2-golden) has the switch again (default off). If it stays good over longer runs,
  the next release could ship it on.
