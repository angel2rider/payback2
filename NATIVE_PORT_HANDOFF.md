# Payback 2 — Native Port Handoff (Superseded by Web Port Plan)

> [!NOTE]
> **Direction Change:** The standalone macOS porting track described below has been **superseded by the Web Build plan (WebAssembly + WebGPU / WebGL2)** documented in [README.md](file:///Users/thegt/Downloads/payback/README.md).
> All reverse-engineering discoveries, JNI mappings, BSS/arena structures, and live-oracle GOT resolutions documented in this file remain the authoritative foundation for the WebAssembly shims. The active MuMu emulator on `127.0.0.1:16384` continues to serve as the live ground-truth oracle.

**Objective (Historical):** run `libpayback2.so`'s own ARM64 code natively on macOS (Apple Silicon),
against a re-implemented Android layer.

**Status:** engine initialization now completes and returns a valid handle.
`android_app_entry` starts and faults at a single unresolved call. Asset loading not yet
reached.

**Workspace:** `/var/folders/gf/1tjhg30n08nbbz5g7sz9gsyw0000gn/T/opencode/payback/`
(also mirrored notes at `/Users/thegt/Downloads/payback/PHASE1_NOTES.md`)

---

## 1. Artifact

| | |
|---|---|
| APK bundle | `net.apex_designs.payback2_2.106.16-178_..._apkmirror.com.apkm` |
| Package | `net.apex_designs.payback2`, versionCode 178, minSdk 23, targetSdk 35 |
| Engine | `lib/arm64-v8a/libpayback2.so`, 38,570,288 bytes |
| `.text` | 2.70 MB of actual code, **but see §10 — the first 51 KB is encrypted on disk** |
| `.bss` | 34.6 MB asset scratch arena |
| Packing | ⚠️ **Corrected in §10.** `.text` `0xE45C0..0xF0D70` (51 KB, incl. `android_main`) is high-entropy ciphertext on disk. The rest of `.text` is plain ARM64. See §10. |
| Symbols | `.symtab` stripped; `.eh_frame` intact → 4,802 function ranges recovered |

**Do not** pursue: Mac.apk (native translation shim). It installs and translates fine
but dies in `com.pairip.VMRunner.openBaseApk()` — all three APK-resolution strategies
are unavailable under Mac.apk's lazy per-class DEX loading. Fully diagnosed in
`PHASE1_NOTES.md`. The emulator route was used briefly to dump runtime data, then
abandoned per user direction; it is **not** the deliverable.

---

## 2. Harness files

| File | Role |
|---|---|
| `emu_run.py` | **Main driver.** Entry point. Run: `./.venv/bin/python emu_run.py [entry] [maxinsns]` |
| `loader.py` | ELF load, relocations, import wiring, GOT sweep, stub builder, `SWEEP` table lives in `emu_run.py` |
| `jni.py` | Fake JNIEnv function table + JavaVM object, canonical JNI names |
| `elf64.py` | ELF64 reader, `.eh_frame` FDE recovery, `read_vaddr`, `section`, `sec_bytes` |
| `native/lib/arm64-v8a/libpayback2.so` | Extracted engine |
| `assets_out/` | 854 extracted assets |
| `decoded/` | 86 decoded `.pak`/`.dat` (62 MB), raw LZMA |
| `ghidra_proj/payback2.rep` | Fully analyzed Ghidra project (AARCH64, base 0x100000) |

Python env: `./.venv/bin/python` (unicorn 2.1.4, capstone 5.0.7, pyghidra 3.1.0).

---

## 3. Current verified state

```
$ ./.venv/bin/python emu_run.py 0x31c908 20000000
[+] SO @0x10000000 | 2001 imports wired | 3891 relocations
[+] returned normally PC raw=0xcead0000 after 1041 insns
[+] initializeNativeCode -> Game handle = 0x30100000
[+] android_app* = 0x30100260 ; entering android_app_entry @ raw 0x320334
[!] android_app_entry: Unhandled CPU exception (UC_ERR_EXCEPTION) after 1173 insns
    PC raw=0xecf84
```

Progress markers across this effort (all measured, not estimated):

| Metric | Start | Now |
|---|---|---|
| `__android_log_assert` failures | 33 | **0** |
| JNI calls reaching the engine | 2 | 42 |
| Native methods mapped | unknown | **22** |
| `initializeNativeCode` | spins 20M insns | **completes in 1041** |
| Returned handle | NULL (`0x0`) | **non-NULL** |
| Distinct imports reached | 3 | 23 |
| `AAssetManager_open` calls | 0 | **0 (still)** |

---

## 4. Architecture that is now understood

### 4.1 `struct android_app*` (0x260 bytes)

Allocated first thing by `initializeNativeCode` via `malloc(0x260)`. Offsets confirmed
by disassembly at the spin site:

| Offset | Field |
|---|---|
| `+0x140` | readiness/running flag (int32) — the loop's exit condition |
| `+0x28` | end of a region compared against the stack canary |
| — | `&app->mutex` and `&app->cond` are passed as separate pointers (x23/x24), not at fixed offsets |

### 4.2 The spin loop (was the main blocker)

```
0x31f498  mov x0, x24              ; &app->cond
0x31f49c  mov x1, x23              ; &app->mutex
0x31f4a0  bl  #0x378bb0            ; GOT 0x38fb80 = pthread_cond_wait
0x31f4a4  ldr w8, [x20, #0x140]    ; app->running
0x31f4a8  cbz w8, #0x31f498        ; spin until non-zero
0x31f4ac  mov x0, x23
0x31f4b0  bl  #0x3787d0            ; pthread_mutex_unlock, then returns
```

**`x20` is a register, not `x0`.** The first implementation wrote to `x0 + 0x140` and
both missed the base and regressed the handle to NULL. Correct implementation is
`_sv_condwait` in `emu_run.py`: read `UC_ARM64_REG_X20`, write a 32-bit `1` to
`x20 + 0x140`, return 0.

### 4.3 Native lifecycle — all 22, recovered from `RegisterNatives`

| native | signature | raw |
|---|---|---|
| initializeNativeCode | `(String;String;String;AssetManager;[BConfiguration;)J` | 0x31c908 |
| getDlError | `()Ljava/lang/String;` | 0x31cf38 |
| terminateNativeCode | `(J)V` | 0x31d078 |
| onStartNative | `(J)V` | 0x31d0a8 |
| onResumeNative | `(J)V` | 0x31d0c0 |
| onSaveInstanceStateNative | `(J)[B` | 0x31d0d8 |
| onPauseNative | `(J)V` | 0x31d14c |
| onStopNative | `(J)V` | 0x31d164 |
| onConfigurationChangedNative | `(JLConfiguration;)V` | 0x31d17c |
| onTrimMemoryNative | `(JI)V` | 0x31d1c0 |
| onWindowFocusChangedNative | `(JZ)V` | 0x31d1dc |
| onSurfaceCreatedNative | `(JLSurface;)V` | 0x31d1fc |
| onSurfaceChangedNative | `(JLSurface;III)V` | 0x31d264 |
| onSurfaceRedrawNeededNative | `(JLSurface;)V` | 0x31d394 |
| onSurfaceDestroyedNative | `(J)V` | 0x31d3b4 |
| onTouchEventNative | `(JLMotionEvent;)Z` | 0x31d400 |
| onKeyDownNative | `(JLKeyEvent;)Z` | 0x31d458 |
| onKeyUpNative | `(JLKeyEvent;)Z` | 0x31d4b0 |
| onTextInputEventNative | `(JLgametextinput/State;)V` | 0x31d508 |
| onWindowInsetsChangedNative | `(J)V` | 0x31d51c |
| setInputConnectionNative | `(JLInputConnection;)V` | 0x31d6ac |
| onContentRectChangedNative | `(JIIII)V` | 0x31d6b8 |

`0x320334` = **`android_app_entry(struct android_app*)`** — invoked from the driver once
`initializeNativeCode` returns.

---

## 5. The 120–122 unmapped GOT slots (important, partly resolved)

`.got.plt` = `0x38f8f8` .. `0x391fe8`, **1246 slots**. Relocation census:

```
.rela.dyn  R_AARCH64_RELATIVE  (1027)  3891
.rela.dyn  R_AARCH64_ABS64     (257)   1511
.rela.plt  R_AARCH64_JUMP_SLOT (1026)  1124
.rela.dyn  R_AARCH64_GLOB_DAT  (1025)   169
```

- **No `R_AARCH64_IRELATIVE` (1032) entries exist.** The ifunc theory is dead — do not
  retry it. (An ifunc resolver pass was added to `apply_relocations`; it correctly finds
  nothing and is harmless.)
- No `.relr.dyn`, no `DT_RELRSZ`, no `DT_ANDROID_RELA`.
- `.rela.dyn` → **0 entries** target `.got.plt`; `.rela.plt` → 1123 do.
- `.plt` has 1245 × 16B slots vs 1124 relocations → **~120 extra stubs**, all pointing at
  GOT slots with **no relocation of any type and no name in `.dynsym`**.
- **PLT stub ordinal ≠ `.rela.plt` ordinal.** Verified by decoding all 1241 stubs:
  ```
  GOT 0x390d08 <- .plt ordinal 640 | rela[640]: off=0x390f38  egbn_mul_low_normal
  GOT 0x391c68 <- .plt ordinal 1132| rela[1132]:off=0x391c70  EGBN_div
  ```
  Both mismatch. Ordinals cannot be used to name a stub.
- These stubs exist for functions **defined in this `.so`** but called through the PLT for
  interposition. libgmp is statically linked (no gmp in `DT_NEEDED`; 103 GMP dynsyms all
  defined locally). Only `strlen` and `memchr` are imported from libc — `malloc`, `free`,
  `memcpy`, `memset`, `memmove`, `memcmp` are **absent from `.dynsym` entirely**.

**Critical insight:** these stubs are only *reached* because an earlier stub returned 0
where the engine expected a real value, dropping it into a fallback path. They are a
**symptom, not the disease**. Stubbing them as no-ops silently broke libc memory
primitives, which cascaded into everything.

### Implemented `SWEEP` table (`emu_run.py`, keyed by GOT offset)

| GOT | Inferred | Behaviour |
|---|---|---|
| `0x38f950` | `malloc` | `bump_alloc(n)` — **this was the root blocker** |
| `0x38f940` | `free` | 0 |
| `0x38f988`, `0x38f990`, `0x391bc8`, `0x391c70` | `memset`/`memcpy` | real copy |
| `0x391c68` | `memcpy` | real copy |
| `0x390d08` | guard | 0 |
| `0x38fb80` | `pthread_cond_wait` | `_sv_condwait` — sets `x20+0x140` |
| `0x38fb78`, `0x38fb88` | libm hole (`sincosf`..`sinf`) | pass arg through |

Neighbourhood-based inference is a heuristic and **has been wrong twice** — verify each
new swept slot by argument signature before trusting it.

---

## 6. JNI implementation notes

- The engine's JNIEnv table is offset **+2 slots (0x10 bytes)** from canonical.
  `JniHost.name_of()` uses `_CANON[idx - 4]`. Confirmed empirically: slot 6 = `FindClass`,
  33 = `GetMethodID`, 21 = `NewGlobalRef`, 23 = `DeleteLocalRef`, 15 = `ExceptionOccurred`
  — all consistent with canonical JNI shifted +2.
- `jni.py` carries the full canonical `_CANON` list. The original hand-guessed
  `JNI_NAMES` dict had **no `0x20` entry**, so `FindClass` was unrecognised and every
  name-based dispatch silently fell through to `return 0`. That was the cause of all 33
  asserts.
- Return-value classification (in `emu_run.py`) — getting this wrong costs asserts:
  - explicit void set (`Register*`, `Unregister*`, `Release*`, `Delete*`, `Monitor*`,
    `Exception*`, `Ensure*`) → 0
  - `FindClass`, `GetMethodID`, `GetStaticMethodID`, `GetFieldID`, `GetStaticFieldID` → handle
  - names starting `Call`/`New`, plus object/class getters → handle
  - everything else → 0
- **Every synthetic handle gets its own 4 KiB page** from a 2 MiB pool. Registered
  natives deref their jobject; an 8-byte bump-aliased tag faults.
- A `JavaVM*` with a real `JNIInvokeInterface` table is built at `0x61000000` so `GetEnv`
  works if the SDK calls it (slot 7).
- `RegisterNatives` is detected by signature (unknown slot + `x3` a small int) and its
  `JNINativeMethod[]` is dumped — that is how the 22 natives were recovered.

---

## 7. Gotchas that each cost real time

1. **Capstone AArch64 operand parse:** the immediate is
   `op_str.split('#')[1].strip().rstrip(']')`. There is **no comma after `#`**. Getting
   this wrong raises `IndexError` and silently yields zero decoded stubs.
2. **`ADRP` immediates come back page-resolved (absolute)** from capstone — do not add the
   instruction address again.
3. **`struct.unpack_from('<Q', ...)` returns a 1-tuple.** Indexing `[1]` raises.
4. **ELF64 header:** `e_shnum` is at offset **0x3C** (not 0x38); `e_shentsize` at 0x3A;
   `e_shstrndx` at 0x3E.
5. **`ElfImage.section()` offsets disagree with raw section-header parsing.** Raw parsing
   finds the `.rela.plt` entries; the `ElfImage` route reports none. Pick one and stay
   consistent — this cost a whole wrong conclusion.
6. **Stub handlers must return `int`.** Dispatch does `r & 0xFFFFFFFFFFFFFFFF`; returning a
   Python `float` raises `TypeError`, is swallowed by a bare `except`, and falls through
   to `return 0` — presenting as "the fix didn't work".
7. **Don't infer `calloc` from arg1.** Extra argument registers are stale. A naive
   `n *= arg(1)` tried to allocate ~10^18 bytes and threw.
8. **`__android_log_assert` was never surfaced**, which hid the entire failure cascade.
   It is now decoded.
9. **`__android_log_print` read `lvl = arg(2)`** (the fmt pointer) instead of `arg(0)`.
10. **The instruction counter used a `.text` range** but the engine executes past
    `.text`'s end, so it always read 0 and produced no backtrace. Now a global hook plus
    a sampled PC histogram (`pchist`) — the histogram is what exposed the spin loop.

---

## 8. Exactly what to do next

### Immediate — resolve the current fault

`android_app_entry` runs 1173 insns then dies:
```
0x320434  mov x0, x19
0x320438  bl  #0x100ecf84     ; target = img.base + 0xecf84
0xecf84   (?)                  ; UC_ERR_EXCEPTION
```

1. Disassemble the bytes at raw `0xecf84` and the region `0x31f420..0x31f460` to see the
   real call sequence. `0xecf84` is *inside* the `.text` address range (`.text` ≈
   `0..0x290000`) but the bytes do not decode. Given the notes that data tables are
   interleaved into `.text` up to `0xe45c0`, and `0xecf84` is just past that, **verify
   whether `0x320438` is really a direct `BL`** or a computed/indirect call being
   mis-decoded. Re-dump with `img.e.read_vaddr(0x320438, 4)` and decode manually.
2. If `0xecf84` is genuinely data, find the *real* target: check whether the preceding
   instructions set up an indirect branch (`ldr xN,[...]; blr xN`) and whether a swept
   stub returned a bad function pointer.
3. If it is a legitimate call, add the target to `SWEEP` with correct semantics.

### Then — reach asset loading

4. Keep calling `android_app_entry` after each fix until `astats["open"] > 0`. The APK
   asset VFS is already implemented in `loader.py::install_asset_api`
   (`AAssetManager_fromJava` / `AAssetManager_open` / `AAsset_read` / `AAsset_getLength` /
   `AAsset_close` are all named and wired) — **no work needed there**; it is purely
   unreachable code right now.
5. Once assets open, decode on the fly from the recorded path strings (note
   `.rodata` already contains `xtures/Opaque_SpecNorm_0.amct`).
6. Then drive the lifecycle natives on the returned handle:
   `onStartNative` → `onResumeNative` → `onSurfaceCreatedNative` →
   `onWindowFocusChangedNative(1)`, and pump `ALooper_pollOnce` + `eglSwapBuffers`.
7. Only after that: render. The engine is a custom GLES2 renderer; a real GL/Metal
   backend is required. Stubbed `gl*`/`egl*` imports currently return 0.

### Diagnostic tooling already in place

- `trace[]` — every import + JNI call with args, return, and caller LR, gated until the
  first entry to `0x374098`. Set `SPIN[0] = 0` to re-gate on a different PC.
- `pchist` — sampled PC histogram, printed as "hottest PCs". **This is the fastest way to
  find the current loop.**
- `imports{}` counter and `registerNatives` table dump, both printed at the end.
- `on_invalid` prints registers x0–x3, x8, x16, x17, x19–x21, LR and the last 14
  instructions.

---

## 9. What is verified vs. inferred

**Verified (measured):** all metrics in §3; the 22-native table; the +2 JNI slot offset;
the relocation census and absence of IRELATIVE; the `.plt` ordinal mismatch; the
`android_app*` spin-loop architecture; that `x20` (not `x0`) holds `android_app*`;
that `0x320334` is `android_app_entry`.

**Inferred / not yet proven:** the `SWEEP` symbol names (neighbourhood-based — two were
wrong); that `0x320438` should branch to real code rather than a data table; the exact
semantics required of `0x390d08` and the other unused swept slots.

**Known dead ends — do not repeat:** Mac.apk; hooking `AAsset_*` via Frida on the
emulator (the engine never calls `AAssetManager_open`); chasing the 120 unmapped GOT
slots as the root cause (they are a symptom); `.relr.dyn`/`DT_ANDROID_RELA` (absent);
IRELATIVE ifunc resolution (no such relocations); memory-dump-and-carve for textures
(textures go straight to the GL driver and never exist in CPU-visible memory).

---

## 10. THE BIG ONE: 51 KB of `.text` is encrypted on disk

This supersedes the earlier "Packing: None" claim and re-frames the whole project.

### Evidence

`android_app_entry` (raw `0x320334`) ends with:

```
0x320434  mov x0, x19            ; x19 = struct android_app*
0x320438  bl  #0xecf84           ; android_main(app)
```

That target is real — 1381 `bl`/`b` instructions across `.text` branch into
`0xE45C0..0xF0D70` — **but the bytes there do not disassemble.**

Entropy scan of `.text` (page-aligned runs, `H > 7.6` = ciphertext):

```
0xe45c0..0xf15c0  ENCRYPTED  H=7.96   (52 KB)   <- exactly the head of .text
0xf15c0..0x3785c0 plain      H=6.51  (2588 KB)
```

Fine-grained: plaintext resumes at **`0xF0D70`** (a hex-nibble encoder + a text
parser both decode cleanly from there).

Cross-checks that prove it is *encrypted*, not corrupt or mis-mapped:

| Test | Result |
|---|---|
| Capstone decode at `0xECF84` | 0 instructions |
| Decodable 4-byte words in the block | ~30–40 % (chance rate for random data) |
| All 256 single-byte XOR keys | best 11/64 — no better than chance |
| Self-XOR lag scan (period detection) | flat, no repeating keystream |
| `.rela.dyn` entries targeting `.got.plt` | 0 (so not a relocation gap) |

### It is restored *externally*, not by the library's own code

- **No `adrp` anywhere in the whole file points at the block's pages** (verified with a
  proper manual linear sweep — *not* capstone `skipdata`, which desyncs on ciphertext
  and silently yields garbage; that bug cost a wrong "NONE" conclusion once).
- Region A (`0x16000..0x18e6c`) is **not** encrypted — it is the `.gnu.hash` chain
  array. Region B (`0x74000..0x7c000`) is ordinary high-entropy `.rodata`. Only
  region C is anomalous.
- The APK ships **`libpairipcore.so`** (525 KB, plaintext) plus a PAIRIP VM bytecode
  program (`StartupLauncher.startupProgramName = "YGSrGWjwRJ6YVRIE"`, inside one of the
  26 `\0IAP` packs). That is the restorer.
- `libpglarmor.so` / `libnms.so` / `libtt_ugen_layout.so` are ByteDance ad-SDK
  libraries, **not** the protector (all plaintext, H≈4–6).

### How the plaintext was obtained — `find_and_dump.py`

The AVD is **arm64-v8a**, so the real arm64 library runs. Two things make the capture
work:

1. `extractNativeLibs=false`, so the library is **mmap'd straight out of
   `split_config.arm64_v8a.apk`** — `/proc/<pid>/maps` shows the *APK* path and the
   library's **zip data offset** (`0x118000`), never `libpayback2.so`. Grepping maps for
   the library name finds nothing. Map zip-data-offset → matching maps file offset → that
   mapping starts at ELF vaddr 0.
2. Read the block out of `/proc/<pid>/mem` **as root**. Do **not** use Frida: spawning
   under Frida makes PAIRIP's anti-debug kill the process immediately after `resume()`.
   (Frida *attach* to a running process still times out, as noted in §7.)

```bash
adb root && adb forward tcp:27042 tcp:27042
adb shell monkey -p net.apex_designs.payback2 -c android.intent.category.LAUNCHER 1
./.venv/bin/python find_and_dump.py     # -> runtime_dump/region_raw.bin
```

**Prerequisite that cost real time:** the app gets stuck on the intro video and then on
an **AppLovin interstitial** (the emulator has no network, so the ad SDK hangs and
ignores BACK). Kill ad networking with a dead proxy and it proceeds:

```bash
adb shell settings put global http_proxy 127.0.0.1:1
```

Only then does `PaybackActivity` → `System.loadLibrary("payback2")` run. Note the
**main menu is Java/WebView-rendered** — the native lib is not mapped while it is up, so
"no `libpayback2.so` in maps" at the menu is expected, not a bug.

### The splice is self-validating

`runtime_dump/region_raw.bin` was spliced over the shipped file → `libpayback2.patched.so`:

```
differing bytes: 50883   first=0xe45c0   last=0xf0d6f
differences in the known-plaintext tail 0xf0d70..0xf45c0: 0
```

**50 883 bytes differ, spanning precisely the ciphertext span, with ZERO differences in
the adjacent bytes that were already plaintext on disk.** That independently confirms
both the module base and the splice. Verified decoding afterwards:

```
0xecf84 -> sub sp, sp, #0xf0 ; stp x29,x30,[sp,#0x90] ; stp x28,x27,[sp,#0xa0]
0xe45c0 -> bti c ; adrp x0,#0x381000 ; add x0,x0,#0x480
0xe45d8 -> bti c ; b #0x374098
0xe45c0/0xe45d8 are the two .fini_array entries (RELATIVE addends 0x38d850/0x38d858)
```

`loader.py::Config.SO` now defaults to `libpayback2.patched.so` (override `$PAYBACK_SO`).

Alternative for anyone re-deriving this: run each `\0IAP` bytecode program under the
PAIRIP VM. Much harder; the memory capture already gives a byte-exact answer.

---

## 11. Session 5–6 state: the engine boots end-to-end in the harness

**TL;DR:** the harness now takes the engine from `dlopen` all the way to a live,
idle game thread with **zero faults**:

```
ctors (9/9)  →  JNI_OnLoad (returns 0x10006)  →  initializeNativeCode (Game=0x30102c70)
  →  lifecycle natives (onStart … onSurfaceChanged)  →  android_main ALooper pump
  →  EGL context (eglGetDisplay/Initialize/ChooseConfig/CreateWindowSurface/
     CreateContext/MakeCurrent)  →  engine JNI bootstrap (FindClass/NewGlobalRef/
     GetMethodID/CallStatic* + field IDs)  →  thread2 blocked in ALooper_pollOnce(-1)
```

Remaining unreached in the harness: `AAssetManager_open` (0 calls) and all `gl*`
(0 calls) — both need *real* implementations, so they are now native-port work,
not harness work.

### 11.1 Threading architecture (fully mapped this session)

`initializeNativeCode 0x31c908` is the Game constructor (F1). It runs on the UI thread:

```
F1 (0x31c908)
  operator new(0x260)                    → Game object
  ALooper_forThread / ALooper_acquire    → Game+0x148
  pipe() + fcntl(O_NONBLOCK) x2          → Game+0x140/+0x144  (Game cmd pipe)
  ALooper_addFd(uiLooper, Game.pipe, ident=0, cb=0x31dc38, data=Game)
  jni[219](env, Game+8)                  ← some ref-typing call, returns 0 normally
  NewGlobalRef(activity jobject)         → Game holds the activity ref
  FindClass/IsVirtualThread/NewDirectByteBuffer x3, AAssetManager_fromJava  → Game+0x10 area
  F2 (0x31d8xx): CallStaticShortMethodV x14 + jni[228]  (Java bridge method IDs)
  F3 (0x31f36c): app-init helper
       malloc(0x178)                     → android_app* (app)
       pthread_mutex_init(app+0xc0); pthread_cond_init(app+0xe8)
       pipe()                            → app+0x118/+0x11c  (app cmd pipe)
       pthread_create(0x320334, app)     → game thread
       pthread_mutex_lock(app+0xc0)
       loop: pthread_cond_wait(app+0xe8, app+0xc0) while !app->running(+0x140)
       pthread_mutex_unlock; Game+0x38 = app; ret
  ...continues Game JNI setup (asset manager, window callbacks)...
  returns Game as jlong

game thread: android_app_entry 0x320334 → android_main 0xecf84
  builds its looper (0x60300010), registers app pipe fd, sets app->running=1,
  broadcasts app->cond, then ALooper_pollOnce(-1) forever:
    event  → source->process(app, source)   (app glue: APP_CMD_* + input)
    ret<0  → 0xea384(app)  = input pump: android_app_swap_input_buffers + per-event
             dispatch (this is what runs when the looper is woken w/o an fd event)
```

### 11.2 Scheduler bug that cost this session (fixed)

Symptom: after the game thread broadcast, MAIN resumed at the *entry* of F1
(0x31c908) instead of inside F3 (0x31f4a4), with `x0=0` → `x21=0` → fault at
`0x31cb54 ldr x8,[x21]`, and the engine started constructing a *second* Game.

Cause: `Sched._run_thread` set `t.regs[PC]` when a blocked thread was snapshotted,
but never updated the `Thread.pc` **attribute** that is passed to
`uc.emu_start(t.pc, …)` on the next resume. The resumed thread therefore restarted
at its stale entry PC with the *resumed* register state.

Fix (both block branches in `_run_thread`):

```python
t.snapshot(uc)
t.regs[PC] = pend["pc"]
t.pc = pend["pc"]        # <-- resume address for emu_start()
t.regs[GP_REGS[0]] = pend["ret"] & MASK
```

Lesson for the native port: there is no such bug class when the engine's threads
are real host threads — one more reason to skip the cooperative model entirely.

### 11.3 The engine's JNI anchors — and why `[0x3b2120]` must be seeded

The engine caches three JNI anchors in module globals:

| global | meaning | who sets it in the real app |
|---|---|---|
| `0x3b2118` | `JavaVM*` | `JNI_OnLoad` (0xecee8) |
| `0x3b2110` | activity `jclass` (global ref) | `JNI_OnLoad` (FindClass+NewGlobalRef) |
| `0x3b2120` | **cached `JNIEnv*`** | PairIP runtime (not in module code!) |

`0x3b2120` is read all over the engine as `ldr x0,[xN,#0x120]` followed by
`ldr x8,[x0]; ldr x8,[x8,#0x108/0x130]; blr x8` — i.e. canonical JNI calls
(`env->GetMethodID`, `env->CallBooleanMethodV`, …). The live oracle dump shows it
holding an ART `JNIEnv*` (`0xb400007d…`); no writer exists anywhere in the module's
`.text` (verified by an ADRP-window scanner over all executable segments), so the
protection runtime plants it. The first consumer is the `android_main` input pump
(0xecde8); with it left 0 the game thread dies at `0xe6b9c ldr x8,[x0]`.

Harness fix (after `JNI_OnLoad`, before `initializeNativeCode`):

```python
uc.mem_write(img.base + 0x3B2120, pack("<Q", jni.jni_env))          # our env cell
uc.mem_write(img.base + 0x3B2208, pack("<Q", jni.new_handle("id"))) # cached jmethodID
```

`0x3b2208` is a cached `jmethodID` passed straight to `CallBooleanMethodV` by the
input pump; a zero ID is fine for the harness (identity is ignored) but must be a
real ID in the port.

Native-port consequence: the port must hand `initializeNativeCode` a **real
JavaVM/JNIEnv pair whose env really works for GetMethodID + Call*MethodV**, and seed
`0x3b2120` with that env pointer (same for `0x3b2118/0x3b2110` via JNI_OnLoad).

### 11.4 Canonical JNI slot map (the harness `name_of()` is wrong past ~slot 66)

Verified against the engine's own call sites: the table is **stock
`JNINativeInterface`**. Slots observed live:

```
 6 FindClass          (0x30)      33 GetMethodID        (0x108)
21 NewGlobalRef       (0xa8)      35 CallObjectMethodV  (0x118)
23 DeleteLocalRef     (0xb8)      38 CallBooleanMethodV (0x130)
94 GetFieldID         (0x2f0)    169 IsVirtualThread    (0x548)
```

(The harness prints "CallStaticByteMethodV" for slot 94 — cosmetic label bug only;
the handler behaviour is correct.) For the port, implement the canonical table.

### 11.5 What the engine does the moment it gets a window (observed in run8)

`onSurfaceCreated(SURFACE)` → APP_CMD_INIT_WINDOW → game thread:

```
ANativeWindow_fromSurface x2 → Game+0x130     (window handle)
ANativeWindow_setBuffersGeometry x4           (1920x1080)
ANativeWindow_getWidth/getHeight
eglGetDisplay/eglInitialize/eglGetConfigAttrib/eglChooseConfig
eglCreateWindowSurface/eglCreateContext/eglMakeCurrent
+ JNI bootstrap: FindClass x2, NewGlobalRef x2, GetMethodID x3, GetFieldID,
  CallStaticByteMethodV x5
```

then back to `pollOnce(-1)`. No `gl*` calls and no asset opens yet — the engine
waits for the next wake (input / more app cmds) before it starts its world.

### 11.6 Harness facts worth keeping

- `9/9` ctors run clean; the old "constructor asserts" (§11 of the previous revision)
  was the spin-loop misread plus missing `initializeNativeCode` args — resolved.
- App pipe protocol: 1-byte `APP_CMD_*`; `android_app_read_cmd`/`pre_exec_cmd`/
  `post_exec_cmd` are **module-local code** (0x31eef8/0x31efb0/0x31f09c), not imports.
- The engine imports `android_app_swap_input_buffers` (GOT 0x38f9d0) — the input pump
  is partially *external* to the module; the port must provide it (or its behaviour).
- Sensor queue: `ASensorManager_createEventQueue(manager, looper, ident=3)` where 3 is
  `LOOPER_ID_USER`; the main loop drains it when `pollOnce` returns ident 3. Returning
  NULL/0 for the sensor manager (as the harness does) keeps the engine happy.
- `ALooper_pollOnce(-1)` blocking forever is correct; the engine's frame/input pump
  (0xea384) runs on `pollOnce` returning **negative** (`ALOOPER_POLL_WAKE`), so the
  port's looper must implement `ALooper_wake` semantics honestly (a wake without an
  fd event returns -1 and triggers the pump).

## 12. Native macOS port — the shim must be real where the harness could stub

The harness proved the control flow; the port must now supply real implementations.

**Import surface (authoritative): `SHIM_MANIFEST.md`** — merged from *two* truth sources:

1. 144 undefined symbols in `.rela.plt`/`.rela.dyn` (NDK APIs, EGL/GL, math,
   a few libc, logging, OpenSL);
2. **119 libc symbols recovered from the live oracle**: PairIP *stripped the
   relocation entries* for ~120 `.got.plt` slots (`malloc`, `pthread_*`, stdio,
   sockets…), so they are invisible to `.dynsym` yet absolutely called. The
   oracle (`runtime_dump/got_truth.json`) mapped each stripped slot to the libc
   function the *live* process had resolved. Total port surface: **268 symbols**
   (real 106, shim 62, map 98, stub 2).

| group | port approach |
|---|---|
| libc/math (98 syms) | **map directly** — GOT slot := `dlsym(RTLD_DEFAULT, name)`. AAPCS64 and Apple arm64 agree for non-variadic calls. |
| **pthread / sync** | **shim** — Bionic mutex 40 B / cond 48 B / attr 56 B vs Darwin's different layouts + signature field. Side table guest-addr→host primitive; `pthread_create` spawns a **real host thread** that calls the guest start routine directly (same ISA!); Bionic cond default clock = MONOTONIC, Darwin timeout = REALTIME; `PTHREAD_MUTEX_RECURSIVE` 1 vs 2. |
| ALooper / android_app glue | shim over host `poll()`; real `pipe(2)`; deliver APP_CMD bytes exactly as the harness does; `ALooper_wake` semantics honest (negative return = input-pump tick). |
| AAssetManager / AAsset | real: back with a directory of the user's extracted assets; `AAssetManager_open(path)`→`AAsset_read` (mmap the .pak/.dat files — engine decodes them). |
| EGL / GLES2 (real 106 incl. these) | real: ANGLE (Metal) preferred; ANativeWindow shim = CAMetalLayer-backed; `eglSwapBuffers` presents. |
| ASensor* | stub returning NULL/0 (engine tolerates — verified); real later (no sensors on a Mac anyway). |
| AConfiguration | en-US stub; language/country copied into caller buffers. |
| `__android_log_print` etc. | shim (variadic!) → fprintf to stderr / os_log. |
| `ExecuteProgram` | stub — the PairIP VM entry; the patched `.so` has plaintext `.text`, so the VM must never be needed. |
| JNIEnv | in-process C table (canonical slots), IDs minted by (class,name,sig); Java side is tiny (PaybackActivity callbacks + a few statics) — emulate, don't embed a JVM. |

### 12.1 Boundary ABI landmines (discovered while writing the manifest)

1. **Variadic calls.** Apple arm64 passes *all* variadic args on the **stack**;
   AAPCS64 passes them in x1..x7 first. A direct map of `printf`/`snprintf`/
   `__android_log_print`/`fscanf`… reads garbage. Every printf-family import needs
   a guest-ABI entry stub that marshals (x1..x7 + stack) into a canonical form and
   runs a local formatter; `vsnprintf` must interpret the guest's AAPCS64
   `va_list` struct (`__stack,__gr_top,__vr_top,__gr_offs,__vr_offs`) directly.
2. **TLS / stack canary.** The engine reads `tpidr_el0 + 0x28` (Bionic TCB slot 5 =
   stack guard) on every guarded function, and Bionic errno lives at TCB + 0x10.
   The port must give each *guest* thread a TCB with a stable canary, while host
   libc keeps its own TLS. Open question to settle empirically on day one: which
   alias does Apple libc use (TPIDR_EL0 vs TPIDRRO_EL0) — decides whether shims
   need TP save/restore around host calls.
3. **fd flags.** `O_CREAT/O_TRUNC/O_APPEND/O_NONBLOCK` differ between Linux and
   macOS (`0x40/0x200/0x400/0x800` vs `0x200/0x400/0x8/0x4`); `fcntl`/`open`/`pipe`
   shims translate.
4. **sockaddr.** BSD carries `sa_len` at offset 0 (family at +1); Linux family is a
   16-bit field at +0. `connect`/`getaddrinfo`/`setsockopt` need translation
   (`AF_INET6` 10 vs 30, `SOCK_NONBLOCK` in-type vs fcntl).
5. **stdio identity.** Bionic `__sF` = FILE[3]; `fopen`-family shims exchange
   guest-side tokens for host FILEs; `strerror_r` GNU-vs-XSI return convention;
   `sysconf`/`getauxval` constant numbering differs.


Non-import work the port must do (all learned from the harness):

1. Map the patched ELF's PT_LOADs at a base; apply **all** relocation types present:
   `R_AARCH64_RELATIVE` (3891), `R_AARCH64_ABS64` (1511), `R_AARCH64_GLOB_DAT` (169).
   Undefined syms → shim targets; defined syms → `base + st_value + addend`.
2. `.init_array` (9 entries; all-zero in the file — fill happens via RELATIVE relocs,
   read post-reloc), then `JNI_OnLoad(javaVM, NULL)` → expect `0x10006`.
3. Seed `[0x3b2120] = env`, `[0x3b2208] = methodID` (§11.3).
4. `initializeNativeCode(env, activity, "Payback 2", "2.106.16", <vc>, assetMgr, 0, 0)`
   with the Game constructor's real argument registers.
5. Drive the lifecycle natives in order (all 5 verified working in run8):
   `onStartNative(0x31D0A8)`, `onResumeNative(0x31D0C0)`,
   `onWindowFocusChangedNative(0x31D1DC, 1)`,
   `onSurfaceCreatedNative(0x31D1FC, surface)`,
   `onSurfaceChangedNative(0x31D264, surface, 1920, 1080, rotation=1)`;
   surface = an `ANativeWindow*` the port fabricates.
6. Keep the game thread blocked in a real `pollOnce(-1)`; feed it input (later phase)
   and let `android_app_swap_input_buffers` deliver events.

## 13. Next steps (port build order)

1. **P1 — loader + shims + boot (no graphics).** mmap/relocs, libc via dlsym, pthread
   shim, ALooper, log, AConfiguration/ASensor stubs, fake JNI, run ctors → JNI_OnLoad →
   initializeNativeCode → lifecycle. Success = same terminal state as run8 (game thread
   idle in poll, no faults) but natively, in a plain console harness.
2. **P2 — assets.** AAsset* over the user's unpacked assets; watch the engine open its
   packs (the harness never got there; expect the first opens right after the wake).
3. **P3 — window + GL.** NSWindow/CAMetalLayer + ANGLE; first `eglSwapBuffers` with a
   cleared frame.
4. **P4 — input & frames.** Pump the engine's input path; see real frames.
5. **P5 — packaging.** .app that loads the user's own copy; zero game content shipped.

## 14. Port status (native, session 7) — P1 complete

`port/` is live: `cd port && make && ./pbnative <patched.so> [engine]`.

**Verified native behavior (20/20 runs clean, ~4 s):** map image @0x8000000000,
6695 relocations, 120 oracle holes wired, 9 ctors, `JNI_OnLoad -> 0x10006`,
`initializeNativeCode(env, activity, "Payback 2", "2.106.16", "178", assetMgr, 0, 0)
-> Game*` (entry 0x31c908), then the five lifecycle natives in PaybackActivity
order (`onStart 0x31D0A8`, `onResume 0x31D0C0`, `onWindowFocusChanged 0x31D1DC(1)`,
`onSurfaceCreated 0x31D1FC(surface)`, `onSurfaceChanged 0x31D264(surface,1920,1080,1)`),
engine logs `[Native] Unable to eglMakeCurrent` (expected: EGL zero-stubs), and the
android_main thread parks in a **real** `poll(2)` inside our ALooper — same terminal
state as harness run8, but native. Zero `[missing]` aborts.

### 14.1 TPIDR_EL0: Darwin malloc reads it (root cause of a flaky SIGSEGV)

`_xzm_xzone_find_and_malloc_from_freelist_chunk` computes its segment-group slot as
`((w10 * (TPIDR_EL0 >> 12) + w9) & 0xff) * 0x340` — i.e. libsystem_malloc treats
TPIDR_EL0 as a tiny per-thread/CPU token (kernel leaves values like 0x1..0x1005 there,
page 0-1). Writing a heap TCB pointer into it (old `wr_tp` in thread_tramp/current_rec)
sends the slot hash to a random index → often an all-zero, never-initialized group
struct → `group->[0x18] == NULL` → segfault in `_xzm_segment_group_alloc_segment`
whenever the guest thread's first 28 KB malloc ran. It presented as 1-in-3 flaky and
looked like heap corruption. **Fix: never write TPIDR_EL0.** Nothing needs it: all 519
guest `mrs tpidr_el0` reads are patch-rewritten to an `adrp` canary at image+0x28, and
errno/TSD ride host pthread TLS (`pthread_getspecific`, TPIDRRO_EL0-safe).

### 14.2 Port-only anchors and tooling

- `0x31c908` = internal `initializeNativeCode` body; exported
  `Java_..._GameActivity_initializeNativeCode` @0x31c894 is a thunk that tail-calls it
  (plus a Game SDK pre-call). The port calls 0x31c908 directly, like the harness.
- Crash reporter: full register dump, frame-pointer chain, `dladdr` symbolization,
  guest-relative `img+` labels, exit 77.
- `PB_TRACE_ALLOC=1` logs every malloc/calloc/realloc/free with the guest call site.
- `PB_HOLD=n` keeps the process alive n seconds after the lifecycle drive.
- EGL/GL are one shared `sh_ret0` stub — headless mode keeps the whole boot path
  exercisable (that is *why* run8 could not open assets: the engine quits GL init early).

### 14.3 P3 reconnaissance (done)

ANGLE for arm64 (Metal backend, system-framework deps only) ships inside the user's
Mac.apk.app: `/Applications/Mac.apk.app/Contents/Resources/libEGL.dylib` (115 egl*
exports) and `libGLESv2.dylib` (829 gl* exports). Plan: dlopen those, EGL platform
display = Metal, window surface = IOSurface-backed pbuffer
(`eglCreatePbufferFromClientBuffer` + `EGL_ANGLE_iosurface_client_buffer`), present by
pointing a `CALayer.contents` at the same IOSurface inside an NSWindow. Fallback:
plain pbuffer + `glReadPixels` readback into the layer.
