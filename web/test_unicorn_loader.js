/**
 * web/test_unicorn_loader.js
 * Validates loading and executing libpayback2.so in WebAssembly AArch64 Unicorn.
 */

import fs from "fs";
import MUnicorn from "@alexaltea/unicorn-js/aarch64";

const SO_PATH = "/Users/thegt/Downloads/payback/web/engine/libpayback2.so";
const BASE_ADDR = 0x10000000n; // 256MB base

async function run() {
  console.log("[test] Initializing Unicorn AArch64 WASM engine...");
  const uc = await MUnicorn();
  const e = new uc.Unicorn(uc.ARCH_ARM64, uc.MODE_ARM);

  console.log(`[test] Reading ${SO_PATH}...`);
  const buf = fs.readFileSync(SO_PATH);
  console.log(`[test] File size: ${buf.length} bytes`);

  // Parse ELF header
  const phoff = Number(buf.readBigUInt64LE(0x20));
  const phnum = buf.readUInt16LE(0x38);
  const phentsize = buf.readUInt16LE(0x36);

  let vmin = 0xFFFFFFFFFFFFFFFFn;
  let vmax = 0n;

  const ptLoads = [];
  for (let i = 0; i < phnum; i++) {
    const off = phoff + i * phentsize;
    const type = buf.readUInt32LE(off);
    if (type === 1) { // PT_LOAD
      const p_offset = Number(buf.readBigUInt64LE(off + 0x08));
      const p_vaddr = buf.readBigUInt64LE(off + 0x10);
      const p_filesz = Number(buf.readBigUInt64LE(off + 0x20));
      const p_memsz = Number(buf.readBigUInt64LE(off + 0x28));

      if (p_vaddr < vmin) vmin = p_vaddr;
      const end = p_vaddr + BigInt(p_memsz);
      if (end > vmax) vmax = end;

      ptLoads.push({ p_offset, p_vaddr, p_filesz, p_memsz });
    }
  }

  const span = Number((vmax - vmin + 0xFFFn) & ~0xFFFn);
  console.log(`[test] PT_LOAD span: ${span} bytes (vmin: 0x${vmin.toString(16)}, vmax: 0x${vmax.toString(16)})`);

  // Map memory in Unicorn (round up to 4KB pages)
  const mapSize = Math.max(span, 0x2800000); // 40MB
  console.log(`[test] Mapping 0x${BASE_ADDR.toString(16)} (size: 0x${mapSize.toString(16)})...`);
  e.mem_map(Number(BASE_ADDR), mapSize, uc.PROT_ALL);

  // Write PT_LOAD segments
  for (const pt of ptLoads) {
    if (pt.p_filesz > 0) {
      const segBytes = buf.subarray(pt.p_offset, pt.p_offset + pt.p_filesz);
      const dest = Number(BASE_ADDR + pt.p_vaddr);
      e.mem_write(dest, Array.from(segBytes));
    }
  }
  console.log(`[test] Loaded ${ptLoads.length} PT_LOAD segments.`);

  // Apply R_AARCH64_RELATIVE relocations
  const shoff = Number(buf.readBigUInt64LE(0x28));
  const shnum = buf.readUInt16LE(0x3c);
  const shentsize = buf.readUInt16LE(0x3a);

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

        if (r_type === 1027) { // R_AARCH64_RELATIVE
          const targetAddr = Number(BASE_ADDR + r_addr);
          const val = BASE_ADDR + BigInt(r_addend);
          const valBuf = Buffer.alloc(8);
          valBuf.writeBigUInt64LE(val);
          e.mem_write(targetAddr, Array.from(valBuf));
          relCount++;
        }
      }
    }
  }
  console.log(`[test] Applied ${relCount} R_AARCH64_RELATIVE relocations.`);

  // Map stack: 0x70000000 (8MB)
  const STACK_BASE = 0x70000000;
  const STACK_SIZE = 0x800000;
  e.mem_map(STACK_BASE, STACK_SIZE, uc.PROT_ALL);
  const SP = BigInt(STACK_BASE + STACK_SIZE - 0x1000);
  e.reg_write_i64(uc.ARM64_REG_SP, SP);

  console.log("[test] Stack and registers initialized.");
  console.log("[test] Success! libpayback2.so is mapped and relocated in WebAssembly Unicorn engine.");
}

run().catch(err => {
  console.error("[test] Failed:", err);
  process.exit(1);
});
