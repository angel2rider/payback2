import fs from "fs";
import MUnicorn from "@alexaltea/unicorn-js/aarch64";

const SO_PATH = "/Users/thegt/Downloads/payback/web/engine/libpayback2.so";
const BASE_ADDR = 0x10000000n;
const TRAMPOLINE_BASE = 0x71000000;
let nextTramp = TRAMPOLINE_BASE;
const trampMap = new Map();
const shims = new Map();

async function main() {
  const uc = await MUnicorn();
  const engine = new uc.Unicorn(uc.ARCH_ARM64, uc.MODE_ARM);

  function addTrampoline(name, handler) {
    const addr = nextTramp;
    nextTramp += 16;
    engine.mem_write(addr, [0x00, 0x00, 0x20, 0xd4]); // brk #0
    trampMap.set(addr, { name, handler });
    return addr;
  }

  function registerShim(name, handler) {
    const addr = addTrampoline(name, handler);
    shims.set(name, addr);
    return addr;
  }

  function getOrEmitShim(name) {
    if (shims.has(name)) return shims.get(name);
    const addr = addTrampoline(name, () => 0);
    shims.set(name, addr);
    return addr;
  }

  engine.hook_add(uc.HOOK_INTR, (handle, intno) => {
    const pc = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
    const lr = Number(engine.reg_read_i64(uc.ARM64_REG_LR));
    const tramp = trampMap.get(pc);
    if (tramp) {
      let retVal;
      try {
        retVal = tramp.handler(engine, lr);
      } catch (err) {
        console.error(`[tramp error in ${tramp.name}]:`, err);
      }
      if (retVal !== undefined) {
        engine.reg_write_i64(uc.ARM64_REG_X0, BigInt(retVal));
      }
      engine.reg_write_i64(uc.ARM64_REG_PC, BigInt(lr));
    } else {
      console.error(`Unhandled intr ${intno} at PC=0x${pc.toString(16)} LR=0x${lr.toString(16)}`);
      engine.emu_stop();
    }
  });

  // Map memory regions
  engine.mem_map(Number(BASE_ADDR), 0x02800000, uc.PROT_ALL);
  engine.mem_map(0x12800000, 0x07800000, uc.PROT_ALL);
  engine.mem_map(0x20000000, 0x10000000, uc.PROT_ALL); // Heap
  engine.mem_map(0x60000000, 0x08000000, uc.PROT_ALL); // JNI
  engine.mem_map(0x70000000, 0x00800000, uc.PROT_ALL); // Stack
  engine.mem_map(0x70800000, 0x00800000, uc.PROT_ALL); // Thread Stack
  engine.mem_map(TRAMPOLINE_BASE, 0x00200000, uc.PROT_ALL);
  engine.mem_map(0x71800000, 0x00100000, uc.PROT_ALL); // TLS

  const STOP_ADDR = 0x71001000;
  engine.mem_write(STOP_ADDR, [0x00, 0x00, 0x20, 0xd4]);
  trampMap.set(STOP_ADDR, { name: "STOP", handler: () => { engine.emu_stop(); } });

  // Basic shims
  let heapPtr = 0x20000000;
  registerShim("malloc", (e) => {
    const sz = Number(e.reg_read_i64(uc.ARM64_REG_X0));
    const aligned = (sz + 15) & ~15;
    const ptr = heapPtr;
    heapPtr += aligned;
    return ptr;
  });
  registerShim("free", () => 0);
  registerShim("pipe", (e) => 0);
  registerShim("fcntl", () => 0);
  registerShim("ALooper_forThread", () => 0x60200040);
  registerShim("ALooper_acquire", () => 0x60200040);
  registerShim("ALooper_addFd", () => 1);
  registerShim("__android_log_print", () => 0);
  registerShim("pthread_create", (e, lr) => {
    const fn = Number(e.reg_read_i64(uc.ARM64_REG_X2));
    const arg = Number(e.reg_read_i64(uc.ARM64_REG_X3));
    console.log(`[pthread_create called from LR=0x${lr.toString(16)}] fn=0x${fn.toString(16)}, arg=0x${arg.toString(16)}`);
    return 0;
  });

  // Load ELF
  const buf = fs.readFileSync(SO_PATH);
  // Read and load PT_LOAD
  const phoff = Number(buf.readBigUInt64LE(0x20));
  const phnum = buf.readUInt16LE(0x38);
  const phentsize = buf.readUInt16LE(0x36);
  for (let i = 0; i < phnum; i++) {
    const off = phoff + i * phentsize;
    if (buf.readUInt32LE(off) === 1) {
      const p_offset = Number(buf.readBigUInt64LE(off + 8));
      const p_vaddr = buf.readBigUInt64LE(off + 0x10);
      const p_filesz = Number(buf.readBigUInt64LE(off + 0x20));
      if (p_filesz > 0) {
        engine.mem_write(Number(BASE_ADDR + p_vaddr), Array.from(buf.subarray(p_offset, p_offset + p_filesz)));
      }
    }
  }

  // Parse symtab and apply relocations
  const shoff = Number(buf.readBigUInt64LE(0x28));
  const shnum = buf.readUInt16LE(0x3c);
  const shentsize = buf.readUInt16LE(0x3a);
  let symtab = null, strtab = null;
  for (let i = 0; i < shnum; i++) {
    const off = shoff + i * shentsize;
    if (buf.readUInt32LE(off + 4) === 11) {
      const symOff = Number(buf.readBigUInt64LE(off + 0x18));
      const symSize = Number(buf.readBigUInt64LE(off + 0x20));
      const link = buf.readUInt32LE(off + 0x28);
      const strOff = Number(buf.readBigUInt64LE(shoff + link * shentsize + 0x18));
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
    return new TextDecoder().decode(strtab.subarray(nameIdx, end));
  }

  function writeU64(v) {
    const b = Buffer.alloc(8);
    b.writeBigUInt64LE(BigInt(v));
    return Array.from(b);
  }

  for (let i = 0; i < shnum; i++) {
    const off = shoff + i * shentsize;
    if (buf.readUInt32LE(off + 4) === 4) {
      const r_offset = Number(buf.readBigUInt64LE(off + 0x18));
      const r_size = Number(buf.readBigUInt64LE(off + 0x20));
      const r_entsize = Number(buf.readBigUInt64LE(off + 0x38));
      for (let roff = r_offset; roff < r_offset + r_size; roff += r_entsize) {
        const r_addr = buf.readBigUInt64LE(roff);
        const r_info = buf.readBigUInt64LE(roff + 8);
        const r_addend = buf.readBigInt64LE(roff + 0x10);
        const r_type = Number(r_info & 0xFFFFFFFFn);
        const symidx = Number(r_info >> 32n);
        const targetAddr = Number(BASE_ADDR + r_addr);
        if (r_type === 1027) {
          engine.mem_write(targetAddr, writeU64(BASE_ADDR + BigInt(r_addend)));
        } else if (r_type === 1025 || r_type === 1026) {
          const shndx = symtab.readUInt16LE(symidx * 24 + 6);
          const symVal = symtab.readBigUInt64LE(symidx * 24 + 8);
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

  // Set SP & registers
  const SP = 0x70000000 + 0x800000 - 0x1000;
  engine.reg_write_i64(uc.ARM64_REG_SP, BigInt(SP));
  engine.reg_write_i64(uc.ARM64_REG_TPIDR_EL0, 0x71800000n);

  console.log("Calling initializeNativeCode at 0x1031c908...");
  engine.reg_write_i64(uc.ARM64_REG_X0, 0x60000000n); // env
  engine.reg_write_i64(uc.ARM64_REG_X1, 0x60200010n); // activity
  engine.reg_write_i64(uc.ARM64_REG_X2, 0x60200050n); // files
  engine.reg_write_i64(uc.ARM64_REG_X3, 0x60200060n); // obb
  engine.reg_write_i64(uc.ARM64_REG_X4, 0x60200070n); // ext
  engine.reg_write_i64(uc.ARM64_REG_X5, 0x60200020n); // asset_mgr
  engine.reg_write_i64(uc.ARM64_REG_X6, 0n);
  engine.reg_write_i64(uc.ARM64_REG_X7, 0x60200030n);
  engine.reg_write_i64(uc.ARM64_REG_LR, BigInt(STOP_ADDR));

  try {
    engine.emu_start(Number(BASE_ADDR + 0x31c908n), STOP_ADDR, 0, 500000);
  } catch (err) {
    console.error("emu_start stopped:", err);
  }

  const finalPC = Number(engine.reg_read_i64(uc.ARM64_REG_PC));
  const finalX0 = Number(engine.reg_read_i64(uc.ARM64_REG_X0));
  console.log(`Finished! Final PC=0x${finalPC.toString(16)}, X0=0x${finalX0.toString(16)}`);
}

main().catch(console.error);
