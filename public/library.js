import { LibraryClient, DeviceError, displayName } from "./library-client.js";
import { createLibraryPreview } from "./library-preview.js";
import { createSampleWaveform } from "./sample-waveform.js";

const el = (id) => document.getElementById(id);
const usb = navigator.usb;
const filters = [{ vendorId: 0x1d6b, productId: 0x0100 }];
const knownFactoryUpdate = "96dc0a80144d";
let client,
  busy = false,
  bank = "samples",
  listedBank;
let beat, profile, version, release, pendingPage, confirmationKey;
let statusRead, pollTimer;
let nextTrackPoll = 0, nextDeckFacts = 0;
let mix, tracks;
const waveform = createSampleWaveform();
let waveSource;
let waveInView = true;
new IntersectionObserver(([entry]) => {
  waveInView = entry.isIntersecting;
  if (!waveInView) waveform.position(null);
  schedulePoll(waveInView);
}).observe(el("sampleWave"));
let livePlayhead = false, hasEffects = false;
let effect, nextEffectPoll = 0;
let mappingCommand = "MAPPING";
const channels = ["master", "beat", "sample"];
const midiRear = { master: null, blend: null };
let usbDecks = new Set();
const pendingMix = new Map();
let writingMix = false;
let hasLampTest = false;
let lampTimer;
let lampDeadline = 0;
let manuallyDisconnected = false;
try {
  manuallyDisconnected = sessionStorage.getItem("wow-usb-disconnected") === "yes";
} catch {}
const page = () => document.querySelector("[data-page]:not([hidden])").dataset.page;
const status = (message) => {
  el("libraryStatus").textContent = message;
};
const preview = createLibraryPreview(() => client, async (error) => {
  await disconnect();
  status(error.message || "The USB connection closed.");
}, () => statusRead);
// Waveform reads use the same serialized USB session as sliders and library transfers.
function syncWaveform(force = false) {
  const key = tracks?.samples;
  if (!force && key && waveSource?.key === key && waveSource.client === client) return;
  if (!key && !waveSource) return;
  waveSource?.abort?.abort();
  waveform.clear();
  waveSource = key && client ? { client, key, state: "pending" } : null;
  if (waveSource) waveform.select(key, displayName(key));
}

async function pollWaveform(current) {
  const source = waveSource;
  if (!source || source.client !== current || document.hidden || page() !== "deck" || !waveInView) return;
  if (source.state === "pending") {
    const abort = new AbortController();
    source.abort = abort;
    source.state = "reading";
    try {
      const blob = await current.download("samples", { key: source.key }, () => {}, null, abort.signal, 32 * 1024 * 1024);
      if (source !== waveSource || abort.signal.aborted) { source.state = "pending"; return; }
      source.state = "decoding";
      waveform.load(blob, source.key).then(ok => {
        if (source !== waveSource) return;
        source.state = ok ? "ready" : "error";
        if (ok && !livePlayhead) waveform.message("Live position requires firmware 1.5.1 or later.");
      }).catch(() => {
        if (source !== waveSource) return;
        source.state = "error";
        waveform.message("Could not decode this sample.");
      });
    } catch (error) {
      if (error.name === "AbortError") { source.state = "pending"; return; }
      if (!(error instanceof DeviceError)) throw error;
      if (source === waveSource) {
        source.state = "error";
        waveform.message(error.message);
      }
    } finally { source.abort = null; }
    return;
  }
  if (!livePlayhead || source.state !== "ready") return;
  let position;
  try {
    position = JSON.parse(await query("PLAYHEAD", "PLAYHEAD"));
  } catch (error) {
    if (!(error instanceof DeviceError)) throw error;
    if (/unknown command/i.test(error.message)) {
      livePlayhead = false;
      waveform.message("Live position requires firmware 1.5.1 or later.");
    }
    waveform.position(null);
    return;
  }
  if (source !== waveSource || client !== current || page() !== "deck") return;
  const duration = position?.duration;
  const seconds = position?.seconds;
  if (position?.sample !== source.key || !Number.isFinite(seconds) || !Number.isFinite(duration) ||
      duration <= 0 || seconds < 0 || seconds > duration) {
    const now = performance.now();
    source.missingSince ??= now;
    if ((position?.sample && position.sample !== source.key) || now - source.missingSince >= 120) waveform.position(null);
    if (now - source.missingSince >= 1000 && !source.positionNotice) {
      waveform.message("Waiting for the deck’s sample position.");
      source.positionNotice = true;
    }
    return;
  }
  if (source.positionNotice) waveform.message("");
  source.positionNotice = false;
  source.missingSince = undefined;
  waveform.position(seconds / duration, seconds, duration);
}

async function readEffect(command = "EFFECT") {
  const value = JSON.parse(await query(command, "EFFECT"));
  if (!value || (value.enabled !== null && typeof value.enabled !== "boolean")) throw new Error("Invalid effect state.");
  return value;
}
function showEffect() {
  el("effectRow").hidden = !effect || (effect.enabled == null && !effect.mode);
  const toggle = el("deckEffect");
  const known = typeof effect?.enabled === "boolean";
  toggle.disabled = !known || busy;
  toggle.checked = effect?.enabled === true;
  toggle.indeterminate = Boolean(effect && !known);
  el("effectValue").textContent = known ? (toggle.checked ? "On" : "Off") : "—";
  el("effectName").textContent = ({ both: "Delay + reverb", delay: "Delay", reverb: "Reverb" })[effect?.mode] || "Effect";
}
el("deckEffect").addEventListener("change", event => {
  const command = event.target.checked ? "EFFECT ON" : "EFFECT OFF";
  run(async () => { effect = await readEffect(command); });
});
function eligible(device) {
  return (
    device.vendorId === filters[0].vendorId &&
    device.productId === filters[0].productId &&
    device.productName === "WOW MIDI Controller"
  );
}
async function authorizedDecks() {
  const devices = (await usb.getDevices()).filter(eligible);
  usbDecks = new Set(devices);
  return devices;
}
function markDisconnected(value) {
  manuallyDisconnected = value;
  try {
    if (value) sessionStorage.setItem("wow-usb-disconnected", "yes");
    else sessionStorage.removeItem("wow-usb-disconnected");
  } catch {}
}
function beatControls() {
  const control = el("beatToggle"), indicator = el("beatPlayback");
  const connected = Boolean(client && version);
  const hasTransport = beat?.playing !== undefined;
  const known = typeof beat?.playing === "boolean";
  control.hidden = !connected || !beat;
  control.disabled = busy || !beat || beat.looper;
  control.textContent = known ? (beat.playing ? "Stop beat" : "Play beat") : "Play / stop beat";
  const state = !connected ? "—" : beat?.looper ? "Looper mode" : !beat ? "Unavailable" :
    !hasTransport || !known ? "—" : beat.playing ? "Playing" : "Stopped";
  if (indicator.textContent !== state) indicator.textContent = state;
}
function controls() {
  const connected = Boolean(client && version);
  for (const button of document.querySelectorAll("[data-usb-prompt]")) {
    button.hidden = connected;
    button.disabled = busy || !usb;
  }
  el("libraryConnect").hidden = !connected;
  el("libraryConnect").disabled = busy;
  el("libraryIntro").hidden = Boolean(usb);
  el("libraryTools").hidden = !connected;
  el("libraryTools")
    .querySelectorAll("button")
    .forEach((control) => {
      control.disabled = busy || !connected;
    });
  document
    .querySelectorAll("[data-bank]")
    .forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.bank === bank)));
  el("firmwareFacts").hidden = !connected;
  el("settingsIntro").hidden = Boolean(usb);
  el("pickUpdate").disabled = !connected || busy;
  beatControls();
  showEffect();
  showMix();
  showTracks();
  el("beatStartup").disabled = busy || !beat;
  el("beatStartup").checked = Boolean(beat?.startup);
  el("lampSettings").hidden = !connected || !hasLampTest;
  el("testLamp").disabled = el("lampColor").disabled = busy || !hasLampTest;
  el("resetLamp").hidden = !lampDeadline;
  el("resetLamp").disabled = busy || !hasLampTest;
  el("midiProfile").disabled = busy || !profile;
  el("midiProfile").value = profile || "";
  if (!usb) {
    const warning = "USB access requires a WebUSB browser.";
    el("libraryIntro").textContent = el("settingsIntro").textContent = warning;
    for (const button of document.querySelectorAll("[data-usb-prompt]")) button.title = warning;
  }
  schedulePoll();
}
function progress(id, done, total) {
  const bar = el(id);
  bar.hidden = false;
  bar.max = total || 1;
  bar.value = done;
}
const fileProgress = (done, total) => progress("transferProgress", done, total);
async function disconnect() {
  const current = client;
  preview.stop();
  mix = tracks = undefined;
  livePlayhead = hasEffects = false;
  effect = null;
  nextEffectPoll = 0;
  syncWaveform();
  mappingCommand = "MAPPING";
  pendingMix.clear();
  for (const key of channels) delete el(key + "Volume").dataset.editing;
  hasLampTest = false;
  finishLampTest("");
  client = beat = profile = version = release = listedBank = undefined;
  el("fileList").replaceChildren();
  el("emptyFolder").hidden = true;
  el("fileCount").textContent = "";
  controls();
  if (current) await current.close().catch(() => {});
}
async function run(action, quiet = false) {
  if (busy) return;
  busy = true;
  waveSource?.abort?.abort();
  controls();
  if (!quiet) status("");
  try {
    // A manual command has priority as soon as the current status read finishes.
    const current = client;
    if (statusRead) await statusRead;
    await preview.releaseRead();
    if (current === client) await action();
  } catch (err) {
    if (!(err instanceof DeviceError)) await disconnect();
    if (!quiet) status(err.message || "The USB connection closed.");
  } finally {
    busy = false;
    el("transferProgress").hidden = el("updateProgress").hidden = true;
    controls();
    if (pendingPage) {
      const next = pendingPage;
      pendingPage = undefined;
      visit(next);
    }
    flushMix();
  }
}
async function query(command, prefix) {
  const current = client;
  await current.send(command);
  const line = await current.line();
  if (!line.startsWith(prefix + " ")) throw new Error("Unexpected response to " + command + " from the WOW.");
  return line.slice(prefix.length + 1);
}
async function optional(read) {
  try {
    return await read();
  } catch (error) {
    if (error instanceof DeviceError && /unknown command/i.test(error.message)) return null;
    throw error;
  }
}
async function readBeat(command = "BEAT") {
  const value = JSON.parse(await query(command, "BEAT"));
  if (typeof value.startup !== "boolean" || typeof value.looper !== "boolean" ||
      ("playing" in value && value.playing !== null && typeof value.playing !== "boolean"))
    throw new Error("Unexpected beat settings from the WOW.");
  return value;
}
function schedulePoll(immediate = false) {
  clearTimeout(pollTimer);
  if (!client || busy || statusRead || document.hidden) return;
  const active = page();
  if (active !== "deck" && active !== "library") return;
  if (!tracks && beat?.playing === undefined && !(active === "deck" && mix)) return;
  const interval = active === "library" ? 1000 : livePlayhead && waveInView && waveSource?.state === "ready" ? 40 : 500;
  pollTimer = setTimeout(poll, immediate ? 0 : interval);
}
function poll() {
  if (!client || busy || statusRead || document.hidden) return;
  if (preview.reading) { schedulePoll(); return; }
  const current = client;
  const active = page();
  statusRead = pollStatus(current, active).catch(async (error) => {
    if (client !== current) return;
    if (error instanceof DeviceError) {
      if (mix && active === "deck") {
        mix.rearMaster = mix.rearBlend = null;
        showMix();
      }
    } else {
      await disconnect();
      status(error.message || "The USB connection closed.");
    }
  }).finally(() => {
    statusRead = undefined;
    schedulePoll();
  });
}
async function pollStatus(current, active) {
  if (active !== "deck" && active !== "library") return;
  const now = performance.now();
  const isDeck = active === "deck";
  const readFacts = !isDeck || now >= nextDeckFacts;
  if (isDeck && readFacts) nextDeckFacts = now + 500;
  let selectionChanged = false;
  if (tracks && (!isDeck || now >= nextTrackPoll)) {
    nextTrackPoll = now + 1000;
    let observed;
    try {
      observed = await readTracks();
    } catch (error) {
      if (!(error instanceof DeviceError)) throw error;
      observed = { samples: null, beats: null };
    }
    if (current !== client) return;
    selectionChanged = tracks.samples !== observed.samples || tracks.beats !== observed.beats;
    tracks = observed;
  }
  let transportChanged = false;
  if (readFacts && beat?.playing !== undefined && !busy && page() === active) {
    let observed;
    try {
      observed = await readBeat();
    } catch (error) {
      if (!(error instanceof DeviceError)) throw error;
      observed = { ...beat, playing: null };
    }
    if (current !== client) return;
    transportChanged = beat.playing !== observed.playing || beat.looper !== observed.looper;
    beat = observed;
    if (transportChanged) beatControls();
  }
  if (selectionChanged || transportChanged) showTracks();
  if (!isDeck || busy || page() !== active || current !== client) return;
  if (effect && now >= nextEffectPoll) {
    nextEffectPoll = now + 1000;
    try {
      const observed = await readEffect();
      if (current !== client) return;
      effect = observed;
    } catch (error) {
      if (!(error instanceof DeviceError)) throw error;
      if (current !== client) return;
      effect = { ...effect, enabled: null };
    }
    showEffect();
  }
  if (readFacts && mix) {
    const observed = await readMix();
    if (current !== client) return;
    mix = observed;
    showMix();
  }
  if (!busy && current === client && page() === active) await pollWaveform(current);
}
const percentage = (value) => Number.isInteger(value) && value >= 0 && value <= 100;
function rearPosition(value) {
  if (!Number.isInteger(value) || value < 0 || value > 4095) return null;
  // 1.3.1 reports ADC positions. Apply the factory's 220–3976 travel scale.
  const step = Math.min(4095, Math.max(0, Math.floor((value - 220) * 4096 / 3756)));
  return Math.round(step * 100 / 4095);
}
async function readMix(command = "MIX") {
  const value = JSON.parse(await query(command, "MIX"));
  if (!percentage(value.beat) || !percentage(value.sample) ||
      ("master" in value && !percentage(value.master)) ||
      ("blendEnabled" in value && typeof value.blendEnabled !== "boolean"))
    throw new Error("Unexpected volume settings from the WOW.");
  if (!("rearMaster" in value)) value.rearMaster = rearPosition(value.rear?.[0]);
  if (!("rearBlend" in value)) value.rearBlend = rearPosition(value.rear?.[1]);
  return value;
}
function showMix() {
  showRear();
  el("mixSliders").hidden = !mix;
  const hasBlend = typeof mix?.blendEnabled === "boolean";
  el("blendSetting").hidden = !hasBlend;
  el("blendEnabled").disabled = !hasBlend;
  if (!mix) return;
  el("masterChannel").hidden = !percentage(mix.master);
  el("blendEnabled").checked = Boolean(pendingMix.get("blend") ?? mix.blendEnabled);
  for (const key of channels) {
    if (!percentage(mix[key])) continue;
    const slider = el(key + "Volume");
    const value = pendingMix.get(key) ?? mix[key];
    // The hardware result from one channel can arrive during a drag on the other.
    if (slider.dataset.editing) continue;
    slider.value = String(value);
    slider.setAttribute("aria-valuetext", value + "%");
    el(key + "Level").textContent = value + "%";
  }
}
function showRear() {
  const master = rearReading(midiRear.master, mix?.rearMaster);
  const balance = rearReading(midiRear.blend, mix?.rearBlend);
  el("mixPanel").hidden = !mix && master == null && balance == null;
  const masterLabel = percentage(master) ? master + "%" : "—";
  const blendLabel = percentage(balance)
    ? balance + "% (" + (balance < 50 ? "beat side" : balance > 50 ? "sample side" : "center") + ")"
    : "—";
  if (el("rearMaster").textContent !== masterLabel) el("rearMaster").textContent = masterLabel;
  if (el("rearBlend").textContent !== blendLabel) el("rearBlend").textContent = blendLabel;
}
function rearReading(midi, usbPosition) {
  if (!midi) return usbPosition;
  if (!client) return midi.value;
  // The WOW repeats both positions every 500 ms. Let USB take over if one stops.
  if (midi.soleWowInput && usbDecks.size === 1 && usbDecks.has(client.device) &&
      performance.now() - midi.time < 1500) return midi.value;
  return usbPosition;
}
document.addEventListener("wowrear", ({ detail }) => {
  if (detail) {
    midiRear[detail.key] = {
      value: rearPosition(detail.raw),
      soleWowInput: detail.soleWowInput,
      time: performance.now(),
    };
  } else midiRear.master = midiRear.blend = null;
  showRear();
});
function flushMix() {
  if (busy || writingMix || !client || !mix || !pendingMix.size) return;
  writingMix = true;
  const current = client;
  run(async () => {
    while (pendingMix.size && client === current) {
      const [key, value] = pendingMix.entries().next().value;
      try {
        mix = await readMix("MIX " + key.toUpperCase() + " " + value);
      } finally {
        if (pendingMix.get(key) === value) pendingMix.delete(key);
      }
      showMix();
    }
  }).finally(() => {
    writingMix = false;
    flushMix();
  });
}
el("blendEnabled").addEventListener("change", (event) => {
  if (typeof mix?.blendEnabled !== "boolean") return;
  pendingMix.set("blend", Number(event.target.checked));
  flushMix();
});
for (const key of channels) {
  const slider = el(key + "Volume");
  slider.addEventListener("input", () => {
    slider.dataset.editing = "1";
    slider.setAttribute("aria-valuetext", slider.value + "%");
    el(key + "Level").textContent = slider.value + "%";
  });
  slider.addEventListener("change", () => {
    delete slider.dataset.editing;
    if (!mix) return;
    pendingMix.set(key, Number(slider.value));
    flushMix();
  });
}
async function readProfile(command = mappingCommand) {
  const value = await query(command, "OK");
  if (mappingCommand === "PROFILE" && value === "cicada") return "performance";
  if (value !== "raw" && value !== "performance") throw new Error("Unexpected MIDI mapping from the WOW.");
  return value;
}
const dateFormat = new Intl.DateTimeFormat(undefined, { year: "numeric", month: "short", day: "numeric" });
const timeFormat = new Intl.DateTimeFormat(undefined, {
  year: "numeric",
  month: "short",
  day: "numeric",
  hour: "numeric",
  minute: "2-digit",
  timeZoneName: "short",
});
function displayDate(value, format = dateFormat) {
  const date = new Date(value);
  return Number.isFinite(date.getTime()) ? format.format(date) : "";
}
function firmwareFacts() {
  const known = version === knownFactoryUpdate;
  el("firmwareReading").textContent = version;
  el("firmwareVersion").textContent = release?.version || (known ? "1.0" : "Controller firmware");
  const date = known ? dateFormat.format(new Date(2026, 10, 25)) : "";
  const built = release?.builtAt ? displayDate(release.builtAt) : date;
  const confirmed = release?.confirmedAt > 0 ? displayDate(release.confirmedAt * 1000, timeFormat) : date;
  el("releaseDate").textContent = built;
  el("confirmedDate").textContent = confirmed;
  el("releaseDateRow").hidden = !built;
  el("confirmedDateRow").hidden = !confirmed;
}
const getRelease = async () => JSON.parse(await query("RELEASE", "RELEASE"));
function receiptKey(device) {
  return "wow-update:" + [device.vendorId, device.productId, device.serialNumber || "wow"].join(":");
}
async function identify() {
  version = await query("VERSION", "OK");
  release = await optional(getRelease);
  const [major, minor, patch = 0] = String(release?.version ?? "")
    .split(".")
    .map(Number);
  hasEffects = major > 1 || (major === 1 && minor >= 5);
  livePlayhead = major > 1 || (major === 1 && (minor > 5 || (minor === 5 && patch >= 1)));
  hasLampTest = major > 1 || (major === 1 && minor >= 2);
  confirmationKey = receiptKey(client.device);
  let receipt;
  try {
    receipt = localStorage.getItem(confirmationKey);
  } catch {}
  if (release && receipt === version && /^[a-f0-9]{12}$/.test(version)) {
    if (!release.confirmedAt) {
      await client.ack("CONFIRMED " + version + " " + Math.floor(Date.now() / 1000));
      release = await getRelease();
    }
    if (release.confirmedAt) {
      try {
        localStorage.removeItem(confirmationKey);
      } catch {}
      status("Firmware update confirmed.");
    }
  }
  firmwareFacts();
}
async function connect(device) {
  if (!eligible(device)) throw new DeviceError("Choose the WOW MIDI Controller.");
  usbDecks.add(device);
  client = new LibraryClient(device);
  try {
    await client.open();
    await identify();
  } catch (error) {
    await disconnect();
    throw error;
  }
  if (el("libraryStatus").textContent === "USB disconnected.") status("");
  beat = await optional(() => readBeat());
  mix = await optional(() => readMix());
  if (hasEffects) effect = await optional(() => readEffect());
  mappingCommand = "MAPPING";
  profile = await optional(() => readProfile());
  if (!profile) {
    mappingCommand = "PROFILE";
    profile = await optional(() => readProfile());
  }
  try {
    tracks = await optional(() => readTracks());
  } catch (err) {
    if (!(err instanceof DeviceError)) throw err;
    tracks = { beats: null, samples: null };
    status(err.message);
  }
  if (page() === "library") await refreshList();
  if (page() === "settings" && hasLampTest) await readLampTest();
}
async function choose() {
  let device;
  const authorized = await authorizedDecks();
  try {
    device = authorized.length === 1 ? authorized[0] : await usb.requestDevice({ filters });
  } catch (err) {
    if (err.name === "NotFoundError") return;
    throw err;
  }
  markDisconnected(false);
  await connect(device);
}
async function restore() {
  if (!usb || busy || client || manuallyDisconnected || page() === "instruments") return;
  await run(async () => {
    const devices = await authorizedDecks();
    if (devices.length === 1) await connect(devices[0]);
  }, true);
}
function visit(next) {
  if (next !== "deck") {
    waveSource?.abort?.abort();
    waveform.position(null);
  }
  if (next !== "library") preview.stop();
  schedulePoll(next === "library");
  if (lampDeadline && performance.now() >= lampDeadline) finishLampTest("");
  if (busy) {
    pendingPage = next;
    return;
  }
  if (!client) {
    restore();
    return;
  }
  if (next === "library" && listedBank !== bank) run(refreshList);
  else if (next === "deck" || next === "settings")
    run(async () => {
      if (beat) beat = await readBeat();
      if (mix) mix = await readMix();
      if (next === "settings" && hasLampTest) await readLampTest();
    });
}
document
  .querySelectorAll("[data-usb-prompt]")
  .forEach((button) => button.addEventListener("click", () => run(choose)));
el("libraryConnect").addEventListener("click", () =>
  run(async () => {
    markDisconnected(true);
    await disconnect();
  }),
);
document.addEventListener("wowpage", (event) => visit(event.detail));
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    waveSource?.abort?.abort();
    waveform.position(null);
  }
  schedulePoll(true);
});
window.addEventListener("pagehide", preview.stop);
function finishLampTest(message) {
  clearTimeout(lampTimer);
  lampDeadline = 0;
  el("resetLamp").hidden = true;
  el("lampStatus").textContent = message;
}
async function readLampTest(idleMessage = "") {
  const { test, remainingMs } = JSON.parse(await query("LAMP", "LAMP"));
  if (!client || !hasLampTest) return;
  if (!test || !(remainingMs > 0)) {
    finishLampTest(idleMessage);
    return;
  }
  clearTimeout(lampTimer);
  el("lampStatus").textContent = test === "off" ? "Testing with the light off." : "Testing " + test;
  lampDeadline = performance.now() + remainingMs;
  lampTimer = setTimeout(() => finishLampTest(""), remainingMs);
}
async function lampCommand(reset) {
  try {
    await client.ack(reset ? "LAMP RESET" : "LAMP TEST " + el("lampColor").value);
    await readLampTest();
  } catch (error) {
    if (!(error instanceof DeviceError)) throw error;
    el("lampStatus").textContent = error.message;
  }
}
el("testLamp").addEventListener("click", () => run(() => lampCommand(false)));
el("resetLamp").addEventListener("click", () => run(() => lampCommand(true)));
el("beatToggle").addEventListener("click", () => {
  if (!beat || beat.looper) return;
  if (typeof beat.playing === "boolean") {
    // Use exactly the action shown on the button, even if the hardware changes
    // during this request. PLAY and STOP are safe to repeat.
    const command = beat.playing ? "BEAT STOP" : "BEAT PLAY";
    run(async () => { beat = await readBeat(command); });
  } else {
    run(async () => {
      beat = await readBeat();
      if (beat.looper) return;
      await client.ack("BEAT TOGGLE");
      if (beat.playing === undefined) status("Beat command sent.");
      else beat = await readBeat();
    });
  }
});
el("beatStartup").addEventListener("change", (event) => {
  const enabled = event.target.checked;
  run(async () => {
    beat = await readBeat("BEAT STARTUP " + (enabled ? "1" : "0"));
  });
});
el("midiProfile").addEventListener("change", (event) => {
  const selected = event.target.value;
  run(async () => {
    const wireName = selected === "performance" && mappingCommand === "PROFILE" ? "cicada" : selected;
    profile = await readProfile(mappingCommand + " " + wireName);
  });
});
function sizeLabel(bytes) {
  if (bytes < 1024) return bytes + " B";
  const unit = bytes >= 1024 ** 3 ? "GiB" : bytes >= 1024 ** 2 ? "MiB" : "KiB";
  const divisor = { KiB: 1024, MiB: 1024 ** 2, GiB: 1024 ** 3 }[unit];
  return (bytes / divisor).toLocaleString(undefined, { maximumFractionDigits: 1 }) + " " + unit;
}
function actionButton(label, name, handler) {
  const button = document.createElement("button");
  button.textContent = label;
  button.setAttribute("aria-label", label + " " + name);
  button.addEventListener("click", handler);
  return button;
}
async function readTracks(command = "TRACKS") {
  const value = JSON.parse(await query(command, "TRACKS"));
  const valid = (key) => key === null || typeof key === "string" &&
    key.length > 0 && key.length <= 344 && key.length % 4 === 0 && /^[A-Za-z0-9+/]+={0,2}$/.test(key);
  if (!value || !valid(value.samples) || !valid(value.beats))
    throw new Error("Unexpected track selection from the WOW.");
  return value;
}
function showTracks() {
  syncWaveform();
  const hasPlayback = beat?.playing !== undefined;
  el("libraryDeck").hidden = !tracks && !hasPlayback;
  el("librarySampleRow").hidden = !tracks;
  const playback = beat?.looper ? "Looper mode" : !hasPlayback ? "" :
    beat.playing === true ? "Playing" : beat.playing === false ? "Stopped" : "Playback unknown";
  const beatName = tracks?.beats ? displayName(tracks.beats) : tracks ? "—" : "";
  if (el("libraryBeatName").textContent !== beatName) el("libraryBeatName").textContent = beatName;
  if (el("libraryBeatState").textContent !== playback) el("libraryBeatState").textContent = playback;
  el("librarySampleName").textContent = tracks?.samples ? displayName(tracks.samples) : "—";
  const selected = tracks?.[bank];
  for (const button of el("fileList").querySelectorAll(".file-load")) {
    const current = selected === button.dataset.key;
    const row = button.closest("li");
    const marker = row.querySelector(".file-state");
    if (current) row.setAttribute("aria-current", "true");
    else row.removeAttribute("aria-current");
    marker.hidden = !current;
    const knownPlayback = bank === "beats" && !beat?.looper && typeof beat?.playing === "boolean";
    marker.textContent = current ? (knownPlayback ? playback : "Loaded") : "";
    const action = current ? "Reload" : "Load";
    button.textContent = action;
    button.setAttribute("aria-label", action + " " + button.dataset.name + " on the WOW");
    button.disabled = busy;
  }
}
async function loadTrack(folder, entry) {
  status("Loading " + entry.name + " on the WOW…");
  try {
    tracks = await readTracks("LOAD " + folder + " " + entry.key);
  } catch (err) {
    if (err instanceof DeviceError) {
      try { tracks = await readTracks(); }
      catch (readError) {
        if (!(readError instanceof DeviceError)) throw readError;
        tracks = { beats: null, samples: null };
      }
    }
    throw err;
  }
  if (tracks[folder] !== entry.key) throw new DeviceError("The WOW did not confirm loading " + entry.name + ".");
  status("");
}
async function refreshList() {
  const folder = bank;
  const entries = await client.list(folder);
  if (tracks) {
    try { tracks = await readTracks(); }
    catch (err) {
      if (!(err instanceof DeviceError)) throw err;
      tracks = { beats: null, samples: null };
      status(err.message);
    }
  }
  entries.sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
  preview.stop();
  const rows = entries.map((entry) => {
    const row = document.createElement("li"),
      name = document.createElement("span"),
      size = document.createElement("small");
    size.textContent = sizeLabel(entry.size);
    name.append(entry.name, size);
    const remove = actionButton("Delete", entry.name, () => {
      confirmAction(
        "Delete " + entry.name + "?",
        "Remove this file from the deck’s " + folder + " folder.",
        "Delete",
        () =>
          run(async () => {
            await client.ack("DELETE " + folder + " " + entry.key);
            if (folder === "samples" && waveSource?.key === entry.key) syncWaveform(true);
            await refreshList();
            status("Deleted " + entry.name + ".");
          }),
      );
    });
    remove.className = "file-delete";
    const actions = document.createElement("div");
    actions.className = "file-actions";
    if (tracks) {
      const marker = document.createElement("small");
      marker.className = "file-state";
      marker.hidden = true;
      name.append(marker);
      const load = actionButton("Load", entry.name, () => run(() => loadTrack(folder, entry)));
      load.className = "file-load";
      load.dataset.key = entry.key;
      load.dataset.name = entry.name;
      actions.append(load);
    }
    actions.append(actionButton("Download", entry.name, () => run(() => save(folder, entry))), remove);
    row.append(preview.button(folder, entry), name, actions);
    return row;
  });
  el("fileList").replaceChildren(...rows);
  el("emptyFolder").hidden = rows.length > 0;
  el("fileCount").textContent = entries.length + (entries.length === 1 ? " file" : " files");
  listedBank = folder;
}
let confirmation;
function confirmAction(title, text, label, action) {
  el("confirmTitle").textContent = title;
  el("confirmText").textContent = text;
  el("confirmAction").textContent = label;
  confirmation = action;
  const dialog = el("libraryDialog");
  dialog.returnValue = "";
  dialog.showModal();
}
el("libraryDialog").addEventListener("close", () => {
  const action = confirmation;
  confirmation = undefined;
  if (el("libraryDialog").returnValue === "confirm" && client) action?.();
});

async function save(bank, entry) {
  let sink;
  if (window.showSaveFilePicker) {
    try {
      const handle = await window.showSaveFilePicker({ suggestedName: entry.name });
      sink = await handle.createWritable();
    } catch (err) {
      if (err.name === "AbortError") return;
      throw new DeviceError(err.message);
    }
  } else if (entry.size > 128 * 1024 * 1024) {
    throw new DeviceError("This browser can save files up to 128 MiB without a direct-to-disk file picker.");
  }
  status("Saving " + entry.name + "…");
  try {
    const blob = await client.download(bank, entry, fileProgress, sink);
    if (!sink) {
      const url = URL.createObjectURL(blob),
        link = document.createElement("a");
      link.href = url;
      link.download = entry.name;
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    }
  } catch (err) {
    if (sink) await sink.abort().catch(() => {});
    throw err;
  }
  status("Saved " + entry.name + ".");
}

el("refreshBank").addEventListener("click", () => run(refreshList));
document.querySelectorAll("[data-bank]").forEach((button) =>
  button.addEventListener("click", () => {
    if (bank === button.dataset.bank) return;
    preview.stop();
    bank = button.dataset.bank;
    listedBank = undefined;
    el("fileList").replaceChildren();
    el("emptyFolder").hidden = true;
    el("fileCount").textContent = "";
    run(refreshList);
  }),
);
el("addFiles").addEventListener("click", () => el("audioFiles").click());
el("audioFiles").addEventListener("change", (event) => {
  const files = Array.from(event.target.files),
    folder = bank;
  event.target.value = "";
  if (!files.length) return;
  run(async () => {
    let completed = 0;
    try {
      for (const file of files) {
        status("Adding " + file.name + "…");
        fileProgress(0, file.size);
        await client.upload(folder, file, fileProgress);
        if (folder === "samples" && waveSource && displayName(waveSource.key) === file.name) syncWaveform(true);
        completed++;
      }
    } catch (err) {
      if (err instanceof DeviceError) {
        await refreshList();
        throw new DeviceError(
          (completed ? completed + " added. " : "") + files[completed].name + " — " + err.message,
        );
      }
      throw err;
    }
    await refreshList();
    status(completed === 1 ? "Added " + files[0].name + "." : "Added " + completed + " files.");
  });
});

el("pickUpdate").addEventListener("click", () => el("updateFile").click());
el("updateFile").addEventListener("change", (event) => {
  const file = event.target.files[0];
  event.target.value = "";
  if (!file) return;
  confirmAction(
    "Load firmware update?",
    file.name + " will be staged for the next power-on. The deck checks the signature before installation.",
    "Load update",
    () =>
      run(async () => {
        const id = Array.from(new Uint8Array(await file.slice(-64, -58).arrayBuffer()), (n) =>
          n.toString(16).padStart(2, "0"),
        ).join("");
        status("Loading " + file.name + "…");
        await client.upload(null, file, (done, total) => progress("updateProgress", done, total));
        if (id.length === 12) {
          try {
            localStorage.setItem(confirmationKey, id);
          } catch {}
        }
        status("Update staged. Switch the WOW off and on to apply it.");
      }),
  );
});
usb?.addEventListener("disconnect", (event) => {
  usbDecks.delete(event.device);
  showRear();
  if (client?.device !== event.device) return;
  if (el("libraryDialog").open) el("libraryDialog").close("cancel");
  disconnect();
  status("USB disconnected.");
});
usb?.addEventListener("connect", (event) => {
  if (eligible(event.device)) {
    usbDecks.add(event.device);
    showRear();
    restore();
  }
});
controls();
restore();
