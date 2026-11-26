# WOW Controller

A browser controller for the HeadacheSound WOW Machine. Connect the deck with a USB data cable to see its platter, fader, pitch and buttons, manage its samples and beats, or use it with a MIDI instrument. The library and mixer use the WOW controller firmware’s USB interface. Browser previews play through the computer.

Use a browser with Web MIDI for the live deck and WebUSB for the library and settings. Grant access to **WOW MIDI Controller** when prompted. The site communicates directly with the deck.

## Run locally

From the repository root, run the following command and open [the local controller](http://127.0.0.1:8786/).

```sh
python3 -m http.server 8786 --bind 127.0.0.1 --directory public
```

Vercel serves the static files in `public/`. The bundled DM Sans font license is in [public/fonts/OFL.txt](public/fonts/OFL.txt).
