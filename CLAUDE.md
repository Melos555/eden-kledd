# Eden Kledd: rules for any Claude working in this repo

This is Melos's fork of the Eden Switch emulator, tuned for Metroid Prime 4: Beyond (1.1.0).
Melos tests in-game; Claude writes the code. Several Claude sessions work on this repo over
time, and they don't share memory. **The logs are the only memory.** A change that isn't
logged forces the next session to reconstruct it from commit messages, blind.

## Before you change anything

1. Find the log for the thread you're working on.
   - Main place: Melos's claude.ai project "Metroid Modding", folder `logs/`
     (e.g. `logs/delta-upload.md` for delta texture upload / black textures,
     `logs/texture-stutter.md`, `logs/mouse-gyro.md`, `logs/scan-fix.md`).
     Use the Projects tool if you have it.
   - If you can't reach the project: ask Melos, or read `docs/logs/` in this repo.
2. Read it fully, plus Melos's latest messages. His in-game observations outrank your
   reading of logs or code.

## After every build or test result: write it down

Add an entry to the thread's log (project `logs/`, or `docs/logs/<thread>.md` in this repo
if the project isn't reachable, and tell Melos where it is). Each entry:

- what changed and why (commit hash, branch, build run / artifact)
- what Melos saw, in his words
- your interpretation, **labeled as interpretation**
- what's verified (in code or logs) vs guessed
- the next step

Never rewrite old entries; add corrections as new ones. A new topic gets a new log file.

## Rules that already bit us

- Never present a guess about which code path does what as fact, and never ship a kit or
  build based on one. Say what each test result would mean.
- Keep changes switchable (a `[Kledd] ... (test)` setting) when testing a theory, so Melos
  can A/B without a new build.
- Check string literals before pushing: a raw line break inside a `tr("...")` string broke
  the Windows build on Nov 10 (use `\n`).
- Daily build branch: `kledd-prime4-v1-golden-texture-switches`. Don't push experiments
  meant for one feature onto it without saying so in the commit message and the log.
- Commits that only touch docs: add `[skip ci]` to the message.
