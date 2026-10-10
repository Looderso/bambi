# bambi-live-bridge — trying it in Live

## Once

- [ ] Install BlackHole 16ch (asks for your password): `brew install --cask blackhole-16ch`
- [ ] `cmake --build build --target bambi-live-bridge`
- [ ] `./build/bambi-live-bridge aggregate`, which creates **bambi bridge**: the MacBook speakers (clock) + BlackHole 16ch (drift-corrected).
      Bluetooth headphones stay out of it.

## Live

Preferences → Audio: device **bambi bridge** for input and output, 48 kHz, buffer 128.
Input Config: enable **13** (mono) and **15/16** (stereo). Output Config: enable **1/2**, **3–10** (mono) and **16** (mono).

Live's outputs: 1/2 are the speakers; BlackHole channel *n* is Live's output *n + 2*. Inputs keep their numbers.

| track | Audio From | Monitor | Audio To |
|---|---|---|---|
| sources 1–8 | (their clips) | Auto | Ext. Out **3**, 4, … **10** (mono) |
| **bambi** (the return) | Ext. In **15/16** | In | Master |
| **probe** (only with `--probe`) | Ext. In **13** | In | Ext. Out **16** |
| Master | | | Ext. Out **1/2** (speakers) |

## Or: the routing button

`max/bambi-routing.amxd` (made by `max/make-device.py`) replaces the table. Drag it onto the **Main** track; nothing
else needs routing. **bambi** on: every track playing to Main (up to 12, not one called *probe*) goes to its own
BlackHole channel, its routing remembered, and a track called **bambi** appears to play the return. Off: every
track gets its routing back and the bambi track is deleted. Output Config needs **3–14** enabled as mono and Input
Config **15/16**; run the tool with `--sources 12`. Messages land in the Max window and in
`~/Library/Application Support/Cycling '74/Max 9/Logs/MaxPlugLib.log`.

## Run

- [ ] Mac volume low first.
- [ ] `./build/bambi-live-bridge run --rotate 20 --probe`, then press play in Live.
- [ ] Each source's meter in the terminal moves when its track plays. If not, its routing is off.
- [ ] Sound arrives on the speakers through the **bambi** track (binaural over speakers won't sound spatial;
      that comes with headphones later).

## Measure

- [ ] The `loop` value after a minute: ___ ms. `spatial path` estimates what the mix lags a dry track: ___ ms
- [ ] Again at buffer 512 in both Live and `--buffer 512`: ___ ms
- [ ] After an hour: is `loop` still within a few samples of where it started (the range in brackets)? ___
- [ ] Any `OVERLOAD` lines? ___

## After

- [ ] ctrl-c stops the tool. `./build/bambi-live-bridge remove` removes the device (switch Live away from it first).
