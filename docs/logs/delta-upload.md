# Delta texture upload / black textures: repo copy of the thread log

**Read this first.** The main log is `logs/delta-upload.md` in Melos's claude.ai project
"Metroid Modding". The Claude Code session that wrote the entries below (Nov 9-10, branch
`kledd-prime4-v1-golden-texture-switches`) could not read that project, so this file was written
*without* having read it. Merge these entries into the project log; where they disagree with it,
the project log and Melos's in-game observations win. Never rewrite entries; add corrections.

Every entry: what changed (commit / build), what Melos saw (his words), interpretation (labeled),
verified vs guessed, next step.

---

## 2026-11-09: FIFO unswizzle (John's 99cdf6d5c3 idea) makes black worse

- **Change:** `be12e454c6` on `kledd-prime4-v1-golden-unswizzle-fifo` (built as #116): removed the
  `std::rotate` in `TickAsyncUnswizzle`, so the front texture finishes before the next.
- **Melos saw:** "I'm getting so much more black poppins than I did before."
- **Interpretation:** with delta on, big delta jobs at the front of the queue make every texture
  behind them wait. Not verified in code paths at run time.
- **Verified:** the rotate is removed in that commit; nothing else changed. John's own branch
  (`John/eden` `Improved-Unswizzle`) has no delta uploads, so FIFO doesn't clog anything there.
- **Next:** FIFO dropped; branch left as a dead end.

## 2026-11-09: build #77 (9031df4, no delta) has no black

- **Melos saw:** #77 (`mousegyro-rec 9031df4`) at async unswizzle GPU + chunk 128 (later also 256):
  Samus, sky, tunnel, altar and the boss all fine. Screenshot of #77's per-game settings showed
  Async Unswizzle Mode **GPU**, chunk Normal (128) set explicitly.
- **Verified:** `9031df4` predates all delta work (delta started Nov 1 on the prime4 branches).
- **Interpretation:** the async GPU queue alone doesn't cause black; something golden adds does.

## 2026-11-09: texture-switches build, delta on vs off (same settings otherwise)

- **Change:** `c5d32d9` (#119): delta upload and chunk size switchable again (were locked by the
  Nov 3 D preset). Settings in both runs: chunk Normal, mode GPU (locked), max descriptors 257,
  stride fix on, resumable cleanup on, ASTC recompression 3, GPU BC7 on.
- **Run A, delta on** (14:13-14:14, 76 s): avg 87.0 / median 97.7 FPS, 8 hitches >100 ms, worst
  498 ms. Melos: arm and Samus dark, boss "pitch dark".
- **Run B, delta off** (14:18-14:19, 89 s): avg 85.6 / median 102.7, 15 hitches, worst 384 ms.
  Melos: "Textures look fine ... Finally is in color." Possibly slowed by another session on the PC.
- **Verified:** delta off = no black in Melos's run; delta on = black.
- **Next:** find why delta causes black while keeping its smoothness.

## 2026-11-09: queue-eviction theory, wrong

- **Change:** `82033d2` (#123): a delta that finds the 18-slot unswizzle queue full no longer
  evicts another job, and evictions prefer non-delta jobs.
- **Melos saw:** still black (screenshot: arm cannon and pillar black, delta on).
- **Verified (Eden log of that session):** only **one** "async delta image" job and six watched
  pools (one 346 MiB, five 173 MiB, 63 z groups each). The queue never filled, so the fix couldn't
  matter. The pools were "declined as accelerated upload before (delta test build)", i.e. with
  delta on they skip the regular sparse upload (Nov 4 change `c22314b`).
- **Interpretation:** the delta write watch misses some writes, and those z groups stay stale/black.

## 2026-11-10: background refresh (fc77af1, #124/#125): black comes back on its own

- **Change:** each frame one z group of one watched pool is re-read from guest memory, round-robin
  ("[Kledd] Delta: background refresh (test)", default on).
- **Melos saw:** "The pop-in is there, but it gets fixed after a while ... the boss is dark and
  then goes bright ... Takes a little while." Save station flowed well.
- **Interpretation:** confirms stale groups from missed writes, refreshed by the lap.

## 2026-11-10: chunk 256 vs 128, delta on + refresh on (#127, 1d3c2ea)

- **256** (15:05, 44 s): avg 62.9, P5 34.7, 1% low 11.8, 6 hitches, worst 369 ms.
- **128** (15:15, 61 s): avg 79.3, P5 51.6, 1% low 17.1, 9 hitches, worst 232 ms.
- **Melos:** black "exactly the same" with 256; keeps 128 for performance. Resumable cleanup (John)
  off in both, and Melos prefers it off.
- **Interpretation:** John's chunk 256 (`113a0bf769` on his `Improved-Unswizzle` branch) doesn't help
  this build's black and costs slow frames. Route and boot not identical, so FPS numbers are
  indicative.

## 2026-11-10: after-door re-reads of remapped groups (6ad0cbf, in #129 df1e6af): didn't fix it

- **Change:** each remap of a watched pool schedules re-reads of exactly those z groups 0.25 s and
  1 s later (shares the background refresh switch).
- **Melos saw:** "The textures always go black whenever the door closes or opens."
- **Verified (Eden log, #129 session):** the re-reads fired ("12 of them after doors"), but remaps
  are rare: of ~9,960 delta uploads logged with a remap count, 9,951 had "0 remap events".
- **Interpretation:** the black at doors is mostly not in remapped groups, so this fix aims at the
  wrong cause. (The later `7b89b7d` refresh-speed commit by another session reads the same log
  differently: after-door re-reads 116-138 groups per 10 s, lap ~4-5 s matching a ~5 s black.)

## 2026-11-10: GPU handoff counts (for the 48ca548 handoff test)

- **Verified (counts in Melos's Eden logs):**
  - #129 session (`df1e6af`): **209** "async delta image" jobs (GPU handoff) vs **64,130**
    on-the-spot "delta upload image" lines.
  - #127 session (`1d3c2ea`): **100** vs **98,293**.
  - The handoff jobs are almost all **3-6 of 63 z groups of the 346 MiB (fmt 31) or 173 MiB
    (fmt 23) pools**, many logged with remap events (doors).
- **Interpretation:** handoffs are rare but cluster at doors, which fits them as the suspect behind
  door black and fits Lanner's "CPU mode fixes it" (CPU mode never hands deltas to the GPU queue).
  Not verified that the handed-off groups are the black ones.
- **Next:** #135 (`48ca548`), delta on, refresh on, chunk 128, same doors with F7 at each:
  "[Kledd] Delta: GPU handoff (test)" **on** vs **off** (restart between). Watch for black and for
  hitches. Handoff off and no black = the GPU handoff path is the cause; still black = not it.

## 2026-11-10: not delta, for context: run-to-run performance

- **Melos:** some boots are "magical" (save station doesn't lag), others aren't, same settings.
- **Verified:** long frame log, good vs bad session: 75 vs 80 long frames, 68 vs 74 of them with
  almost no texture/shader work, texture memory avg 1574 vs 1639 MiB. nvidia-smi during a good run:
  GPU load avg 39% (max 53%), ~1870 MHz, VRAM < 3 of 6 GB, no throttling. typeperf: CPU at ~99% of
  2.9 GHz base (Melos keeps boost off on purpose, for fan noise), Eden ~4.5 cores, busiest core ~71%.
- **Interpretation:** neither GPU nor CPU is maxed in good runs; the bad-run difference is
  stretches of steady low FPS (~33) that the long frame log (> 50 ms only) can't see. Cause open.

## 2026-11-10 20:27: #135 (48ca548), delta off: cleanest run so far

- **Settings (F7 summary):** delta upload off, GPU handoff off, refresh off (speed 4), resumable
  cleanup off, async unswizzle GPU.
- **Run:** 23.3 s, avg 78.0 / median 81.6 FPS, 1% low 44.8, 0.1% low 35.1, worst frame 28 ms,
  0 frames over 50 ms.
- **Melos:** "I have 90 FPS where the [game] crackled to 30 FPS" and "fuck the delta texture" -
  wants delta off.
- **Verified:** the recording above. Short stretch, one run.
- **Interpretation:** at this spot, delta off has no hitches at all, while earlier delta-on runs had
  200-500 ms freezes; it's one 23 s run, so not proven over a full route.
- **Next:** Melos's call: delta off as the daily setup. The handoff on/off test is not done.
