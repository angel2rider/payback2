# Phase 1 Working Notes — Payback 2 Porting

> [!NOTE]
> Project focus transitioned to a **Web Port (WebAssembly + WebGPU/WebGL2)**. See [README.md](file:///Users/thegt/Downloads/payback/README.md) for the current architectural roadmap.

Goal: Re-implement the Android OS layer underneath `libpayback2.so` using WebAssembly and WebGPU for the browser.

## Confirmed facts about the target

- Library: `lib/arm64-v8a/libpayback2.so`, 38.57 MB file, **2.70 MB of actual code**
  (`.text`), remainder is a 34.6 MB `.bss` arena (runtime asset scratch space).
- **Not packed, not encrypted.** 100% of `.text` beyond the first ~44 KB is valid,
  unencrypted ARM64. The first 44 KB (0xe45c0–0xf05c0) is data tables interleaved in
  the code section (NEON lookup/jump tables), not obfuscation.
- `.symtab` stripped, but **`.eh_frame` intact** → 4,806 FDEs → **4,802 recovered
  function ranges covering 100% of `.text`**. This is the function map Ghidra uses.
- 2,443 C++ mangled symbols survive, but they're almost all **ExitGames Photon + libc++**,
  not the game engine. The engine's own classes are stripped (only `GameTextInput` leaks).
  ⇒ Photon multiplayer is *already symbolized* (can be reused/dropped trivially); the
  engine is the real RE target.

## Tooling built (this session)

| File | Purpose |
|---|---|
| `elf64.py` | ELF64 reader; `.eh_frame` FDE recovery; `DT_NEEDED`; symbols |
| `pltcallers.py` | Decodes each 16-byte AArch64 PLT stub to recover its exact GOT slot, maps GOT→symbol via `.rela.plt`, then scans `.text` for `bl` call sites |
| `xref.py` | ADRP/ADD string xref index (partial — see gotchas) |
| `funcs.txt` | 4,802-entry function map (addr, size, symbol if known) |
| `ghidra_proj/payback2.rep` | **Ghidra project, fully analyzed** (AARCH64:LE:64:v8A) |
| `scripts/py_discover.py` | PyGhidra: open project, stats, symbol lookup, decompile a function |
| `scripts/py_thunks.py` | PyGhidra: PLT-thunk caller query, largest-function listing |

Environment: Ghidra 12.1.4 at `/opt/homebrew/Cellar/ghidra/12.1.4/libexec`, Java 25
(Temurin). PyGhidra via `./.venv/bin/python` (pyghidra 3.1.0, unicorn 2.1.4,
capstone 5.0.7). Run PyGhidra with `GHIDRA_INSTALL_DIR` set.

Results: **1,123/1,243 PLT stubs resolved to the 150 real imports**; full import-surface
categorization (150 undefined symbols: 24 NDK, 71 GLES2, OpenSL, sensors, BSD sockets).

### Gotchas hit (important if continuing by hand)
- Capstone `skipdata=True` required; ~69% of the first 44 KB is non-instruction data.
- Capstone returns the **ADRP immediate already page-resolved (absolute)** — do NOT add
  the instruction address again.
- `.rela.plt` has 1,124 entries but `.plt` has 1,243 stubs (extra = IRELATIVE ifuncs);
  stub order ≠ rela order, so decode the stub's GOT offset instead of indexing.
- `ldr x17,[x16,#imm]` has **2** operands (mem is operands[1], not [2]).

## SOLVED: the asset API is hidden behind a runtime function-pointer table

The engine deliberately defeats static call-graph analysis of its asset loader:

- Every `AAsset_*` **GOT slot is `0x0` in the file** (lazy binding) — no pointers to them
  anywhere in `.rodata` / `.data.rel.ro` / `.data`.
- `.text` contains **1,028 indirect `blr`** calls.
- Only `AAssetManager_fromJava` is called directly (`bl` at `0x31ccb0`, 1 site).
  All of `AAsset_open / getLength / read / close / getRemainingLength` have
  **ZERO direct callers** — they're fetched once into a struct (inside the init
  function) and invoked through that vtable.

This is why both my PLT scan *and* Ghidra's reference manager report zero refs. It is
intentional obfuscation of the loader — but it does NOT block the port, because we don't
need to find the loader at all: we supply the `AAsset_*` implementations ourselves in the
fake `libandroid.so` and let the engine call them. The vtable becomes an *asset* of the
emulation, not an obstacle.

### Engine entry points (raw vaddr; add 0x100000 for Ghidra)
| Symbol | Raw addr | Size |
|---|---|---|
| `Java_com_google_androidgamesdk_GameActivity_initializeNativeCode` | `0x31c894` | 116 B |
| `FUN_0031c908` — init / asset-manager bootstrap (allocates 0x260-byte struct, zero-inits, calls `AAssetManager_fromJava`) | `0x31c908` | ~3.0 KB |
| `FUN_00317774` (Ghidra: contains 0x31c908) | `0x317774` | 23 KB |

Ghidra analysis result: **6,308 functions, 38,047 symbols**.
Image base `0x100000` → Ghidra addr = raw vaddr + `0x100000` (do not forget this).

## Asset format status

- `.pak` + `.dat` (89 files, 26.6 MB): **raw LZMA**, not encrypted. 45/89 decode with
  stock `lzma.decompress()` today. Header = props(1) + dict(4 LE=16 MB) + uncsize(8 LE).
- `.glsl` (76): plaintext.
- `.amct` (103, 78.6 MB): magic `TCMA`, CRN codec linked in → likely compression, loader TBD.
- `.ama` (514, 2.6 MB) + `.IAP` (27, 3.0 MB): custom/encrypted, RE TBD.
- The 27 `.IAP` packs (~100 KB each) likely = the 24 languages' text → highest-value
  single target after the init function.

## Next steps (Phase 1)

1. Ghidra headless: import `libpayback2.so`, apply relocations, run autoanalysis on
   `FUN_0x0031c908`; recover the asset fn-pointer struct type and its indirect callers
   (the real loader + unpacker).
2. Build the fake-OS shim (24 NDK symbols + GLES→GL + OpenSL→miniaudio + BSD sockets),
   plus a fake `JNIEnv*` for the ~12 `Java_*` JNI exports and the
   `ANDROID_*`/`GMS_*`/`HW_*` callbacks the native side invokes back into Java.
3. Drive `FUN_0x0031c908` (or the `initializeNativeCode` JNI entry) under **Unicorn**
   (ARM64) with the shim; hook `AAsset_read` to feed real asset bytes, and dump every
   buffer handed back → produces the full unpacked texture/audio/text corpus.

Everything hinges on step 2/3 running the original code, so the game logic itself never
needs a line-by-line port.