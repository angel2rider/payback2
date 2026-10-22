/**
 * Low-latency Web Audio Sink for Payback 2 Web Port.
 * Emulates OpenSL ES PCM buffer queues with interactive audio unlocking.
 */

export class AudioSink {
  constructor() {
    this.ctx = null;
    this.sampleRate = 44100;
    this.channels = 2;
    this.masterGain = null;
    this.isUnlocked = false;
  }

  async initialize() {
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) {
      console.warn("[audio] Web Audio API not supported.");
      return;
    }

    this.ctx = new AudioContextClass({ latencyHint: "interactive" });
    this.sampleRate = this.ctx.sampleRate;
    this.masterGain = this.ctx.createGain();
    this.masterGain.gain.value = 0.8;
    this.masterGain.connect(this.ctx.destination);

    console.log(`[audio] Initialized Web Audio Sink @ ${this.sampleRate} Hz`);
  }

  unlock() {
    if (this.ctx && this.ctx.state === "suspended") {
      this.ctx.resume().then(() => {
        this.isUnlocked = true;
        console.log("[audio] AudioContext unlocked by user gesture.");
      });
    } else {
      this.isUnlocked = true;
    }
  }

  playTestPing() {
    if (!this.ctx || this.ctx.state === "suspended") return;
    const osc = this.ctx.createOscillator();
    const gain = this.ctx.createGain();
    osc.type = "sine";
    osc.frequency.setValueAtTime(587.33, this.ctx.currentTime); // D5
    gain.gain.setValueAtTime(0.15, this.ctx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, this.ctx.currentTime + 0.3);
    osc.connect(gain);
    gain.connect(this.masterGain);
    osc.start();
    osc.stop(this.ctx.currentTime + 0.3);
  }

  enqueuePcm16(int16Array) {
    if (!this.ctx || this.ctx.state === "suspended") return;

    const frameCount = int16Array.length / this.channels;
    const audioBuffer = this.ctx.createBuffer(this.channels, frameCount, this.sampleRate);

    for (let channel = 0; channel < this.channels; channel++) {
      const channelData = audioBuffer.getChannelData(channel);
      for (let i = 0; i < frameCount; i++) {
        channelData[i] = int16Array[i * this.channels + channel] / 32768.0;
      }
    }

    const source = this.ctx.createBufferSource();
    source.buffer = audioBuffer;
    source.connect(this.masterGain);
    source.start();
  }
}
