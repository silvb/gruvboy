# Phase 0: Platform Findings

**Status:** the test app builds and passes its desktop self-test, but has not yet run on a Miyoo. Nothing here about on-device behaviour is measured yet.

Source findings were read from Onion at tag `v4.3.1-1` where it matters (`stop_audioserver.sh` and `keymap_hw.h` were compared directly) and otherwise from its current main branch.

## Missing device measurements

The click test has not run on the Miyoo, so none of these are known yet:

| Measurement | Checklist step | Why it matters |
|---|---|---|
| Whether the app launches, with `DRIVER MIAO`, `INPUT EVDEV` and `RT YES` | 2 | Confirms the bundled SDL, direct input and real-time priority work |
| Screen resolution and orientation | 2 | The app assumes 640×480 |
| Buffer size the driver actually uses | 3 | The driver may override the requested size |
| Smallest buffer with no dropouts, with and without UI stress | 3 | Sets the lowest usable latency |
| Press-to-sound latency, with direct and SDL input | 4 | Decides the go/no-go gate |
| How playing rhythms feels, through headphones and the speaker | 5 | Final judgement for the 20 to 30 ms range |
| Menu button behaviour | 5 | Whether Gruvboy can use it |
| Audio after sleep and wake | 5 | Whether Gruvboy must reopen audio after a short power press |
| Volume level and volume buttons | 5 | Whether the driver resets volume when it opens |
| Sound in Onion after quitting | 5 | Confirms the audio server comes back |
| `clicktest.log` and `launch.log` | 6 | Exact timing data and any errors |

## Findings

### Toolchain

- The prebuilt [Miyoo Mini toolchain](https://github.com/shauninman/miyoomini-toolchain-buildroot-aarch64) runs in Docker on Apple Silicon. Colima provides Docker on macOS.
- It provides `arm-linux-gnueabihf-g++`, glibc 2.28 and SDL 1.2 in its sysroot.
- It also includes the vendor `mi_ao.h` headers, so a direct audio backend is possible if needed.
- Target flags: `-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`.

### SDL and audio

- **SDL 1.2 is the right choice.** It is what Onion's own tools use, and C++17 works with it. SDL2 ports exist but add dependencies without a clear benefit here.
- **Onion's stock SDL has no `miao` driver.** Its audio goes through `/dev/dsp`, emulated by a `libpadsp.so` preload that talks to `audioserver`, which is the path that adds latency.
- **Other SDL 1.2 builds contain a `miao` driver** that calls `MI_AO_SendFrame` directly. Gmu, GnGeo, PCSX and LittleGPTracker all ship one, so Gruvboy must bundle its own copy.
- **Exclusive audio access needs one script.** `/mnt/SDCARD/.tmp_update/script/stop_audioserver.sh` stops the audio server and restores the volume. Gmu and ChooChooTracker use it.
- **Volume may reset when audio opens.** ChooChooTracker's code notes this for its SDL2 driver, so we need to check it on our driver.
- **LittleGPTracker is not a latency benchmark.** It uses 1024-sample buffers with 8 pre-buffers, and is also built on SDL 1.2.

### App packaging

An Onion app is a folder at `/mnt/SDCARD/App/<name>/` containing `config.json`, `launch.sh` and an icon. It appears in the Apps list.

The launch script should:
- Set the CPU governor to `performance`.
- Run `stop_audioserver.sh`.
- Create `/tmp/stay_awake` to prevent Onion's hibernate timer, and remove it on exit.
- Clear `LD_PRELOAD` so that `libpadsp.so` is not loaded.

### Input

Buttons appear as keyboard keys on `/dev/input/event0`:

| Button | Key | Button | Key |
|---|---|---|---|
| A | Space | L | E |
| B | Left Ctrl | R | T |
| X | Left Shift | L2 | Tab |
| Y | Left Alt | R2 | Backspace |
| Start | Enter | Select | Right Ctrl |
| Menu | Esc | Power | Power |

- The Mini has **L2 and R2**, which the plan did not account for.
- Reading `/dev/input/event0` directly avoids waiting on the UI loop, and its timestamps let the app measure input delay. Onion's key monitor reads the same device, and it keeps working as long as we don't grab it.

### Onion's key monitor

These are read from `keymon` source and need checking on the device:

| Input | Effect while an app runs |
|---|---|
| Select + Start + L or R | **Changes the CPU clock. Gruvboy must never use this combination** |
| Menu, single press | No action found for app mode, so it may be free for Gruvboy |
| Power, short press | Suspends processes with `SIGSTOP`, which will stop audio |
| Power, held | Shuts down and sends `SIGTERM`, so Gruvboy should autosave on that signal |

### Licences

| Project | Licence | Use |
|---|---|---|
| LittleGPTracker | GPLv3 | Reference only |
| ChooChooTracker | MIT | Reference only |
| Onion | GPLv3 | Reference only |
| Miyoo toolchain | No licence file found in the Docker repo | Downloaded at image build time, not committed or shipped |
| miyoo-mini-audio-latency-reduction | No licence found | Not used |
| Bundled `libSDL-1.2.so.0` | SDL 1.2 is LGPL | Downloaded at build time, not committed |

**Open question before any public release:** the bundled SDL is a binary from Onion's Gmu package. LGPL distribution requires making the matching source available, so we need to identify that source or build the driver ourselves.

## The click test

`tools/clicktest/main.cpp` plays a sharp click when a face button is pressed. Settings change on the device, so one SD card install covers the whole test.

| Control | Function |
|---|---|
| A, B, X, Y | Play the click |
| L / R | Smaller / larger audio buffer |
| Left / Right | Switch between 48000 and 44100 Hz |
| Up | Toggle UI stress, which redraws as fast as possible |
| Down | Switch input between direct (`EVDEV`) and `SDL` |
| Start | Toggle a steady tone, which makes dropouts easy to hear |
| Select + Start | Quit |

On-screen values:
- **PRESS>AUDIO:** time from button press to the next audio callback. This excludes audio buffered after the callback, so true latency is higher.
- **CB GAP MAX:** longest time between audio callbacks over the last second.
- **CB RENDER MAX:** longest time spent generating one buffer.
- **LATE CALLBACKS:** callbacks arriving over 1.5 times later than expected, a likely sign of dropouts.
- **RT:** whether the audio thread obtained real-time priority.

### Building

```sh
make selftest                  # Desktop build and automated checks
make image                     # Build the toolchain image, once only
make sd SD=/Volumes/YOUR_CARD  # Cross-compile, package and install
```

Docker commands need Colima running: `colima start`.

## On-device checklist

### 1. Install

1. Back up the SD card. The install only writes to `App/GruvboyClickTest`, but the card holds your Onion setup.
2. Put the card in your Mac and run `make sd SD=/Volumes/YOUR_CARD`, using the card's real name.
3. Eject it, put it back in the Miyoo and turn it on.

Remove the test later by deleting `App/GruvboyClickTest`.

### 2. First launch

**The click is loud and volume may reset, so keep headphones away from your ears until you've heard it.**

1. Open **Apps > Gruvboy Click Test**.
2. Confirm the screen shows `DRIVER MIAO`, `INPUT EVDEV` and `RT YES`, and press a face button.

If the screen stays black, or the app exits or freezes, hold Power to shut down if needed, then skip to step 6 and send the logs.

### 3. Find the smallest stable buffer

Using `L` and `R` to change the buffer, start at 128 and work up:

1. Turn the tone on with Start, and listen for one minute while tapping pads.
2. Press Up for UI stress and listen for another minute.
3. The buffer passes if there are no crackles and `LATE CALLBACKS` stays at 0 or nearly so.

Note whether the `BUFFER` line's second number matches what you asked for, since the driver may change it.

### 4. Measure latency

At the smallest stable buffer, with the tone and UI stress off, in a quiet room:

1. Start a voice recorder on your phone, with its mic close to the Miyoo's speaker and button.
2. Press A firmly about 15 times, about one second apart. Each press should record the button's mechanical click, then the sound.
3. Press Down to switch to `SDL` input and repeat, then switch back.
4. Send me the recording. I can measure the gaps, or you can read them in a tool like Audacity.

This measurement includes small errors from the button's travel and the mic, so treat it as accurate to a few milliseconds.

### 5. Judge it and check Onion's behaviour

- **Feel:** tap a rhythm. Does it feel like an instrument, with headphones and with the speaker?
- **Menu:** press it once, then hold it. Does anything happen?
- **Power:** press it briefly to sleep, then wake. Does audio come back?
- **Volume:** do the volume buttons work? Was the level the same as before launch?
- **Exit:** does Select + Start return to Onion, and does sound still work in the menu or a game afterwards?

### 6. Send back

- The two logs from `App/GruvboyClickTest/`: `clicktest.log` and `launch.log`.
- The phone recording.
- Your results from steps 3 and 5.

## Gate

Measured press-to-sound latency decides what happens next:

| Latency | Next step |
|---|---|
| 20 ms or less | Start Phase 1 |
| 20 to 30 ms | Decide together, based on feel |
| Above 30 ms | Try a direct `MI_AO` backend using the toolchain's headers |
