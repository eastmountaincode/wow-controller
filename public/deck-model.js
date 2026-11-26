const buttonIds = ['start', 'beat', 'sample', 'cue1', 'cue2', 'cue3', 'cue4'];
const buttonLabels = ['Start', 'Beat', 'Sample', 'Cue 1', 'Cue 2', 'Cue 3', 'Cue 4'];
const lampColors = ['', '#ff6555', '#81f589', '#ffe76b', '#86a3ff', '#d096ff', '#83edf1', '#fff9e7'];
const lampNames = ['off', 'red', 'green', 'yellow', 'blue', 'purple', 'cyan', 'white'];
const layoutDescription = 'Top view of the WOW. The platter is in the middle. Four cue buttons run down the left edge. Start, pitch, beat and sample are on the upper right, with the diagonal cut fader below them.';

// The platter carries the mark in negative. The START lens has luminous cuts.
const emblem = `<svg viewBox="0 0 100 100" aria-hidden="true" focusable="false">
  <path fill="none" stroke="currentColor" stroke-width="4.2" stroke-linejoin="round"
    d="M9 8 38 38 48 26 61 39 90 7 M91 92 62 62 52 74 39 61 10 93
       M64 44 101 29 M65 50H101 M64 56 101 71
       M36 56 -1 71 M35 50H-1 M36 44 -1 29" />
  <circle cx="50" cy="50" r="10.4" fill="currentColor" />
</svg>`;

const chassis = `
  <div class="wm-body" role="img">
    <div class="wm-speaker" title="Built-in speaker"></div>
    <div class="wm-panel-recess"></div>
    <div class="wm-bevel"></div>
    <div class="wm-platter" title="Platter">
      <div class="wm-record">
        <div class="wm-index"><i></i><b>WOW &amp; FLUTTER</b></div>
        <i class="wm-hub">${emblem}</i>
        <i class="wm-stud"></i>
      </div>
      <div class="wm-platter-light"></div>
    </div>
    <div class="wm-start wm-key" data-key="start" title="Start / stop">
      <i class="wm-start-emblem">${emblem}</i>
    </div>
    <div class="wm-pitch" title="Pitch knob"><i></i><span>PITCH</span></div>
    <div class="wm-beat wm-key" data-key="beat" title="Beat"><span>BEAT</span></div>
    <div class="wm-sample wm-key" data-key="sample" title="Sample"><span>SAMP</span></div>
    <div class="wm-cues">
      <i class="wm-key" data-key="cue1" title="Cue 1"></i>
      <i class="wm-key" data-key="cue2" title="Cue 2"></i>
      <i class="wm-key" data-key="cue3" title="Cue 3"></i>
      <i class="wm-key" data-key="cue4" title="Cue 4"></i>
    </div>
    <div class="wm-fader" title="Cut fader">
      <i class="wm-rail"></i><i class="wm-travel"></i>
      <i class="wm-end wm-left"></i><i class="wm-end wm-right"></i>
      <i class="wm-cap"></i>
    </div>
    <div class="wm-brand">WOW &amp; FLUTTER MACHINE<small>HEADACHE SOUND ELECTRONICS</small></div>
  </div>`;

const faderTitles = {
  unknown: 'Cut fader — awaiting end-stop readings',
  left: 'Cut fader — left end stop',
  right: 'Cut fader — right end stop',
  both: 'Cut fader — both end stops active',
  travel: 'Cut fader — between end stops. The exact position is unmeasured.',
};

function faderPosition(left, right) {
  if (left == null || right == null) return 'unknown';
  if (left && right) return 'both';
  if (left) return 'left';
  return right ? 'right' : 'travel';
}

// MIDI can deliver many angle messages per frame. Apply the most recent snapshot once.
export function createDeckModel(host) {
  host.innerHTML = chassis;
  const body = host.querySelector('.wm-body');
  const platter = body.querySelector('.wm-record');
  const pitchKnob = body.querySelector('.wm-pitch');
  const fader = body.querySelector('.wm-fader');
  const keys = buttonIds.map(id => body.querySelector(`[data-key="${id}"]`));
  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  let state = {};
  let frame = 0;
  const painted = {};

  function render() {
    frame = 0;
    const { connected, angle, rpm, motor, led, ledRed, ledGreen, ledBlue, pitch, left, right, keys: keyMask } = state;
    let rotation = connected && Number.isFinite(angle) ? angle : null;
    if (rotation !== null && reducedMotion.matches && rpm !== 0) rotation = painted.rotation ?? null;
    if (rotation !== painted.rotation) {
      platter.style.transform = rotation == null ? '' : `rotate(${rotation - 90}deg)`;
      painted.rotation = rotation;
    }

    const pitchValue = connected && Number.isFinite(pitch) ? Math.min(4095, Math.max(0, pitch)) : null;
    if (pitchValue !== painted.pitch) {
      pitchKnob.classList.toggle('wm-known', pitchValue !== null);
      pitchKnob.style.setProperty('--wm-pitch-angle', `${pitchValue == null ? 0 : -135 + 270 * pitchValue / 4095}deg`);
      painted.pitch = pitchValue;
    }

    const motorState = connected && motor != null ? (motor ? 'on' : 'off') : 'unknown';
    let lamp = connected && Number.isInteger(led) && led >= 0 && led < lampColors.length ? led : null;
    const measured = lamp !== null && ledRed != null && ledGreen != null && ledBlue != null;
    const peak = measured ? Math.max(ledRed, ledGreen, ledBlue) : null;
    if (peak === 0) lamp = 0;
    const mask = connected ? (keyMask ?? 0) : 0;
    if (mask !== painted.mask || lamp !== painted.lamp || connected !== painted.connected) {
      const description = [layoutDescription, connected ? 'MIDI connected.' : 'MIDI disconnected.'];
      if (lamp !== null) description.push(`Start light ${lampNames[lamp]}.`);
      if (mask) description.push(`Currently holding ${buttonLabels.filter((_, index) => mask & (1 << index)).join(', ')}.`);
      body.setAttribute('aria-label', description.join(' '));
      painted.connected = connected;
    }
    const channelsChanged = ledRed !== painted.red || ledGreen !== painted.green || ledBlue !== painted.blue;
    if (lamp !== painted.lamp || motorState !== painted.motor || channelsChanged) {
      const start = keys[0];
      start.dataset.motor = motorState;
      start.dataset.lamp = lamp === null ? 'unknown' : lamp === 0 ? 'off' : 'lit';
      if (lamp) {
        const hue = lampColors[lamp];
        const levels = peak ? [ledRed, ledGreen, ledBlue].map(channel => Math.round(channel * 255 / peak)) : null;
        // The factory balances white with unequal PWM levels. Keep that white
        // neutral and use the reported channel proportions to tint other hues.
        const tint = levels && lamp !== 7 ? `color-mix(in srgb, ${hue} 75%, rgb(${levels.join(' ')}))` : hue;
        start.style.setProperty('--wm-lamp', tint);
        const strength = peak === null ? 1 : Math.sqrt(peak / 127);
        start.style.setProperty('--wm-strength', `${Math.round(100 * strength)}%`);
      }
      const status = ['Start / stop'];
      if (lamp !== null) status.push(`light ${lampNames[lamp]}`);
      if (motorState !== 'unknown') status.push(`motor ${motorState}`);
      start.title = status.join(', ');
      painted.lamp = lamp;
      painted.motor = motorState;
      painted.red = ledRed;
      painted.green = ledGreen;
      painted.blue = ledBlue;
    }

    const stop = connected ? faderPosition(left, right) : 'unknown';
    if (stop !== painted.stop) {
      fader.dataset.stop = stop;
      fader.title = faderTitles[stop];
      painted.stop = stop;
    }

    if (mask !== painted.mask) {
      keys.forEach((key, index) => {
        const held = Boolean(mask & (1 << index));
        key.classList.toggle('wm-pressed', held);
      });
      painted.mask = mask;
    }
  }

  function schedule() {
    if (!frame) frame = requestAnimationFrame(render);
  }
  reducedMotion.addEventListener('change', schedule);
  render();
  return {
    update(snapshot) { state = snapshot; schedule(); },
    destroy() {
      cancelAnimationFrame(frame);
      reducedMotion.removeEventListener('change', schedule);
    },
  };
}
