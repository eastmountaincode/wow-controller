import { LibraryClient, DeviceError, displayName } from "./library-client.js";
import { createLibraryPreview } from "./library-preview.js";

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
let mix, tracks;
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
  indicator.hidden = !connected || !hasTransport || beat.looper;
  const label = !known ? "Waiting for playback state" : beat.playing ? "Playing" : "Stopped";
  if (indicator.textContent !== label) indicator.textContent = label;
  indicator.dataset.live = String(known && beat.playing);
  el("beatDetail").textContent = !connected
    ? "Connect the deck to control its beat."
    : !beat
      ? "Beat control requires a firmware update in Settings."
      : beat.looper
        ? "Exit looper mode on the deck to control its beat."
        : hasTransport
          ? "Tap BEAT for the next beat. Double-tap to play or stop."
          : "On the deck, hold BEAT and press START.";
}
function controls() {
  const connected = Boolean(client && version);
  for (const button of document.querySelectorAll("[data-usb-prompt]")) {
    button.hidden = connected;
    button.disabled = busy || !usb;
  }
  for (const label of document.querySelectorAll(".device-status")) {
    label.textContent = connected ? "USB connected" : "";
    label.dataset.live = String(connected);
  }
  el("libraryConnect").hidden = !connected;
  el("libraryConnect").disabled = busy;
  el("libraryIntro").hidden = connected;
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
  el("settingsIntro").hidden = connected;
  el("pickUpdate").disabled = !connected || busy;
  beatControls();
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
  el("startupDescription").textContent = !connected
    ? "Connect the deck to change its startup behavior."
    : !beat
      ? "Available with the controller firmware update."
      : "Applies the next time the deck starts.";
  el("profileDescription").textContent = !connected
    ? "Connect the deck to change its MIDI mapping."
    : !profile
      ? "Available with the controller firmware update."
      : profile === "performance"
        ? "The platter also sends modulation (CC 1), and the pitch knob sends pitch bend."
        : "Sends the deck’s native control values and button notes.";
  if (!usb) {
    const warning = "This browser does not provide USB access to the deck.";
    el("libraryIntro").textContent = el("settingsIntro").textContent = el("beatDetail").textContent = warning;
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
  mappingCommand = "MAPPING";
  pendingMix.clear();
  for (const key of channels) delete el(key + "Volume").dataset.editing;
  hasLampTest = false;
  finishLampTest("");
  client = beat = profile = version = release = listedBank = undefined;
  el("fileList").replaceChildren();
  el("emptyFolder").hidden = true;
  el("fileCount").textContent = "";
  el("previewHint").hidden = true;
  controls();
  if (current) await current.close().catch(() => {});
}
async function run(action, quiet = false) {
  if (busy) return;
  busy = true;
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
  if ((active === "deck" && (beat?.playing !== undefined || mix)) || (active === "library" && tracks))
    pollTimer = setTimeout(poll, immediate ? 0 : active === "library" ? 1000 : 500);
}
function poll() {
  if (!client || busy || statusRead || document.hidden) return;
  if (preview.reading) { schedulePoll(); return; }
  const current = client;
  const active = page();
  statusRead = pollStatus(current, active).catch(async (error) => {
    if (client !== current) return;
    if (error instanceof DeviceError) {
      if (active === "library") {
        tracks = { samples: null, beats: null };
        showTracks();
      } else if (mix) {
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
  if (active === "library" && tracks) {
    const observed = await readTracks();
    if (current === client && (tracks.samples !== observed.samples || tracks.beats !== observed.beats)) {
      tracks = observed;
      showTracks();
    }
    return;
  }
  if (active !== "deck") return;
  if (beat?.playing !== undefined) {
    try {
      const observed = await readBeat();
      if (current !== client) return;
      const changed = beat.playing !== observed.playing || beat.looper !== observed.looper;
      beat = observed;
      if (changed) beatControls();
    } catch (err) {
      if (!(err instanceof DeviceError)) throw err;
      if (current !== client) return;
      beat.playing = null;
      beatControls();
    }
  }
  if (mix && current === client && !busy && page() === "deck") {
    const observed = await readMix();
    if (current === client) {
      mix = observed;
      showMix();
    }
  }
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
  el("mixHint").hidden = el("mixSliders").hidden = !mix;
  const hasBlend = typeof mix?.blendEnabled === "boolean";
  el("blendSetting").hidden = !hasBlend;
  el("blendEnabled").disabled = !hasBlend;
  if (!mix) return;
  el("masterChannel").hidden = !percentage(mix.master);
  el("blendEnabled").checked = Boolean(pendingMix.get("blend") ?? mix.blendEnabled);
  el("mixHint").textContent = mix.blendEnabled === false
    ? "The rear master sets the maximum volume. The rear mix is bypassed."
    : "The rear master sets the maximum volume. The rear mix balances the beat and sample.";
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
  const masterLabel = percentage(master) ? master + "%" : "Awaiting reading";
  const blendLabel = percentage(balance)
    ? balance + "% (" + (balance < 50 ? "beat side" : balance > 50 ? "sample side" : "center") + ")"
    : "Awaiting reading";
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
  const [major, minor] = String(release?.version ?? "")
    .split(".")
    .map(Number);
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
  if (next !== "library") preview.stop();
  schedulePoll(next === "library");
  if (lampDeadline && performance.now() >= lampDeadline) finishLampTest("Normal light restored.");
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
    status("USB disconnected.");
  }),
);
document.addEventListener("wowpage", (event) => visit(event.detail));
document.addEventListener("visibilitychange", () => schedulePoll(true));
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
  el("lampStatus").textContent = test === "off" ? "Testing with the light off." : "Testing " + test + ".";
  lampDeadline = performance.now() + remainingMs;
  lampTimer = setTimeout(() => finishLampTest("Normal light restored."), remainingMs);
}
async function lampCommand(reset) {
  try {
    await client.ack(reset ? "LAMP RESET" : "LAMP TEST " + el("lampColor").value);
    await readLampTest(reset ? "Test ended." : "Normal light restored.");
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
    status(beat.startup ? "The beat will play on startup." : "The beat will stay off on startup.");
  });
});
el("midiProfile").addEventListener("change", (event) => {
  const selected = event.target.value;
  run(async () => {
    const wireName = selected === "performance" && mappingCommand === "PROFILE" ? "cicada" : selected;
    profile = await readProfile(mappingCommand + " " + wireName);
    status(profile === "performance" ? "Performance MIDI mapping saved." : "Raw MIDI mapping saved.");
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
  el("loadedTrack").hidden = !tracks;
  if (!tracks) return;
  const selected = tracks[bank];
  const label = selected ? displayName(selected) : bank === "samples" ? "No sample reported" : "No beat reported";
  el("loadedName").textContent = label;
  for (const button of el("fileList").querySelectorAll(".file-load")) {
    const loaded = selected === button.dataset.key;
    const action = loaded ? "Reload" : "Load";
    button.toggleAttribute("data-loaded", loaded);
    button.textContent = action + " on WOW";
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
  status("Loaded " + entry.name + " on the WOW.");
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
            await refreshList();
            status("Deleted " + entry.name + ".");
          }),
      );
    });
    remove.className = "file-delete";
    const actions = document.createElement("div");
    actions.className = "file-actions";
    if (tracks) {
      const load = actionButton("Load on WOW", entry.name, () => run(() => loadTrack(folder, entry)));
      load.className = "file-load";
      load.dataset.key = entry.key;
      load.dataset.name = entry.name;
      actions.append(load);
    }
    actions.append(actionButton("Save", entry.name, () => run(() => save(folder, entry))), remove);
    row.append(preview.button(folder, entry), name, actions);
    return row;
  });
  el("fileList").replaceChildren(...rows);
  el("emptyFolder").hidden = rows.length > 0;
  el("previewHint").hidden = !rows.length;
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
    el("previewHint").hidden = true;
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
