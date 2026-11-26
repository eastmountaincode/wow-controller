# WOW Controller

[Open WOW Controller](https://wow-controller.vercel.app)

Use a HeadacheSound WOW Machine as a USB MIDI controller and manage its onboard audio from a browser. The platter, fader stops, pitch knob and buttons can control another MIDI instrument while the WOW plays its own samples and beats.

## Connect a WOW

A stock deck needs the controller firmware on its microSD before the website can connect. Check your card revision in the [setup and firmware guide](HARDWARE.md). The [firmware release](https://github.com/eastmountaincode/wow-controller/releases/latest) includes the current controller and the factory system firmware for the supported revision.

With the controller installed, connect the deck with a USB data cable and switch it on normally. Open the site in a browser with Web MIDI and WebUSB. Choose **Connect MIDI** to see the live deck, then **Connect deck** for the mixer, library and settings. When prompted, select **WOW MIDI Controller**.

| Page | Controls |
| --- | --- |
| Deck | Watch the platter and hardware controls, start or stop the beat, and set the master, sample and beat levels. |
| Library | Preview audio through the computer, load a sample or beat on the WOW, or transfer files to and from the deck. |
| MIDI | See the message map or try the platter synth. Another browser instrument can use the MIDI input directly. |
| Settings | Set startup playback, bypass the rear mix knob, test the START light or install a signed update. |

The site communicates directly with the connected deck. Library audio stays on the card or in the browser during transfers and previews.

## Run locally

The website in `public/` uses JavaScript modules and has no build step. From the repository root, run the following command and open [the local controller](http://127.0.0.1:8786/).

```sh
python3 -m http.server 8786 --bind 127.0.0.1 --directory public
```

Vercel serves `public/`. From a checkout linked to your Vercel project, run `vercel deploy --prod` to publish. The custom firmware source and build tools are in `controller/`. The bundled DM Sans font includes its [license](public/fonts/OFL.txt).
