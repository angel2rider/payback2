# Payback 2 — Web Port (WebAssembly + WebGPU / WebGL2)

A cross-platform web port of *Payback 2: The Battle Sandbox*, executing the game engine in the browser via **WebAssembly (WASM)**, modern web graphics (**WebGPU / WebGL2**), and the **Web Audio API**.

---

## 1. Project Overview & Plan Transition

> [!IMPORTANT]
> **Direction Change:** The project previously focused on a native macOS (Apple Silicon ARM64) harness (`pbnative`). Per updated project goals, development has pivoted to a **universal Web build** running in modern browsers. All reverse-engineered ABI shims, oracle-recovered GOT mappings, and asset decoders developed during the native phase are being migrated to the WebAssembly runtime.

The target binary is `lib/arm64-v8a/libpayback2.so` (version 2.106.16, versionCode 178), an AArch64 ELF engine developed by Apex Designs.

### Architecture Comparison

| Subsystem | Original Android Target | Prior Native macOS Port | **Target Web Port (WASM + WebGPU)** |
|---|---|---|---|
| **CPU / Execution** | ARM64 CPU (Android Bionic) | Native ARM64 execution (Darwin) | **WebAssembly (WASM)** (Emscripten AArch64 execution engine / static binary translation) |
| **Graphics** | OpenGL ES 2.0 (EGL) | ANGLE (Metal) + CALayer | **WebGPU** (with WebGL2 fallback) on `<canvas>` |
| **Audio** | OpenSL ES (PCM buffer queue) | Silent stub / CoreAudio | **Web Audio API** (`AudioContext` / `AudioWorklet`) |
| **Asset Storage** | Android `AAssetManager` / APK | Local filesystem (`PB_ASSETS`) | **Virtual File System (VFS)** / `fetch` + IndexedDB cache |
| **Input** | Android `MotionEvent` / `KeyEvent` | macOS NSEvent translation | **HTML5 Pointer Events & Keyboard Events** |
| **OS / Runtime** | Android Game SDK / NDK | Custom C shims (`port/src`) | **Emscripten / JavaScript C-WASM shims** |

---

## 2. Heavy Utilization of MuMu Emulator as Ground-Truth Oracle

To ensure 100% accuracy during web porting, a live instance of the **MuMu Android Emulator** (running Android 12, ARM64 on `127.0.0.1:16384`) serves as the active ground-truth oracle:

```
┌─────────────────────────────────┐          ┌───────────────────────────────────┐
│     Live MuMu Emulator          │          │        Target Web Build           │
│  (Android 12 · ARM64 · Root)   │          │     (Browser WASM + WebGPU)       │
│                                 │          │                                   │
│  • Memory dumping & validation  │          │  • Virtualized guest memory       │
│  • Asset decryption (AAsset_*)  │ ───────> │  • Decrypted assets in VFS        │
│  • JNI call sequence tracing   │          │  • High-fidelity JNI JS stubs     │
│  • Shader & texture capture     │          │  • WebGPU / WGSL pipelines        │
│  • OpenSL buffer profiling      │          │  • Web Audio Worklet sink         │
└─────────────────────────────────┘          └───────────────────────────────────┘
```

### Oracle Workflows
1. **Live State Extraction**: Extract decrypted text segments, BSS arenas, and runtime configuration straight from the live process memory using root ADB.
2. **Asset Capture**: Intercept proprietary asset decoders (`.amct` textures, `.ama` models, `.IAP` text/script packs) at runtime.
3. **Trace Reconciliation**: Compare JNI lifecycle event sequences, audio buffer cadence, and frame draws between MuMu and the Web runtime.

---

## 3. Web Architecture & Implementation Plan

### Phase 1: Web Scaffolding & Virtual Asset Server
- Modern web frontend (`index.html`, responsive canvas container, loading UI).
- Browser VFS mounting the 854 extracted and decrypted assets.
- Web Worker architecture to run the engine off the main UI thread.

### Phase 2: WebAssembly Execution Core
- Compile the runtime shims (`android_shim`, `jni_env`, `egl_shim`, `opensles_shim`) using Emscripten.
- AArch64 execution layer in WASM to drive `libpayback2.so` without native host dependency.
- Wire JNI environment to JavaScript bindings.

### Phase 3: WebGPU & WebGL2 Graphics Pipeline
- Bridge the engine's 83 OpenGL ES 2.0 draw calls and shaders to **WebGPU** (or WebGL2).
- Texture format translation (ETC1/RGBA to WebGPU compatible formats).
- Framebuffer presentation to the web canvas.

### Phase 4: Web Audio & Input Integration
- OpenSL ES buffer queue emulation via `AudioWorkletNode`.
- Mouse, touch, keyboard, and Gamepad API mappings to Android `AInputEvent`.

---

## 4. Key Documentation Links

* [PAYBACK2_ANALYSIS_REPORT.md](file:///Users/thegt/Downloads/payback/PAYBACK2_ANALYSIS_REPORT.md) — Complete APK architecture, security analysis, and static inspection report.
* [NATIVE_PORT_HANDOFF.md](file:///Users/thegt/Downloads/payback/NATIVE_PORT_HANDOFF.md) — Technical details of engine internals, thread models, JNI anchors, and recovered GOT slots.
* [SHIM_MANIFEST.md](file:///Users/thegt/Downloads/payback/SHIM_MANIFEST.md) — Census of 268 Android/libc symbols required by the engine.
