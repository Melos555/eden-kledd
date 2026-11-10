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

## 2026-11-10: v2 golden locks Melos's best-run settings in code

- **Melos:** "Hardwire my latest settings, except for BC7, leave that as is."
- **Locked in code, switch hidden** (the settings stay defined so old configs load; the code
  ignores them): delta upload **off** (so background refresh, after-door re-reads, GPU handoff and
  refresh speed never run either), resumable cleanup **off**, async unswizzle **GPU / chunk 128**,
  descriptor stride fix **on**, max descriptors **256**, audio hold **on**, cheat write fix **on**.
  Gentle cleanup hidden too (it had no effect, see above). Keep seconds stays the fixed 5.
- **Left changeable:** ASTC recompression / BC7, and the debug logs (DMA readback, long frame log,
  shot probe).
- The F7 summary prints the locked values as "(locked)".
- Not compiled locally; the CI build of this commit is the first check.
