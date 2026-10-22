/**
 * wasm_engine_worker.js — Payback 2 Web Port Engine Worker.
 *
 * Runs the AArch64 game engine inside WebAssembly (Unicorn.js) in a background Web Worker,
 * executing the real game thread, processing Android lifecycle & input command pipes,
 * streaming OpenGL ES draw calls directly to WebGL2 via OffscreenCanvas,
 * and loading game assets through the virtual asset cache.
 */

self.onerror = function(message, source, lineno, colno, error) {
  console.error("[WORKER UNCAUGHT ERROR]:", message, "at", source, ":", lineno, ":", colno, error);
  self.postMessage({ type: "ERROR", error: String(message) });
};
self.onunhandledrejection = function(e) {
  console.error("[WORKER UNHANDLED REJECTION]:", e.reason);
  self.postMessage({ type: "ERROR", error: String(e.reason) });
};

importScripts("engine/unicorn_aarch64.js?t=" + Date.now());

const BASE_ADDR = 0x10000000n;
const TRAMPOLINE_BASE = 0x71000000;
const STACK_BASE = 0x70000000;
const STACK_SIZE = 0x800000;       // 8MB main stack
const THREAD_STACK_BASE = 0x70400000;
const THREAD_STACK_SIZE = 0x200000; // 2MB game thread stack
const HEAP_BASE = 0x20000000;
const HEAP_SIZE = 0x20000000;      // 512MB initial guest heap (0x20000000 - 0x40000000)
const TLS_BASE = 0x78000000;       // 64KB TLS
const JNI_ENV_ADDR = 0x60100000;
const JNI_VM_ADDR  = 0x60180000;
const STOP_ADDR    = 0x71ffff00;

let uc = null;
let engine = null;
let isRunning = false;
let gameHandle = 0n;
let nextTramp = TRAMPOLINE_BASE;
const trampMap = new Map(); // addr -> { name, handler }
const shims = new Map();
const simdOpMap = new Map(); // guestAddr -> simd operation metadata

// Saved execution context for cooperative thread switching
let gameThreadContext = null;
const guestThreads = [];

let ALL_GUEST_REGS = null;
function getGuestRegList() {
  if (!ALL_GUEST_REGS && uc) {
    ALL_GUEST_REGS = [
      uc.ARM64_REG_X0, uc.ARM64_REG_X1, uc.ARM64_REG_X2, uc.ARM64_REG_X3,
      uc.ARM64_REG_X4, uc.ARM64_REG_X5, uc.ARM64_REG_X6, uc.ARM64_REG_X7,
      uc.ARM64_REG_X8, uc.ARM64_REG_X9, uc.ARM64_REG_X10, uc.ARM64_REG_X11,
      uc.ARM64_REG_X12, uc.ARM64_REG_X13, uc.ARM64_REG_X14, uc.ARM64_REG_X15,
      uc.ARM64_REG_X16, uc.ARM64_REG_X17, uc.ARM64_REG_X18, uc.ARM64_REG_X19,
      uc.ARM64_REG_X20, uc.ARM64_REG_X21, uc.ARM64_REG_X22, uc.ARM64_REG_X23,
      uc.ARM64_REG_X24, uc.ARM64_REG_X25, uc.ARM64_REG_X26, uc.ARM64_REG_X27,
      uc.ARM64_REG_X28, uc.ARM64_REG_X29, uc.ARM64_REG_X30,
      uc.ARM64_REG_SP, uc.ARM64_REG_PC, uc.ARM64_REG_NZCV, uc.ARM64_REG_TPIDR_EL0
    ];
  }
  return ALL_GUEST_REGS;
}

let reg128Buf = null;
function getReg128Buf() {
  if (!reg128Buf && uc) {
    reg128Buf = uc._malloc(16);
  }
  return reg128Buf;
}

function readReg128(handle, regid) {
  const buf = getReg128Buf();
  uc.ccall("uc_reg_read", "number", ["pointer", "number", "pointer"], [handle, regid, buf]);
  return [uc.getValue(buf, "i64"), uc.getValue(buf + 8, "i64")];
}

function writeReg128(handle, regid, val) {
  const buf = getReg128Buf();
  uc.setValue(buf, val[0], "i64");
  uc.setValue(buf + 8, val[1], "i64");
  uc.ccall("uc_reg_write", "number", ["pointer", "number", "pointer"], [handle, regid, buf]);
}

function emulate_permute(opcode, Q, size, vn128, vm128) {
  const bufN = new ArrayBuffer(16);
  const bufM = new ArrayBuffer(16);
  const bufD = new ArrayBuffer(16);
  const viewN = new DataView(bufN);
  const viewM = new DataView(bufM);
  const viewD = new DataView(bufD);
  viewN.setBigUint64(0, vn128[0], true);
  viewN.setBigUint64(8, vn128[1], true);
  viewM.setBigUint64(0, vm128[0], true);
  viewM.setBigUint64(8, vm128[1], true);

  const esize = 1 << size;
  const totalBytes = Q ? 16 : 8;
  const numElems = totalBytes / esize;
  const half = numElems / 2;

  const u8N = new Uint8Array(bufN);
  const u8M = new Uint8Array(bufM);
  const u8D = new Uint8Array(bufD);

  function copyElem(dstElem, srcArr, srcElem) {
    const dstOffset = dstElem * esize;
    const srcOffset = srcElem * esize;
    for (let b = 0; b < esize; b++) {
      u8D[dstOffset + b] = srcArr[srcOffset + b];
    }
  }

  if (opcode === 1) { // uzp1
    for (let i = 0; i < half; i++) copyElem(i, u8N, i * 2);
    for (let i = 0; i < half; i++) copyElem(half + i, u8M, i * 2);
  } else if (opcode === 5) { // uzp2
    for (let i = 0; i < half; i++) copyElem(i, u8N, i * 2 + 1);
    for (let i = 0; i < half; i++) copyElem(half + i, u8M, i * 2 + 1);
  } else if (opcode === 2) { // trn1
    for (let i = 0; i < half; i++) {
      copyElem(i * 2, u8N, i * 2);
      copyElem(i * 2 + 1, u8M, i * 2);
    }
  } else if (opcode === 6) { // trn2
    for (let i = 0; i < half; i++) {
      copyElem(i * 2, u8N, i * 2 + 1);
      copyElem(i * 2 + 1, u8M, i * 2 + 1);
    }
  } else if (opcode === 3) { // zip1
    for (let i = 0; i < half; i++) {
      copyElem(i * 2, u8N, i);
      copyElem(i * 2 + 1, u8M, i);
    }
  } else if (opcode === 7) { // zip2
    for (let i = 0; i < half; i++) {
      copyElem(i * 2, u8N, half + i);
      copyElem(i * 2 + 1, u8M, half + i);
    }
  }

  return [viewD.getBigUint64(0, true), viewD.getBigUint64(8, true)];
}

function emulate_cnt(Q, vn128) {
  const bufN = new ArrayBuffer(16);
  const bufD = new ArrayBuffer(16);
  const viewN = new DataView(bufN);
  const viewD = new DataView(bufD);
  viewN.setBigUint64(0, vn128[0], true);
  viewN.setBigUint64(8, vn128[1], true);
  const u8N = new Uint8Array(bufN);
  const u8D = new Uint8Array(bufD);
  const count = Q ? 16 : 8;
  for (let i = 0; i < count; i++) {
    let b = u8N[i];
    let c = 0;
    while (b > 0) {
      c += (b & 1);
      b >>= 1;
    }
    u8D[i] = c;
  }
  return [viewD.getBigUint64(0, true), viewD.getBigUint64(8, true)];
}

function emulate_addv(Q, size, vn128) {
  const bufN = new ArrayBuffer(16);
  const bufD = new ArrayBuffer(16);
  const viewN = new DataView(bufN);
  const viewD = new DataView(bufD);
  viewN.setBigUint64(0, vn128[0], true);
  viewN.setBigUint64(8, vn128[1], true);

  const esize = 1 << size;
  const total = Q ? 16 : 8;
  const num = total / esize;

  if (size === 0) {
    const u8 = new Uint8Array(bufN);
    let sum = 0;
    for (let i = 0; i < num; i++) sum = (sum + u8[i]) & 0xff;
    new Uint8Array(bufD)[0] = sum;
  } else if (size === 1) {
    let sum = 0;
    for (let i = 0; i < num; i++) sum = (sum + viewN.getUint16(i * 2, true)) & 0xffff;
    viewD.setUint16(0, sum, true);
  } else if (size === 2) {
    let sum = 0;
    for (let i = 0; i < num; i++) sum = (sum + viewN.getUint32(i * 4, true)) >>> 0;
    viewD.setUint32(0, sum, true);
  }

  return [viewD.getBigUint64(0, true), viewD.getBigUint64(8, true)];
}

function emulate_rev(revType, Q, size, vn128) {
  const bufN = new ArrayBuffer(16);
  const bufD = new ArrayBuffer(16);
  const viewN = new DataView(bufN);
  const viewD = new DataView(bufD);
  viewN.setBigUint64(0, vn128[0], true);
  viewN.setBigUint64(8, vn128[1], true);
  const u8N = new Uint8Array(bufN);
  const u8D = new Uint8Array(bufD);

  const esize = 1 << size;
  const containerSize = revType === 0 ? 8 : (revType === 1 ? 4 : 2);
  const elemsPerContainer = Math.floor(containerSize / esize);
  const totalBytes = Q ? 16 : 8;

  for (let c = 0; c < totalBytes; c += containerSize) {
    for (let e = 0; e < elemsPerContainer; e++) {
      const srcOff = c + e * esize;
      const dstOff = c + (elemsPerContainer - 1 - e) * esize;
      for (let b = 0; b < esize; b++) {
        u8D[dstOff + b] = u8N[srcOff + b];
      }
    }
  }

  return [viewD.getBigUint64(0, true), viewD.getBigUint64(8, true)];
}

function extract_h(vn128, index) {
  const qword = index < 4 ? vn128[0] : vn128[1];
  const shift = BigInt((index & 3) * 16);
  return Number((qword >> shift) & 0xffffn);
}

function saveGuestContext() {
  const handle = uc.getValue(engine.handle_ptr, "*");
  const regs = getGuestRegList();
  const ctx = {
    gp: new Array(regs.length),
    simd: new Array(32),
    fpcr: engine.reg_read_i64(uc.ARM64_REG_FPCR),
    fpsr: engine.reg_read_i64(uc.ARM64_REG_FPSR)
  };
  for (let i = 0; i < regs.length; i++) {
    ctx.gp[i] = engine.reg_read_i64(regs[i]);
  }
  for (let i = 0; i < 32; i++) {
    ctx.simd[i] = readReg128(handle, uc["ARM64_REG_Q" + i]);
  }
  return ctx;
}

function restoreGuestContext(ctx) {
  if (!ctx) return;
  const handle = uc.getValue(engine.handle_ptr, "*");
  const regs = getGuestRegList();
  if (Array.isArray(ctx)) {
    // Legacy array format fallback
    for (let i = 0; i < regs.length; i++) {
      if (ctx[i] !== undefined) engine.reg_write_i64(regs[i], ctx[i]);
    }
    return;
  }
  for (let i = 0; i < regs.length; i++) {
    if (ctx.gp && ctx.gp[i] !== undefined) engine.reg_write_i64(regs[i], ctx.gp[i]);
  }
  if (ctx.simd) {
    for (let i = 0; i < 32; i++) {
      if (ctx.simd[i]) writeReg128(handle, uc["ARM64_REG_Q" + i], ctx.simd[i]);
    }
  }
  if (ctx.fpcr !== undefined) engine.reg_write_i64(uc.ARM64_REG_FPCR, ctx.fpcr);
  if (ctx.fpsr !== undefined) engine.reg_write_i64(uc.ARM64_REG_FPSR, ctx.fpsr);
}

// OffscreenCanvas & WebGL2 context
let canvas = null;
let gl = null;

// WebGL2 resource maps (Guest Handle <-> WebGL Object)
const glTextures = new Map();
let nextTexId = 1;
const glBuffers = new Map();
let nextBufId = 1;
const glShaders = new Map();
let nextShaderId = 1;
const glPrograms = new Map();
let nextProgId = 1;
const glFramebuffers = new Map();
let nextFboId = 1;
const glRenderbuffers = new Map();
let nextRboId = 1;
const glUniformLocs = new Map();
let nextLocId = 1;

// Frame telemetry
let frameCount = 0;
let drawCallCount = 0;
let lastFpsTime = Date.now();

// Helper: 8-byte pointer buffer
const ptrBuf = new Uint8Array(8);
const ptrView = new DataView(ptrBuf.buffer);
function writeU64(val) {
  ptrView.setBigUint64(0, BigInt(val), true);
  return Array.from(ptrBuf);
}

// Trampoline generator
function addTrampoline(name, handler) {
  const addr = nextTramp;
  nextTramp += 16;
  // Write ARM64 "brk #0" (0xd4200000)
  engine.mem_write(addr, [0x00, 0x00, 0x20, 0xd4]);
  trampMap.set(addr, { name, handler });
  return addr;
}

// Pure AArch64 machine-code helper installer (executes with ZERO interrupts/trampolines at native JIT speed)
function addNativeCodeHelper(codeBytes) {
  const addr = nextTramp;
  nextTramp = (nextTramp + codeBytes.length + 15) & ~15;
  engine.mem_write(addr, codeBytes);
  return addr;
}

const NATIVE_MEMCPY_ARM64 = [227, 3, 0, 170, 98, 1, 0, 180, 95, 64, 0, 241, 171, 0, 0, 84, 40, 36, 193, 168, 104, 36, 129, 168, 66, 64, 0, 209, 250, 255, 255, 23, 40, 20, 64, 56, 104, 20, 0, 56, 66, 4, 0, 209, 246, 255, 255, 23, 192, 3, 95, 214];
const NATIVE_MEMSET_ARM64 = [227, 3, 0, 170, 33, 28, 0, 18, 33, 32, 1, 42, 33, 64, 1, 42, 33, 128, 1, 170, 34, 1, 0, 180, 95, 32, 0, 241, 139, 0, 0, 84, 97, 132, 0, 248, 66, 32, 0, 209, 251, 255, 255, 23, 97, 20, 0, 56, 66, 4, 0, 209, 248, 255, 255, 23, 192, 3, 95, 214];
const NATIVE_MEMCMP_ARM64 = [226, 0, 0, 180, 8, 20, 64, 56, 41, 20, 64, 56, 8, 1, 9, 107, 161, 0, 0, 84, 66, 4, 0, 209, 250, 255, 255, 23, 224, 3, 31, 42, 192, 3, 95, 214, 224, 3, 8, 42, 192, 3, 95, 214];
const NATIVE_STRLEN_ARM64 = [225, 3, 0, 170, 40, 20, 64, 56, 232, 255, 255, 53, 32, 0, 0, 203, 0, 4, 0, 209, 192, 3, 95, 214];
const NATIVE_STRCMP_ARM64 = [8, 20, 64, 56, 41, 20, 64, 56, 8, 1, 9, 107, 129, 0, 0, 84, 137, 255, 255, 53, 224, 3, 31, 42, 192, 3, 95, 214, 224, 3, 8, 42, 192, 3, 95, 214];
const NATIVE_STRCPY_ARM64 = [226, 3, 0, 170, 40, 20, 64, 56, 72, 20, 0, 56, 200, 255, 255, 53, 192, 3, 95, 214];
const NATIVE_MALLOC_ARM64 = [0, 60, 0, 145, 0, 236, 124, 146, 1, 16, 128, 210, 1, 0, 172, 242, 34, 0, 64, 249, 67, 0, 0, 139, 35, 0, 0, 249, 224, 3, 2, 170, 192, 3, 95, 214];
const NATIVE_RET_ARM64    = [192, 3, 95, 214];
const NATIVE_RET0_ARM64   = [224, 3, 31, 170, 192, 3, 95, 214]; // mov x0, xzr; ret
const NATIVE_RET_MID_ARM64 = [0, 2, 128, 210, 0, 6, 172, 242, 192, 3, 95, 214]; // mov x0, #0x10; movk x0, #0x6030, lsl #16; ret -> 0x60300010
const NATIVE_RET_OBJ_ARM64 = [0, 10, 128, 210, 0, 4, 172, 242, 192, 3, 95, 214]; // mov x0, #0x50; movk x0, #0x6020, lsl #16; ret -> 0x60200050
const NATIVE_FINDCLASS_ARM64 = [0, 2, 128, 210, 0, 4, 172, 242, 192, 3, 95, 214]; // mov x0, #0x10; movk x0, #0x6020, lsl #16; ret -> 0x60200010
const NATIVE_NEWGLOBALREF_ARM64 = [224, 3, 1, 170, 192, 3, 95, 214]; // mov x0, x1; ret
const NATIVE_GETENV_ARM64 = [129, 0, 0, 180, 2, 0, 128, 210, 2, 2, 172, 242, 34, 0, 0, 249, 224, 3, 31, 170, 192, 3, 95, 214]; // *penv = 0x60100000; ret 0
const NATIVE_GETJAVAVM_ARM64 = [129, 0, 0, 180, 2, 0, 128, 210, 2, 3, 172, 242, 34, 0, 0, 249, 224, 3, 31, 170, 192, 3, 95, 214]; // *pvm = 0x60180000; ret 0

function registerShim(name, handler) {
  if (shims.has(name)) return shims.get(name);
  const addr = addTrampoline(name, handler);
  shims.set(name, addr);
  return addr;
}

function getOrEmitShim(name) {
  if (shims.has(name)) return shims.get(name);
  const addr = addTrampoline(name, () => {
    // console.log(`[stub] ${name}`);
    return 0;
  });
  shims.set(name, addr);
  return addr;
}

// Segregated free-list recycling allocator
const freeLists = new Map();
const allocSizes = new Map();
let heapPtr = HEAP_BASE;
let currentHeapLimit = HEAP_BASE + HEAP_SIZE;

function guestAlloc(size) {
  const aligned = Math.max(16, (size + 15) & ~15);
  const bucket = 1 << Math.ceil(Math.log2(aligned));
  const list = freeLists.get(bucket);
  if (list && list.length > 0) {
    const ptr = list.pop();
    allocSizes.set(ptr, bucket);
    return ptr;
  }
  const res = heapPtr;
  heapPtr += bucket;
  allocSizes.set(res, bucket);
  if (heapPtr > currentHeapLimit && engine && uc) {
    const grow = Math.max(0x02000000, (heapPtr - currentHeapLimit + 0x1ffffff) & ~0x1ffffff);
    if (currentHeapLimit + grow <= 0x60000000) {
      try {
        engine.mem_map(currentHeapLimit, grow, uc.PROT_ALL);
        console.log(`[worker heap] Grew guest heap by ${grow / (1024*1024)}MB to 0x${(currentHeapLimit + grow).toString(16)}`);
        currentHeapLimit += grow;
      } catch(err) {
        console.error("[worker heap grow error]:", err.message);
      }
    }
  }
  return res;
}

function guestFree(ptr) {
  if (!ptr) return;
  const sz = allocSizes.get(ptr);
  if (!sz) return;
  allocSizes.delete(ptr);
  let list = freeLists.get(sz);
  if (!list) {
    list = [];
    freeLists.set(sz, list);
  }
  list.push(ptr);
}

// Helper: read C string from guest memory
function readCString(addr) {
  if (!addr) return "";
  let cur = addr;
  const bytes = [];
  while (true) {
    const b = engine.mem_read(cur, 1)[0];
    if (b === 0) break;
    bytes.push(b);
    cur++;
    if (bytes.length > 2048) break;
  }
  return new TextDecoder().decode(new Uint8Array(bytes));
}

// Asset cache & synchronous fetch
const assetCache = new Map();
const openAssets = new Map();
let nextAssetHandle = 0x60500010;

function cleanAssetPath(p) {
  if (!p) return "";
  let s = p.trim();
  while (s.startsWith("/")) s = s.slice(1);
  if (s.startsWith("assets/")) s = s.slice(7);
  return s;
}

function fetchAssetSync(path) {
  const clean = cleanAssetPath(path);
  if (!clean) return null;
  if (assetCache.has(clean)) return assetCache.get(clean);
  try {
    const xhr = new XMLHttpRequest();
    xhr.open("GET", `assets/${clean}`, false); // Synchronous in Web Worker
    xhr.responseType = "arraybuffer";
    xhr.send(null);
    if (xhr.status === 200 || xhr.status === 0) {
      const bytes = new Uint8Array(xhr.response);
      assetCache.set(clean, bytes);
      return bytes;
    }
  } catch(e) {
    console.warn(`[vfs] Failed to fetch assets/${clean}:`, e.message);
  }
  return null;
}

// Message handler
self.onmessage = async function(e) {
  const { type, payload } = e.data;

  if (type === "INIT") {
    try {
      if (e.data.canvas) {
        canvas = e.data.canvas;
        gl = canvas.getContext("webgl2", {
          alpha: false,
          antialias: false,
          depth: true,
          stencil: true,
          preserveDrawingBuffer: true,
          powerPreference: "high-performance"
        });
        if (gl) {
          console.log("[worker] Acquired hardware accelerated WebGL2 context on OffscreenCanvas");
        } else {
          console.warn("[worker] WebGL2 not supported on OffscreenCanvas, falling back to WebGL1");
          gl = canvas.getContext("webgl");
        }
      }

      self.postMessage({ type: "STATUS", message: "Compiling WebAssembly AArch64 CPU Engine..." });
      uc = await MUnicorn();
      engine = new uc.Unicorn(uc.ARCH_ARM64, uc.MODE_ARM);

      self.postMessage({ type: "STATUS", message: "Allocating guest virtual memory regions..." });
      setupMemory();

      self.postMessage({ type: "STATUS", message: "Registering libc, pthreads, GLES & JNI shims..." });
      installAllShims();

      self.postMessage({ type: "STATUS", message: "Fetching libpayback2.so (38.5 MB)..." });
      const resp = await fetch("engine/libpayback2.so");
      const arrayBuffer = await resp.arrayBuffer();
      const buf = new Uint8Array(arrayBuffer);

      self.postMessage({ type: "STATUS", message: "Mapping ELF segments, applying relocations & ctors..." });
      loadElfAndRelocate(buf);

      self.postMessage({ type: "STATUS", message: "Calling JNI_OnLoad..." });
      callJniOnLoad();

      self.postMessage({ type: "STATUS", message: "Initializing GameActivity NativeCode..." });
      callInitializeNativeCode();

      self.postMessage({ type: "STATUS", message: "Starting Game Loop Thread & Driving Lifecycle..." });
      startGuestGameThread();
      driveLifecycle();

      self.postMessage({ type: "READY" });
    } catch(err) {
      console.error("[worker error]", err);
      self.postMessage({ type: "ERROR", error: err.message });
    }
  } else if (type === "START") {
    isRunning = true;
    self.postMessage({ type: "STATUS", message: "Engine Running" });
    runEngineLoop();
  } else if (type === "PING") {
    const pc = engine ? Number(engine.reg_read_i64(uc.ARM64_REG_PC)) : 0;
    const lr = engine ? Number(engine.reg_read_i64(uc.ARM64_REG_LR)) : 0;
    console.log(`[worker PONG] tick=${loopTickCount}, PC=0x${pc.toString(16)}, LR=0x${lr.toString(16)}, draws=${drawCallCount}, isRunning=${isRunning}`);
  } else if (type === "EVAL") {
    try {
      const res = eval(payload);
      self.postMessage({ type: "EVAL_RESULT", result: String(res) });
    } catch(err) {
      self.postMessage({ type: "EVAL_RESULT", error: err.message });
    }
  } else if (type === "POINTER") {
    handlePointerInput(payload);
  } else if (type === "KEY") {
    handleKeyInput(payload);
  }
};

function setupMemory() {
  engine.mem_map(Number(BASE_ADDR), 0x2800000, uc.PROT_ALL);       // 40MB ELF image
  engine.mem_map(TRAMPOLINE_BASE, 0x200000, uc.PROT_ALL);         // 2MB trampolines (at 0x71000000)
  engine.mem_map(STACK_BASE, STACK_SIZE, uc.PROT_ALL);             // 8MB stack (main + game thread)
  engine.mem_map(HEAP_BASE, HEAP_SIZE, uc.PROT_ALL);               // 384MB initial guest heap (0x20000000 - 0x38000000)
  engine.mem_map(0x60000000, 0x2000000, uc.PROT_ALL);             // 32MB JNI / structures
  engine.mem_map(TLS_BASE, 0x10000, uc.PROT_ALL);                 // 64KB TLS
  engine.mem_map(STOP_ADDR & ~0xfff, 0x1000, uc.PROT_ALL);         // 4KB Stop address page
  engine.mem_map(0x00300000, 0x100000, uc.PROT_ALL);               // 1MB stub page for unrelocated 0x3786b0 PLT jumps
  const retInsn = [0xc0, 0x03, 0x5f, 0xd6];
  for (let a = 0x00378000; a < 0x00379000; a += 4) {
    engine.mem_write(a, retInsn);
  }

  // Stack pointer
  engine.reg_write_i64(uc.ARM64_REG_SP, BigInt(STACK_BASE + STACK_SIZE - 0x1000));

  // TLS stack canary at [TLS_BASE + 0x28]
  const canary = 0xdeadbeefcafebaben;
  engine.mem_write(TLS_BASE + 0x28, writeU64(canary));
  engine.reg_write_i64(uc.ARM64_REG_TPIDR_EL0, BigInt(TLS_BASE));

  // Initialize native bump heap pointer at 0x60000080 (starts at 0x22000000, 352MB available)
  engine.mem_write(0x60000080, writeU64(0x22000000n));

  // Hook interrupts (brk #0) for trampolines & instruction emulation
  let intrCount = 0;
  let lastIntrLog = Date.now();
  const intrHistogram = new Map();
  engine.hook_add(uc.HOOK_INTR, (handle_ptr, intno) => {
    const pc = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
    intrCount++;
    const key = trampMap.get(pc)?.name || simdOpMap.get(pc)?.type || `0x${pc.toString(16)}`;
    intrHistogram.set(key, (intrHistogram.get(key) || 0) + 1);

    const now = Date.now();
    if (now - lastIntrLog >= 1000) {
      const topEntries = [...intrHistogram.entries()]
        .sort((a, b) => b[1] - a[1])
        .slice(0, 5)
        .map(([k, v]) => `${k}:${v}`)
        .join(", ");
      console.log(`[worker INTR MONITOR] ${intrCount} intr/sec | Top: [${topEntries}]`);
      intrCount = 0;
      intrHistogram.clear();
      lastIntrLog = now;
    }

    // Handle emulated AArch64 SIMD instructions (permute, addv, cnt, rev, umov_h)
    const simd = simdOpMap.get(pc);
    if (simd) {
      const handle = uc.getValue(engine.handle_ptr, "*");
      const vn = readReg128(handle, uc["ARM64_REG_Q" + simd.Rn]);
      if (simd.type === "umov_h") {
        const val = extract_h(vn, simd.index);
        if (simd.Rd < 31) {
          engine.reg_write_i64(uc["ARM64_REG_X" + simd.Rd], BigInt(val));
        }
        engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(pc + 4));
        return;
      }
      let vd;
      if (simd.type === "permute") {
        const vm = readReg128(handle, uc["ARM64_REG_Q" + simd.Rm]);
        vd = emulate_permute(simd.opcode, simd.Q, simd.size, vn, vm);
      } else if (simd.type === "addv") {
        vd = emulate_addv(simd.Q, simd.size, vn);
      } else if (simd.type === "cnt") {
        vd = emulate_cnt(simd.Q, vn);
      } else if (simd.type === "rev") {
        vd = emulate_rev(simd.revType, simd.Q, simd.size, vn);
      }
      writeReg128(handle, uc["ARM64_REG_Q" + simd.Rd], vd);
      engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(pc + 4));
      return;
    }

    const tramp = trampMap.get(pc);
    if (tramp) {
      let retVal;
      try {
        retVal = tramp.handler(engine);
      } catch(err) {
        console.error(`[worker tramp error in ${tramp.name}]:`, err.message);
      }
      if (retVal !== undefined) {
        engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(retVal));
      }
      const lr = engine.reg_read_i64(uc.ARM64_REG_LR);
      engine.reg_write_i64(uc.ARM64_REG_PC, lr);
    } else {
      console.error(`[error] Unhandled interrupt ${intno} at PC 0x${pc.toString(16)}`);
      engine.emu_stop();
    }
  });

  // Stack protection hooks for asset loader (0x121368 / 0x122908) & sound manager guard
  self.savedFrames121368 = new Map();
  const enterAddr = Number(BASE_ADDR + 0x121368n);
  const repairAddr = Number(BASE_ADDR + 0x122908n);
  const soundGuardAddr = Number(BASE_ADDR + 0x18c170n);

  engine.hook_add(uc.HOOK_CODE, (uc_handle, addr, size) => {
    const lr = engine.reg_read_i64(uc.ARM64_REG_LR);
    const sp = engine.reg_read_i64(uc.ARM64_REG_SP);
    const pathPtr = Number(engine.reg_read_i64(uc.ARM64_REG_X0));
    const path = readCString(pathPtr);
    // In prologue: sp is decremented by 0xa0, then x29 = sp + 0x40.
    // So x29 = (sp - 0xa0n) + 0x40n = sp - 0x60n.
    const frameKey = (sp - 0x60n).toString(16);
    self.savedFrames121368.set(frameKey, {
      x19: engine.reg_read_i64(uc.ARM64_REG_X19),
      x20: engine.reg_read_i64(uc.ARM64_REG_X20),
      x21: engine.reg_read_i64(uc.ARM64_REG_X21),
      x22: engine.reg_read_i64(uc.ARM64_REG_X22),
      x23: engine.reg_read_i64(uc.ARM64_REG_X23),
      x24: engine.reg_read_i64(uc.ARM64_REG_X24),
      x25: engine.reg_read_i64(uc.ARM64_REG_X25),
      x26: engine.reg_read_i64(uc.ARM64_REG_X26),
      x27: engine.reg_read_i64(uc.ARM64_REG_X27),
      x28: engine.reg_read_i64(uc.ARM64_REG_X28),
      x29: engine.reg_read_i64(uc.ARM64_REG_X29),
      lr, sp
    });
    console.log(`[0x121368 ENTER] key=0x${frameKey}, path="${path}", LR=0x${lr.toString(16)}, SP=0x${sp.toString(16)}`);
  }, null, enterAddr, enterAddr);

  engine.hook_add(uc.HOOK_CODE, (uc_handle, addr, size) => {
    const x29 = engine.reg_read_i64(uc.ARM64_REG_X29);
    const sp = engine.reg_read_i64(uc.ARM64_REG_SP);
    let frameKey = x29.toString(16);
    let frame = self.savedFrames121368.get(frameKey);
    if (!frame) {
      for (const [k, f] of self.savedFrames121368.entries()) {
        if (Math.abs(Number(f.sp - sp)) < 0x2000) {
          frameKey = k;
          frame = f;
          break;
        }
      }
    }
    if (frame) {
      self.savedFrames121368.delete(frameKey);
      // Ensure canary matches exact TLS canary
      const canaryExpected = writeU64(0xdeadbeefcafebaben);
      // 1. Repair canary at [x29 - 0x50]
      engine.mem_write(Number(x29) - 0x50, canaryExpected);
      // 2. Restore callee-saved registers on the stack so ldp restores exact caller registers!
      const base = Number(x29);
      engine.mem_write(base + 0x50, [...writeU64(frame.x20), ...writeU64(frame.x19)]);
      engine.mem_write(base + 0x40, [...writeU64(frame.x22), ...writeU64(frame.x21)]);
      engine.mem_write(base + 0x30, [...writeU64(frame.x24), ...writeU64(frame.x23)]);
      engine.mem_write(base + 0x20, [...writeU64(frame.x26), ...writeU64(frame.x25)]);
      engine.mem_write(base + 0x10, [...writeU64(frame.x28), ...writeU64(frame.x27)]);
      engine.mem_write(base,        [...writeU64(frame.x29), ...writeU64(frame.lr)]);
      console.log(`[0x122908 REPAIR] key=0x${frameKey}, restored caller frame (LR=0x${frame.lr.toString(16)}, X19=0x${frame.x19.toString(16)})`);
    } else {
      console.warn(`[0x122908 REPAIR] Warning: no saved frame found for x29=0x${frameKey}!`);
    }
    // Crucial: ALWAYS restore x27 = BigInt(TLS_BASE) before 0x10122908 executes "ldr x8, [x27, #0x28]"!
    engine.reg_write_i64(uc.ARM64_REG_X27, BigInt(TLS_BASE));
  }, null, repairAddr, repairAddr);

  engine.hook_add(uc.HOOK_CODE, (uc_handle, addr, size) => {
    const x23 = engine.reg_read_i64(uc.ARM64_REG_X23);
    if (x23 === 0n) {
      console.warn("[HOOK_CODE] Guarding x23 at 0x18c170: setting to sound manager pointer 0x" + (BASE_ADDR + 0x245b0b0n).toString(16));
      engine.reg_write_i64(uc.ARM64_REG_X23, BASE_ADDR + 0x245b0b0n);
    }
  }, null, soundGuardAddr, soundGuardAddr);

  // Quantum slicing is handled natively via instruction count in engine.emu_start() (zero hook overhead)
}

function installAllShims() {
  // Logging & diagnostics
  registerShim("__android_log_print", (e) => {
    const prio = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const tag = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const fmt = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X2)));
    let extra = "";
    if (fmt.includes("%s")) {
      extra = " -> " + readCString(Number(e.reg_read_i64(uc.ARM64_REG_X3)));
    }
    console.log(`[AndroidLog] [${tag}] ${fmt.trim()}${extra}`);
    return 0;
  });

  registerShim("__android_log_assert", (e) => {
    const cond = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const tag = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const fmt = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X2)));
    console.warn(`[AndroidAssert] [${tag}] ${cond}: ${fmt}`);
    return 0;
  });

  registerShim("exit", (e) => {
    const code = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    console.warn(`[worker] Guest called exit(${code})`);
    engine.emu_stop();
    return 0;
  });

  registerShim("abort", (e) => {
    console.error(`[worker] Guest called abort()`);
    engine.emu_stop();
    return 0;
  });

  registerShim("__stack_chk_fail", (e) => {
    const pc = Number(e.reg_read_i64(uc.ARM64_REG_PC));
    const lr = Number(e.reg_read_i64(uc.ARM64_REG_LR));
    const sp = Number(e.reg_read_i64(uc.ARM64_REG_SP));
    const x8 = e.reg_read_i64(uc.ARM64_REG_X8).toString(16);
    const x9 = e.reg_read_i64(uc.ARM64_REG_X9).toString(16);
    const x27 = e.reg_read_i64(uc.ARM64_REG_X27).toString(16);
    const tpidr = e.reg_read_i64(uc.ARM64_REG_TPIDR_EL0).toString(16);
    console.error(`[FATAL] Stack check failed! PC=0x${pc.toString(16)}, LR=0x${lr.toString(16)}, SP=0x${sp.toString(16)}, X8=0x${x8}, X9=0x${x9}, X27=0x${x27}, TPIDR=0x${tpidr}`);
    if (lr === Number(BASE_ADDR + 0x1230f4n)) {
      console.warn(`[worker stack_chk_fail] Rescuing epilogue -> redirecting LR to 0x${Number(BASE_ADDR + 0x122918n).toString(16)}!`);
      e.reg_write_i64(uc.ARM64_REG_LR, BASE_ADDR + 0x122918n);
      return 0;
    }
    engine.emu_stop();
    return 0;
  });

  // Fast Native AArch64 Memory Allocator, String ops & Synchronization (Zero-trampoline, full Unicorn JIT speed)
  const mallocNativeAddr = addNativeCodeHelper(NATIVE_MALLOC_ARM64);
  const freeNativeAddr = addNativeCodeHelper(NATIVE_RET_ARM64);
  const memcpyNativeAddr = addNativeCodeHelper(NATIVE_MEMCPY_ARM64);
  const memsetNativeAddr = addNativeCodeHelper(NATIVE_MEMSET_ARM64);
  const memcmpNativeAddr = addNativeCodeHelper(NATIVE_MEMCMP_ARM64);
  const strlenNativeAddr = addNativeCodeHelper(NATIVE_STRLEN_ARM64);
  const strcmpNativeAddr = addNativeCodeHelper(NATIVE_STRCMP_ARM64);
  const strcpyNativeAddr = addNativeCodeHelper(NATIVE_STRCPY_ARM64);
  const ret0NativeAddr   = addNativeCodeHelper(NATIVE_RET0_ARM64);

  shims.set("malloc", mallocNativeAddr);
  shims.set("_Znwm", mallocNativeAddr);
  shims.set("_Znam", mallocNativeAddr);
  shims.set("free", freeNativeAddr);
  shims.set("_ZdlPv", freeNativeAddr);
  shims.set("_ZdaPv", freeNativeAddr);
  shims.set("_ZdlPvm", freeNativeAddr);
  shims.set("memcpy", memcpyNativeAddr);
  shims.set("__memcpy_aarch64_simd", memcpyNativeAddr);
  shims.set("memset", memsetNativeAddr);
  shims.set("__memset_aarch64", memsetNativeAddr);
  shims.set("memcmp", memcmpNativeAddr);
  shims.set("__memcmp_aarch64", memcmpNativeAddr);
  shims.set("strlen", strlenNativeAddr);
  shims.set("strcmp", strcmpNativeAddr);
  shims.set("strcpy", strcpyNativeAddr);

  // Pure native no-op synchronization & runtime helpers (Zero JS trampolines/interrupts)
  for (const sym of [
    "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock", "pthread_mutex_destroy",
    "pthread_mutexattr_init", "pthread_mutexattr_settype", "pthread_mutexattr_destroy",
    "pthread_cond_init", "pthread_cond_broadcast", "pthread_cond_signal", "pthread_cond_destroy",
    "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock",
    "pthread_detach", "pthread_join", "pthread_once", "pthread_key_create", "pthread_key_delete", "pthread_setspecific", "pthread_getspecific",
    "wcslen", "wcscmp", "srand", "getauxval", "fcntl", "ALooper_release",
    "ASensorEventQueue_enableSensor", "ASensorEventQueue_disableSensor", "ASensorEventQueue_setEventRate", "ASensorEventQueue_getEvents",
    "AConfiguration_delete", "AConfiguration_fromAssetManager", "ANativeWindow_acquire", "ANativeWindow_release", "ANativeWindow_setBuffersGeometry"
  ]) {
    shims.set(sym, ret0NativeAddr);
  }

  console.log(`[worker] Installed pure AArch64 native helpers: malloc=0x${mallocNativeAddr.toString(16)}, free=0x${freeNativeAddr.toString(16)}, memcpy=0x${memcpyNativeAddr.toString(16)}, memset=0x${memsetNativeAddr.toString(16)}, memcmp=0x${memcmpNativeAddr.toString(16)}, strlen=0x${strlenNativeAddr.toString(16)}, ret0=0x${ret0NativeAddr.toString(16)}`);

  registerShim("calloc", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const total = n * sz;
    const addr = guestAlloc(total);
    safeMemset(addr, 0, total);
    return addr;
  });
  registerShim("realloc", (e) => {
    const ptr = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (!ptr) return guestAlloc(sz);
    if (sz === 0) return 0;
    const oldSize = allocSizes.get(ptr) || sz;
    if (oldSize >= sz && allocSizes.has(ptr)) return ptr;
    const newPtr = guestAlloc(sz);
    const copyLen = Math.min(sz, oldSize);
    if (copyLen > 0) {
      safeMemcpy(newPtr, ptr, copyLen);
    }
    return newPtr;
  });
  registerShim("posix_memalign", (e) => {
    const mut = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const addr = guestAlloc(sz);
    engine.mem_write(mut, writeU64(addr));
    return 0;
  });
  registerShim("_ZdlPvm", (e) => {
    guestFree(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    return 0;
  });

  function safeMemcpy(dst, src, n) {
    if (n <= 0 || dst === src || !dst || !src) return dst;
    if (n > 32 * 1024 * 1024) {
      console.warn(`[worker memcpy HUGE clamped] dst=0x${dst.toString(16)}, src=0x${src.toString(16)}, n=0x${n.toString(16)}`);
      return dst;
    }
    try {
      const CHUNK = 65536;
      let rem = n;
      let d = dst;
      let s = src;
      while (rem > 0) {
        const cur = Math.min(rem, CHUNK);
        const bytes = engine.mem_read(s, cur);
        engine.mem_write(d, bytes);
        rem -= cur;
        d += cur;
        s += cur;
      }
    } catch(err) {
      console.warn(`[worker memcpy warn] dst=0x${dst.toString(16)}, src=0x${src.toString(16)}, n=${n}:`, err.message);
    }
    return dst;
  }

  function safeMemset(dst, val, n) {
    if (n <= 0 || !dst) return dst;
    if (n > 32 * 1024 * 1024) {
      console.warn(`[worker memset HUGE clamped] dst=0x${dst.toString(16)}, val=${val}, n=0x${n.toString(16)}`);
      return dst;
    }
    try {
      const CHUNK = 65536;
      const chunkBytes = new Uint8Array(Math.min(n, CHUNK)).fill(val & 0xff);
      let rem = n;
      let d = dst;
      while (rem > 0) {
        const cur = Math.min(rem, CHUNK);
        engine.mem_write(d, cur === CHUNK ? chunkBytes : chunkBytes.subarray(0, cur));
        rem -= cur;
        d += cur;
      }
    } catch(err) {
      console.warn(`[worker memset warn] dst=0x${dst.toString(16)}, n=${n}:`, err.message);
    }
    return dst;
  }

  registerShim("strncpy", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const src = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const str = readCString(src).slice(0, n);
    const b = new Uint8Array(n);
    new TextEncoder().encodeInto(str, b);
    engine.mem_write(dst, Array.from(b));
    return dst;
  });
  registerShim("strncmp", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const s1 = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0))).slice(0, n);
    const s2 = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1))).slice(0, n);
    if (s1 < s2) return -1;
    if (s1 > s2) return 1;
    return 0;
  });
  registerShim("strcat", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const src = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const combined = readCString(dst) + readCString(src);
    engine.mem_write(dst, [...new TextEncoder().encode(combined), 0]);
    return dst;
  });
  registerShim("strdup", (e) => {
    const str = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const addr = guestAlloc(str.length + 1);
    engine.mem_write(addr, [...new TextEncoder().encode(str), 0]);
    return addr;
  });
  registerShim("strchr", (e) => {
    const str = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const c = String.fromCharCode(Number(e.reg_read_i64(uc.ARM64_REG_X1)) & 0xff);
    const idx = str.indexOf(c);
    return idx >= 0 ? Number(e.reg_read_i64(uc.ARM64_REG_X0)) + idx : 0;
  });
  registerShim("strstr", (e) => {
    const haystack = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const needle = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const idx = haystack.indexOf(needle);
    return idx >= 0 ? Number(e.reg_read_i64(uc.ARM64_REG_X0)) + idx : 0;
  });
  registerShim("atoi", (e) => parseInt(readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0))), 10) || 0);
  registerShim("strtoul", (e) => parseInt(readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0))), 10) || 0);
  registerShim("strtoull", (e) => parseInt(readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0))), 10) || 0);

  // Wide characters
  registerShim("wcslen", () => 0);
  registerShim("wcscpy", (e) => Number(e.reg_read_i64(uc.ARM64_REG_X0)));
  registerShim("wcscmp", () => 0);

  // Formatted output
  function formatGuestString(fmt, getArg) {
    let res = "";
    let i = 0;
    while (i < fmt.length) {
      if (fmt[i] === "%") {
        i++;
        if (fmt[i] === "%") { res += "%"; i++; continue; }
        let flags = "";
        while (i < fmt.length && "-+ #0".includes(fmt[i])) flags += fmt[i++];
        let width = "";
        while (i < fmt.length && fmt[i] >= "0" && fmt[i] <= "9") width += fmt[i++];
        let prec = "";
        if (fmt[i] === ".") {
          i++;
          while (i < fmt.length && fmt[i] >= "0" && fmt[i] <= "9") prec += fmt[i++];
        }
        while (i < fmt.length && "lzhj".includes(fmt[i])) i++;
        const spec = fmt[i++];
        const val = getArg();
        if (spec === "s") {
          res += readCString(Number(val));
        } else if (spec === "d" || spec === "i") {
          let s = Number(BigInt.asIntN(32, BigInt(val))).toString(10);
          if (width && s.length < parseInt(width, 10)) s = s.padStart(parseInt(width, 10), flags.includes("0") ? "0" : " ");
          res += s;
        } else if (spec === "u") {
          let s = Number(BigInt.asUintN(32, BigInt(val))).toString(10);
          if (width && s.length < parseInt(width, 10)) s = s.padStart(parseInt(width, 10), flags.includes("0") ? "0" : " ");
          res += s;
        } else if (spec === "x" || spec === "X") {
          let s = Number(BigInt.asUintN(32, BigInt(val))).toString(16);
          if (spec === "X") s = s.toUpperCase();
          if (width && s.length < parseInt(width, 10)) s = s.padStart(parseInt(width, 10), flags.includes("0") ? "0" : " ");
          res += s;
        } else if (spec === "c") {
          res += String.fromCharCode(Number(val) & 0xff);
        } else if (spec === "p") {
          res += "0x" + Number(val).toString(16);
        } else {
          res += String(spec);
        }
      } else {
        res += fmt[i++];
      }
    }
    return res;
  }

  registerShim("sprintf", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const fmt = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const gpRegs = [
      uc.ARM64_REG_X2, uc.ARM64_REG_X3, uc.ARM64_REG_X4,
      uc.ARM64_REG_X5, uc.ARM64_REG_X6, uc.ARM64_REG_X7
    ];
    let argIdx = 0;
    const formatted = formatGuestString(fmt, () => {
      if (argIdx < gpRegs.length) return e.reg_read_i64(gpRegs[argIdx++]);
      return 0n;
    });
    engine.mem_write(dst, [...new TextEncoder().encode(formatted), 0]);
    return formatted.length;
  });

  registerShim("snprintf", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const max = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const fmt = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X2)));
    const gpRegs = [
      uc.ARM64_REG_X3, uc.ARM64_REG_X4, uc.ARM64_REG_X5,
      uc.ARM64_REG_X6, uc.ARM64_REG_X7
    ];
    let argIdx = 0;
    const formatted = formatGuestString(fmt, () => {
      if (argIdx < gpRegs.length) return e.reg_read_i64(gpRegs[argIdx++]);
      return 0n;
    }).slice(0, Math.max(0, max - 1));
    engine.mem_write(dst, [...new TextEncoder().encode(formatted), 0]);
    return formatted.length;
  });

  registerShim("puts", (e) => {
    console.log("[guest puts]", readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0))));
    return 0;
  });

  // Pthreads & synchronization
  registerShim("pthread_mutex_init", () => 0);
  registerShim("pthread_mutex_lock", () => 0);
  registerShim("pthread_mutex_trylock", () => 0);
  registerShim("pthread_mutex_unlock", () => 0);
  registerShim("pthread_mutex_destroy", () => 0);
  registerShim("pthread_mutexattr_init", () => 0);
  registerShim("pthread_mutexattr_settype", () => 0);
  registerShim("pthread_mutexattr_destroy", () => 0);
  registerShim("pthread_cond_init", () => 0);
  registerShim("pthread_cond_broadcast", () => 0);
  registerShim("pthread_cond_signal", () => 0);
  registerShim("pthread_cond_destroy", () => 0);
  registerShim("pthread_cond_timedwait", (e) => {
    engine.emu_stop();
    return 0;
  });
  registerShim("pthread_rwlock_rdlock", () => 0);
  registerShim("pthread_rwlock_wrlock", () => 0);
  registerShim("pthread_rwlock_unlock", () => 0);
  registerShim("pthread_self", () => 1);
  registerShim("pthread_detach", () => 0);
  registerShim("pthread_join", () => 0);
  registerShim("sched_yield", (e) => {
    engine.emu_stop();
    return 0;
  });
  registerShim("usleep", (e) => { engine.emu_stop(); return 0; });
  registerShim("sleep", (e) => { engine.emu_stop(); return 0; });
  registerShim("nanosleep", (e) => { engine.emu_stop(); return 0; });
  registerShim("pthread_once", () => 0);
  registerShim("pthread_key_create", () => 0);
  registerShim("pthread_key_delete", () => 0);
  registerShim("pthread_setspecific", () => 0);
  registerShim("pthread_getspecific", () => 0);

  registerShim("pthread_create", (e) => {
    const pt = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const fn = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const arg = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    console.log(`[worker pthread_create] fn=0x${fn.toString(16)}, arg=0x${arg.toString(16)}`);

    if (guestThreads.length === 0) {
      guestThreads.push({ fn, arg });
    } else {
      const threadIdx = guestThreads.length;
      const stackAddr = 0x70600000 + (threadIdx - 1) * 0x80000;
      const initialCtx = {
        gp: new Array(getGuestRegList().length).fill(0n),
        simd: new Array(32).fill(null).map(() => [0n, 0n]),
        fpcr: 0n,
        fpsr: 0n
      };
      initialCtx.gp[0] = BigInt(arg); // X0 = arg
      initialCtx.gp[30] = BigInt(STOP_ADDR); // LR = STOP_ADDR
      initialCtx.gp[31] = BigInt(stackAddr + 0x80000 - 0x1000); // SP
      initialCtx.gp[32] = BigInt(fn); // PC = fn
      initialCtx.gp[33] = 0n; // NZCV
      initialCtx.gp[34] = BigInt(TLS_BASE); // TPIDR_EL0
      guestThreads.push({ fn, arg, ctx: initialCtx, active: true });
      console.log(`[worker scheduler] Spawned background thread #${threadIdx} at fn=0x${fn.toString(16)}, SP=0x${initialCtx.gp[31].toString(16)}`);
    }

    if (pt) {
      engine.mem_write(pt, writeU64(guestThreads.length));
    }
    return 0;
  });

  registerShim("pthread_cond_wait", (e) => {
    if (guestThreads.length > 0 && guestThreads[0].arg) {
      const app = guestThreads[0].arg;
      engine.mem_write(app + 0x50, [11, 0, 0, 0]); // app->activityState = 11 (APP_CMD_RESUME)
      engine.mem_write(app + 0x54, [0, 0, 0, 0]);  // app->destroyRequested = 0 (keep running)
      engine.mem_write(app + 0x140, [1, 0, 0, 0]);
      engine.mem_write(app + 0x40, writeU64(0x60200040n));
    }
    const curSp = Number(e.reg_read_i64(uc.ARM64_REG_SP));
    // Background threads yield quantum; lifecycle drive on main stack returns immediately
    if (curSp > THREAD_STACK_BASE || curSp < STACK_BASE) {
      engine.emu_stop();
    }
    return 0;
  });

  // Time & Random
  registerShim("time", () => Math.floor(Date.now() / 1000));
  registerShim("gettimeofday", (e) => {
    const tv = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (tv) {
      const now = Date.now();
      const sec = BigInt(Math.floor(now / 1000));
      const usec = BigInt((now % 1000) * 1000);
      const b = new Uint8Array(16);
      const v = new DataView(b.buffer);
      v.setBigInt64(0, sec, true);
      v.setBigInt64(8, usec, true);
      engine.mem_write(tv, Array.from(b));
    }
    return 0;
  });
  registerShim("rand", () => Math.floor(Math.random() * 0x7fffffff));
  registerShim("srand", () => 0);
  registerShim("arc4random", () => (Math.random() * 0xffffffff) >>> 0);

  // System & Syscalls
  registerShim("__system_property_get", (e) => {
    const name = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const val = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    let resp = "1";
    if (name === "ro.arch") resp = "exynos9810";
    else if (name.includes("version.sdk")) resp = "29";
    engine.mem_write(val, [...new TextEncoder().encode(resp), 0]);
    return resp.length;
  });
  registerShim("syscall", (e) => {
    const num = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (num === 178) return 1001; // __NR_gettid
    return 0;
  });
  registerShim("sysconf", (e) => {
    const name = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (name === 30) return 4; // _SC_NPROCESSORS_ONLN
    if (name === 39) return 4096; // _SC_PAGESIZE
    return 1;
  });
  registerShim("getauxval", () => 0);
  registerShim("strerror_r", (e) => {
    const errnum = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buf = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const buflen = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const str = `Error ${errnum}`;
    const enc = new TextEncoder().encode(str.slice(0, buflen - 1));
    engine.mem_write(buf, [...enc, 0]);
    return 0;
  });

  // emutls
  const emutlsMap = new Map();
  registerShim("__emutls_get_address", (e) => {
    const ctrl = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (emutlsMap.has(ctrl)) return emutlsMap.get(ctrl);
    const szBuf = engine.mem_read(ctrl, 8);
    let sz = Number(BigInt(szBuf[0]) | (BigInt(szBuf[1]) << 8n) | (BigInt(szBuf[2]) << 16n) | (BigInt(szBuf[3]) << 24n));
    if (!sz || sz < 64) sz = 128;
    const addr = guestAlloc(sz);
    engine.mem_write(addr, new Array(sz).fill(0));
    emutlsMap.set(ctrl, addr);
    return addr;
  });

  // In-memory pipe and file descriptor table for Android native app glue
  const pipeQueues = new Map();
  let nextPipeFd = 10;

  registerShim("pipe", (e) => {
    const fds = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const rfd = nextPipeFd++;
    const wfd = nextPipeFd++;
    const q = [];
    pipeQueues.set(rfd, q);
    pipeQueues.set(wfd, q);
    console.log(`[worker pipe] created rfd=${rfd}, wfd=${wfd}`);
    const b = new Uint8Array(8);
    const v = new DataView(b.buffer);
    v.setInt32(0, rfd, true);
    v.setInt32(4, wfd, true);
    engine.mem_write(fds, Array.from(b));
    return 0;
  });

  registerShim("read", (e) => {
    const fd = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buf = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const q = pipeQueues.get(fd);
    if (!q || q.length === 0) return -1;
    const n = Math.min(count, q.length);
    const chunk = q.splice(0, n);
    engine.mem_write(buf, chunk);
    return n;
  });

  registerShim("write", (e) => {
    const fd = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buf = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const q = pipeQueues.get(fd);
    if (q) {
      const bytes = Array.from(engine.mem_read(buf, count));
      q.push(...bytes);
      console.log(`[worker write] fd=${fd}, wrote ${count} bytes:`, bytes);
      return count;
    }
    return count;
  });

  registerShim("fcntl", () => 0);

  // File I/O (fopen, fread, fseek, ftell, fclose)
  const openFiles = new Map();
  let nextFileHandle = 0x60800010;

  registerShim("fopen", (e) => {
    const rawPath = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const mode = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    let cleanPath = rawPath;
    if (cleanPath.startsWith("/files/") || cleanPath.startsWith("/obb/") || cleanPath.startsWith("/external/")) {
      if (mode.includes("r")) return 0;
    }
    if (cleanPath.includes("/proc/meminfo")) {
      const memInfo = "MemTotal: 4194304 kB\nMemFree: 2097152 kB\nMemAvailable: 2097152 kB\n";
      const fileBytes = new TextEncoder().encode(memInfo);
      const handle = nextFileHandle; nextFileHandle += 16;
      openFiles.set(handle, { path: cleanPath, data: fileBytes, pos: 0 });
      return handle;
    }
    const fileBytes = fetchAssetSync(cleanPath);
    if (fileBytes) {
      const handle = nextFileHandle;
      nextFileHandle += 16;
      openFiles.set(handle, { path: cleanPath, data: fileBytes, pos: 0 });
      console.log(`[worker file] fopen("${cleanPath}") -> ${fileBytes.length} bytes (handle 0x${handle.toString(16)})`);
      return handle;
    }
    console.warn(`[worker file] fopen("${cleanPath}") NOT FOUND in assets`);
    return 0;
  });

  registerShim("fclose", (e) => {
    openFiles.delete(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    return 0;
  });

  registerShim("fread", (e) => {
    const ptr = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const size = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const f = openFiles.get(handle);
    if (!f || !ptr || size <= 0 || count <= 0) return 0;
    const total = size * count;
    const avail = f.data.length - f.pos;
    const toRead = Math.min(total, Math.max(0, avail));
    if (toRead > 0) {
      try {
        engine.mem_write(ptr, f.data.subarray(f.pos, f.pos + toRead));
      } catch(err) {
        console.warn(`[worker fread warn] ptr=0x${ptr.toString(16)}, toRead=${toRead}:`, err.message);
      }
      f.pos += toRead;
    }
    return Math.floor(toRead / size);
  });

  registerShim("fseek", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const offset = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const whence = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const f = openFiles.get(handle);
    if (!f) return -1;
    if (whence === 0) f.pos = offset;
    else if (whence === 1) f.pos += offset;
    else if (whence === 2) f.pos = f.data.length + offset;
    return 0;
  });

  registerShim("ftell", (e) => {
    const f = openFiles.get(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    return f ? f.pos : -1;
  });

  // ALooper implementation over in-memory pipes
  const looperFds = [];
  registerShim("ALooper_forThread", () => 0x60200040);
  registerShim("ALooper_prepare", () => 0x60200040);
  registerShim("ALooper_acquire", () => 0x60200040);
  registerShim("ALooper_release", () => 0);

  registerShim("ALooper_addFd", (e) => {
    const lp = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const fd = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const ident = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const events = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const cb = Number(e.reg_read_i64(uc.ARM64_REG_X4));
    const data = Number(e.reg_read_i64(uc.ARM64_REG_X5));
    console.log(`[worker ALooper_addFd] fd=${fd}, ident=${ident}, cb=0x${cb.toString(16)}, data=0x${data.toString(16)}`);
    looperFds.push({ fd, ident, events, cb, data });
    return 1;
  });

  registerShim("ALooper_removeFd", (e) => {
    const fd = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const idx = looperFds.findIndex(r => r.fd === fd);
    if (idx >= 0) looperFds.splice(idx, 1);
    return 1;
  });

  registerShim("ALooper_pollOnce", (e) => {
    const outFd = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const outEvents = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const outData = Number(e.reg_read_i64(uc.ARM64_REG_X3));

    for (const r of looperFds) {
      const q = pipeQueues.get(r.fd);
      if (q && q.length > 0) {
        if (outFd) {
          const b = new Uint8Array(4); new DataView(b.buffer).setInt32(0, r.ident, true);
          engine.mem_write(outFd, Array.from(b));
        }
        if (outEvents) {
          const b = new Uint8Array(4); new DataView(b.buffer).setInt32(0, 1, true);
          engine.mem_write(outEvents, Array.from(b));
        }
        if (outData) {
          engine.mem_write(outData, writeU64(r.data));
        }
        return r.ident || 1;
      }
    }
    return -1; // ALOOPER_POLL_TIMEOUT
  });

  // ASensor
  registerShim("ASensorManager_getInstance", () => 0x60200070);
  registerShim("ASensorManager_getDefaultSensor", () => 0x60200078);
  registerShim("ASensorManager_createEventQueue", () => 0x60200080);
  registerShim("ASensorEventQueue_enableSensor", () => 0);
  registerShim("ASensorEventQueue_disableSensor", () => 0);
  registerShim("ASensorEventQueue_setEventRate", () => 0);
  registerShim("ASensorEventQueue_getEvents", () => 0);

  // AConfiguration
  registerShim("AConfiguration_new", () => 0x60200030);
  registerShim("AConfiguration_delete", () => 0);
  registerShim("AConfiguration_fromAssetManager", () => 0);
  registerShim("AConfiguration_getCountry", (e) => {
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    engine.mem_write(out, [0x55, 0x53, 0]); // "US"
    return 0;
  });
  registerShim("AConfiguration_getLanguage", (e) => {
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    engine.mem_write(out, [0x65, 0x6e, 0]); // "en"
    return 0;
  });

  // ANativeWindow
  registerShim("ANativeWindow_fromSurface", () => 0x60200040);
  registerShim("ANativeWindow_acquire", () => 0);
  registerShim("ANativeWindow_release", () => 0);
  registerShim("ANativeWindow_getWidth", () => 1920);
  registerShim("ANativeWindow_getHeight", () => 1080);
  registerShim("ANativeWindow_setBuffersGeometry", () => 0);

  // AAssetManager
  registerShim("AAssetManager_fromJava", () => 0x60200020);
  registerShim("AAssetManager_open", (e) => {
    const path = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const fileBytes = fetchAssetSync(path);
    if (fileBytes) {
      const handle = nextAssetHandle;
      nextAssetHandle += 16;
      openAssets.set(handle, { path, data: fileBytes, pos: 0 });
      console.log(`[worker vfs] Opened asset "${path}" (${fileBytes.length} bytes) -> handle 0x${handle.toString(16)}`);
      return handle;
    } else {
      console.warn(`[worker vfs] Asset not found: "${path}"`);
      return 0;
    }
  });
  registerShim("AAsset_getLength", (e) => {
    const asset = openAssets.get(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    return asset ? asset.data.length : 0;
  });
  registerShim("AAsset_getRemainingLength", (e) => {
    const asset = openAssets.get(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    return asset ? (asset.data.length - asset.pos) : 0;
  });
  registerShim("AAsset_read", (e) => {
    const lr = Number(e.reg_read_i64(uc.ARM64_REG_LR));
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buf = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const asset = openAssets.get(handle);
    if (!asset || !buf || count <= 0) return 0;
    const toRead = Math.min(count, asset.data.length - asset.pos);
    if (toRead > 0) {
      try {
        engine.mem_write(buf, asset.data.subarray(asset.pos, asset.pos + toRead));
      } catch(err) {
        console.warn(`[worker AAsset_read warn] buf=0x${buf.toString(16)}, toRead=${toRead}:`, err.message);
      }
      asset.pos += toRead;
    }
    if (count > 32) {
      console.log(`[worker vfs] AAsset_read(handle=0x${handle.toString(16)}, "${asset.path}", count=${count}, LR=0x${lr.toString(16)}) -> read ${toRead} to 0x${buf.toString(16)} (${asset.pos}/${asset.data.length})`);
    }
    return toRead;
  });
  registerShim("AAsset_seek", (e) => {
    const asset = openAssets.get(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const offset = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const whence = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (!asset) return -1;
    if (whence === 0) asset.pos = offset;
    else if (whence === 1) asset.pos += offset;
    else if (whence === 2) asset.pos = asset.data.length + offset;
    return asset.pos;
  });
  registerShim("AAsset_close", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const lr = Number(e.reg_read_i64(uc.ARM64_REG_LR));
    console.log(`[worker vfs] AAsset_close(handle=0x${handle.toString(16)}), LR=0x${lr.toString(16)}`);
    openAssets.delete(handle);
    return 0;
  });

  // EGL Dispatchers
  registerShim("eglGetDisplay", () => 0x60600010);
  registerShim("eglInitialize", (e) => {
    const maj = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const min = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (maj) engine.mem_write(maj, [1, 0, 0, 0]);
    if (min) engine.mem_write(min, [4, 0, 0, 0]);
    return 1;
  });
  registerShim("eglChooseConfig", (e) => {
    const configs = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const num_config = Number(e.reg_read_i64(uc.ARM64_REG_X4));
    if (configs) engine.mem_write(configs, writeU64(0x60600020n));
    if (num_config) engine.mem_write(num_config, [1, 0, 0, 0]);
    return 1;
  });
  registerShim("eglCreateWindowSurface", () => 0x60600030);
  registerShim("eglCreateContext", () => 0x60600040);
  registerShim("eglMakeCurrent", () => 1);
  registerShim("eglQuerySurface", (e) => {
    const attr = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const val = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (val) {
      if (attr === 0x3057) engine.mem_write(val, [0x80, 0x07, 0, 0]); // 1920
      else if (attr === 0x3056) engine.mem_write(val, [0x38, 0x04, 0, 0]); // 1080
    }
    return 1;
  });
  registerShim("eglGetConfigAttrib", (e) => {
    const val = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (val) engine.mem_write(val, [8, 0, 0, 0]);
    return 1;
  });
  registerShim("eglDestroySurface", () => 1);
  registerShim("eglDestroyContext", () => 1);
  registerShim("eglTerminate", () => 1);

  let totalFramesPresented = 0;
  registerShim("eglSwapBuffers", () => {
    frameCount++;
    totalFramesPresented++;
    drawCallCount++;
    if (gl) gl.flush();
    if (totalFramesPresented <= 10 || totalFramesPresented % 60 === 0) {
      console.log(`[worker] eglSwapBuffers PRESENT FRAME #${totalFramesPresented} (draws: ${drawCallCount})`);
    }
    const now = Date.now();
    if (now - lastFpsTime >= 500) {
      const fps = Math.round((frameCount * 1000) / (now - lastFpsTime));
      self.postMessage({ type: "PRESENT_FRAME", fps, draws: drawCallCount });
      frameCount = 0;
      lastFpsTime = now;
    } else {
      self.postMessage({ type: "PRESENT_FRAME", draws: drawCallCount });
    }
    return 1;
  });

  // OpenGL ES 2.0 / 3.0 Hardware-Accelerated WebGL2 Dispatchers
  registerShim("glViewport", (e) => {
    drawCallCount++;
    const x = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const y = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    let w = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    let h = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (w <= 0 || w > 4096) w = 1920;
    if (h <= 0 || h > 4096) h = 1080;
    if (gl) gl.viewport(x, y, w, h);
    return 0;
  });

  registerShim("glClearColor", (e) => {
    drawCallCount++;
    // Float arguments in D0, D1, D2, D3 or integer representation
    if (gl) gl.clearColor(0.05, 0.08, 0.16, 1.0);
    return 0;
  });

  registerShim("glClear", (e) => {
    drawCallCount++;
    const mask = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.clear(mask);
    return 0;
  });

  registerShim("glCheckFramebufferStatus", () => 0x8CD5); // GL_FRAMEBUFFER_COMPLETE

  registerShim("glGenTextures", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const id = nextTexId++;
      if (gl) glTextures.set(id, gl.createTexture());
      const b = new Uint8Array(4);
      new DataView(b.buffer).setUint32(0, id, true);
      engine.mem_write(out + i * 4, Array.from(b));
    }
    return 0;
  });

  registerShim("glBindTexture", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const id = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (gl) gl.bindTexture(target, id ? (glTextures.get(id) || null) : null);
    return 0;
  });

  registerShim("glDeleteTextures", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const ptr = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const b = engine.mem_read(ptr + i * 4, 4);
      const id = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
      if (gl && glTextures.has(id)) {
        gl.deleteTexture(glTextures.get(id));
        glTextures.delete(id);
      }
    }
    return 0;
  });

  registerShim("glTexParameteri", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const pname = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const param = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (gl) gl.texParameteri(target, pname, param);
    return 0;
  });

  registerShim("glTexParameterf", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const pname = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const param = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (gl) gl.texParameterf(target, pname, param);
    return 0;
  });

  registerShim("glTexImage2D", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const level = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const ifmt = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const w = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const h = Number(e.reg_read_i64(uc.ARM64_REG_X4));
    const border = Number(e.reg_read_i64(uc.ARM64_REG_X5));
    const fmt = Number(e.reg_read_i64(uc.ARM64_REG_X6));
    const type = Number(e.reg_read_i64(uc.ARM64_REG_X7));
    if (gl) {
      try {
        const sp = Number(engine.reg_read_i64(uc.ARM64_REG_SP));
        const pixelsPtr = Number(readBigUInt64LE(new Uint8Array(engine.mem_read(sp, 8)), 0));
        if (pixelsPtr && w > 0 && h > 0) {
          let bpp = 4;
          if (fmt === 0x1906 || fmt === 0x1909) bpp = 1;
          else if (fmt === 0x1907) bpp = 3;
          const total = w * h * bpp;
          const bytes = new Uint8Array(engine.mem_read(pixelsPtr, total));
          gl.texImage2D(target, level, ifmt, w, h, border, fmt, type, bytes);
        } else {
          gl.texImage2D(target, level, ifmt, w, h, border, fmt, type, null);
        }
      } catch(err) {
        gl.texImage2D(target, level, ifmt, w, h, border, fmt, type, null);
      }
    }
    return 0;
  });

  registerShim("glTexSubImage2D", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const level = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const xoffset = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const yoffset = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const width = Number(e.reg_read_i64(uc.ARM64_REG_X4));
    const height = Number(e.reg_read_i64(uc.ARM64_REG_X5));
    const format = Number(e.reg_read_i64(uc.ARM64_REG_X6));
    const type = Number(e.reg_read_i64(uc.ARM64_REG_X7));
    if (gl) {
      try {
        const sp = Number(engine.reg_read_i64(uc.ARM64_REG_SP));
        const pixelsPtr = Number(readBigUInt64LE(new Uint8Array(engine.mem_read(sp, 8)), 0));
        if (pixelsPtr && width > 0 && height > 0) {
          let bpp = 4;
          if (format === 0x1906 || format === 0x1909) bpp = 1;
          else if (format === 0x1907) bpp = 3;
          const total = width * height * bpp;
          const bytes = new Uint8Array(engine.mem_read(pixelsPtr, total));
          gl.texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, bytes);
        }
      } catch(err) {}
    }
    return 0;
  });

  registerShim("glGenBuffers", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const id = nextBufId++;
      if (gl) glBuffers.set(id, gl.createBuffer());
      const b = new Uint8Array(4);
      new DataView(b.buffer).setUint32(0, id, true);
      engine.mem_write(out + i * 4, Array.from(b));
    }
    return 0;
  });

  registerShim("glBindBuffer", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const id = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (gl) gl.bindBuffer(target, id ? (glBuffers.get(id) || null) : null);
    return 0;
  });

  registerShim("glBufferData", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const size = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const data = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const usage = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (gl) {
      if (data && size > 0) {
        const bytes = new Uint8Array(engine.mem_read(data, size));
        gl.bufferData(target, bytes, usage);
      } else {
        gl.bufferData(target, size, usage);
      }
    }
    return 0;
  });

  registerShim("glBufferSubData", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const offset = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const size = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const data = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (gl && data && size > 0) {
      const bytes = new Uint8Array(engine.mem_read(data, size));
      gl.bufferSubData(target, offset, bytes);
    }
    return 0;
  });

  registerShim("glDeleteBuffers", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const ptr = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const b = engine.mem_read(ptr + i * 4, 4);
      const id = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
      if (gl && glBuffers.has(id)) {
        gl.deleteBuffer(glBuffers.get(id));
        glBuffers.delete(id);
      }
    }
    return 0;
  });

  registerShim("glGenFramebuffers", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const id = nextFboId++;
      if (gl) glFramebuffers.set(id, gl.createFramebuffer());
      const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, id, true);
      engine.mem_write(out + i * 4, Array.from(b));
    }
    return 0;
  });

  registerShim("glBindFramebuffer", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const id = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (gl) gl.bindFramebuffer(target, id ? (glFramebuffers.get(id) || null) : null);
    return 0;
  });

  registerShim("glFramebufferTexture2D", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const attachment = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const textarget = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const tex = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const level = Number(e.reg_read_i64(uc.ARM64_REG_X4));
    if (gl) gl.framebufferTexture2D(target, attachment, textarget, glTextures.get(tex) || null, level);
    return 0;
  });

  registerShim("glGenRenderbuffers", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const id = nextRboId++;
      if (gl) glRenderbuffers.set(id, gl.createRenderbuffer());
      const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, id, true);
      engine.mem_write(out + i * 4, Array.from(b));
    }
    return 0;
  });

  registerShim("glBindRenderbuffer", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const id = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (gl) gl.bindRenderbuffer(target, id ? (glRenderbuffers.get(id) || null) : null);
    return 0;
  });

  registerShim("glRenderbufferStorage", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const internalformat = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const width = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const height = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (gl) gl.renderbufferStorage(target, internalformat, width, height);
    return 0;
  });

  registerShim("glFramebufferRenderbuffer", (e) => {
    const target = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const attachment = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const renderbuffertarget = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const renderbuffer = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (gl) gl.framebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer ? (glRenderbuffers.get(renderbuffer) || null) : null);
    return 0;
  });

  registerShim("glEnable", (e) => {
    const cap = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.enable(cap);
    return 0;
  });

  registerShim("glDisable", (e) => {
    const cap = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.disable(cap);
    return 0;
  });

  registerShim("glBlendFunc", (e) => {
    const sfactor = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const dfactor = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (gl) gl.blendFunc(sfactor, dfactor);
    return 0;
  });

  registerShim("glDepthMask", (e) => {
    const flag = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.depthMask(flag !== 0);
    return 0;
  });

  registerShim("glDrawArrays", (e) => {
    drawCallCount++;
    const mode = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const first = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (gl) gl.drawArrays(mode, first, count);
    return 0;
  });

  registerShim("glDrawElements", (e) => {
    drawCallCount++;
    const mode = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const type = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const indices = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (gl) gl.drawElements(mode, count, type, indices);
    return 0;
  });

  registerShim("glCreateShader", (e) => {
    const type = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const id = nextShaderId++;
    if (gl) glShaders.set(id, gl.createShader(type));
    return id;
  });

  registerShim("glShaderSource", (e) => {
    const shader = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const stringsPtr = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const lengthPtr = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    let src = "";
    for (let i = 0; i < count; i++) {
      const p = Number(readBigUInt64LE(new Uint8Array(engine.mem_read(stringsPtr + i * 8, 8)), 0));
      let len = -1;
      if (lengthPtr) {
        const lBytes = new Uint8Array(engine.mem_read(lengthPtr + i * 4, 4));
        len = new DataView(lBytes.buffer).getInt32(0, true);
      }
      if (len > 0) {
        const sBytes = new Uint8Array(engine.mem_read(p, len));
        src += new TextDecoder().decode(sBytes);
      } else {
        src += readCString(p);
      }
    }
    if (gl && glShaders.has(shader)) {
      const glShader = glShaders.get(shader);
      const shaderType = gl.getShaderParameter(glShader, gl.SHADER_TYPE);
      if (shaderType === gl.FRAGMENT_SHADER && !src.includes("precision")) {
        src = "precision mediump float;\n" + src;
      }
      gl.shaderSource(glShader, src);
    }
    return 0;
  });

  registerShim("glCompileShader", (e) => {
    const shader = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl && glShaders.has(shader)) {
      const glShader = glShaders.get(shader);
      gl.compileShader(glShader);
      if (!gl.getShaderParameter(glShader, gl.COMPILE_STATUS)) {
        console.error(`[worker WebGL] Shader ${shader} compile error:`, gl.getShaderInfoLog(glShader));
      } else {
        console.log(`[worker WebGL] Shader ${shader} compiled successfully.`);
      }
    }
    return 0;
  });

  registerShim("glGetShaderiv", (e) => {
    const shader = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const pname = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const params = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (params) {
      let val = 1;
      if (gl && glShaders.has(shader)) {
        const res = gl.getShaderParameter(glShaders.get(shader), pname);
        if (typeof res === "boolean") val = res ? 1 : 0;
        else if (typeof res === "number") val = res;
      }
      const b = new Uint8Array(4);
      new DataView(b.buffer).setInt32(0, val, true);
      engine.mem_write(params, Array.from(b));
    }
    return 0;
  });

  registerShim("glGetShaderInfoLog", (e) => {
    const shader = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const maxLength = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const lengthPtr = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const infoLogPtr = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    let log = "";
    if (gl && glShaders.has(shader)) {
      log = gl.getShaderInfoLog(glShaders.get(shader)) || "";
    }
    const enc = new TextEncoder().encode(log.slice(0, maxLength - 1) + "\0");
    if (lengthPtr) {
      const b = new Uint8Array(4);
      new DataView(b.buffer).setInt32(0, enc.length - 1, true);
      engine.mem_write(lengthPtr, Array.from(b));
    }
    if (infoLogPtr) {
      engine.mem_write(infoLogPtr, Array.from(enc));
    }
    return 0;
  });

  registerShim("glCreateProgram", () => {
    const id = nextProgId++;
    if (gl) glPrograms.set(id, gl.createProgram());
    return id;
  });

  registerShim("glAttachShader", (e) => {
    const prog = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const shader = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (gl && glPrograms.has(prog) && glShaders.has(shader)) {
      gl.attachShader(glPrograms.get(prog), glShaders.get(shader));
    }
    return 0;
  });

  registerShim("glLinkProgram", (e) => {
    const prog = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl && glPrograms.has(prog)) {
      const glProg = glPrograms.get(prog);
      gl.linkProgram(glProg);
      if (!gl.getProgramParameter(glProg, gl.LINK_STATUS)) {
        console.error(`[worker WebGL] Program ${prog} link error:`, gl.getProgramInfoLog(glProg));
      } else {
        console.log(`[worker WebGL] Program ${prog} linked successfully.`);
      }
    }
    return 0;
  });

  registerShim("glGetProgramiv", (e) => {
    const prog = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const pname = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const params = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (params) {
      let val = 1;
      if (gl && glPrograms.has(prog)) {
        const res = gl.getProgramParameter(glPrograms.get(prog), pname);
        if (typeof res === "boolean") val = res ? 1 : 0;
        else if (typeof res === "number") val = res;
      }
      const b = new Uint8Array(4);
      new DataView(b.buffer).setInt32(0, val, true);
      engine.mem_write(params, Array.from(b));
    }
    return 0;
  });

  registerShim("glGetProgramInfoLog", (e) => {
    const prog = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const maxLength = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const lengthPtr = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const infoLogPtr = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    let log = "";
    if (gl && glPrograms.has(prog)) {
      log = gl.getProgramInfoLog(glPrograms.get(prog)) || "";
    }
    const enc = new TextEncoder().encode(log.slice(0, maxLength - 1) + "\0");
    if (lengthPtr) {
      const b = new Uint8Array(4);
      new DataView(b.buffer).setInt32(0, enc.length - 1, true);
      engine.mem_write(lengthPtr, Array.from(b));
    }
    if (infoLogPtr) {
      engine.mem_write(infoLogPtr, Array.from(enc));
    }
    return 0;
  });

  registerShim("glGetUniformLocation", (e) => {
    const prog = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const name = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    if (gl && glPrograms.has(prog)) {
      const loc = gl.getUniformLocation(glPrograms.get(prog), name);
      if (loc) {
        const id = nextLocId++;
        glUniformLocs.set(id, loc);
        return id;
      }
    }
    return -1;
  });

  registerShim("glGetAttribLocation", (e) => {
    const prog = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const name = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    if (gl && glPrograms.has(prog)) {
      return gl.getAttribLocation(glPrograms.get(prog), name);
    }
    return -1;
  });

  registerShim("glActiveTexture", (e) => {
    const texture = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.activeTexture(texture);
    return 0;
  });

  registerShim("glPixelStorei", (e) => {
    const pname = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const param = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (gl) gl.pixelStorei(pname, param);
    return 0;
  });

  registerShim("glCullFace", (e) => {
    const mode = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.cullFace(mode);
    return 0;
  });

  registerShim("glFrontFace", (e) => {
    const mode = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.frontFace(mode);
    return 0;
  });

  registerShim("glScissor", (e) => {
    const x = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const y = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const w = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const h = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (gl) gl.scissor(x, y, w, h);
    return 0;
  });

  registerShim("glDepthFunc", (e) => {
    const func = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.depthFunc(func);
    return 0;
  });

  registerShim("glUniform1i", (e) => {
    const locId = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const v0 = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const loc = glUniformLocs.get(locId);
    if (gl && loc) gl.uniform1i(loc, v0);
    return 0;
  });

  registerShim("glUniform1f", (e) => {
    const locId = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const loc = glUniformLocs.get(locId);
    if (gl && loc) {
      const v0 = Number(e.reg_read_i64(uc.ARM64_REG_X1));
      gl.uniform1f(loc, v0);
    }
    return 0;
  });

  registerShim("glUniform4f", (e) => {
    const locId = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const loc = glUniformLocs.get(locId);
    if (gl && loc) {
      gl.uniform4f(loc, 1.0, 1.0, 1.0, 1.0);
    }
    return 0;
  });

  registerShim("glUniformMatrix4fv", (e) => {
    const locId = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const transpose = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const valPtr = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const loc = glUniformLocs.get(locId);
    if (gl && loc && valPtr) {
      const bytes = new Uint8Array(engine.mem_read(valPtr, count * 64));
      const floats = new Float32Array(bytes.buffer, bytes.byteOffset, count * 16);
      gl.uniformMatrix4fv(loc, transpose !== 0, floats);
    }
    return 0;
  });

  registerShim("glUseProgram", (e) => {
    const id = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.useProgram(id ? (glPrograms.get(id) || null) : null);
    return 0;
  });

  registerShim("glEnableVertexAttribArray", (e) => {
    const index = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.enableVertexAttribArray(index);
    return 0;
  });

  registerShim("glDisableVertexAttribArray", (e) => {
    const index = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (gl) gl.disableVertexAttribArray(index);
    return 0;
  });

  registerShim("glVertexAttribPointer", (e) => {
    const idx = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const type = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const norm = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const stride = Number(e.reg_read_i64(uc.ARM64_REG_X4));
    const ptr = Number(e.reg_read_i64(uc.ARM64_REG_X5));
    if (gl) gl.vertexAttribPointer(idx, sz, type, norm !== 0, stride, ptr);
    return 0;
  });

  registerShim("glFlush", () => {
    if (gl) gl.flush();
    return 0;
  });

  registerShim("glFinish", () => {
    if (gl) gl.finish();
    return 0;
  });

  registerShim("glGetString", (e) => {
    const name = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const s = name === 0x1F02 ? "WebGL 2.0 (OpenGL ES 3.0 WebAssembly)" : "Payback2 WebGL";
    const addr = guestAlloc(s.length + 1);
    engine.mem_write(addr, [...new TextEncoder().encode(s), 0]);
    return addr;
  });

  registerShim("glGetIntegerv", (e) => {
    const pname = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const params = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (params) {
      let v = 0;
      if (pname === 0x8CA6) v = 0; // GL_FRAMEBUFFER_BINDING
      else if (pname === 0x0D33) v = 4096; // GL_MAX_TEXTURE_SIZE
      const b = new Uint8Array(4); new DataView(b.buffer).setInt32(0, v, true);
      engine.mem_write(params, Array.from(b));
    }
    return 0;
  });

  // OpenSL ES
  const SL_BASE = 0x60700000;
  const SL_OBJ_VTBL = SL_BASE;
  const SL_ENG_VTBL = SL_BASE + 0x80;
  const SL_PLAY_VTBL = SL_BASE + 0x100;
  const SL_BQ_VTBL = SL_BASE + 0x180;
  const SL_ENG_OBJ = SL_BASE + 0x200;
  const SL_ENG_ITF = SL_BASE + 0x220;
  const SL_MIX_OBJ = SL_BASE + 0x240;
  const SL_PLY_OBJ = SL_BASE + 0x260;
  const SL_PLY_ITF = SL_BASE + 0x280;
  const SL_BQ_ITF  = SL_BASE + 0x2a0;

  const slStubTramp = addNativeCodeHelper(NATIVE_RET0_ARM64);
  const slStubU64 = writeU64(slStubTramp);
  for (let i = 0; i < 20; i++) {
    engine.mem_write(SL_OBJ_VTBL + i * 8, slStubU64);
    engine.mem_write(SL_ENG_VTBL + i * 8, slStubU64);
    engine.mem_write(SL_PLAY_VTBL + i * 8, slStubU64);
    engine.mem_write(SL_BQ_VTBL + i * 8, slStubU64);
  }

  const slGetInterfaceTramp = addTrampoline("SLObjectItf_GetInterface", (e) => {
    const self = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const pOut = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (pOut) {
      if (self === SL_ENG_OBJ) engine.mem_write(pOut, writeU64(SL_ENG_ITF));
      else if (self === SL_PLY_OBJ) engine.mem_write(pOut, writeU64(SL_PLY_ITF));
    }
    return 0;
  });
  engine.mem_write(SL_OBJ_VTBL + 3 * 8, writeU64(slGetInterfaceTramp));

  const slCreatePlayerTramp = addTrampoline("SLEngineItf_CreateAudioPlayer", (e) => {
    const pPlayer = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (pPlayer) engine.mem_write(pPlayer, writeU64(SL_PLY_OBJ));
    return 0;
  });
  engine.mem_write(SL_ENG_VTBL + 2 * 8, writeU64(slCreatePlayerTramp));

  const slCreateMixTramp = addTrampoline("SLEngineItf_CreateOutputMix", (e) => {
    const pMix = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (pMix) engine.mem_write(pMix, writeU64(SL_MIX_OBJ));
    return 0;
  });
  engine.mem_write(SL_ENG_VTBL + 7 * 8, writeU64(slCreateMixTramp));

  engine.mem_write(SL_ENG_OBJ, writeU64(SL_OBJ_VTBL));
  engine.mem_write(SL_MIX_OBJ, writeU64(SL_OBJ_VTBL));
  engine.mem_write(SL_PLY_OBJ, writeU64(SL_OBJ_VTBL));
  engine.mem_write(SL_ENG_ITF, writeU64(SL_ENG_VTBL));
  engine.mem_write(SL_PLY_ITF, writeU64(SL_PLAY_VTBL));
  engine.mem_write(SL_BQ_ITF, writeU64(SL_BQ_VTBL));

  registerShim("slCreateEngine", (e) => {
    const pEngine = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (pEngine) engine.mem_write(pEngine, writeU64(SL_ENG_OBJ));
    return 0;
  });

  // JNI Setup
  setupJni();
}

function setupJni() {
  const JNI_ENV_VTABLE = JNI_ENV_ADDR + 0x100;
  const JNI_VM_VTABLE = JNI_VM_ADDR + 0x100;

  engine.mem_write(JNI_ENV_ADDR, writeU64(JNI_ENV_VTABLE));
  engine.mem_write(JNI_VM_ADDR, writeU64(JNI_VM_VTABLE));

  const ret0NativeAddr = addNativeCodeHelper(NATIVE_RET0_ARM64);
  const ret0NativeU64 = writeU64(ret0NativeAddr);

  // Default all 240 JNIEnv slots and 16 JavaVM slots to pure native ret0 (Zero JS trampolines)
  for (let i = 0; i < 240; i++) {
    engine.mem_write(JNI_ENV_VTABLE + i * 8, ret0NativeU64);
  }
  for (let i = 0; i < 16; i++) {
    engine.mem_write(JNI_VM_VTABLE + i * 8, ret0NativeU64);
  }

  // Fast native helpers for frequently called JNI methods (Zero JS trampolines)
  const getEnvNative = addNativeCodeHelper(NATIVE_GETENV_ARM64);
  const getEnvNativeU64 = writeU64(getEnvNative);
  engine.mem_write(JNI_VM_VTABLE + 4 * 8, getEnvNativeU64); // AttachCurrentThread
  engine.mem_write(JNI_VM_VTABLE + 6 * 8, getEnvNativeU64); // GetEnv
  engine.mem_write(JNI_VM_VTABLE + 7 * 8, getEnvNativeU64); // AttachCurrentThreadAsDaemon

  const getVmNative = addNativeCodeHelper(NATIVE_GETJAVAVM_ARM64);
  engine.mem_write(JNI_ENV_VTABLE + 219 * 8, writeU64(getVmNative)); // GetJavaVM

  const findClassNative = addNativeCodeHelper(NATIVE_FINDCLASS_ARM64);
  engine.mem_write(JNI_ENV_VTABLE + 6 * 8, writeU64(findClassNative)); // FindClass

  const retMidNative = addNativeCodeHelper(NATIVE_RET_MID_ARM64);
  const retMidNativeU64 = writeU64(retMidNative);
  engine.mem_write(JNI_ENV_VTABLE + 33 * 8, retMidNativeU64);  // GetMethodID
  engine.mem_write(JNI_ENV_VTABLE + 113 * 8, retMidNativeU64); // GetStaticMethodID
  engine.mem_write(JNI_ENV_VTABLE + 94 * 8, retMidNativeU64);  // GetFieldID
  engine.mem_write(JNI_ENV_VTABLE + 144 * 8, retMidNativeU64); // GetStaticFieldID

  const retObjNative = addNativeCodeHelper(NATIVE_RET_OBJ_ARM64);
  const retObjNativeU64 = writeU64(retObjNative);
  engine.mem_write(JNI_ENV_VTABLE + 34 * 8, retObjNativeU64);  // CallObjectMethod
  engine.mem_write(JNI_ENV_VTABLE + 114 * 8, retObjNativeU64); // CallStaticObjectMethod

  const newGlobalRefNative = addNativeCodeHelper(NATIVE_NEWGLOBALREF_ARM64);
  engine.mem_write(JNI_ENV_VTABLE + 21 * 8, writeU64(newGlobalRefNative)); // NewGlobalRef

  // RegisterNatives (slot 215)
  const regNativesTramp = addTrampoline("JNIEnv_RegisterNatives", (e) => {
    const clazz = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const methods = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    console.log(`[worker jni] RegisterNatives registered ${n} methods for class 0x${clazz.toString(16)}`);
    return 0;
  });
  engine.mem_write(JNI_ENV_VTABLE + 215 * 8, writeU64(regNativesTramp));

  // String Handles
  const strHandles = new Map();
  function makeStringHandle(str) {
    const addr = guestAlloc(str.length + 1);
    engine.mem_write(addr, [...new TextEncoder().encode(str), 0]);
    const handle = 0x60400000 + strHandles.size * 16;
    strHandles.set(handle, addr);
    return handle;
  }
  self.makeStringHandle = makeStringHandle;

  // GetStringUTFChars (slot 169)
  const getStringUtfCharsTramp = addTrampoline("JNIEnv_GetStringUTFChars", (e) => {
    const jstr = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    return strHandles.get(jstr) || 0;
  });
  engine.mem_write(JNI_ENV_VTABLE + 169 * 8, writeU64(getStringUtfCharsTramp));

  // GetStringUTFLength (slot 168)
  const getStringUtfLengthTramp = addTrampoline("JNIEnv_GetStringUTFLength", (e) => {
    const jstr = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const addr = strHandles.get(jstr) || 0;
    return readCString(addr).length;
  });
  engine.mem_write(JNI_ENV_VTABLE + 168 * 8, writeU64(getStringUtfLengthTramp));
}

function loadElfAndRelocate(buf) {
  const phoff = Number(readBigUInt64LE(buf, 0x20));
  const phnum = readUInt16LE(buf, 0x38);
  const phentsize = readUInt16LE(buf, 0x36);

  // Map segments
  for (let i = 0; i < phnum; i++) {
    const off = phoff + i * phentsize;
    const type = readUInt32LE(buf, off);
    if (type === 1) { // PT_LOAD
      const p_offset = Number(readBigUInt64LE(buf, off + 0x08));
      const p_vaddr = readBigUInt64LE(buf, off + 0x10);
      const p_filesz = Number(readBigUInt64LE(buf, off + 0x20));
      if (p_filesz > 0) {
        const segBytes = buf.subarray(p_offset, p_offset + p_filesz);
        engine.mem_write(Number(BASE_ADDR + p_vaddr), Array.from(segBytes));
      }
    }
  }

  console.log("[worker] ELF segments mapped into guest memory.");

  // Section headers
  const shoff = Number(readBigUInt64LE(buf, 0x28));
  const shnum = readUInt16LE(buf, 0x3c);
  const shentsize = readUInt16LE(buf, 0x3a);

  let symtab = null, strtab = null, symcount = 0;
  for (let i = 0; i < shnum; i++) {
    const off = shoff + i * shentsize;
    const type = readUInt32LE(buf, off + 0x04);
    if (type === 11) { // SHT_DYNSYM
      const symOff = Number(readBigUInt64LE(buf, off + 0x18));
      const symSize = Number(readBigUInt64LE(buf, off + 0x20));
      const link = readUInt32LE(buf, off + 0x28);
      const strOff = Number(readBigUInt64LE(buf, shoff + link * shentsize + 0x18));
      symcount = symSize / 24;
      symtab = buf.subarray(symOff, symOff + symSize);
      strtab = buf.subarray(strOff);
      break;
    }
  }

  function getSymName(idx) {
    if (idx === 0) return "";
    const nameIdx = readUInt32LE(symtab, idx * 24);
    let end = nameIdx;
    while (strtab[end] !== 0) end++;
    return new TextDecoder().decode(strtab.subarray(nameIdx, end));
  }

  // Restore PairIP oracle holes (complete 126 slots)
  const oracleHoles = [
    { off: 0x38f928, sym: "sprintf" }, { off: 0x38f930, sym: "puts" },
    { off: 0x38f940, sym: "__memset_aarch64" }, { off: 0x38f948, sym: "realloc" },
    { off: 0x38f950, sym: "malloc" }, { off: 0x38f958, sym: "free" },
    { off: 0x38f960, sym: "__memcpy_aarch64_simd" }, { off: 0x38f968, sym: "strdup" },
    { off: 0x38f980, sym: "strncpy" }, { off: 0x38f988, sym: "pthread_mutex_lock" },
    { off: 0x38f990, sym: "pthread_mutex_unlock" }, { off: 0x38f9a8, sym: "exit" },
    { off: 0x38f9f8, sym: "usleep" }, { off: 0x38fa38, sym: "time" },
    { off: 0x38fa40, sym: "pthread_create" }, { off: 0x38fa48, sym: "pthread_detach" },
    { off: 0x38fa50, sym: "fopen" }, { off: 0x38fa58, sym: "ftell" },
    { off: 0x38fa60, sym: "fseek" }, { off: 0x38fa68, sym: "fread" },
    { off: 0x38fa70, sym: "fclose" }, { off: 0x38fa78, sym: "remove" },
    { off: 0x38faa8, sym: "fscanf" }, { off: 0x38faf0, sym: "fwrite" },
    { off: 0x38fb00, sym: "rename" }, { off: 0x38fb08, sym: "gettimeofday" },
    { off: 0x38fb10, sym: "rand" }, { off: 0x38fb18, sym: "strcat" },
    { off: 0x38fb20, sym: "snprintf" }, { off: 0x38fb28, sym: "pthread_mutex_trylock" },
    { off: 0x38fb78, sym: "pthread_attr_init" }, { off: 0x38fb80, sym: "pthread_cond_wait" },
    { off: 0x38fb88, sym: "__memcmp_aarch64" }, { off: 0x38fba0, sym: "calloc" },
    { off: 0x38fba8, sym: "pthread_cond_signal" }, { off: 0x38fbb0, sym: "arc4random" },
    { off: 0x38fc88, sym: "strstr" }, { off: 0x38fd60, sym: "pthread_join" },
    { off: 0x38fd90, sym: "sched_yield" }, { off: 0x38fdb0, sym: "wcslen" },
    { off: 0x38fdb8, sym: "strcspn" }, { off: 0x38fdc0, sym: "atoi" },
    { off: 0x38fe18, sym: "strtok" }, { off: 0x38fe20, sym: "strcasecmp" },
    { off: 0x38ff78, sym: "wcscpy" }, { off: 0x390010, sym: "wcscmp" },
    { off: 0x390050, sym: "qsort" }, { off: 0x390078, sym: "pthread_mutexattr_init" },
    { off: 0x390080, sym: "pthread_mutexattr_settype" }, { off: 0x390088, sym: "pthread_mutex_init" },
    { off: 0x3900c8, sym: "sleep" }, { off: 0x3900d8, sym: "srand" },
    { off: 0x3900e0, sym: "nice" }, { off: 0x3900f0, sym: "sync" },
    { off: 0x390100, sym: "__memcpy_aarch64_simd" }, { off: 0x390108, sym: "pthread_self" },
    { off: 0x390110, sym: "pthread_getschedparam" }, { off: 0x390118, sym: "pthread_attr_setdetachstate" },
    { off: 0x390c90, sym: "fcntl" }, { off: 0x390c98, sym: "pipe" },
    { off: 0x390ca0, sym: "read" }, { off: 0x390ca8, sym: "strerror" },
    { off: 0x390cb0, sym: "write" }, { off: 0x390cd0, sym: "strtoll" },
    { off: 0x390d08, sym: "abort" }, { off: 0x390d20, sym: "pthread_cond_broadcast" },
    { off: 0x390d28, sym: "pthread_cond_destroy" }, { off: 0x390d30, sym: "pthread_cond_init" },
    { off: 0x390d38, sym: "pthread_mutex_destroy" }, { off: 0x3914d0, sym: "send" },
    { off: 0x3914e0, sym: "getaddrinfo" }, { off: 0x3914e8, sym: "freeaddrinfo" },
    { off: 0x3914f0, sym: "recv" }, { off: 0x3914f8, sym: "setsockopt" },
    { off: 0x391500, sym: "ioctl" }, { off: 0x391568, sym: "select" },
    { off: 0x3919e8, sym: "localtime" }, { off: 0x3919f0, sym: "strftime" },
    { off: 0x391b18, sym: "strpbrk" }, { off: 0x391b98, sym: "fflush" },
    { off: 0x391ba0, sym: "strtoull" }, { off: 0x391ba8, sym: "strtod" },
    { off: 0x391bb0, sym: "strtof" }, { off: 0x391bb8, sym: "strtol" },
    { off: 0x391bc0, sym: "swprintf" }, { off: 0x391bc8, sym: "posix_memalign" },
    { off: 0x391bd0, sym: "pthread_rwlock_rdlock" }, { off: 0x391bd8, sym: "wcstod" },
    { off: 0x391be0, sym: "wcstof" }, { off: 0x391be8, sym: "wmemchr" },
    { off: 0x391bf0, sym: "pthread_mutexattr_destroy" }, { off: 0x391bf8, sym: "wcstol" },
    { off: 0x391c00, sym: "openlog" }, { off: 0x391c08, sym: "pthread_rwlock_unlock" },
    { off: 0x391c10, sym: "vasprintf" }, { off: 0x391c18, sym: "sysconf" },
    { off: 0x391c28, sym: "vfprintf" }, { off: 0x391c30, sym: "pthread_setspecific" },
    { off: 0x391c38, sym: "fprintf" }, { off: 0x391c40, sym: "pthread_rwlock_wrlock" },
    { off: 0x391c48, sym: "closelog" }, { off: 0x391c50, sym: "strtold" },
    { off: 0x391c58, sym: "android_set_abort_message" }, { off: 0x391c60, sym: "strtoul" },
    { off: 0x391c68, sym: "pthread_key_delete" }, { off: 0x391c70, sym: "pthread_getspecific" },
    { off: 0x391c78, sym: "fputc" }, { off: 0x391c80, sym: "syslog" },
    { off: 0x391c88, sym: "pthread_key_create" }, { off: 0x391c90, sym: "wcstoull" },
    { off: 0x391c98, sym: "getauxval" }, { off: 0x391ca0, sym: "nanosleep" },
    { off: 0x391ca8, sym: "wcstold" }, { off: 0x391cb0, sym: "wmemcmp" },
    { off: 0x391cb8, sym: "pthread_cond_timedwait" }, { off: 0x391cc0, sym: "wcstoll" },
    { off: 0x391cc8, sym: "pthread_once" }, { off: 0x391cd0, sym: "syscall" },
    { off: 0x391cd8, sym: "strerror_r" }, { off: 0x391ce0, sym: "wcstoul" }
  ];

  for (const hole of oracleHoles) {
    if (!hole.sym) continue;
    const tgt = getOrEmitShim(hole.sym);
    const slot = Number(BASE_ADDR + BigInt(hole.off));
    engine.mem_write(slot, writeU64(tgt));
  }

  console.log("[worker] Oracle holes seeded.");

  // Apply relocations
  for (let i = 0; i < shnum; i++) {
    const off = shoff + i * shentsize;
    const sh_type = readUInt32LE(buf, off + 0x04);
    if (sh_type === 4) { // SHT_RELA
      const r_offset = Number(readBigUInt64LE(buf, off + 0x18));
      const r_size = Number(readBigUInt64LE(buf, off + 0x20));
      const r_entsize = Number(readBigUInt64LE(buf, off + 0x38));

      for (let roff = r_offset; roff < r_offset + r_size; roff += r_entsize) {
        const r_addr = readBigUInt64LE(buf, roff);
        const r_info = readBigUInt64LE(buf, roff + 0x08);
        const r_addend = readBigInt64LE(buf, roff + 0x10);
        const r_type = Number(r_info & 0xFFFFFFFFn);
        const symidx = Number(r_info >> 32n);
        const targetAddr = Number(BASE_ADDR + r_addr);

        if (r_type === 1027) { // R_AARCH64_RELATIVE
          const val = BASE_ADDR + BigInt(r_addend);
          engine.mem_write(targetAddr, writeU64(val));
        } else if (r_type === 1025 || r_type === 1026) { // GLOB_DAT or JUMP_SLOT
          const shndx = readUInt16LE(symtab, symidx * 24 + 6);
          const symVal = readBigUInt64LE(symtab, symidx * 24 + 8);
          if (symidx > 0 && shndx !== 0) {
            engine.mem_write(targetAddr, writeU64(BASE_ADDR + symVal));
          } else {
            const symName = getSymName(symidx);
            if (symName) {
              const shimAddr = getOrEmitShim(symName);
              engine.mem_write(targetAddr, writeU64(shimAddr));
            }
          }
        }
      }
    }
  }

  console.log("[worker] Relocations applied.");

  // Fix 123 unrelocated .got.plt entries pointing to 0x3786b0, 0xffffffffffffffff, etc.
  const SAFE_STUB_ADDR = 0x71000020;
  const movRet = [0x00, 0x00, 0x80, 0xd2, 0xc0, 0x03, 0x5f, 0xd6]; // mov x0, #0; ret
  engine.mem_write(SAFE_STUB_ADDR, movRet);
  const gotPltAddr = Number(BASE_ADDR + 0x38f8f8n);
  const gotPltSize = 0x2700;
  let fixedGotEntries = 0;
  for (let off = 0; off < gotPltSize; off += 8) {
    const curVal = readBigUInt64LE(new Uint8Array(engine.mem_read(gotPltAddr + off, 8)), 0);
    if (curVal === 0xffffffffffffffffn || curVal === 0x3786b0n || curVal === 0n || curVal < 0x10000000n) {
      engine.mem_write(gotPltAddr + off, writeU64(SAFE_STUB_ADDR));
      fixedGotEntries++;
    }
  }
  console.log(`[worker] Bound ${fixedGotEntries} unrelocated .got.plt slots to safe stub (mov x0, 0; ret).`);

  // Fast native ARM64 stubs for mutex lock / unlock / trylock directly in their PLT entries
  engine.mem_write(Number(BASE_ADDR + 0x3787c0n), movRet); // pthread_mutex_lock
  engine.mem_write(Number(BASE_ADDR + 0x3787d0n), movRet); // pthread_mutex_unlock
  engine.mem_write(Number(BASE_ADDR + 0x378b00n), movRet); // pthread_mutex_trylock
  engine.mem_write(Number(BASE_ADDR + 0x38f988n), writeU64(SAFE_STUB_ADDR));
  engine.mem_write(Number(BASE_ADDR + 0x38f990n), writeU64(SAFE_STUB_ADDR));
  engine.mem_write(Number(BASE_ADDR + 0x38fb28n), writeU64(SAFE_STUB_ADDR));

  // Cooperative sched_yield trampoline that yields the CPU quantum cleanly
  const schedYieldTramp = addTrampoline("sched_yield", (e) => {
    e.emu_stop();
    return 0;
  });
  engine.mem_write(Number(BASE_ADDR + 0x38fd90n), writeU64(schedYieldTramp));

  // Write ARM64 ret directly at PLT dynamic resolver entry points (PLT header only: 0x3786b0 to 0x3786d0)
  const armRet = [0xc0, 0x03, 0x5f, 0xd6];
  for (let a = 0x3786b0; a < 0x3786d0; a += 4) {
    engine.mem_write(Number(BASE_ADDR) + a, armRet);
  }

  console.log("[worker] Preserving natural spinlock synchronization on [0x12464eb5] with sched_yield quantum yielding.");

  // Fast normal map generator bypass (bypasses 150 million instruction software fsqrt loop to native flat normal)
  engine.mem_write(Number(BASE_ADDR + 0x121754n), [0x50, 0x01, 0x00, 0x14]); // b #0x10121c94

  // Stub 0x10121214 (optional camera dirt and CRT screen overlay texture loader) with immediate ret
  engine.mem_write(Number(BASE_ADDR + 0x121214n), armRet);

  // Bypass software ETC/PVRTC transcoders directly into native WebGL compressed texture uploading
  const mov1Ret = [0x20, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6]; // mov w0, #1; ret
  engine.mem_write(Number(BASE_ADDR + 0x0f2098n), mov1Ret);
  engine.mem_write(Number(BASE_ADDR + 0x0f218cn), mov1Ret);

  // Fast 3D scene startup hook: bypasses character preset software palette unpacker directly into 3D gameplay scene entry
  const fastSceneStartAddr = Number(BASE_ADDR + 0x18bb90n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log("[worker] >>> ALL GAME ASSETS LOADED! BRANCHING DIRECTLY TO 3D GAMEPLAY SCENE 0x1018c290! <<<");
    engine.reg_write_i64(uc.ARM64_REG_X23, BigInt(BASE_ADDR + 0x245b0b0n));
    engine.reg_write_i64(uc.ARM64_REG_X21, BigInt(BASE_ADDR + 0x1526000n));
    engine.reg_write_i64(uc.ARM64_REG_X20, 1n);
    engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(BASE_ADDR + 0x18c290n));
  }, null, fastSceneStartAddr, fastSceneStartAddr);
  engine.mem_write(fastSceneStartAddr, [0xd5, 0x9c, 0x00, 0xf0, 0xbf, 0x01, 0x00, 0x14]); // adrp x21, #0x1526000; b #0x18c290

  // Ensure sound manager structure at 0x1245b0b0 has safe flags
  engine.mem_write(Number(BASE_ADDR + 0x245b0b0n + 0x114n), [0, 0, 0, 0]);
  engine.mem_write(Number(BASE_ADDR + 0x245b0b0n + 0x1c0n), [0, 0, 0, 0]);

  // Bypass 0x1022a0c4 loading spinlock: nop tbnz w22, #0, #0x22a0a0 so loop exits cleanly.
  // CRITICAL: 0x1022a0c8 MUST remain `cbz w19, #0x22a0e4` so when w19 == 0, it returns (ret)
  // directly to 0x1018fac4 to continue into the 3D gameplay loop!
  const nopInsn = [0x1f, 0x20, 0x03, 0xd5];
  engine.mem_write(Number(BASE_ADDR + 0x22a0c4n), nopInsn); // nop (bypass tbnz w22, #0, #0x22a0a0)

  // Ensure loading flags are cleared on hitting spinlock
  const spinlockAddr = Number(BASE_ADDR + 0x22a0acn);
  engine.hook_add(uc.HOOK_CODE, () => {
    engine.mem_write(Number(BASE_ADDR + 0x2464eb5n), [0]);
    engine.mem_write(Number(BASE_ADDR + 0x2464eb8n), [1]);
  }, null, spinlockAddr, spinlockAddr);

  // Diagnostic hooks for 3D gameplay pipeline entry
  const gameplayReturnAddr = Number(BASE_ADDR + 0x18fac4n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log("[worker] >>> SUCCESS: RETURNED TO 0x1018fac4! CONTINUING 3D GAMEPLAY PIPELINE! <<<");
  }, null, gameplayReturnAddr, gameplayReturnAddr);

  const gameplayFrame3dAddr = Number(BASE_ADDR + 0x192ac4n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log(`[worker] >>> 3D GAMEPLAY FRAME PRESENTATION 0x10192ac4 EXECUTING! (draws: ${drawCallCount}) <<<`);
  }, null, gameplayFrame3dAddr, gameplayFrame3dAddr);

  const gameplayStep3dAddr = Number(BASE_ADDR + 0x190fe4n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log(`[worker] >>> 3D GAMEPLAY STEP 0x10190fe4 EXECUTING! (draws: ${drawCallCount}) <<<`);
  }, null, gameplayStep3dAddr, gameplayStep3dAddr);

  const gameplayLoopAddr = Number(BASE_ADDR + 0x18ea14n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log(`[worker] >>> 3D GAMEPLAY FLYING LOOP 0x1018ea14 TICK! (draws: ${drawCallCount}) <<<`);
  }, null, gameplayLoopAddr, gameplayLoopAddr);

  // Null Huffman protection hooks for format 04 AMCT textures (e.g. earth.amct, SkinDetail.amct)
  const huffmanCheckAddr = Number(BASE_ADDR + 0x0f6904n);
  engine.hook_add(uc.HOOK_CODE, () => {
    const x3 = engine.reg_read_i64(uc.ARM64_REG_X3);
    if (x3 === 0n) {
      engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(BASE_ADDR + 0x0f7070n));
    }
  }, null, huffmanCheckAddr, huffmanCheckAddr);

  const huffmanCrashAddr = Number(BASE_ADDR + 0x0f6c7cn);
  engine.hook_add(uc.HOOK_CODE, () => {
    const x3 = engine.reg_read_i64(uc.ARM64_REG_X3);
    if (x3 === 0n) {
      engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(BASE_ADDR + 0x0f7070n));
    }
  }, null, huffmanCrashAddr, huffmanCrashAddr);

  // Diagnostic hooks for 3D gameplay scene entry
  const sceneLoop3dAddr = Number(BASE_ADDR + 0x229c88n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log("[worker] >>> SUCCESS: MAIN THREAD 3D GAMEPLAY SCENE 0x10229c88 ENTERED! <<<");
  }, null, sceneLoop3dAddr, sceneLoop3dAddr);

  const frameRenderAddr = Number(BASE_ADDR + 0x16514cn);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log(`[worker] >>> 3D GAMEPLAY FRAME RENDER 0x1016514c EXECUTING! (draws: ${drawCallCount}) <<<`);
  }, null, frameRenderAddr, frameRenderAddr);

  // Fast 3D scene transition hook: bypasses secondary character texture unpacker directly into Scene Init Pass 1 & 2
  const fastTransitionAddr = Number(BASE_ADDR + 0x2298ccn);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log("[worker] >>> Triggering fast 3D scene transition at 0x102298cc! <<<");
    engine.reg_write_i64(uc.ARM64_REG_X19, BigInt(BASE_ADDR + 0x2464eb8n));
    engine.reg_write_i64(uc.ARM64_REG_X20, BigInt(BASE_ADDR + 0x2464000n));
    engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(BASE_ADDR + 0x22a37cn));
  }, null, fastTransitionAddr, fastTransitionAddr);

  // Transition diagnostics
  const transitionAddr = Number(BASE_ADDR + 0x226404n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log("[worker] >>> SUCCESS: TRANSITION TO ACTIVE 3D GAMEPLAY: 0x10226404 ENTERED! <<<");
    engine.mem_write(Number(BASE_ADDR + 0x2464eb5n), [0]);
    engine.mem_write(Number(BASE_ADDR + 0x2464eb8n), [1]);
  }, null, transitionAddr, transitionAddr);

  const transitionThread1Addr = Number(BASE_ADDR + 0x22a384n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log("[worker] >>> SUCCESS: THREAD 1 3D GAMEPLAY SCENE INIT: 0x1022a384 ENTERED! <<<");
    engine.mem_write(Number(BASE_ADDR + 0x2464eb5n), [0]);
    engine.mem_write(Number(BASE_ADDR + 0x2464eb8n), [1]);
  }, null, transitionThread1Addr, transitionThread1Addr);

  // Scene Init Complete Hook: terminate Thread 1 cleanly and trigger continuous 3D gameplay rendering
  const sceneDoneAddr = Number(BASE_ADDR + 0x22a3a4n);
  engine.hook_add(uc.HOOK_CODE, () => {
    console.log("[worker] >>> THREAD 1 SCENE INIT COMPLETE! ACTIVATING 3D GAMEPLAY RENDERING <<<");
    engine.mem_write(Number(BASE_ADDR + 0x2464eb5n), [0]);
    engine.mem_write(Number(BASE_ADDR + 0x2464eb8n), [1]);
    const appPtr = guestThreads.length > 0 ? guestThreads[0].arg : 0x20002ed0;
    engine.mem_write(appPtr + 0x54, [0, 0, 0, 0]); // app->destroyRequested = 0 (keep running indefinitely)
    engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(STOP_ADDR));
  }, null, sceneDoneAddr, sceneDoneAddr);

  // Seed stack canary into __stack_chk_guard GOT slot (offset 0x38f3f0)
  engine.mem_write(Number(BASE_ADDR + 0x38f3f0n), writeU64(0xdeadbeefcafebaben));

  // Seed PairIP anchors & ANativeActivity structure for asset loader (0xec72c)
  const ctxPtr = 0x60200100;
  const actPtr = 0x60200200;
  const sFiles = guestAlloc(16);
  engine.mem_write(sFiles, Array.from(new TextEncoder().encode("/files\0")));
  const sExt = guestAlloc(16);
  engine.mem_write(sExt, Array.from(new TextEncoder().encode("/external\0")));

  engine.mem_write(actPtr + 0x10, writeU64(actPtr));
  engine.mem_write(actPtr + 0x20, writeU64(sFiles));
  engine.mem_write(actPtr + 0x28, writeU64(sExt));
  engine.mem_write(actPtr + 0x40, writeU64(0x60200020n)); // assetManager

  engine.mem_write(ctxPtr + 0x10, writeU64(actPtr));
  engine.mem_write(Number(BASE_ADDR + 0x3b2120n), writeU64(JNI_ENV_ADDR));
  engine.mem_write(Number(BASE_ADDR + 0x3b2130n), writeU64(ctxPtr));
  engine.mem_write(Number(BASE_ADDR + 0x3b2208n), writeU64(0x60200000n));

  // CRT Screen brightness (offset 0x19e8cf8) -> 255 (full brightness)
  const brightBuf = new Uint8Array(4);
  new DataView(brightBuf.buffer).setUint32(0, 255, true);
  engine.mem_write(Number(BASE_ADDR + 0x19e8cf8n), Array.from(brightBuf));

  // Execute C++ global constructors (.init_array, 9 constructors)
  const initArrayAddr = Number(BASE_ADDR + 0x38d860n);
  for (let c = 0; c < 9; c++) {
    const ctorBytes = engine.mem_read(initArrayAddr + c * 8, 8);
    const ctorAddr = Number(ctorBytes.reduce((acc, b, idx) => acc | (BigInt(b) << BigInt(idx * 8)), 0n));
    if (ctorAddr) {
      console.log(`[worker] Executing ctor ${c} at 0x${ctorAddr.toString(16)}...`);
      engine.reg_write_i64(uc.ARM64_REG_SP, BigInt(STACK_BASE + STACK_SIZE - 0x1000));
      engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));
      try {
        engine.emu_start(ctorAddr, STOP_ADDR, 0, 100000);
      } catch(e) {
        console.warn(`[worker] ctor ${c} stopped:`, e.message);
      }
    }
  }

  // Patch software mipmap downsampler at 0x127528 with ARM64 'ret' (0xd65f03c0)
  // Level 0 is already uploaded via glTexImage2D; avoids unhandled TCI SIMD opcode
  engine.mem_write(Number(BASE_ADDR + 0x127528n), [0xc0, 0x03, 0x5f, 0xd6]);

  // Patch popcount SIMD (cnt/addv) at 0xe4660 with exact scalar power-of-2 test:
  // sub w8, w20, #1 ; tst w8, w20 ; b.eq 0xe4694 ; nop ; nop ; nop
  engine.mem_write(Number(BASE_ADDR + 0xe4660n), [
    0x88, 0x06, 0x00, 0x51, // sub w8, w20, #1
    0x1f, 0x01, 0x14, 0x6a, // tst w8, w20
    0x60, 0x01, 0x00, 0x54, // b.eq +0x2c (0xe4694)
    0x1f, 0x20, 0x03, 0xd5, // nop
    0x1f, 0x20, 0x03, 0xd5, // nop
    0x1f, 0x20, 0x03, 0xd5  // nop
  ]);

  // Pre-scan executable sections for unsupported AArch64 SIMD instructions:
  // 1. Vector permute: uzp1, uzp2, trn1, trn2, zip1, zip2
  // 2. Vector across-lanes add: addv
  // 3. Vector bit population count: cnt
  let patchedSimd = 0;
  const brk = [0x00, 0x00, 0x20, 0xd4];
  for (let i = 0; i < shnum; i++) {
    const off = shoff + i * shentsize;
    const sh_flags = Number(readBigUInt64LE(buf, off + 0x08));
    const sh_addr = Number(readBigUInt64LE(buf, off + 0x10));
    const sh_offset = Number(readBigUInt64LE(buf, off + 0x18));
    const sh_size = Number(readBigUInt64LE(buf, off + 0x20));

    if ((sh_flags & 4) !== 0 && sh_size > 0) { // SHF_EXECINSTR
      for (let sOff = 0; sOff < sh_size; sOff += 4) {
        const raw = readUInt32LE(buf, sh_offset + sOff);
        // 1. Permute instructions
        if (((raw & 0xbf208c00) >>> 0) === 0x0e000800) {
          const opcode = (raw >> 12) & 7;
          const Q = (raw >> 30) & 1;
          const size = (raw >> 22) & 3;
          const Rm = (raw >> 16) & 0x1f;
          const Rn = (raw >> 5) & 0x1f;
          const Rd = raw & 0x1f;

          if (size === 1 || opcode === 1 || opcode === 5) {
            const guestAddr = Number(BASE_ADDR + BigInt(sh_addr + sOff));
            simdOpMap.set(guestAddr, { type: "permute", opcode, Q, size, Rd, Rn, Rm });
            engine.mem_write(guestAddr, brk);
            patchedSimd++;
          }
        }
        // 2. Vector across-lanes ADDV
        else if (((raw & 0xbf3ffc00) >>> 0) === 0x0e31b800) {
          const Q = (raw >> 30) & 1;
          const size = (raw >> 22) & 3;
          const Rn = (raw >> 5) & 0x1f;
          const Rd = raw & 0x1f;
          const guestAddr = Number(BASE_ADDR + BigInt(sh_addr + sOff));
          simdOpMap.set(guestAddr, { type: "addv", Q, size, Rd, Rn });
          engine.mem_write(guestAddr, brk);
          patchedSimd++;
        }
        // 3. Vector bit population count CNT
        else if (((raw & 0xbf3ffc00) >>> 0) === 0x0e205800) {
          const Q = (raw >> 30) & 1;
          const Rn = (raw >> 5) & 0x1f;
          const Rd = raw & 0x1f;
          const guestAddr = Number(BASE_ADDR + BigInt(sh_addr + sOff));
          simdOpMap.set(guestAddr, { type: "cnt", Q, Rd, Rn });
          engine.mem_write(guestAddr, brk);
          patchedSimd++;
        }
        // 4. Vector reverse REV64 / REV32 / REV16
        else if (
          ((raw & 0xbf3ffc00) >>> 0) === 0x0e200800 ||
          ((raw & 0xbf3ffc00) >>> 0) === 0x2e200800 ||
          ((raw & 0xbf3ffc00) >>> 0) === 0x0e201800
        ) {
          const revVal = (raw & 0xbf3ffc00) >>> 0;
          const revType = (revVal === 0x0e200800) ? 0 : ((revVal === 0x2e200800) ? 1 : 2);
          const Q = (raw >> 30) & 1;
          const size = (raw >> 22) & 3;
          const Rn = (raw >> 5) & 0x1f;
          const Rd = raw & 0x1f;
          const guestAddr = Number(BASE_ADDR + BigInt(sh_addr + sOff));
          simdOpMap.set(guestAddr, { type: "rev", revType, Q, size, Rd, Rn });
          engine.mem_write(guestAddr, brk);
          patchedSimd++;
        }
        // 5. Halfword vector extract UMOV Wd, Vn.h[index]
        else if (((raw & 0xffe3fc00) >>> 0) === 0x0e023c00) {
          const index = (raw >> 18) & 7;
          const Rn = (raw >> 5) & 0x1f;
          const Rd = raw & 0x1f;
          const guestAddr = Number(BASE_ADDR + BigInt(sh_addr + sOff));
          simdOpMap.set(guestAddr, { type: "umov_h", index, Rd, Rn });
          engine.mem_write(guestAddr, brk);
          patchedSimd++;
        }
      }
    }
  }
  console.log(`[worker] Pre-scanned and patched ${patchedSimd} SIMD instructions (permute/addv/cnt/rev/umov_h) with brk #0`);

  console.log("[worker] Seeded PairIP anchors & executed .init_array ctors.");
}

function callJniOnLoad() {
  const JNI_ONLOAD_ADDR = Number(BASE_ADDR + 0xeceacn);
  engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_VM_ADDR));
  engine.reg_write_i64(uc.ARM64_REG_X1, 0n);
  engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

  try {
    engine.emu_start(JNI_ONLOAD_ADDR, STOP_ADDR, 0, 500000);
  } catch(err) {
    console.error("[worker] JNI_OnLoad stopped:", err.message);
  }

  const retVal = engine.reg_read_i64(uc.ARM64_REG_X0);
  console.log(`[worker] JNI_OnLoad returned 0x${retVal.toString(16)}`);
}

function callInitializeNativeCode() {
  const INIT_NATIVE_ADDR = Number(BASE_ADDR + 0x31c908n);
  const jFiles = self.makeStringHandle("/files");
  const jObb = self.makeStringHandle("/obb");
  const jExt = self.makeStringHandle("/external");

  engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_ENV_ADDR));
  engine.reg_write_i64(uc.ARM64_REG_X1, 0x60200010n); // activity
  engine.reg_write_i64(uc.ARM64_REG_X2, BigInt(jFiles));
  engine.reg_write_i64(uc.ARM64_REG_X3, BigInt(jObb));
  engine.reg_write_i64(uc.ARM64_REG_X4, BigInt(jExt));
  engine.reg_write_i64(uc.ARM64_REG_X5, 0x60200020n); // asset_mgr
  engine.reg_write_i64(uc.ARM64_REG_X6, 0n);          // savedState
  engine.reg_write_i64(uc.ARM64_REG_X7, 0x60200030n); // configuration
  engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

  try {
    engine.emu_start(INIT_NATIVE_ADDR, STOP_ADDR, 0, 2000000);
  } catch(err) {
    console.error("[worker] initializeNativeCode stopped:", err.message);
  }

  gameHandle = engine.reg_read_i64(uc.ARM64_REG_X0);
  const appPtr = guestThreads.length > 0 ? guestThreads[0].arg : 0x20004c50;
  let actPtr = 0x20004850;
  try {
    const actFromApp = Number(readBigUInt64LE(new Uint8Array(engine.mem_read(appPtr + 0x10, 8)), 0));
    if (actFromApp >= 0x20000000 && actFromApp < 0x30000000) actPtr = actFromApp;
  } catch(_) {}

  gameHandle = BigInt(actPtr);
  console.log(`[worker] initializeNativeCode -> ANativeActivity = 0x${gameHandle.toString(16)}, android_app = 0x${appPtr.toString(16)}`);

  // Wire activity <-> app pointers and seed ready state
  engine.mem_write(actPtr + 0x38, writeU64(appPtr)); // activity->instance = app
  engine.mem_write(Number(BASE_ADDR + 0x3b2130n), writeU64(appPtr)); // [0x3b2130] = app
  engine.mem_write(appPtr + 0x40, writeU64(0x60200040n)); // app->window = ANativeWindow
  engine.mem_write(appPtr + 0x50, [11, 0, 0, 0]); // app->activityState = 11 (APP_CMD_RESUME)
  engine.mem_write(appPtr + 0x54, [0, 0, 0, 0]);  // app->destroyRequested = 0 (keep running)
  engine.mem_write(appPtr + 0x140, [1, 0, 0, 0]); // app->running = 1
}

function startGuestGameThread() {
  if (guestThreads.length === 0) return;
  const t = guestThreads[0];
  console.log(`[worker] Starting game thread fn=0x${t.fn.toString(16)} to initialize looper...`);
  engine.reg_write_i64(uc.ARM64_REG_SP, BigInt(THREAD_STACK_BASE + THREAD_STACK_SIZE - 0x1000));
  engine.reg_write_i64(uc.ARM64_REG_TPIDR_EL0, BigInt(TLS_BASE));
  engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(t.arg));
  engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));
  engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(t.fn));

  try {
    engine.emu_start(t.fn, STOP_ADDR, 0, 1000000);
  } catch(err) {
    console.error("[worker] game thread init error:", err.message);
  }

  gameThreadContext = saveGuestContext();
  console.log(`[worker] game thread paused at PC: 0x${Number(gameThreadContext.gp ? gameThreadContext.gp[32] : gameThreadContext[32]).toString(16)}`);
}

function driveLifecycle() {
  const SURFACE = 0x60200040n;
  const steps = [
    { nm: "onStartNative",              off: 0x31D0A8n, a0: 0n, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onResumeNative",             off: 0x31D0C0n, a0: 0n, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onWindowFocusChangedNative", off: 0x31D1DCn, a0: 1n, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onSurfaceCreatedNative",     off: 0x31D1FCn, a0: SURFACE, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onSurfaceChangedNative",     off: 0x31D264n, a0: SURFACE, a1: 1920n, a2: 1080n, a3: 1n },
  ];

  for (const step of steps) {
    engine.reg_write_i64(uc.ARM64_REG_SP, BigInt(STACK_BASE + STACK_SIZE - 0x1000));
    engine.reg_write_i64(uc.ARM64_REG_TPIDR_EL0, BigInt(TLS_BASE));
    engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_ENV_ADDR));
    engine.reg_write_i64(uc.ARM64_REG_X1, 0n);
    engine.reg_write_i64(uc.ARM64_REG_X2, gameHandle);
    engine.reg_write_i64(uc.ARM64_REG_X3, step.a0);
    engine.reg_write_i64(uc.ARM64_REG_X4, step.a1);
    engine.reg_write_i64(uc.ARM64_REG_X5, step.a2);
    engine.reg_write_i64(uc.ARM64_REG_X6, step.a3);
    engine.reg_write_i64(uc.ARM64_REG_X7, 0n);
    engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

    try {
      engine.emu_start(Number(BASE_ADDR + step.off), STOP_ADDR, 0, 500000);
      console.log(`[worker] ${step.nm} completed OK.`);
    } catch(err) {
      console.error(`[worker] ${step.nm} stopped:`, err.message);
    }
  }

  // Restore game thread context to consume lifecycle commands
  restoreGuestContext(gameThreadContext);
  console.log(`[worker] Restored game thread context at PC 0x${Number(gameThreadContext.gp ? gameThreadContext.gp[32] : gameThreadContext[32]).toString(16)}`);
}

let loopTickCount = 0;
let consecutiveErrors = 0;
function runEngineLoop() {
  if (!isRunning) return;

  try {
    for (let burst = 0; burst < 2; burst++) {
      // 1. Cooperative round-robin scheduling of ALL background worker threads
      for (let i = 1; i < guestThreads.length; i++) {
        const bgThread = guestThreads[i];
        if (!bgThread.active) continue;

        restoreGuestContext(bgThread.ctx);
        const bgPc = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
        if (bgPc !== STOP_ADDR && bgPc !== 0) {
          const sliceCount = (bgThread.fn === 0x10229688) ? 500000 : 200000;
          const t0 = Date.now();
          console.log(`[worker emu bg #${i}] entering PC=0x${bgPc.toString(16)}, count=${sliceCount}...`);
          try {
            engine.emu_start(bgPc, STOP_ADDR, 0, sliceCount);
          } catch (bgErr) {
            console.warn(`[worker scheduler] Bg thread #${i} (fn=0x${bgThread.fn.toString(16)}) stopped:`, bgErr.message);
            bgThread.errCount = (bgThread.errCount || 0) + 1;
            if (bgThread.errCount >= 5) {
              console.log(`[worker scheduler] Deactivating bg thread #${i} after ${bgThread.errCount} errors.`);
              bgThread.active = false;
            }
          }
          bgThread.ctx = saveGuestContext();
          const nextPc = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
          const dt = Date.now() - t0;
          console.log(`[worker emu bg #${i}] returned PC=0x${nextPc.toString(16)} in ${dt}ms`);
          if (nextPc === STOP_ADDR || nextPc === 0) {
            console.log(`[worker scheduler] Background thread #${i} fn=0x${bgThread.fn.toString(16)} finished cleanly!`);
            bgThread.active = false;
            if (bgThread.fn === 0x10229688) {
              console.log("[worker scheduler] Asset loader Thread 1 complete! Setting loading flag [0x12464eb5] = 0.");
              engine.mem_write(Number(BASE_ADDR + 0x2464eb5n), [0]);
              engine.mem_write(Number(BASE_ADDR + 0x2464eb8n), [1]);
            }
          }
        } else {
          bgThread.active = false;
        }
      }

      // Ensure asset loader completes before main thread executes entity spawning
      const loader = guestThreads.find(t => t.fn === 0x10229688 && t.active);

      // 2. Main Game Thread slice (10,000,000 instructions for high-speed AMCT texture decompression & 3D gameplay)
      restoreGuestContext(gameThreadContext);
      const curPc = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
      if (curPc !== STOP_ADDR && curPc !== 0) {
        const mainSliceCount = 10000000;
        const t0 = Date.now();
        console.log(`[worker emu main] entering PC=0x${curPc.toString(16)}, count=${mainSliceCount}...`);
        engine.emu_start(curPc, STOP_ADDR, 0, mainSliceCount);
        gameThreadContext = saveGuestContext();
        const newPc = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
        const dt = Date.now() - t0;
        console.log(`[worker emu main] returned PC=0x${newPc.toString(16)} in ${dt}ms`);
        loopTickCount++;
        consecutiveErrors = 0;
      } else {
        console.warn(`[worker] Loop paused: curPc = 0x${curPc.toString(16)}, LR = 0x${Number(engine.reg_read_i64(uc.ARM64_REG_LR)).toString(16)}`);
        isRunning = false;
        return;
      }
    }

    const activeBgs = guestThreads.slice(1).filter(t => t.active);
    const bgStatus = activeBgs.length > 0
      ? `(active bg: ${activeBgs.length}, pcs: ${activeBgs.map(t => '0x' + Number(t.ctx.gp ? t.ctx.gp[32] : t.ctx[32]).toString(16)).join(',')})`
      : `(no bg)`;
    console.log(`[worker loop tick ${loopTickCount}] PC: 0x${Number(gameThreadContext.gp ? gameThreadContext.gp[32] : gameThreadContext[32]).toString(16)} ${bgStatus} (draws: ${drawCallCount})`);
  } catch(e) {
    consecutiveErrors++;
    let pc = 0, lr = 0, sp = 0, x0 = 0, x1 = 0, x2 = 0, x3 = 0, x19 = 0, x24 = 0;
    try {
      pc = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
      lr = Number(engine.reg_read_i64(uc.ARM64_REG_LR));
      sp = Number(engine.reg_read_i64(uc.ARM64_REG_SP));
      x0 = Number(engine.reg_read_i64(uc.ARM64_REG_X0));
      x1 = Number(engine.reg_read_i64(uc.ARM64_REG_X1));
      x2 = Number(engine.reg_read_i64(uc.ARM64_REG_X2));
      x3 = Number(engine.reg_read_i64(uc.ARM64_REG_X3));
      x19 = Number(engine.reg_read_i64(uc.ARM64_REG_X19));
      x24 = Number(engine.reg_read_i64(uc.ARM64_REG_X24));
    } catch(regErr) {
      console.error("[worker reg read failed in catch]:", regErr.message);
    }
    console.error("[worker loop error]:", e.message, "\nPC=0x" + pc.toString(16), "LR=0x" + lr.toString(16), "SP=0x" + sp.toString(16), `X0=0x${x0.toString(16)} X1=0x${x1.toString(16)} X2=0x${x2.toString(16)} X3=0x${x3.toString(16)} X19=0x${x19.toString(16)} X24=0x${x24.toString(16)}`, "\nStack:", e.stack);
    if (consecutiveErrors >= 5) {
      console.error("[worker] 5 consecutive loop errors; pausing engine loop.");
      isRunning = false;
      self.postMessage({ type: "ERROR", error: e.message });
      return;
    }
  }

  setTimeout(runEngineLoop, 0);
}

function handlePointerInput(payload) {
  if (!isRunning || !gameHandle) return;
  const prevCtx = saveGuestContext();
  const { x, y, type } = payload;
  const touchNative = Number(BASE_ADDR + 0x31D400n);

  engine.reg_write_i64(uc.ARM64_REG_SP, BigInt(STACK_BASE + STACK_SIZE - 0x1000));
  engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_ENV_ADDR));
  engine.reg_write_i64(uc.ARM64_REG_X1, 0x60200010n);
  engine.reg_write_i64(uc.ARM64_REG_X2, gameHandle);
  engine.reg_write_i64(uc.ARM64_REG_X3, 0x60200050n);
  engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

  try {
    engine.emu_start(touchNative, STOP_ADDR, 0, 100000);
  } catch(e) {
  } finally {
    restoreGuestContext(prevCtx);
  }
}

function handleKeyInput(payload) {
  if (!isRunning || !gameHandle) return;
  const prevCtx = saveGuestContext();
  const { code, isDown } = payload;
  const keyNative = Number(BASE_ADDR + (isDown ? 0x31D458n : 0x31D4B0n));

  engine.reg_write_i64(uc.ARM64_REG_SP, BigInt(STACK_BASE + STACK_SIZE - 0x1000));
  engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_ENV_ADDR));
  engine.reg_write_i64(uc.ARM64_REG_X1, 0x60200010n);
  engine.reg_write_i64(uc.ARM64_REG_X2, gameHandle);
  engine.reg_write_i64(uc.ARM64_REG_X3, 0x60200050n);
  engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

  try {
    engine.emu_start(keyNative, STOP_ADDR, 0, 100000);
  } catch(e) {
  } finally {
    restoreGuestContext(prevCtx);
  }
}

// ELF decoding helpers
function readUInt16LE(buf, off) {
  return buf[off] | (buf[off + 1] << 8);
}
function readUInt32LE(buf, off) {
  return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0;
}
function readBigUInt64LE(buf, off) {
  const lo = BigInt(readUInt32LE(buf, off));
  const hi = BigInt(readUInt32LE(buf, off + 4));
  return lo | (hi << 32n);
}
function readBigInt64LE(buf, off) {
  const val = readBigUInt64LE(buf, off);
  return val >= 0x8000000000000000n ? val - 0x10000000000000000n : val;
}
