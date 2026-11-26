import { DeviceError } from "./library-client.js";

const memoryLimit = 128 * 1024 * 1024;

function nameButton(button, action, name) {
  const label = action + " " + name + " in browser";
  button.setAttribute("aria-label", label);
  button.title = label;
  button.dataset.action = action.toLowerCase();
}

export function createLibraryPreview(getClient, onTransportError, waitForStatus) {
  let active, reading;

  function stop() {
    if (!active) return;
    const track = active;
    active = undefined;
    track.controller.abort();
    if (track.url) {
      track.audio.pause();
      track.audio.removeAttribute("src");
      track.audio.load();
      URL.revokeObjectURL(track.url);
      track.cleanup?.();
    }
    const refocus = track.panel.contains(document.activeElement);
    track.panel.remove();
    nameButton(track.button, "Play", track.entry.name);
    delete track.button.dataset.active;
    if (refocus) track.button.focus();
  }

  function reflect(track) {
    if (active !== track || !track.url) return;
    const { audio, entry, button } = track;
    const action = audio.ended ? "Replay" : audio.paused ? (audio.currentTime ? "Resume" : "Play") : "Pause";
    nameButton(button, action, entry.name);
    track.label.textContent = audio.ended ? "Browser preview finished" : audio.paused ? "Browser preview paused" : "Playing in browser";
  }

  async function play(track) {
    try {
      if (track.audio.ended) track.audio.currentTime = 0;
      await track.audio.play();
    } catch (error) {
      if (active !== track || error.name === "AbortError") return;
      track.label.textContent = error.name === "NotAllowedError"
        ? "Ready to play in browser"
        : "This browser can’t play this file.";
    }
  }

  async function readFile(current, track) {
    const { entry, controller, progress } = track;
    const signal = controller.signal;
    let sink, cache, filename;
    const cleanup = () => cache?.removeEntry(filename).catch(() => {});
    try {
      // A long recording can be streamed to a private temporary file. Keep
      // short previews in memory and release either backing store on Stop.
      if (entry.size > memoryLimit) {
        if (!navigator.storage?.getDirectory)
          throw new DeviceError("This browser can preview files up to 128 MiB. Save the file to listen to it.");
        try {
          cache = await navigator.storage.getDirectory();
          filename = "wow-preview-" + crypto.randomUUID();
          const handle = await cache.getFileHandle(filename, { create: true });
          sink = await handle.createWritable();
          track.handle = handle;
        } catch {
          throw new DeviceError("Browser storage is unavailable for a preview this large. Save the file to listen to it.");
        }
      }
      const blob = await current.download(track.folder, entry, (done, total) => {
        progress.max = total || 1;
        progress.value = done;
      }, sink, signal);
      signal.throwIfAborted();
      let file = blob;
      if (sink) {
        try { file = await track.handle.getFile(); }
        catch { throw new DeviceError("The browser could not open the temporary preview file."); }
      }
      signal.throwIfAborted();
      const type = /\.mp3$/i.test(entry.name) ? "audio/mpeg" : /\.wav$/i.test(entry.name) ? "audio/wav" : file.type;
      track.url = URL.createObjectURL(file.slice(0, file.size, type));
      track.cleanup = cleanup;
      track.audio.src = track.url;
      track.audio.hidden = false;
      track.progress.hidden = true;
      track.close.textContent = "Stop";
      track.close.setAttribute("aria-label", "Stop preview of " + entry.name);
      track.label.textContent = "Ready to play in browser";
      nameButton(track.button, "Play", entry.name);
      void play(track);
    } finally {
      if (!track.url && sink) await sink.abort().catch(() => {});
      if (!track.url && cache && filename) await cleanup();
    }
  }

  function select(folder, entry, button) {
    if (active?.button === button) {
      if (!active.url) stop();
      else if (active.audio.paused || active.audio.ended) void play(active);
      else active.audio.pause();
      return;
    }
    stop();
    const panel = document.getElementById("previewTemplate").content.firstElementChild.cloneNode(true);
    const track = {
      folder, entry, button, panel, controller: new AbortController(),
      progress: panel.querySelector("progress"), audio: panel.querySelector("audio"),
      label: panel.querySelector(".preview-state"), close: panel.querySelector(".preview-stop"),
    };
    active = track;
    button.dataset.active = "";
    nameButton(button, "Cancel", entry.name);
    panel.setAttribute("aria-label", "Browser preview of " + entry.name);
    track.audio.setAttribute("aria-label", "Preview of " + entry.name);
    track.close.setAttribute("aria-label", "Cancel preview of " + entry.name);
    track.close.addEventListener("click", stop);
    for (const event of ["playing", "pause", "ended"])
      track.audio.addEventListener(event, () => reflect(track));
    track.audio.addEventListener("error", () => {
      if (active === track) {
        track.label.textContent = "This browser can’t play this file.";
        nameButton(button, "Play", entry.name);
      }
    });
    button.closest("li").append(panel);
    const current = getClient();
    const previous = reading;
    const operation = (async () => {
      try {
        if (previous) await previous;
        await waitForStatus();
        if (active === track && current === getClient()) await readFile(current, track);
      } catch (err) {
        if (err.name === "AbortError" || current !== getClient()) return;
        if (!(err instanceof DeviceError)) {
          await onTransportError(err);
        } else if (active === track) {
          track.progress.hidden = true;
          track.close.textContent = "Close";
          track.close.setAttribute("aria-label", "Close preview of " + entry.name);
          track.label.textContent = err.message;
          nameButton(button, "Close", entry.name);
        }
      }
    })();
    reading = operation;
    operation.finally(() => {
      if (reading === operation) reading = undefined;
    });
  }

  return {
    stop,
    get reading() { return Boolean(reading); },
    async releaseRead() {
      if (!reading) return;
      if (active && !active.url) stop();
      await reading;
    },
    button(folder, entry) {
      const control = document.createElement("button");
      control.type = "button";
      control.className = "preview-toggle";
      nameButton(control, "Play", entry.name);
      control.addEventListener("click", () => select(folder, entry, control));
      return control;
    },
  };
}
