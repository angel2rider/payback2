/**
 * Engine Bridge for Payback 2 Web Port.
 * Connects the C/WASM runtime with the browser's WebGPU, Web Audio, and VFS backends.
 */

export class EngineBridge {
  constructor({ gfx, audio, vfs, onFrameSwap }) {
    this.gfx = gfx;
    this.audio = audio;
    this.vfs = vfs;
    this.onFrameSwap = onFrameSwap || (() => {});

    // Open asset handles: handleId -> { uint8Array, pos, len }
    this.nextHandleId = 1;
    this.openHandles = new Map();
  }

  // --- Asset VFS Bridge (AAssetManager_open / read / close) ---

  async openAsset(path) {
    const data = await this.vfs.loadAsset(path);
    if (!data) return 0;

    const handle = this.nextHandleId++;
    this.openHandles.set(handle, {
      data,
      pos: 0,
      len: data.byteLength,
    });
    return handle;
  }

  readAsset(handle, destUint8Array, maxBytes) {
    const file = this.openHandles.get(handle);
    if (!file) return -1;

    const remaining = file.len - file.pos;
    if (remaining <= 0) return 0;

    const toRead = Math.min(remaining, maxBytes);
    destUint8Array.set(file.data.subarray(file.pos, file.pos + toRead));
    file.pos += toRead;
    return toRead;
  }

  getAssetLength(handle) {
    const file = this.openHandles.get(handle);
    return file ? file.len : 0;
  }

  closeAsset(handle) {
    this.openHandles.delete(handle);
  }

  // --- Audio Bridge (OpenSL ES -> Web Audio) ---

  enqueuePcmAudio(pcm16Array) {
    if (this.audio) {
      this.audio.enqueuePcm16(pcm16Array);
    }
  }

  // --- Graphics Bridge (GLES2 / EGL -> WebGPU / WebGL2) ---

  swapBuffers(rgbaArray, width, height) {
    if (this.gfx) {
      // In WebGPU / WebGL2 mode, present frame
      this.onFrameSwap();
    }
  }
}
