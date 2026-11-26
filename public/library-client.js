// The deck's MIDI interface stays under the host MIDI driver's control.
const VENDOR_INTERFACE = { interfaceClass: 0xff, interfaceSubclass: 0x57, interfaceProtocol: 1 };
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });
const chunkSize = 24576;
const mediaLimit = 0xffffffff;
const updateLimit = 8 * 1024 * 1024;

const crcTable = Uint32Array.from({ length: 256 }, (_, n) => {
  for (let bit = 0; bit < 8; bit++) n = (n >>> 1) ^ (n & 1 ? 0xedb88320 : 0);
  return n >>> 0;
});
function updateCRC(crc, bytes) {
  for (const value of bytes) crc = (crc >>> 8) ^ crcTable[(crc ^ value) & 255];
  return crc;
}
function hexCRC(crc) {
  return ((crc ^ 0xffffffff) >>> 0).toString(16).padStart(8, "0");
}
function base64(bytes) {
  return btoa(String.fromCharCode(...bytes));
}
function bytes64(encoded) {
  return Uint8Array.from(atob(encoded), c => c.charCodeAt(0));
}
export function displayName(token) {
  const bytes = bytes64(token);
  try { return decoder.decode(bytes); }
  catch { return new TextDecoder("windows-1252").decode(bytes); }
}

export class DeviceError extends Error {}
export class LibraryClient {
  buffer = "";
  constructor(device) { this.device = device; }

  async open() {
    await this.device.open();
    if (!this.device.configuration) await this.device.selectConfiguration(1);
    const iface = this.device.configuration.interfaces.find(iface =>
      iface.alternates.some(a => Object.entries(VENDOR_INTERFACE).every(([key, value]) => a[key] === value)));
    if (!iface) throw new Error("This WOW needs the library firmware.");
    const alt = iface.alternates.find(a => Object.entries(VENDOR_INTERFACE).every(([key, value]) => a[key] === value));
    this.index = iface.interfaceNumber;
    await this.device.claimInterface(this.index);
    if (iface.alternate.alternateSetting !== alt.alternateSetting)
      await this.device.selectAlternateInterface(this.index, alt.alternateSetting);
    this.input = alt.endpoints.find(e => e.type === "bulk" && e.direction === "in");
    this.output = alt.endpoints.find(e => e.type === "bulk" && e.direction === "out");
    if (!this.input || !this.output) throw new Error("The WOW library USB endpoints are unavailable.");
    await this.resetSession();
  }

  async resetSession() {
    const reset = await this.wait(this.device.controlTransferOut({
      requestType: "vendor", recipient: "interface", request: 0x51, value: 0, index: this.index
    }));
    if (reset.status !== "ok") throw new Error("The WOW library could not start a new session.");
    this.buffer = "";
    await this.ack("HELLO", "OK WOW-LIBRARY 1");
  }

  async wait(promise) {
    let timer;
    try {
      return await Promise.race([promise, new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error("The WOW stopped responding. Reconnect the library.")), 30000);
      })]);
    } finally { clearTimeout(timer); }
  }

  async close() {
    if (this.device.opened) await this.device.close();
    this.buffer = "";
  }

  async send(line) {
    // Every protocol token is ASCII. Pad an exact-packet command so the
    // FunctionFS reader gets a final short OUT packet, including at full speed.
    const ending = (line.length + 1) % this.output.packetSize === 0 ? " \n" : "\n";
    const bytes = encoder.encode(line + ending);
    const result = await this.wait(this.device.transferOut(this.output.endpointNumber, bytes));
    if (result.status !== "ok" || result.bytesWritten !== bytes.length)
      throw new Error("The USB write was interrupted.");
  }

  async line(expectedBytes = 0) {
    for (;;) {
      const end = this.buffer.indexOf("\n");
      if (end > 32800 || (end < 0 && this.buffer.length > 32800))
        throw new Error("Invalid response length from the WOW.");
      if (end >= 0) {
        const result = this.buffer.slice(0, end);
        this.buffer = this.buffer.slice(end + 1);
        if (result.startsWith("ERR ")) throw new DeviceError(result.slice(4));
        return result;
      }
      // GET gives us a size in advance. Read full packets up to the expected
      // DATA line boundary; a one-packet tail also handles exact-packet responses.
      const packet = this.input.packetSize;
      const remaining = expectedBytes - this.buffer.length;
      const requestSize = Math.max(packet, Math.floor(remaining / packet) * packet);
      const next = await this.wait(this.device.transferIn(this.input.endpointNumber, requestSize));
      if (next.status !== "ok" || !next.data) throw new Error("The USB read was interrupted.");
      this.buffer += decoder.decode(next.data);
    }
  }

  async ack(command, expected = "OK") {
    await this.send(command);
    if (await this.line() !== expected) throw new Error("Unexpected response from the WOW.");
  }

  async list(bank) {
    await this.send("LIST " + bank);
    const result = [];
    for (;;) {
      const line = await this.line();
      if (line === "OK") return result;
      const match = /^FILE (\d+) (\S+)$/.exec(line);
      if (!match) throw new Error("Invalid library listing.");
      result.push({ name: displayName(match[2]), key: match[2], size: Number(match[1]) });
    }
  }

  async upload(bank, file, onProgress) {
    const firmware = bank === null;
    const limit = firmware ? updateLimit : mediaLimit;
    if (!file.size || file.size > limit) throw new DeviceError(firmware ? "Choose a nonempty update of up to 8 MiB." : "Choose a nonempty file smaller than 4 GiB.");
    if (firmware && !/\.upd$/i.test(file.name)) throw new DeviceError("Choose a signed .upd file.");
    if (!firmware && !/\.(wav|mp3)$/i.test(file.name)) throw new DeviceError("Choose a WAV or MP3 file.");
    const command = firmware ? "STAGE " + file.size : "PUT " + bank + " " + base64(encoder.encode(file.name)) + " " + file.size;
    await this.ack(command);
    let crc = 0xffffffff;
    try {
      for (let offset = 0; offset < file.size; offset += chunkSize) {
        const block = new Uint8Array(await file.slice(offset, offset + chunkSize).arrayBuffer());
        crc = updateCRC(crc, block);
        await this.ack("DATA " + base64(block));
        onProgress(offset + block.length, file.size);
      }
      await this.ack("COMMIT " + hexCRC(crc));
    } catch (err) {
      // A transport fault requires a fresh session; never issue another command
      // against an uncertain response boundary.
      if (err instanceof DeviceError) await this.ack("ABORT");
      throw err;
    }
  }

  async download(bank, file, onProgress, sink, signal) {
    signal?.throwIfAborted();
    await this.send("GET " + bank + " " + file.key);
    const header = /^SIZE (\d+)$/.exec(await this.line());
    if (!header || !Number.isSafeInteger(Number(header[1])) || Number(header[1]) > 0xffffffff)
      throw new Error("Invalid size from the WOW.");
    const size = Number(header[1]), blocks = [];
    if (!sink && size > 128 * 1024 * 1024) {
      await this.resetSession();
      throw new DeviceError("This file needs a browser that supports streaming directly to disk.");
    }
    let length = 0, crc = 0xffffffff;
    for (;;) {
      if (signal?.aborted) {
        // GET streams its entire response. Start a fresh library session before
        // a new request so that no stale DATA is mistaken for a command reply.
        await this.resetSession();
        signal.throwIfAborted();
      }
      const nextBytes = Math.min(chunkSize, size - length);
      const line = await this.line(nextBytes ? 6 + 4 * Math.ceil(nextBytes / 3) : 0);
      if (line.startsWith("END ")) {
        if (length !== size || line.slice(4).toLowerCase() !== hexCRC(crc))
          throw new Error("The downloaded file failed its checksum.");
        if (sink) {
          try { await sink.close(); } catch (err) { throw new DeviceError(err.message); }
          return;
        }
        return new Blob(blocks);
      }
      if (!line.startsWith("DATA ")) throw new Error("Invalid download from the WOW.");
      const block = bytes64(line.slice(5));
      if (!block.length || block.length > chunkSize || length + block.length > size)
        throw new Error("Invalid download block from the WOW.");
      length += block.length;
      if (sink) {
        try { await sink.write(block); }
        catch (err) { await this.resetSession(); throw new DeviceError(err.message); }
      } else blocks.push(block);
      crc = updateCRC(crc, block);
      onProgress(length, size);
    }
  }
}
