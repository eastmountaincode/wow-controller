# Using the WOW Machine as a USB controller

The HeadacheSound WOW Machine normally appears as a USB drive when connected to a computer. With this project's custom controller installed on its microSD, the WOW can run its onboard audio engine, send MIDI and manage its sample library over one USB data cable. If your deck still runs the factory software, see [Starting with a factory card](#starting-with-a-factory-card). [HeadacheSound's WOW FAQ](https://www.headachesound.com/wow-faq)

## Connect your deck

1. Connect the WOW to your computer or dock with a USB data cable. Switch it on with no buttons held.
2. Open the [WOW Controller](https://wow-controller.vercel.app). On **Deck**, select **Connect MIDI** to see platter position and speed, pitch, buttons, fader end stops and the START light. On the diagram, 12 o'clock is 0° and a clockwise turn increases the angle.
3. Select **Connect deck** and choose **WOW MIDI Controller** to use the library, mixer and settings. The browser needs Web MIDI for the live diagram and WebUSB for deck management.

To use a different instrument, select **WOW MIDI Controller** as its MIDI input and assign the controls there. Your instrument supplies the audio. For a quick test in the controller site, open **MIDI** and select **Enable synth**. Turning the platter moves up or down a scale.

## Playback and library

| What you want to do | Control |
| --- | --- |
| Listen to a file on your computer | Open **Library**, choose **Samples** or **Beats**, and press the play button beside a WAV or MP3. You can pause, seek or stop in the same row. |
| Select a track for the WOW | Choose **Load on WOW** beside a sample or beat. The loaded track is marked in its bank. Start or stop the backing beat on **Deck**. |
| Add or remove files | Use **Library** to upload, download or delete files. MIDI can stay connected during a transfer. |
| Start or stop the backing beat | Choose **Play beat** or **Stop beat** on **Deck**. Double-tap **BEAT** on the hardware to toggle playback. A single tap starts playback or advances to the next beat. The transport works in normal playback mode. |
| Start quietly | Turn off **Play beat on startup** in **Settings**. The deck saves the change for its next normal power-on. |
| Adjust the deck's output | Set the physical rear master to your preferred upper limit. Adjust the **Master**, **Beat** and **Sample** levels on **Deck**. To control the balance entirely from the site, turn off **Use rear mix knob** in **Settings**. Both rear knob positions remain visible. The browser's library preview has its own volume. |
| Check the START light | In **Settings**, choose a color or **Off** and select **Test light**. The factory pattern resumes after four seconds, or immediately when you select **End test**. |

Track selection, rear-knob readouts and the rear mix bypass require controller firmware 1.4 or later. **Settings** shows the version and release ID reported by the connected deck.

## Update a deck with the custom updater

1. Download the signed controller update (`.upd`) from the [latest release](https://github.com/eastmountaincode/wow-controller/releases/latest).
2. In **Settings**, select **Connect deck**, then **Load firmware update**. Choose the file and confirm.
3. Wait for the site to report that the update is staged. Switch the WOW off and back on normally, leaving the card inside.
4. In the same browser, reconnect the deck and check **Version** and **Release ID**. **Last update confirmed** records when the browser confirmed its staged release was running.

If the browser cannot reach the library, hold **SAMPLE alone** while powering on to mount the media as a USB drive. Copy the signed update to the top level as `WOWCTRL.UPD`, eject the drive, and power-cycle normally. Holding **BEAT alone** during power-on starts the controller built into the card for recovery. The previous controller boot log is available as `wow-midi.log` on the media drive.

## Starting with a factory card

A stock deck needs a one-time installation on its internal microSD before it can accept this project's USB updates. Development and earlier hardware tests used one WOW revision. The version 1.4.0 first-install image and the rebuilt factory image still need a physical boot test.

### Identify your card

Power the deck off, disconnect USB and remove the microSD. Create a full backup of **your own card**. Verify the backup against the card and keep it for recovery. Inspect the backup's partition table. The reference card is 8,063,549,440 bytes and uses an MBR with 512-byte sectors.

| Partition | Start in bytes | Length in bytes |
| --- | ---: | ---: |
| 1. Linux system | 1,048,576 | 134,217,728 |
| 2. FAT32 media | 135,266,304 | 6,442,450,944 |
| 3. Linux swap | 6,577,717,248 | 1,073,741,824 |

Export the first partition of the verified backup as `stock-system.img`. These read-only commands check the detached file on macOS.

```sh
wc -c < stock-system.img
shasum -a 256 stock-system.img
```

The **original raw factory partition** has size `134217728` and SHA-256 `68163fad670ad3b2c4ebbd6f1a5e61e72d3afc5a33648f6fad31ce3d4f0e29c5`. This fingerprint identifies the stock card used in development. If your stock partition or layout differs, stop before writing. Other revisions have not been validated.

### Install the controller partition

1. Download `WOW-controller-1.4.0-system.img.gz` and the release checksums. Verify the compressed file against the entry with its exact filename. Decompress it, confirm the resulting image is 134,217,728 bytes, and record the SHA-256 of that decompressed file. [Latest release](https://github.com/eastmountaincode/wow-controller/releases/latest)
2. Confirm the target is the same card you backed up. Unmount its volumes and use a partition imaging tool to write the decompressed image **only to partition 1**, the Linux system partition. A whole-card write would overwrite the boot area and partition table.
3. Flush the write, then read back all 134,217,728 bytes of partition 1 into a new file. Check that this file has the same size and SHA-256 as the decompressed image used in step 1. If either differs, keep the card out of the deck. Restore partition 1 from your verified backup and verify that readback before booting.
4. Eject the card, return it to the powered-off deck, connect USB and power on normally. Open **Settings**, select **Connect deck**, and check the running version and release ID. The readback verifies the bytes written. This connection and version check verifies that the deck actually booted the controller. Future signed updates use the browser.

### Restore the factory system

Your verified full-card backup preserves the original boot area, partition table, system, library, recordings and swap. For an exact restoration of the original system partition, use partition 1 from that backup.

The **factory system image** in the release was rebuilt from allocated stock firmware files, including the onboard default sounds. It omits the original filesystem's free-space contents and maintenance history. The separate card library and recordings live on partition 2 and are also excluded. The rebuilt filesystem has its own SHA-256, different from the raw factory fingerprint used above to identify a compatible stock card.

To write the rebuilt factory image on a previously matched card, verify the download against **its own** release checksum and follow the partition-only write and readback steps above. Compare the readback with the decompressed **factory release file** you wrote. Power on to check factory operation. Restoring this partition returns the deck to the stock USB behavior.

The project includes its [partition builder](controller/build-partition.py) and [development card writer](controller/bootstrap-card.py) for reference. The writer checks a specific full-card backup and CalDigit TS4 reader. HeadacheSound publishes its stock software and update instructions in the [official WOW user manual](https://www.headachesound.com/wowusermanual).

## MIDI reference

The WOW sends on channel 1. In **Settings → Output mapping**, choose **Raw MIDI** for the sensor readings, or **Performance MIDI** to add modulation and pitch bend. For paired control changes, combine the first value (high) and second value (low) as `(high << 7) | low`. The controller repeats a snapshot of the raw controls twice per second.

| Control | Raw MIDI | Performance MIDI | Values |
| --- | --- | --- | --- |
| Platter | CC 16 + 48 | CC 16 + 48 and CC 1 | The encoder reports 0–16383 per revolution. Calculate motion from differences across the wrap point. CC 1 reports the angle at 7-bit resolution. |
| Left and right fader sensors | CC 17 + 49 (left), CC 18 + 50 (right) | Same | Shift each combined value right by two bits for the 12-bit sensor reading. The sensors detect the two end stops. Travel between them is unmeasured. |
| Pitch knob | CC 19 + 51 | CC 19 + 51 and pitch bend | Same 12-bit sensor encoding. Pitch bend is centered at 8192. |
| Rear master and mix knobs | CC 23 + 55 (master), CC 24 + 56 (mix) | Same | Same 12-bit sensor encoding. Both report the physical position before software adjustments, including when the rear mix knob is bypassed. |
| Motor | CC 20 | CC 102 | 127 when the engine requests rotation, 0 when it requests a stop. Platter readings show actual movement. |
| Held buttons | CC 21 | CC 103 | Bits 0–2 are START, BEAT and SAMPLE. Bits 3–6 are Cue 1–4. |
| Fader end stops | CC 22 | CC 104 | Bit 0 is left and bit 1 is right. A set bit means the sensor crossed its calibrated threshold. |
| Button events | Notes 36–42 | Same | Press and release for START, BEAT, SAMPLE and Cue 1–4, respectively, where fitted. |
| START light color | CC 105 | Same | Bits 0–2 report the dominant red, green and blue channels. Zero means off. 127 means the first lamp reading is pending. |
| START light brightness | CC 106, 107, 108 | Same | Red, green and blue channel intensities, respectively, on the MIDI 0–127 scale. |
