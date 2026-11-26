const sampleRate = 44100;
const pcmLimit = 128 * 1024 * 1024;
const maxColumns = 8192;

function audioDuration(blob, signal) {
  const audio = document.createElement("audio");
  const url = URL.createObjectURL(blob);
  return new Promise((resolve, reject) => {
    function finish() {
      const duration = audio.duration;
      clearTimeout(timer);
      audio.onloadedmetadata = audio.onerror = null;
      signal.removeEventListener("abort", finish);
      audio.removeAttribute("src");
      audio.load();
      URL.revokeObjectURL(url);
      if (!signal.aborted && Number.isFinite(duration) && duration > 0) resolve(duration);
      else reject(new Error("Couldn’t read the sample duration."));
    }
    const timer = setTimeout(finish, 10000);
    signal.addEventListener("abort", finish, { once: true });
    audio.onloadedmetadata = audio.onerror = finish;
    audio.preload = "metadata";
    audio.src = url;
  });
}

function channelBound(bytes, name) {
  const view = new DataView(bytes);
  if (view.byteLength >= 12 && view.getUint32(0) === 0x52494646 && view.getUint32(8) === 0x57415645) {
    // Count the source channels from the RIFF fmt chunk before allocating decoded audio.
    for (let i = 12; i + 12 <= view.byteLength;) {
      const size = view.getUint32(i + 4, true);
      if (view.getUint32(i) === 0x666d7420 && size >= 4) return view.getUint16(i + 10, true) || 32;
      i += 8 + size + (size % 2);
    }
  }
  return /\.mp3$/i.test(name) ? 2 : 32;
}

function envelope(buffer) {
  const columns = Math.min(buffer.length, maxColumns);
  const low = new Float32Array(columns).fill(Infinity);
  const high = new Float32Array(columns).fill(-Infinity);
  for (let ch = 0; ch < buffer.numberOfChannels; ch++) {
    const samples = buffer.getChannelData(ch);
    let start = 0;
    for (let x = 0; x < columns; x++) {
      const end = Math.floor((x + 1) * samples.length / columns);
      let min = low[x], max = high[x];
      for (let i = start; i < end; i++) {
        const v = samples[i];
        if (v < min) min = v;
        if (v > max) max = v;
      }
      low[x] = min;
      high[x] = max;
      start = end;
    }
  }
  return { low, high, duration: buffer.duration };
}

function timeLabel(seconds) {
  const tenths = Math.floor(seconds * 10);
  return Math.floor(tenths / 600) + ":" + String(Math.floor(tenths / 10) % 60).padStart(2, "0") + "." + tenths % 10;
}

export function createSampleWaveform() {
  const section = document.getElementById("sampleWave");
  const canvas = document.getElementById("waveShape");
  const track = canvas.parentElement;
  const name = document.getElementById("waveName");
  const time = document.getElementById("waveTime");
  const note = document.getElementById("waveNote");
  const cursor = document.getElementById("waveCursor");
  let key, peaks, fraction, engineSeconds, engineDuration;
  let job, decoder, decodeQueue = Promise.resolve();
  time.setAttribute("aria-live", "off");

  function message(text = "") {
    note.textContent = text;
    note.hidden = !text;
  }

  function visibleDuration() {
    if (!peaks) return 0;
    // The stock decoder can load a prefix of the file. Draw only the frames it loaded.
    return engineDuration && engineDuration < peaks.duration - peaks.tolerance ? engineDuration : peaks.duration;
  }

  function draw() {
    if (!peaks || !canvas.clientWidth || !canvas.clientHeight) return;
    const seconds = visibleDuration();
    const ratio = Math.min(window.devicePixelRatio || 1, 3);
    const width = Math.max(1, Math.round(canvas.clientWidth * ratio));
    const height = Math.max(1, Math.round(canvas.clientHeight * ratio));
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const style = getComputedStyle(canvas);
    const center = Math.floor(height / 2);
    ctx.fillStyle = style.getPropertyValue("--wave-rule");
    ctx.fillRect(0, center, width, ratio);
    // Grid divisions are whole multiples of 1, 2, or 5 seconds (or powers of ten).
    const target = seconds * 90 / canvas.clientWidth;
    const unit = 10 ** Math.floor(Math.log10(target));
    const step = [1, 2, 5, 10].find(n => n * unit >= target) * unit;
    for (let tick = step; tick < seconds; tick += step) {
      const x = Math.round(tick * width / seconds);
      ctx.fillRect(x, 0, ratio, 5 * ratio);
      ctx.fillRect(x, height - 5 * ratio, ratio, 5 * ratio);
    }
    ctx.fillStyle = style.color;
    const span = Math.min(peaks.low.length, peaks.low.length * seconds / peaks.duration);
    const scale = height * 0.44;
    for (let x = 0; x < width; x++) {
      const from = Math.floor(x * span / width);
      const to = Math.max(from + 1, Math.floor((x + 1) * span / width));
      let lo = peaks.low[from], hi = peaks.high[from];
      for (let i = from + 1; i < to; i++) {
        lo = Math.min(lo, peaks.low[i]);
        hi = Math.max(hi, peaks.high[i]);
      }
      if (lo === 0 && hi === 0) continue;
      ctx.fillRect(x, center - hi * scale, 1, Math.max(1, (hi - lo) * scale));
    }
  }

  function paintPosition() {
    const measured = Boolean(peaks) && fraction !== null;
    const duration = engineDuration ?? peaks?.duration;
    const elapsed = engineSeconds ?? (measured ? fraction * duration : null);
    let x = fraction;
    // If the engine reports more time than the decoded file contains, keep the
    // file's time scale. Past its final sample there is no waveform to mark.
    if (measured && engineDuration > peaks.duration + peaks.tolerance) x = elapsed / peaks.duration;
    const located = measured && x >= 0 && x <= 1;
    cursor.hidden = !located;
    if (located) cursor.style.left = (x * 100) + "%";
    const value = duration == null ? "" : measured
      ? timeLabel(elapsed) + " / " + timeLabel(duration)
      : timeLabel(duration);
    const label = measured ? "Position / duration" : "Duration";
    if (time.textContent !== value) time.textContent = value;
    if (time.getAttribute("aria-label") !== label) time.setAttribute("aria-label", label);
  }

  const observer = new ResizeObserver(draw);
  function clear() {
    job?.abort();
    job = key = peaks = null;
    fraction = engineSeconds = engineDuration = null;
    observer.disconnect();
    section.hidden = track.hidden = true;
    name.textContent = "";
    message();
    paintPosition();
  }

  function select(nextKey, filename) {
    if (!nextKey) { clear(); return; }
    if (key === nextKey) return;
    clear();
    key = nextKey;
    name.textContent = filename;
    section.hidden = false;
    message("Loading waveform…");
    canvas.setAttribute("aria-label", "Waveform of " + filename);
  }

  function load(blob, expectedKey) {
    if (!key || key !== expectedKey) return Promise.resolve(false);
    job?.abort();
    observer.disconnect();
    peaks = null;
    track.hidden = true;
    paintPosition();
    message("Loading waveform…");
    const current = job = new AbortController();
    const filename = name.textContent;
    // Browser audio decodes cannot be cancelled. Limit concurrent memory and skip superseded requests.
    const next = decodeQueue.then(async () => {
      if (current.signal.aborted) return false;
      try {
        const [seconds, header] = await Promise.all([
          audioDuration(blob, current.signal), blob.slice(0, 65536).arrayBuffer()
        ]);
        if (current.signal.aborted) return false;
        if (seconds * channelBound(header, filename) * sampleRate * 4 > pcmLimit) {
          message("Sample is too long for a waveform.");
          return false;
        }
        decoder ??= new OfflineAudioContext(1, 1, sampleRate);
        const bytes = await blob.arrayBuffer();
        if (current.signal.aborted) return false;
        const decoded = await decoder.decodeAudioData(bytes);
        if (current.signal.aborted) return false;
        if (!decoded.length || !decoded.duration) throw new Error("Empty sample.");
        peaks = envelope(decoded);
        // WAV resampling can round a frame; MP3 decoders can differ by encoder padding.
        const frameRounding = 2 / sampleRate;
        peaks.tolerance = /\.mp3$/i.test(filename)
          ? Math.max(frameRounding, Math.min(0.026, peaks.duration * 0.005))
          : frameRounding;
        track.hidden = false;
        message();
        observer.observe(canvas);
        draw();
        paintPosition();
        return true;
      } catch {
        if (!current.signal.aborted) message("Waveform unavailable.");
        return false;
      }
    });
    decodeQueue = next;
    return next;
  }

  clear();
  return {
    select, load, clear, message,
    position(value, seconds, duration) {
      fraction = Number.isFinite(value) ? Math.min(1, Math.max(0, value)) : null;
      engineSeconds = Number.isFinite(seconds) && seconds >= 0 ? seconds : null;
      if (Number.isFinite(duration) && duration > 0 && engineDuration !== duration) {
        const previous = visibleDuration();
        engineDuration = duration;
        if (previous !== visibleDuration()) draw();
      }
      paintPosition();
    },
    get duration() { return peaks?.duration ?? null; },
    destroy() { clear(); decoder = null; }
  };
}
