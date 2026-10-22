/**
 * tests/test_guest_boot.js
 * Validates JNI_OnLoad and early boot execution of libpayback2.so in Unicorn AArch64.
 */

import fs from "fs";
import MUnicorn from "@alexaltea/unicorn-js/aarch64";

const SO_PATH = "/Users/thegt/Downloads/payback/web/engine/libpayback2.so";
const BASE_ADDR = 0x10000000n;
const TRAMPOLINE_BASE = 0x50000000;
const STACK_BASE = 0x70000000;
const STACK_SIZE = 0x800000;
const HEAP_BASE = 0x20000000;
const HEAP_SIZE = 0x18000000;

const JNI_ENV_ADDR = 0x60100000;
const JNI_VM_ADDR  = 0x60180000;

async function main() {
  console.log("[boot] Initializing Unicorn...");
  const uc = await MUnicorn();
  const e = new uc.Unicorn(uc.ARCH_ARM64, uc.MODE_ARM);

  console.log("[boot] Reading libpayback2.so...");
  const buf = fs.readFileSync(SO_PATH);

  // 1. Map memory regions
  e.mem_map(Number(BASE_ADDR), 0x2800000, uc.PROT_ALL); // 40MB for ELF
  e.mem_map(TRAMPOLINE_BASE, 0x200000, uc.PROT_ALL);   // 2MB for trampolines
  e.mem_map(STACK_BASE, STACK_SIZE, uc.PROT_ALL);       // 8MB stack
  e.mem_map(HEAP_BASE, HEAP_SIZE, uc.PROT_ALL);         // 64MB heap
  e.mem_map(0x60000000, 0x2000000, uc.PROT_ALL);       // 32MB JNI / structures

  // SP
  e.reg_write_i64(uc.ARM64_REG_SP, BigInt(STACK_BASE + STACK_SIZE - 0x1000));

  // 2. Load PT_LOAD segments
  const phoff = Number(buf.readBigUInt64LE(0x20));
  const phnum = buf.readUInt16LE(0x38);
  const phentsize = buf.readUInt16LE(0x36);

  for (let i = 0; i < phnum; i++) {
    const off = phoff + i * phentsize;
    const type = buf.readUInt32LE(off);
    if (type === 1) { // PT_LOAD
      const p_offset = Number(buf.readBigUInt64LE(off + 0x08));
      const p_vaddr = buf.readBigUInt64LE(off + 0x10);
      const p_filesz = Number(buf.readBigUInt64LE(off + 0x20));
      if (p_filesz > 0) {
        const segBytes = buf.subarray(p_offset, p_offset + p_filesz);
        e.mem_write(Number(BASE_ADDR + p_vaddr), Array.from(segBytes));
      }
    }
  }

  // 3. Trampoline system
  let nextTramp = TRAMPOLINE_BASE;
  const trampMap = new Map(); // addr -> { name, handler }

  function addTrampoline(name, handler) {
    const addr = nextTramp;
    nextTramp += 16;
    // Write "brk #0" (0xd4200000)
    e.mem_write(addr, [0x00, 0x00, 0x20, 0xd4]);
    trampMap.set(addr, { name, handler });
    return addr;
  }

  e.hook_add(uc.HOOK_MEM_UNMAPPED, (handle, type, addr, size, value) => {
    const pc = Number(e.reg_read_i64(uc.ARM64_REG_PC));
    const lr = Number(e.reg_read_i64(uc.ARM64_REG_LR));
    const x0 = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const x8 = Number(e.reg_read_i64(uc.ARM64_REG_X8));
    console.error(`[UNMAPPED MEMORY] Type: ${type}, Addr: 0x${Number(addr).toString(16)}, Size: ${size}, PC: 0x${pc.toString(16)}, LR: 0x${lr.toString(16)}, X0: 0x${x0.toString(16)}, X8: 0x${x8.toString(16)}`);
    return false;
  });
  e.hook_add(uc.HOOK_INTR, (handle, intno) => {
    const pc = Number(e.reg_read_i64(uc.ARM64_REG_PC));
    const tramp = trampMap.get(pc);
    if (tramp) {
      // console.log(`[tramp] ${tramp.name} called`);
      const retVal = tramp.handler(e);
      if (retVal !== undefined) {
        e.reg_write_i64(uc.ARM64_REG_X0, BigInt(retVal));
      }
      const lr = e.reg_read_i64(uc.ARM64_REG_LR);
      e.reg_write_i64(uc.ARM64_REG_PC, lr);
    } else {
      console.error(`[error] Unknown interrupt ${intno} at PC 0x${pc.toString(16)}`);
      e.emu_stop();
    }
  });

  // Setup simple guest heap bump allocator
  let heapPtr = HEAP_BASE;
  function guestAlloc(size, who = "") {
    const aligned = (size + 15) & ~15;
    const res = heapPtr;
    heapPtr += aligned;
    console.log(`[alloc] 0x${res.toString(16)} (size ${size}, aligned ${aligned}) ${who}`);
    return res;
  }

  // Helper: read C string from guest memory
  function readCString(addr) {
    if (!addr) return "";
    let cur = addr;
    const bytes = [];
    while (true) {
      const b = e.mem_read(cur, 1)[0];
      if (b === 0) break;
      bytes.push(b);
      cur++;
      if (bytes.length > 2048) break;
    }
    return Buffer.from(bytes).toString("utf8");
  }

  // Default shim registry
  const shims = new Map();

  function registerShim(name, handler) {
    const addr = addTrampoline(name, handler);
    shims.set(name, addr);
    return addr;
  }

  // Logging & abort
  registerShim("__android_log_print", (e) => {
    const prio = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const tag = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const fmt = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X2)));
    console.log(`[log_print] [${tag}] ${fmt}`);
    return 0;
  });

  registerShim("__android_log_assert", (e) => {
    const cond = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const tag = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const fmt = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X2)));
    console.error(`[ASSERT] [${tag}] ${cond}: ${fmt}`);
    return 0;
  });

  registerShim("__stack_chk_fail", () => {
    console.error("[FATAL] Stack check failed!");
    return 0;
  });

  // Memory operations
  registerShim("malloc", (e) => {
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    return guestAlloc(sz);
  });
  registerShim("calloc", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const total = n * sz;
    const addr = guestAlloc(total);
    e.mem_write(addr, new Array(total).fill(0));
    return addr;
  });
  // C++ operator new & delete
  registerShim("_Znwm", (e) => {
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    return guestAlloc(sz);
  });
  registerShim("_Znam", (e) => {
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    return guestAlloc(sz);
  });
  registerShim("_ZdlPv", () => 0);
  registerShim("_ZdaPv", () => 0);
  registerShim("_ZdlPvm", () => 0);
  registerShim("realloc", (e) => {
    const ptr = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const newPtr = guestAlloc(sz);
    if (ptr) {
      const oldBytes = Array.from(e.mem_read(ptr, Math.min(sz, 256)));
      e.mem_write(newPtr, oldBytes);
    }
    return newPtr;
  });

  registerShim("__memcpy_aarch64_simd", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const src = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (n > 0) {
      const bytes = Array.from(e.mem_read(src, n));
      e.mem_write(dst, bytes);
    }
    return dst;
  });
  registerShim("__memset_aarch64", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const val = Number(e.reg_read_i64(uc.ARM64_REG_X1)) & 0xff;
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (n > 0) {
      e.mem_write(dst, new Array(n).fill(val));
    }
    return dst;
  });
  registerShim("__memcmp_aarch64", (e) => {
    const s1 = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const s2 = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const b1 = e.mem_read(s1, n);
    const b2 = e.mem_read(s2, n);
    for (let i = 0; i < n; i++) {
      if (b1[i] !== b2[i]) return b1[i] - b2[i];
    }
    return 0;
  });

  // Pthreads & Sync stubs (single-threaded cooperative emulation)
  registerShim("pthread_mutex_init", () => 0);
  registerShim("pthread_mutex_lock", () => 0);
  registerShim("pthread_mutex_trylock", () => 0);
  registerShim("pthread_mutex_unlock", () => 0);
  registerShim("pthread_mutex_destroy", () => 0);
  registerShim("pthread_cond_init", () => 0);
  registerShim("pthread_cond_wait", (e) => {
    if (guestThreads.length > 0 && guestThreads[0].arg) {
      const app = guestThreads[0].arg;
      const runningBuf = e.mem_read(app + 0x140, 4);
      if (runningBuf[0] === 0) {
        e.mem_write(app + 0x140, [1, 0, 0, 0]);
      }
      const pendingWin = e.mem_read(app + 0x150, 8);
      const curWin = e.mem_read(app + 0x38, 8);
      const pendingVal = pendingWin.reduce((acc, b, idx) => acc | (BigInt(b) << BigInt(idx * 8)), 0n);
      const curVal = curWin.reduce((acc, b, idx) => acc | (BigInt(b) << BigInt(idx * 8)), 0n);
      if (pendingVal !== 0n && curVal === 0n) {
        e.mem_write(app + 0x38, pendingWin);
        console.log(`[pthread_cond_wait] synced window=0x${pendingVal.toString(16)} to app+0x38`);
      }
    }
    return 0;
  });
  registerShim("pthread_cond_signal", () => 0);
  registerShim("pthread_cond_broadcast", () => 0);
  registerShim("pthread_cond_destroy", () => 0);
  registerShim("pthread_self", () => 1);
  const guestThreads = [];
  registerShim("pthread_create", (e) => {
    const pt = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const attr = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const fn = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const arg = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    console.log(`[pthread_create] fn=0x${fn.toString(16)}, arg=0x${arg.toString(16)}`);
    guestThreads.push({ fn, arg });
    if (pt) {
      ptrBuf.writeBigUInt64LE(BigInt(guestThreads.length), 0);
      e.mem_write(pt, Array.from(ptrBuf));
    }
    return 0;
  });
  registerShim("pthread_detach", () => 0);
  registerShim("pthread_join", () => 0);
  registerShim("sched_yield", () => 0);
  registerShim("usleep", () => 0);
  registerShim("sleep", () => 0);

  // Time & Rand
  registerShim("time", () => Math.floor(Date.now() / 1000));
  registerShim("gettimeofday", (e) => {
    const tv = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (tv) {
      const now = Date.now();
      const sec = Math.floor(now / 1000);
      const usec = (now % 1000) * 1000;
      const b = Buffer.alloc(16);
      b.writeBigInt64LE(BigInt(sec), 0);
      b.writeBigInt64LE(BigInt(usec), 8);
      e.mem_write(tv, Array.from(b));
    }
    return 0;
  });
  registerShim("rand", () => Math.floor(Math.random() * 0x7fffffff));
  registerShim("arc4random", () => (Math.random() * 0xffffffff) >>> 0);

  // System property
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
  registerShim("pthread_once", () => 0);
  registerShim("pthread_key_create", () => 0);
  registerShim("pthread_key_delete", () => 0);
  registerShim("pthread_setspecific", () => 0);
  registerShim("pthread_getspecific", () => 0);
  registerShim("pthread_rwlock_wrlock", () => 0);
  registerShim("nanosleep", () => 0);
  registerShim("getauxval", () => 0);
  registerShim("strerror_r", (e) => {
    const errnum = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buf = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const buflen = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const str = `Error ${errnum}`;
    e.mem_write(buf, [...Buffer.from(str.slice(0, buflen - 1)), 0]);
    return 0;
  });
  registerShim("strtoul", (e) => {
    const s = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    return parseInt(s, 10) || 0;
  });
  registerShim("strtoull", (e) => {
    const s = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    return parseInt(s, 10) || 0;
  });

  // JNI Setup:
  // JNIEnv* points to a pointer to the method table
  const JNI_ENV_VTABLE = JNI_ENV_ADDR + 0x100;
  const JNI_VM_VTABLE = JNI_VM_ADDR + 0x100;

  // env cell: [0x60100000] = 0x60100100
  const ptrBuf = Buffer.alloc(8);
  ptrBuf.writeBigUInt64LE(BigInt(JNI_ENV_VTABLE), 0);
  e.mem_write(JNI_ENV_ADDR, Array.from(ptrBuf));

  // vm cell: [0x60180000] = 0x60180100
  ptrBuf.writeBigUInt64LE(BigInt(JNI_VM_VTABLE), 0);
  e.mem_write(JNI_VM_ADDR, Array.from(ptrBuf));

  // Default all JNIEnv slots to a fallback stub
  for (let i = 0; i < 240; i++) {
    const slotIdx = i;
    const tramp = addTrampoline(`JNIEnv_slot_${slotIdx}`, () => 0);
    ptrBuf.writeBigUInt64LE(BigInt(tramp), 0);
    e.mem_write(JNI_ENV_VTABLE + slotIdx * 8, Array.from(ptrBuf));
  }

  // GetJavaVM (slot 219 = 0xdb)
  const getVmTramp = addTrampoline("JNIEnv_GetJavaVM", (e) => {
    const pvm = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    ptrBuf.writeBigUInt64LE(BigInt(JNI_VM_ADDR), 0);
    e.mem_write(pvm, Array.from(ptrBuf));
    return 0;
  });
  ptrBuf.writeBigUInt64LE(BigInt(getVmTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 219 * 8, Array.from(ptrBuf));

  // GetMethodID (slot 33) & GetStaticMethodID (slot 113)
  let nextMethodId = 0x60300010;
  function makeMethodTramp(name) {
    return addTrampoline(name, (e) => {
      const cls = Number(e.reg_read_i64(uc.ARM64_REG_X1));
      const mName = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X2)));
      const sig = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X3)));
      console.log(`[jni] ${name}(cls=0x${cls.toString(16)}, "${mName}", "${sig}")`);
      const mid = nextMethodId;
      nextMethodId += 16;
      return mid;
    });
  }
  const getMethodTramp = makeMethodTramp("GetMethodID");
  ptrBuf.writeBigUInt64LE(BigInt(getMethodTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 33 * 8, Array.from(ptrBuf));

  const getStaticMethodTramp = makeMethodTramp("GetStaticMethodID");
  ptrBuf.writeBigUInt64LE(BigInt(getStaticMethodTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 113 * 8, Array.from(ptrBuf));

  // GetFieldID (slot 94) & GetStaticFieldID (slot 144)
  const getFieldTramp = makeMethodTramp("GetFieldID");
  ptrBuf.writeBigUInt64LE(BigInt(getFieldTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 94 * 8, Array.from(ptrBuf));

  const getStaticFieldTramp = makeMethodTramp("GetStaticFieldID");
  ptrBuf.writeBigUInt64LE(BigInt(getStaticFieldTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 144 * 8, Array.from(ptrBuf));

  // CallObjectMethod (slot 34) & CallStaticObjectMethod (slot 114)
  const callObjTramp = addTrampoline("CallObjectMethod", (e) => {
    return 0x60200050; // dummy object handle
  });
  ptrBuf.writeBigUInt64LE(BigInt(callObjTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 34 * 8, Array.from(ptrBuf));
  e.mem_write(JNI_ENV_VTABLE + 114 * 8, Array.from(ptrBuf));

  // NewGlobalRef (slot 21)
  const newGlobalRefTramp = addTrampoline("NewGlobalRef", (e) => {
    const obj = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    return obj || 0x60200060;
  });
  ptrBuf.writeBigUInt64LE(BigInt(newGlobalRefTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 21 * 8, Array.from(ptrBuf));

  // String handle support
  const strHandles = new Map();
  function makeStringHandle(str) {
    const addr = guestAlloc(str.length + 1);
    e.mem_write(addr, [...Buffer.from(str), 0]);
    const handle = 0x60400000 + strHandles.size * 16;
    strHandles.set(handle, addr);
    return handle;
  }

  // GetStringUTFChars (slot 169)
  const getStringUtfCharsTramp = addTrampoline("JNIEnv_GetStringUTFChars", (e) => {
    const jstr = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const addr = strHandles.get(jstr) || 0;
    return addr;
  });
  ptrBuf.writeBigUInt64LE(BigInt(getStringUtfCharsTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 169 * 8, Array.from(ptrBuf));

  // GetStringUTFLength (slot 168)
  const getStringUtfLengthTramp = addTrampoline("JNIEnv_GetStringUTFLength", (e) => {
    const jstr = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const addr = strHandles.get(jstr) || 0;
    return readCString(addr).length;
  });
  ptrBuf.writeBigUInt64LE(BigInt(getStringUtfLengthTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 168 * 8, Array.from(ptrBuf));

  // Asset Manager support
  const assetMap = new Map();
  let nextAssetHandle = 0x60500010;

  registerShim("AAssetManager_open", (e) => {
    const path = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    const mode = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    console.log(`[asset] AAssetManager_open("${path}", mode=${mode})`);

    const fullPath = `/Users/thegt/Downloads/payback/web/assets/${path}`;
    if (fs.existsSync(fullPath)) {
      const fileBytes = fs.readFileSync(fullPath);
      const handle = nextAssetHandle;
      nextAssetHandle += 16;
      assetMap.set(handle, { path, data: fileBytes, pos: 0 });
      console.log(`[asset] Opened "${path}" (${fileBytes.length} bytes) -> handle 0x${handle.toString(16)}`);
      return handle;
    } else {
      console.warn(`[asset] NOT FOUND: "${path}"`);
      return 0;
    }
  });

  registerShim("AAsset_getLength", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const asset = assetMap.get(handle);
    return asset ? asset.data.length : 0;
  });

  registerShim("AAsset_getRemainingLength", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const asset = assetMap.get(handle);
    return asset ? (asset.data.length - asset.pos) : 0;
  });

  registerShim("AAsset_read", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buf = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const asset = assetMap.get(handle);
    if (!asset) return -1;
    const toRead = Math.min(count, asset.data.length - asset.pos);
    if (toRead > 0) {
      const chunk = asset.data.subarray(asset.pos, asset.pos + toRead);
      e.mem_write(buf, Array.from(chunk));
      asset.pos += toRead;
    }
    return toRead;
  });

  registerShim("AAsset_seek", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const offset = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const whence = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const asset = assetMap.get(handle);
    if (!asset) return -1;
    if (whence === 0) asset.pos = offset; // SEEK_SET
    else if (whence === 1) asset.pos += offset; // SEEK_CUR
    else if (whence === 2) asset.pos = asset.data.length + offset; // SEEK_END
    return asset.pos;
  });

  registerShim("AAsset_close", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    assetMap.delete(handle);
    return 0;
  });

  // OpenGL ES 2.0 Dispatchers with logging
  registerShim("glViewport", (e) => {
    const x = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const y = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const w = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const h = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    console.log(`[GL] glViewport(${x}, ${y}, ${w}, ${h})`);
    return 0;
  });
  registerShim("glClearColor", (e) => {
    console.log("[GL] glClearColor called");
    return 0;
  });
  registerShim("glClear", (e) => {
    const mask = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    console.log(`[GL] glClear(0x${mask.toString(16)})`);
    return 0;
  });
  registerShim("glDrawArrays", (e) => {
    const mode = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const first = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    console.log(`[GL] glDrawArrays(mode=${mode}, first=${first}, count=${count})`);
    return 0;
  });
  registerShim("glDrawElements", (e) => {
    const mode = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const type = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    console.log(`[GL] glDrawElements(mode=${mode}, count=${count}, type=${type})`);
    return 0;
  });
  registerShim("glGetString", (e) => {
    const name = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    let str = "WebGL 2.0";
    if (name === 0x1f00) str = "WebKit"; // GL_VENDOR
    else if (name === 0x1f01) str = "WebGL Metal/Vulkan"; // GL_RENDERER
    else if (name === 0x1f02) str = "OpenGL ES 2.0 (WebGL 2.0)"; // GL_VERSION
    else if (name === 0x1f03) str = "GL_OES_standard_derivatives"; // GL_EXTENSIONS
    const addr = guestAlloc(str.length + 1);
    e.mem_write(addr, [...Buffer.from(str), 0]);
    return addr;
  });
  registerShim("glGetIntegerv", (e) => {
    const pname = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const params = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (params) {
      if (pname === 0x8CA6) e.mem_write(params, [0, 0, 0, 0]); // GL_FRAMEBUFFER_BINDING
      else e.mem_write(params, [0, 0, 0, 0]);
    }
    return 0;
  });
  registerShim("glGenTextures", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const textures = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const b = Buffer.alloc(4);
      b.writeUInt32LE(i + 1, 0);
      e.mem_write(textures + i * 4, Array.from(b));
    }
    return 0;
  });
  registerShim("glGenBuffers", (e) => {
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buffers = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    for (let i = 0; i < n; i++) {
      const b = Buffer.alloc(4);
      b.writeUInt32LE(i + 1, 0);
      e.mem_write(buffers + i * 4, Array.from(b));
    }
    return 0;
  });
  registerShim("ANativeWindow_fromSurface", () => 0x60200040);
  registerShim("ANativeWindow_acquire", () => 0);
  registerShim("ANativeWindow_release", () => 0);
  registerShim("ANativeWindow_getWidth", () => 1920);
  registerShim("ANativeWindow_getHeight", () => 1080);
  registerShim("ANativeWindow_setBuffersGeometry", () => 0);

  // EGL Stubs
  registerShim("eglGetDisplay", () => 0x60600010);
  registerShim("eglInitialize", (e) => {
    const maj = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const min = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (maj) e.mem_write(maj, [1, 0, 0, 0]);
    if (min) e.mem_write(min, [4, 0, 0, 0]);
    return 1;
  });
  registerShim("eglChooseConfig", (e) => {
    const configs = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const num_config = Number(e.reg_read_i64(uc.ARM64_REG_X4));
    if (configs) ptrBuf.writeBigUInt64LE(0x60600020n, 0), e.mem_write(configs, Array.from(ptrBuf));
    if (num_config) e.mem_write(num_config, [1, 0, 0, 0]);
    return 1;
  });
  registerShim("eglCreateWindowSurface", () => 0x60600030);
  registerShim("eglCreateContext", () => 0x60600040);
  registerShim("eglMakeCurrent", () => 1);
  registerShim("eglSwapBuffers", () => {
    console.log("[EGL] eglSwapBuffers PRESENT FRAME!");
    return 1;
  });
  registerShim("glCheckFramebufferStatus", () => 0x8CD5); // GL_FRAMEBUFFER_COMPLETE
  registerShim("eglGetConfigAttrib", (e) => {
    const val = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    if (val) e.mem_write(val, [8, 0, 0, 0]);
    return 1;
  });
  registerShim("eglDestroySurface", () => 1);
  registerShim("eglDestroyContext", () => 1);
  registerShim("eglTerminate", () => 1);
  registerShim("AAssetManager_fromJava", () => 0x60200020);

  // OpenSL ES Object Model
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
  const SL_BQ_ITF = SL_BASE + 0x2a0;

  // Generic SL vtable stub returning 0
  const slStubTramp = addTrampoline("SL_stub", () => 0);
  for (let i = 0; i < 20; i++) {
    ptrBuf.writeBigUInt64LE(BigInt(slStubTramp), 0);
    e.mem_write(SL_OBJ_VTBL + i * 8, Array.from(ptrBuf));
    e.mem_write(SL_ENG_VTBL + i * 8, Array.from(ptrBuf));
    e.mem_write(SL_PLAY_VTBL + i * 8, Array.from(ptrBuf));
    e.mem_write(SL_BQ_VTBL + i * 8, Array.from(ptrBuf));
  }

  // SLObjectItf->GetInterface (slot 3, offset 0x18)
  const slGetInterfaceTramp = addTrampoline("SLObjectItf_GetInterface", (e) => {
    const self = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const pOut = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    if (pOut) {
      if (self === SL_ENG_OBJ) {
        ptrBuf.writeBigUInt64LE(BigInt(SL_ENG_ITF), 0);
        e.mem_write(pOut, Array.from(ptrBuf));
      } else if (self === SL_PLY_OBJ) {
        ptrBuf.writeBigUInt64LE(BigInt(SL_PLY_ITF), 0);
        e.mem_write(pOut, Array.from(ptrBuf));
      }
    }
    return 0; // SL_RESULT_SUCCESS
  });
  ptrBuf.writeBigUInt64LE(BigInt(slGetInterfaceTramp), 0);
  e.mem_write(SL_OBJ_VTBL + 3 * 8, Array.from(ptrBuf));

  // SLEngineItf->CreateAudioPlayer (slot 2, offset 0x10)
  const slCreatePlayerTramp = addTrampoline("SLEngineItf_CreateAudioPlayer", (e) => {
    const pPlayer = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (pPlayer) {
      ptrBuf.writeBigUInt64LE(BigInt(SL_PLY_OBJ), 0);
      e.mem_write(pPlayer, Array.from(ptrBuf));
    }
    return 0;
  });
  ptrBuf.writeBigUInt64LE(BigInt(slCreatePlayerTramp), 0);
  e.mem_write(SL_ENG_VTBL + 2 * 8, Array.from(ptrBuf));

  // SLEngineItf->CreateOutputMix (slot 7, offset 0x38)
  const slCreateMixTramp = addTrampoline("SLEngineItf_CreateOutputMix", (e) => {
    const pMix = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    if (pMix) {
      ptrBuf.writeBigUInt64LE(BigInt(SL_MIX_OBJ), 0);
      e.mem_write(pMix, Array.from(ptrBuf));
    }
    return 0;
  });
  ptrBuf.writeBigUInt64LE(BigInt(slCreateMixTramp), 0);
  e.mem_write(SL_ENG_VTBL + 7 * 8, Array.from(ptrBuf));

  // Point object instances to vtables
  ptrBuf.writeBigUInt64LE(BigInt(SL_OBJ_VTBL), 0);
  e.mem_write(SL_ENG_OBJ, Array.from(ptrBuf));
  e.mem_write(SL_MIX_OBJ, Array.from(ptrBuf));
  e.mem_write(SL_PLY_OBJ, Array.from(ptrBuf));

  ptrBuf.writeBigUInt64LE(BigInt(SL_ENG_VTBL), 0);
  e.mem_write(SL_ENG_ITF, Array.from(ptrBuf));

  ptrBuf.writeBigUInt64LE(BigInt(SL_PLAY_VTBL), 0);
  e.mem_write(SL_PLY_ITF, Array.from(ptrBuf));

  ptrBuf.writeBigUInt64LE(BigInt(SL_BQ_VTBL), 0);
  e.mem_write(SL_BQ_ITF, Array.from(ptrBuf));

  registerShim("slCreateEngine", (e) => {
    const pEngine = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (pEngine) {
      ptrBuf.writeBigUInt64LE(BigInt(SL_ENG_OBJ), 0);
      e.mem_write(pEngine, Array.from(ptrBuf));
    }
    return 0; // SL_RESULT_SUCCESS
  });
  registerShim("AConfiguration_new", () => 0x60200030);
  registerShim("AConfiguration_delete", () => 0);
  registerShim("AConfiguration_fromAssetManager", () => 0);
  registerShim("AConfiguration_getCountry", (e) => {
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    e.mem_write(out, [0x55, 0x53, 0]); // "US"
    return 0;
  });
  registerShim("AConfiguration_getLanguage", (e) => {
    const out = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    e.mem_write(out, [0x65, 0x6e, 0]); // "en"
    return 0;
  });

  // In-memory pipe and file descriptor table for Android message queues
  const pipeQueues = new Map(); // fd -> array of bytes
  let nextPipeFd = 10;

  registerShim("pipe", (e) => {
    const fds = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const rfd = nextPipeFd++;
    const wfd = nextPipeFd++;
    const q = [];
    pipeQueues.set(rfd, q);
    pipeQueues.set(wfd, q);
    console.log(`[pipe] created rfd=${rfd}, wfd=${wfd}`);
    const b = Buffer.alloc(8);
    b.writeInt32LE(rfd, 0);
    b.writeInt32LE(wfd, 4);
    e.mem_write(fds, Array.from(b));
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
    e.mem_write(buf, chunk);
    // console.log(`[read] fd=${fd}, read ${n} bytes`);
    return n;
  });

  registerShim("write", (e) => {
    const fd = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const buf = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const q = pipeQueues.get(fd);
    if (q) {
      const bytes = Array.from(e.mem_read(buf, count));
      q.push(...bytes);
      console.log(`[write] fd=${fd}, wrote ${count} bytes:`, bytes);
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
      console.log(`[file] fopen("${cleanPath}", "${mode}") -> user data`);
      if (mode.includes("r")) return 0;
    }
    const fullPath = `/Users/thegt/Downloads/payback/web/assets/${cleanPath}`;
    if (fs.existsSync(fullPath)) {
      const fileBytes = fs.readFileSync(fullPath);
      const handle = nextFileHandle;
      nextFileHandle += 16;
      openFiles.set(handle, { path: cleanPath, data: fileBytes, pos: 0 });
      console.log(`[file] fopen("${cleanPath}", "${mode}") -> ${fileBytes.length} bytes (handle 0x${handle.toString(16)})`);
      return handle;
    }
    console.warn(`[file] fopen("${cleanPath}") NOT FOUND at ${fullPath}`);
    return 0;
  });

  registerShim("fclose", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    openFiles.delete(handle);
    return 0;
  });

  registerShim("fread", (e) => {
    const ptr = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const size = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const count = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    const f = openFiles.get(handle);
    if (!f) return 0;
    const total = size * count;
    const avail = f.data.length - f.pos;
    const toRead = Math.min(total, Math.max(0, avail));
    if (toRead > 0) {
      const chunk = f.data.subarray(f.pos, f.pos + toRead);
      e.mem_write(ptr, Array.from(chunk));
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
    if (whence === 0) f.pos = offset; // SEEK_SET
    else if (whence === 1) f.pos += offset; // SEEK_CUR
    else if (whence === 2) f.pos = f.data.length + offset; // SEEK_END
    return 0;
  });

  registerShim("ftell", (e) => {
    const handle = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const f = openFiles.get(handle);
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
    console.log(`[ALooper_addFd] fd=${fd}, ident=${ident}, cb=0x${cb.toString(16)}, data=0x${data.toString(16)}`);
    looperFds.push({ fd, ident, events, cb, data });
    return 1;
  });

  registerShim("ALooper_pollOnce", (e) => {
    const timeout = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const outFd = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const outEvents = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const outData = Number(e.reg_read_i64(uc.ARM64_REG_X3));

    // Check all registered fds
    for (const r of looperFds) {
      const q = pipeQueues.get(r.fd);
      if (q && q.length > 0) {
        // Events pending!
        // console.log(`[ALooper_pollOnce] Event on fd=${r.fd}! cb=0x${r.cb.toString(16)}`);
        if (r.cb) {
          // Call the callback cb(fd, events, data)
          const cbAddr = r.cb;
          e.reg_write_i64(uc.ARM64_REG_X0, BigInt(r.fd));
          e.reg_write_i64(uc.ARM64_REG_X1, 1n); // ALOOPER_EVENT_INPUT
          e.reg_write_i64(uc.ARM64_REG_X2, BigInt(r.data));
          e.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));
          try {
            e.emu_start(cbAddr, STOP_ADDR, 0, 50000);
          } catch(err) {
            console.error("[ALooper callback error]:", err.message);
          }
          return r.ident || 1;
        }
        if (outFd) {
          const b = Buffer.alloc(4); b.writeInt32LE(r.ident, 0);
          e.mem_write(outFd, Array.from(b));
        }
        if (outEvents) {
          const b = Buffer.alloc(4); b.writeInt32LE(1, 0);
          e.mem_write(outEvents, Array.from(b));
        }
        if (outData) {
          ptrBuf.writeBigUInt64LE(BigInt(r.data), 0);
          e.mem_write(outData, Array.from(ptrBuf));
        }
        return r.ident || 1;
      }
    }
    return -1; // ALOOPER_POLL_TIMEOUT
  });

  // emutls
  const emutlsMap = new Map();
  registerShim("__emutls_get_address", (e) => {
    const ctrl = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    if (emutlsMap.has(ctrl)) {
      return emutlsMap.get(ctrl);
    }
    const szBuf = e.mem_read(ctrl, 8);
    let sz = Number(BigInt(szBuf[0]) | (BigInt(szBuf[1]) << 8n) | (BigInt(szBuf[2]) << 16n) | (BigInt(szBuf[3]) << 24n));
    if (!sz || sz < 64) sz = 128;
    const addr = guestAlloc(sz);
    e.mem_write(addr, new Array(sz).fill(0));
    emutlsMap.set(ctrl, addr);
    return addr;
  });

  // String / memory functions
  registerShim("strlen", (e) => {
    const s = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    return readCString(s).length;
  });
  registerShim("strcpy", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const src = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const str = readCString(src);
    e.mem_write(dst, [...Buffer.from(str), 0]);
    return dst;
  });
  registerShim("strncpy", (e) => {
    const dst = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const src = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const str = readCString(src).slice(0, n);
    const b = Buffer.alloc(n, 0);
    Buffer.from(str).copy(b);
    e.mem_write(dst, Array.from(b));
    return dst;
  });
  registerShim("strcmp", (e) => {
    const s1 = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X0)));
    const s2 = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    if (s1 < s2) return -1;
    if (s1 > s2) return 1;
    return 0;
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
    const s1 = readCString(dst);
    const s2 = readCString(src);
    const combined = s1 + s2;
    e.mem_write(dst, [...Buffer.from(combined), 0]);
    return dst;
  });
  registerShim("strdup", (e) => {
    const src = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const str = readCString(src);
    const addr = guestAlloc(str.length + 1);
    e.mem_write(addr, [...Buffer.from(str), 0]);
    return addr;
  });

  // Formatted output helpers
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
    console.log(`[sprintf] fmt="${fmt}" -> "${formatted}"`);
    e.mem_write(dst, [...Buffer.from(formatted), 0]);
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
    console.log(`[snprintf] fmt="${fmt}" -> "${formatted}"`);
    e.mem_write(dst, [...Buffer.from(formatted), 0]);
    return formatted.length;
  });
  const getEnvTramp = addTrampoline("JavaVM_GetEnv", (e) => {
    const penv = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    ptrBuf.writeBigUInt64LE(BigInt(JNI_ENV_ADDR), 0);
    e.mem_write(penv, Array.from(ptrBuf));
    return 0; // JNI_OK
  });
  ptrBuf.writeBigUInt64LE(BigInt(getEnvTramp), 0);
  e.mem_write(JNI_VM_VTABLE + 0x30, Array.from(ptrBuf));

  // VM Vtable: AttachCurrentThread is at index 4 (offset 4 * 8 = 32 = 0x20)
  const attachThreadTramp = addTrampoline("JavaVM_AttachCurrentThread", (e) => {
    const penv = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    console.log(`[jni] JavaVM_AttachCurrentThread called, writing env 0x${JNI_ENV_ADDR.toString(16)} to 0x${penv.toString(16)}`);
    ptrBuf.writeBigUInt64LE(BigInt(JNI_ENV_ADDR), 0);
    e.mem_write(penv, Array.from(ptrBuf));
    return 0; // JNI_OK
  });
  ptrBuf.writeBigUInt64LE(BigInt(attachThreadTramp), 0);
  e.mem_write(JNI_VM_VTABLE + 0x20, Array.from(ptrBuf));
  e.mem_write(JNI_VM_VTABLE + 0x38, Array.from(ptrBuf)); // AttachCurrentThreadAsDaemon at index 7

  // JNIEnv Vtable:
  // RegisterNatives is at slot 215 (offset 215 * 8 = 1720 = 0x6b8)
  const regNativesTramp = addTrampoline("JNIEnv_RegisterNatives", (e) => {
    const clazz = Number(e.reg_read_i64(uc.ARM64_REG_X1));
    const methods = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const n = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    console.log(`[jni] RegisterNatives called for class 0x${clazz.toString(16)}, ${n} methods:`);
    for (let i = 0; i < n; i++) {
      const entry = methods + i * 24;
      const mNamePtr = Number(e.mem_read(entry, 8).reduce((acc, b, idx) => acc | (BigInt(b) << BigInt(idx * 8)), 0n));
      const mSigPtr = Number(e.mem_read(entry + 8, 8).reduce((acc, b, idx) => acc | (BigInt(b) << BigInt(idx * 8)), 0n));
      const mFnPtr = Number(e.mem_read(entry + 16, 8).reduce((acc, b, idx) => acc | (BigInt(b) << BigInt(idx * 8)), 0n));
      console.log(`  - ${readCString(mNamePtr)} ${readCString(mSigPtr)} -> 0x${mFnPtr.toString(16)}`);
    }
    return 0; // JNI_OK
  });
  ptrBuf.writeBigUInt64LE(BigInt(regNativesTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 215 * 8, Array.from(ptrBuf));

  // FindClass slot 6 (offset 6 * 8 = 48)
  const findClassTramp = addTrampoline("JNIEnv_FindClass", (e) => {
    const name = readCString(Number(e.reg_read_i64(uc.ARM64_REG_X1)));
    console.log(`[jni] FindClass("${name}")`);
    return 0x60200010;
  });
  ptrBuf.writeBigUInt64LE(BigInt(findClassTramp), 0);
  e.mem_write(JNI_ENV_VTABLE + 6 * 8, Array.from(ptrBuf));

  // 4. Apply Relocations
  const shoff = Number(buf.readBigUInt64LE(0x28));
  const shnum = buf.readUInt16LE(0x3c);
  const shentsize = buf.readUInt16LE(0x3a);

  // Parse dynsym & strtab
  let symtab = null, strtab = null, symcount = 0;
  for (let i = 0; i < shnum; i++) {
    const off = shoff + i * shentsize;
    const type = buf.readUInt32LE(off + 0x04);
    if (type === 11) { // SHT_DYNSYM
      const symOff = Number(buf.readBigUInt64LE(off + 0x18));
      const symSize = Number(buf.readBigUInt64LE(off + 0x20));
      const link = buf.readUInt32LE(off + 0x28);
      const strOff = Number(buf.readBigUInt64LE(shoff + link * shentsize + 0x18));
      symcount = symSize / 24;
      symtab = buf.subarray(symOff, symOff + symSize);
      strtab = buf.subarray(strOff);
      break;
    }
  }

  function getSymName(idx) {
    if (idx === 0) return "";
    const nameIdx = symtab.readUInt32LE(idx * 24);
    let end = nameIdx;
    while (strtab[end] !== 0) end++;
    return strtab.subarray(nameIdx, end).toString("utf8");
  }

  // Fallback missing import generator
  function getOrEmitShim(name) {
    if (shims.has(name)) return shims.get(name);
    // Generic stub
    const addr = addTrampoline(name, () => {
      console.log(`[stub call] ${name}`);
      return 0;
    });
    shims.set(name, addr);
    return addr;
  }

  // Read oracle_holes
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
    const valBuf = Buffer.alloc(8);
    valBuf.writeBigUInt64LE(BigInt(tgt));
    e.mem_write(slot, Array.from(valBuf));
  }
  console.log(`[boot] Wired ${oracleHoles.length} oracle holes.`);

  // Apply relocations
  let relCount = 0;
  for (let i = 0; i < shnum; i++) {
    const off = shoff + i * shentsize;
    const sh_type = buf.readUInt32LE(off + 0x04);
    if (sh_type === 4) { // SHT_RELA
      const r_offset = Number(buf.readBigUInt64LE(off + 0x18));
      const r_size = Number(buf.readBigUInt64LE(off + 0x20));
      const r_entsize = Number(buf.readBigUInt64LE(off + 0x38));

      for (let roff = r_offset; roff < r_offset + r_size; roff += r_entsize) {
        const r_addr = buf.readBigUInt64LE(roff);
        const r_info = buf.readBigUInt64LE(roff + 0x08);
        const r_addend = buf.readBigInt64LE(roff + 0x10);
        const r_type = Number(r_info & 0xFFFFFFFFn);
        const symidx = Number(r_info >> 32n);
        const targetAddr = Number(BASE_ADDR + r_addr);

        if (r_type === 1027) { // R_AARCH64_RELATIVE
          const val = BASE_ADDR + BigInt(r_addend);
          const valBuf = Buffer.alloc(8);
          valBuf.writeBigUInt64LE(val);
          e.mem_write(targetAddr, Array.from(valBuf));
          relCount++;
        } else if (r_type === 1025 || r_type === 1026) { // GLOB_DAT or JUMP_SLOT
          const shndx = symtab.readUInt16LE(symidx * 24 + 6);
          const symVal = symtab.readBigUInt64LE(symidx * 24 + 8);
          if (symidx > 0 && shndx !== 0) {
            // Defined internally in libpayback2.so!
            const target = BASE_ADDR + symVal;
            const valBuf = Buffer.alloc(8);
            valBuf.writeBigUInt64LE(target);
            e.mem_write(targetAddr, Array.from(valBuf));
            relCount++;
          } else {
            const symName = getSymName(symidx);
            if (symName) {
              const shimAddr = getOrEmitShim(symName);
              const valBuf = Buffer.alloc(8);
              valBuf.writeBigUInt64LE(BigInt(shimAddr));
              e.mem_write(targetAddr, Array.from(valBuf));
              relCount++;
            }
          }
        }
      }
    }
  }
  console.log(`[boot] Applied ${relCount} relocations.`);

  // Seed PairIP anchors:
  // [0x3b2120] = env (0x60100000)
  // [0x3b2208] = mint_id (0x60200000)
  ptrBuf.writeBigUInt64LE(BigInt(JNI_ENV_ADDR), 0);
  e.mem_write(Number(BASE_ADDR + 0x3b2120n), Array.from(ptrBuf));
  ptrBuf.writeBigUInt64LE(0x60200000n, 0);
  e.mem_write(Number(BASE_ADDR + 0x3b2208n), Array.from(ptrBuf));
  console.log("[boot] Seeded PairIP anchors.");

  // TCB / TLS (Thread Control Block) for TPIDR_EL0
  const TLS_BASE = 0x78000000;
  e.mem_map(TLS_BASE, 0x10000, uc.PROT_ALL);
  const canary = 0xdeadbeefcafebaben;
  ptrBuf.writeBigUInt64LE(canary, 0);
  e.mem_write(TLS_BASE + 0x28, Array.from(ptrBuf));
  e.reg_write_i64(uc.ARM64_REG_TPIDR_EL0, BigInt(TLS_BASE));
  console.log("[boot] Configured TPIDR_EL0 at 0x" + TLS_BASE.toString(16));

  // 5. Call JNI_OnLoad(vm, 0)
  const JNI_ONLOAD_ADDR = Number(BASE_ADDR + 0xeceacn);
  console.log(`[boot] Calling JNI_OnLoad at 0x${JNI_ONLOAD_ADDR.toString(16)}...`);
  e.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_VM_ADDR));
  e.reg_write_i64(uc.ARM64_REG_X1, 0n);

  // Return address: stop address
  const STOP_ADDR = 0x5fffff00;
  e.mem_map(STOP_ADDR & ~0xfff, 0x1000, uc.PROT_ALL);
  e.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

  try {
    e.emu_start(JNI_ONLOAD_ADDR, STOP_ADDR, 0, 500000);
  } catch(err) {
    console.error("[boot] emu_start finished/stopped:", err.message);
  }

  const retVal = e.reg_read_i64(uc.ARM64_REG_X0);
  const pc = e.reg_read_i64(uc.ARM64_REG_PC);
  console.log(`[boot] JNI_OnLoad returned 0x${retVal.toString(16)} (PC: 0x${pc.toString(16)})`);

  // Call initializeNativeCode
  const INIT_NATIVE_ADDR = Number(BASE_ADDR + 0x31c908n);
  console.log(`[boot] Calling initializeNativeCode at 0x${INIT_NATIVE_ADDR.toString(16)}...`);

  const jFiles = makeStringHandle("/files");
  const jObb = makeStringHandle("/obb");
  const jExt = makeStringHandle("/external");

  e.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_ENV_ADDR));
  e.reg_write_i64(uc.ARM64_REG_X1, 0x60200010n); // activity
  e.reg_write_i64(uc.ARM64_REG_X2, BigInt(jFiles));
  e.reg_write_i64(uc.ARM64_REG_X3, BigInt(jObb));
  e.reg_write_i64(uc.ARM64_REG_X4, BigInt(jExt));
  e.reg_write_i64(uc.ARM64_REG_X5, 0x60200020n); // asset_mgr
  e.reg_write_i64(uc.ARM64_REG_X6, 0n);          // savedState
  e.reg_write_i64(uc.ARM64_REG_X7, 0x60200030n); // configuration

  e.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

  try {
    e.emu_start(INIT_NATIVE_ADDR, STOP_ADDR, 0, 1000000);
  } catch(err) {
    console.error("[boot] initializeNativeCode stopped:", err.message);
  }

  const gameHandle = e.reg_read_i64(uc.ARM64_REG_X0);
  const pc2 = e.reg_read_i64(uc.ARM64_REG_PC);
  console.log(`[boot] initializeNativeCode returned Game = 0x${gameHandle.toString(16)} (PC: 0x${pc2.toString(16)})`);

  console.log("[game thread] Starting game thread fn=0x" + guestThreads[0].fn.toString(16) + " to initialize looper...");
  const threadStack = 0x70400000;
  e.reg_write_i64(uc.ARM64_REG_SP, BigInt(threadStack + 0x200000 - 0x1000));
  e.reg_write_i64(uc.ARM64_REG_X0, BigInt(guestThreads[0].arg));
  e.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));
  e.reg_write_i64(uc.ARM64_REG_PC, BigInt(guestThreads[0].fn));

  // Run game thread until it reaches ALooper_pollOnce
  try {
    e.emu_start(guestThreads[0].fn, STOP_ADDR, 0, 1000000);
  } catch(err) {
    console.error("[game thread init error]:", err.message);
  }
  console.log(`[game thread] reached PC: 0x${e.reg_read_i64(uc.ARM64_REG_PC).toString(16)}`);
  let gameThreadPC = Number(e.reg_read_i64(uc.ARM64_REG_PC));
  let gameThreadSP = Number(e.reg_read_i64(uc.ARM64_REG_SP));

  // NOW drive lifecycle while looper is listening!
  const SURFACE = 0x60200040n;
  const steps = [
    { nm: "onStartNative",              off: 0x31D0A8n, a0: 0n, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onResumeNative",             off: 0x31D0C0n, a0: 0n, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onWindowFocusChangedNative", off: 0x31D1DCn, a0: 1n, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onSurfaceCreatedNative",     off: 0x31D1FCn, a0: SURFACE, a1: 0n, a2: 0n, a3: 0n },
    { nm: "onSurfaceChangedNative",     off: 0x31D264n, a0: SURFACE, a1: 1920n, a2: 1080n, a3: 1n },
  ];

  for (const step of steps) {
    console.log(`[lifecycle] Calling ${step.nm}...`);
    e.reg_write_i64(uc.ARM64_REG_X0, BigInt(JNI_ENV_ADDR));
    e.reg_write_i64(uc.ARM64_REG_X1, 0n);
    e.reg_write_i64(uc.ARM64_REG_X2, gameHandle);
    e.reg_write_i64(uc.ARM64_REG_X3, step.a0);
    e.reg_write_i64(uc.ARM64_REG_X4, step.a1);
    e.reg_write_i64(uc.ARM64_REG_X5, step.a2);
    e.reg_write_i64(uc.ARM64_REG_X6, step.a3);
    e.reg_write_i64(uc.ARM64_REG_X7, 0n);
    e.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

    try {
      e.emu_start(Number(BASE_ADDR + step.off), STOP_ADDR, 0, 500000);
    } catch(err) {
      console.error(`[lifecycle] ${step.nm} stopped:`, err.message);
    }
  }

  // Now resume game thread to process lifecycle commands!
  console.log("[game thread] Resuming game thread to consume lifecycle events...");
  e.reg_write_i64(uc.ARM64_REG_SP, BigInt(gameThreadSP));
  e.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));
  e.reg_write_i64(uc.ARM64_REG_PC, BigInt(gameThreadPC));

  for (let tick = 0; tick < 1200; tick++) {
    const pc = Number(e.reg_read_i64(uc.ARM64_REG_PC));
    if (pc === STOP_ADDR) {
      console.log("[game thread] Thread stopped at STOP_ADDR");
      break;
    }
    try {
      e.emu_start(pc, STOP_ADDR, 0, 500000);
    } catch(err) {
      console.error("[game tick error]:", err.message);
      break;
    }
    if (tick % 10 === 0) console.log(`[game tick ${tick}] PC: 0x${e.reg_read_i64(uc.ARM64_REG_PC).toString(16)}`);
  }

  console.log("[game fields after thread init]");
  for (let i = 0; i < 40; i++) {
    const val = e.mem_read(Number(gameHandle) + i * 8, 8).reduce((acc, b, idx) => acc | (BigInt(b) << BigInt(idx * 8)), 0n);
    if (val !== 0n) {
      console.log(`  game[+0x${(i * 8).toString(16)}] = 0x${val.toString(16)}`);
    }
  }
}

main().catch(err => {
  console.error("[FATAL]", err);
  process.exit(1);
});
