# Rapid fire (Power Beam) and the shot probe

Thread: make MP4 Beyond 1.1.0's Power Beam fire faster while fire is held. Static game patches
live in `%APPDATA%\eden\load\010019A01E2F2000\` (pchtxt, NSO build id 8275FBE1EC5191C2...);
the emulator-side probe lives in `src/core/kledd_shot_probe.cpp`.
More reverse-engineering detail: side-session notes `mp4_rapid_fire_findings.md`.

## 2026-11-10: static patches v1-v5 (no effect on rate)

What changed (all pchtxt, all toggleable add-ons; none in this repo):
- v1 `ShouldRapidFire` (0x1210B24): burst cap removed (0x1210CC4), 0.158 s threshold lowered
  to 0.08/0.05 s (0x1210CC8/0x1210CD0); add-on `ShouldStartGunCharge` -> false.
- v2 `ShouldSnapToIdleWhenShooting` -> true. v3 probes: ShouldRapidFire "only held" / "never".
- v4 GunShooting (0xCB0E94) dt doubled (0xCB0F28). v5 shot-anim rate callback 0x1211E40 forced
  to 1.5/2/3.

What Melos saw: vanilla = hold fire -> 3 shots at ~356 rpm, 4th is a charge shot. With every
variant: still 3 shots at ~356 rpm. "When the game sped up the shots were faster but only when
the game sped up." Eden's log confirmed the pchtxt files were parsed and applied.

Verified: the 0.158 constant (0x13BEF00) is only used by ShouldRapidFire; patches applied.
Interpretation (not verified): the burst and its timing come from the behaviour/animation data
on game time, not from the native predicates patched above.

## 2026-11-10: shot probe, first version 3840435c06 (+ 42cedbd build fix by another session)

What changed: setting `[Kledd] Shot probe (MP4 Beyond)` (`kledd_shot_probe`, off by default, log
only). Called from `GPU::Impl::RendererFrameEndNotify`; scans the heap 16 MB per frame for the
gun FSM component's vtable (main+0x180CE08), then logs its fields every frame to
`log/kledd_shot_probe.txt`.

What Melos saw: turning the probe on freezes the client.

Verified in code: the scan and the sampling used `Memory::ReadBlock`, which calls
`HandleRasterizerDownload` for GPU-cached pages (src/core/memory.cpp, ReadBlockImpl). The heap
holds plenty of GPU-cached buffers, and this runs on the GPU thread.
Interpretation: the GPU thread waited on a download it was supposed to do itself (deadlock),
made worse by 16 MB of copying per frame. Also found: `Write` passed `"\n"` as a 2-char span, so a
NUL byte followed every log line.

## 2026-11-10: shot probe fix (this commit, branch kledd-prime4-v1-golden-shot-probe)

What changed: `ReadBlockUnsafe` everywhere (plain copy, no GPU download); scan budget 2 MB of
mapped memory and at most 1024 block checks (64 MB of range) per frame; unmapped blocks are
skipped without reading; log lines written as one string (no NUL bytes). Still off by default and
log only. Not compiled locally (no toolchain on that machine); untested.

Next step: Melos turns the probe on in a build with this fix, waits for "# scan done", taps fire
once, then holds through a full burst + charge, turns it off and sends the log. What the log would
mean:
- a gun field that resets on each shot and counts up to ~0.17 s -> that's the limiter to patch;
- "scan done: 0 candidate(s)" -> the vtable guess (main+0x180CE08) is wrong;
- still freezes -> the frame-end path itself is the problem; move the probe to its own thread.
