import { PlatterVoice } from "./platter-voice.js";
import { createDeckModel } from "./deck-model.js";

const el = (id) => document.getElementById(id);
const voice = new PlatterVoice();
const model = createDeckModel(el("deckModel"));
const blank = {
  connected: false,
  angle: null,
  rpm: null,
  motor: null,
  led: null,
  ledRed: null,
  ledGreen: null,
  ledBlue: null,
  pitch: null,
  left: null,
  right: null,
  keys: 0,
  beat: null,
};
const state = { ...blank };
const lampChannels = ["ledRed", "ledGreen", "ledBlue"];
const high = new Map();
let access,
  current,
  previousAngle,
  sampledAt,
  lastMovement = 0,
  distance = 0,
  frame = 0;
let hasFaderMask = false;
let soleWowInput = false;
const looksLikeWow = (port) => /wow|headachesound|midi gadget|f_midi/i.test(port.name + " " + port.manufacturer);
// At physical noon the original 3-o’clock artwork was rotated 270 degrees.
const ticksPerTurn = 16384;
const noonTicks = 3 * ticksPerTurn / 4;
function heading(ticks) {
  return ((noonTicks - ticks + ticksPerTurn) % ticksPerTurn) * 360 / ticksPerTurn;
}

function showValue(id, text) {
  if (el(id).textContent !== text) el(id).textContent = text;
}
function render() {
  frame = 0;
  model.update(state);
  const speed = state.rpm === 0 ? "0" : state.rpm?.toFixed(1);
  showValue("speed", speed == null ? "—" : speed + " rpm");
  showValue("angle", state.angle === null ? "—" : (Math.round(state.angle) % 360) + "°");
  showValue("pitch", state.pitch === null ? "—" : Math.round((state.pitch * 100) / 4095) + "%");
  showValue("motor", state.motor === null ? "—" : state.motor ? "On" : "Off");
  let fader = "—";
  if (state.left !== null && state.right !== null)
    fader =
      state.left && state.right
        ? "Both stops"
        : state.left
          ? "Left"
          : state.right
            ? "Right"
            : "Between stops";
  showValue("fader", fader);
}
function changed() {
  if (!frame) frame = requestAnimationFrame(render);
}
function setStop(id, active) {
  state[id] = voice[id] = active;
}
function reset() {
  Object.assign(state, blank);
  voice.reset();
  high.clear();
  hasFaderMask = false;
  previousAngle = sampledAt = undefined;
  distance = lastMovement = 0;
}
function angle(ticks, time) {
  voice.angle(ticks);
  state.angle = heading(ticks);
  if (previousAngle === undefined) {
    previousAngle = ticks;
    sampledAt = time;
    state.rpm = 0;
    return;
  }
  const delta = ((ticks - previousAngle + 24576) % 16384) - 8192;
  previousAngle = ticks;
  distance -= delta;
  if (Math.abs(delta) > 1) lastMovement = time;
  const elapsed = time - sampledAt;
  if (elapsed >= 40) {
    const rpm = (distance * 60000) / (16384 * elapsed);
    state.rpm = Math.abs(rpm) < 0.15 ? 0 : rpm;
    sampledAt = time;
    distance = 0;
  }
}
function incoming(event) {
  const [command, control, value] = event.data;
  if (command === 0x90 || command === 0x80) {
    const key = control - 36;
    if (key < 0 || key >= 7) return;
    const down = command === 0x90 && value > 0;
    state.keys = down ? state.keys | (1 << key) : state.keys & ~(1 << key);
    if (down) voice.note(key);
  } else if (command === 0xb0) {
    if (control === 105) {
      state.led = value < 8 ? value : null;
      if (state.led === null) for (const channel of lampChannels) state[channel] = null;
    } else if (control >= 106 && control <= 108) state[lampChannels[control - 106]] = value;
    else if (control === 20 || control === 102) state.motor = value > 0;
    else if (control === 21 || control === 103) state.keys = value;
    else if (control === 22 || control === 104) {
      hasFaderMask = true;
      setStop("left", Boolean(value & 1));
      setStop("right", Boolean(value & 2));
    } else if ((control >= 16 && control <= 19) || control === 23 || control === 24) {
      high.set(control, value);
      return;
    } else {
      const base = control - 32;
      if (!high.has(base)) return;
      const number = (high.get(base) << 7) | value;
      high.delete(base);
      const raw = number >> 2;
      if (base === 16) angle(number, event.timeStamp);
      if (base === 19) state.pitch = voice.pitch = raw;
      if (base === 23 || base === 24) {
        document.dispatchEvent(new CustomEvent("wowrear", {
          detail: { key: base === 23 ? "master" : "blend", raw, soleWowInput },
        }));
        return;
      }
      if (!hasFaderMask && base === 17) setStop("left", raw > 3671);
      if (!hasFaderMask && base === 18) setStop("right", raw > 3731);
    }
  } else return;
  changed();
}
setInterval(() => {
  if (state.rpm && performance.now() - lastMovement > 200) {
    state.rpm = 0;
    changed();
  }
}, 100);

function listen(port) {
  // Web MIDI and WebUSB have no shared device ID. Pair only an explicitly
  // named WOW when no second WOW (including generic gadget ports) is present.
  const soleWow = Boolean(port && /\bWOW MIDI Controller\b/i.test(port.name) &&
    [...access.inputs.values()].filter((p) => p.state === "connected" && looksLikeWow(p)).length === 1);
  if (current !== port || soleWowInput !== soleWow) {
    soleWowInput = soleWow;
    document.dispatchEvent(new CustomEvent("wowrear", { detail: null }));
  }
  if (current !== port) {
    if (current) current.onmidimessage = null;
    current = port;
    reset();
    if (port) port.onmidimessage = incoming;
    state.connected = Boolean(port);
    changed();
  }
  const available = [...access.inputs.values()].some(p => p.state === "connected");
  el("chooseMidiInput").hidden = Boolean(port) || !available;
  el("midiNotice").hidden = Boolean(port) || available;
  if (!port && !available) showValue("midiNotice", "No MIDI input found.");
}
function listPorts() {
  const ports = [...access.inputs.values()].filter((p) => p.state === "connected");
  const select = el("ports");
  const active =
    ports.find((p) => p.id === select.value) ??
    ports.find(looksLikeWow);
  select.replaceChildren(new Option("Choose MIDI input", ""), ...ports.map((p) => new Option(p.name, p.id)));
  select.value = active?.id ?? "";
  select.disabled = !ports.length;
  listen(active);
}
let midiRequest;
function connectMidi() {
  if (access) return Promise.resolve(true);
  if (!midiRequest) midiRequest = requestMidi();
  return midiRequest;
}
async function requestMidi() {
  const buttons = [el("connect"), el("connectInput")];
  buttons.forEach((button) => {
    button.disabled = true;
  });
  try {
    access = await navigator.requestMIDIAccess({ sysex: false });
    buttons.forEach((button) => {
      button.hidden = true;
    });
    el("ports").hidden = false;
    access.onstatechange = listPorts;
    listPorts();
    return true;
  } catch (err) {
    el("libraryStatus").textContent = err.message || "Could not connect to MIDI.";
    return false;
  } finally {
    midiRequest = undefined;
    buttons.forEach((button) => {
      button.disabled = false;
    });
  }
}
el("ports").addEventListener("change", () => listen(access?.inputs.get(el("ports").value)));
el("connect").addEventListener("click", connectMidi);
el("connectInput").addEventListener("click", connectMidi);
if (!navigator.requestMIDIAccess) {
  el("connect").disabled = el("connectInput").disabled = el("sound").disabled = true;
  el("connect").textContent = el("connectInput").textContent = "MIDI unavailable";
} else if (navigator.permissions) {
  navigator.permissions
    .query({ name: "midi", sysex: false })
    .then((result) => {
      if (result.state === "granted") connectMidi();
    })
    .catch(() => {});
}
el("sound").addEventListener("click", async () => {
  const button = el("sound");
  const enable = button.getAttribute("aria-pressed") !== "true";
  button.disabled = true;
  try {
    if (enable) {
      if (!access && !(await connectMidi())) return;
      await voice.enable();
    } else await voice.disable();
    button.setAttribute("aria-pressed", String(enable));
    button.textContent = enable ? "Mute synth" : "Enable synth";
  } catch (err) {
    el("libraryStatus").textContent = err.message;
  } finally {
    button.disabled = false;
  }
});
render();
