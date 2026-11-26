const steps = [0, 3, 5, 7, 10, 12, 15, 17];
const ticksPerStep = 16384 / steps.length;

export class PlatterVoice {
  context;
  lastAngle;
  travel = 0;
  step = 0;
  pitch = 2048;
  left = 0;
  right = 0;

  reset() {
    this.lastAngle = undefined;
    this.travel = 0;
    this.step = 0;
    this.pitch = 2048;
    this.left = this.right = 0;
  }

  async enable() {
    this.context ??= new AudioContext({ latencyHint: "interactive" });
    await this.context.resume();
  }

  async disable() {
    if (this.context) {
      await this.context.close();
      this.context = undefined;
    }
  }

  note(step) {
    const ctx = this.context;
    if (ctx?.state !== "running") return;
    const start = ctx.currentTime;
    const transpose = Math.round((this.pitch / 4095 - 0.5) * 24);
    const frequency = 130.81 * 2 ** ((steps[step] + transpose) / 12);
    const tone = ctx.createOscillator();
    const filter = ctx.createBiquadFilter();
    const envelope = ctx.createGain();
    tone.type = "sawtooth";
    tone.frequency.value = frequency;
    filter.type = "lowpass";
    filter.frequency.value = 400 * 2 ** (2 + 2 * (Number(this.right) - Number(this.left)));
    filter.Q.value = 0.6;
    envelope.gain.setValueAtTime(0, start);
    envelope.gain.linearRampToValueAtTime(0.085, start + 0.006);
    envelope.gain.exponentialRampToValueAtTime(0.0001, start + 0.18);
    tone.connect(filter).connect(envelope).connect(ctx.destination);
    tone.onended = () => { tone.disconnect(); filter.disconnect(); envelope.disconnect(); };
    tone.start(start);
    tone.stop(start + 0.19);
  }

  angle(ticks) {
    if (this.lastAngle !== undefined) {
      this.travel += ((ticks - this.lastAngle + 24576) % 16384) - 8192;
      const advances = Math.trunc(this.travel / ticksPerStep);
      if (advances) {
        this.travel -= advances * ticksPerStep;
        this.step = (this.step + advances + steps.length) % steps.length;
        this.note(this.step);
      }
    }
    this.lastAngle = ticks;
  }
}
