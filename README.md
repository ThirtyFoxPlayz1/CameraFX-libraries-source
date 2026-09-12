# CameraFX (native AML mod for GTA SA Android)

By ThirtyFoxMC. Requires AML (Android Mod Loader) 1.4.0+ and SAUtils.

## ⚠️ Important note on this copy

This source was reconstructed from a long development conversation after
the build environment reset mid-session, right before the conversation
itself was about to time out. Every individual piece in it — every opcode,
every crash fix, every design decision documented in the comments — was
verified earlier against real sources (the Sanny Builder `sa_mobile` opcode
database, `enums.json`, the real SAUtils/AML source, and actual device
crash logs). What was **not** repeated in this final pass is an actual
clean recompile of this exact file, since no NDK toolchain was available
at reconstruction time.

**Before publishing or shipping a build from this copy: do a clean build
and a quick smoke test on-device first.** It should be correct — brace/paren
balance was checked and the design is faithfully transcribed — but "should
compile" isn't the same as "verified to compile," and this file hasn't had
that last check.

## What it does

- **Sprint Zoom** — camera zooms out while holding the Sprint button and
  moving fast on foot.
- **Fall Zoom** — zooms out while falling above a speed threshold.
- **Parachute Zoom** — separate zoom while parachuting.
- **Vehicle Zoom** — continuous, speed-based zoom while driving (car, bike,
  plane, or helicopter) — the faster you go, the more it zooms out, easing
  back smoothly as you slow down. Accelerate can also trigger/sustain it at
  low speed; Brake forces an immediate return.
- **Weapon FOV Effect** — a smaller zoom-in while wielding a shotgun,
  assault rifle, or rifle on foot without sprinting. Sniper is excluded on
  purpose (it has its own scope zoom already).
- Settings live under their own **CameraFX** tab (SAUtils settings menu),
  persist to `configs/CameraFX.ini`, and support 8 languages.

## Structure

```
jni/
  main.cpp          — all mod logic (single file)
  Android.mk
  Application.mk
  mod/
    amlmod.h         — AML mod macros (MYMODCFGNAME, DEFOPCODE, CALLSCM, ...)
    iaml.h           — AML interface
    icfg.h           — AML Config interface (ICFG)
    config.h/.cpp    — Config wrapper class, backs configs/CameraFX.ini
    interface.h      — GetInterface() declaration
    isautils.h       — SAUtils interface (ISAUtils)
    logger.h/.cpp    — AML logger wrapper
    thirdparty/
      stb_sprintf.h  — used internally by logger.cpp
```

## Building

Standard `ndk-build` with the NDK's `armv7a-linux-androideabi24-clang++`
and `aarch64-linux-android24-clang++` toolchains (or `ndk-build` directly,
using the included `Android.mk`/`Application.mk`). Output:
`libCameraFX.so` for `armeabi-v7a` and `arm64-v8a`.

Deploy the built `.so` to the AML `mods/` folder for the target ABI.

## Known open items (as of this snapshot)

- **Camlock** (requested feature: lock camera rotation unless a specific
  widget is held) — not implemented. No camera-input-lock opcode was found
  in the verified opcode database; would need a different mechanism than
  anything else in this mod.
- **Swipe-during-animation bug** (reported: rapid taps during a zoom
  transition can trigger a camera-angle swipe instead of normal
  look-around) — not diagnosed yet, not enough information to pin down a
  cause confidently.
- Vehicle Zoom's speed-threshold settings are tuned relative to the
  verified on-foot `CharSpeed` anchor (6.5), not independently verified for
  vehicles — may need retuning in-app.
