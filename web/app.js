import { GraphicsBackend } from "./webgpu_renderer.js";
import { AudioSink } from "./webaudio_sink.js";
import { VirtualFileSystem } from "./vfs.js";

class PaybackWebApp {
  constructor() {
    this.canvas = document.getElementById("game-canvas");
    this.apiBadge = document.getElementById("api-badge");
    this.fpsBadge = document.getElementById("fps-badge");
    this.statusBadge = document.getElementById("status-badge");
    this.loadingOverlay = document.getElementById("loading-overlay");
    this.progressText = document.getElementById("progress-text");

    this.gfx = new GraphicsBackend(this.canvas);
    this.audio = new AudioSink();
    this.vfs = new VirtualFileSystem("./assets/");

    this.worker = null;
    this.frameCount = 0;
    this.lastFpsUpdate = performance.now();
  }

  async start() {
    try {
      this.progressText.textContent = "Detecting WebGL2 / WebGPU hardware acceleration...";
      let offscreen = null;
      if (this.canvas.transferControlToOffscreen) {
        offscreen = this.canvas.transferControlToOffscreen();
        this.apiBadge.textContent = "WEBGL2 (GPU ACCEL)";
        this.apiBadge.style.borderColor = "#3fb950";
        this.apiBadge.style.color = "#3fb950";
      } else {
        const backendType = await this.gfx.initialize();
        this.apiBadge.textContent = backendType.toUpperCase();
      }

      this.progressText.textContent = "Initializing Web Audio system...";
      await this.audio.initialize();

      this.setupInputHandlers();

      this.progressText.textContent = "Spawning WebAssembly AArch64 Engine Worker...";
      this.initEngineWorker(offscreen);
    } catch (err) {
      console.error("[app] Startup error:", err);
      this.progressText.textContent = `Error: ${err.message}`;
      this.statusBadge.textContent = "Init Failed";
      this.statusBadge.style.color = "#f85149";
    }
  }

  initEngineWorker(offscreen) {
    this.worker = new Worker("wasm_engine_worker.js?t=" + Date.now());

    this.worker.onerror = (e) => {
      console.error("[MAIN THREAD WORKER ONERROR]:", e.message, e.filename, e.lineno);
      this.statusBadge.textContent = "Worker Error: " + e.message;
      this.statusBadge.style.color = "#f85149";
    };

    this.worker.onmessage = (e) => {
      const { type, message, error } = e.data;

      if (type === "STATUS") {
        this.progressText.textContent = message;
        console.log("[worker]", message);
      } else if (type === "READY") {
        this.progressText.textContent = "Engine Ready! Starting game...";
        setTimeout(() => {
          this.loadingOverlay.style.opacity = "0";
          setTimeout(() => {
            this.loadingOverlay.style.display = "none";
          }, 400);
        }, 400);

        this.statusBadge.textContent = "Engine Running";
        this.statusBadge.style.color = "#3fb950";

        this.worker.postMessage({ type: "START" });
        setInterval(() => this.worker.postMessage({ type: "PING" }), 2000);
        if (!offscreen) {
          this.runRenderLoop();
        }
      } else if (type === "PRESENT_FRAME") {
        if (e.data.draws !== undefined) {
          this.draws = e.data.draws;
        }
        if (e.data.fps !== undefined) {
          this.fpsBadge.textContent = `${e.data.fps} FPS | ${e.data.draws} draws`;
        }
      } else if (type === "EVAL_RESULT") {
        window.lastEvalResult = e.data;
        if (window.onEvalResult) window.onEvalResult(e.data);
      } else if (type === "ERROR") {
        console.error("[worker error]", error);
        this.progressText.textContent = `Worker Error: ${error}`;
        this.statusBadge.textContent = "Worker Error";
        this.statusBadge.style.color = "#f85149";
      }
    };

    this.worker.postMessage({ type: "INIT", canvas: offscreen }, offscreen ? [offscreen] : []);
  }

  setupInputHandlers() {
    const unlockAudio = () => {
      this.audio.unlock();
      this.audio.playTestPing();
      window.removeEventListener("pointerdown", unlockAudio);
      window.removeEventListener("keydown", unlockAudio);
    };
    window.addEventListener("pointerdown", unlockAudio);
    window.addEventListener("keydown", unlockAudio);

    this.canvas.addEventListener("pointerdown", (e) => this.handlePointer(e, "down"));
    this.canvas.addEventListener("pointermove", (e) => this.handlePointer(e, "move"));
    this.canvas.addEventListener("pointerup", (e) => this.handlePointer(e, "up"));

    window.addEventListener("keydown", (e) => this.handleKey(e, true));
    window.addEventListener("keyup", (e) => this.handleKey(e, false));
  }

  handlePointer(e, type) {
    const rect = this.canvas.getBoundingClientRect();
    const x = ((e.clientX - rect.left) / rect.width) * 1920;
    const y = ((e.clientY - rect.top) / rect.height) * 1080;
    if (this.worker) {
      this.worker.postMessage({ type: "POINTER", payload: { x, y, type } });
    }
  }

  handleKey(e, isDown) {
    if (this.worker) {
      this.worker.postMessage({ type: "KEY", payload: { code: e.code, isDown } });
    }
  }

  runRenderLoop() {
    const render = () => {
      const now = performance.now();
      this.gfx.renderFrame(now);
      this.frameCount++;

      if (now - this.lastFpsUpdate >= 500) {
        const fps = Math.round((this.frameCount * 1000) / (now - this.lastFpsUpdate));
        this.fpsBadge.textContent = `${fps} FPS | ${this.gfx.stats.drawCalls} draws`;
        this.frameCount = 0;
        this.lastFpsUpdate = now;
      }

      requestAnimationFrame(render);
    };

    requestAnimationFrame(render);
  }
}

window.addEventListener("error", (e) => {
  console.error("[WINDOW ERROR]", e.error || e.message, e.filename, e.lineno, e.colno);
});
window.addEventListener("unhandledrejection", (e) => {
  console.error("[UNHANDLED REJECTION]", e.reason);
});

const app = new PaybackWebApp();
window.app = app;
app.start();
