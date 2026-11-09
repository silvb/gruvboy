# Gruvboy
## Product & Technical Implementation Plan

### 1. Project Overview

Gruvboy is a standalone, sample-based four-track drum machine for the Miyoo Mini running OnionOS.

The concept is inspired by the Elektron Digitakt, but deliberately simplified around the Miyoo's hardware controls and small screen.

The application should function as a compact groovebox:
- Load drum samples from the SD card.
- Assign one sample to each of four tracks.
- Play samples immediately using the four front-facing buttons.
- Compose basic grooves using a 16-step sequencer.
- Add humanisation through velocity randomisation, probability and timing variation.
- Save work automatically and reliably.

The goal is to create something that feels like a musical instrument, not a general-purpose music tracker.

### 2. Core Design Principles

1. **Immediate playability:** An unmodified face-button press always triggers its track, on every page.
2. **Minimal interface:** Avoid deeply nested menus and unnecessary configuration.
3. **Reliable audio:** Playback and sequencing must remain stable and independent of UI rendering.
4. **SD-card-first:** Samples and projects should be accessible through the existing SD card.
5. **Hardware-aware:** Keep memory consumption and CPU usage appropriate for the Miyoo.
6. **Standalone application:** Do not fork or replace OnionOS.
7. **Few SD card swaps:** The target device has no Wi-Fi, so most development and testing must happen off-device.

### 3. Hardware & Platform

Target device:
- Miyoo Mini (no Wi-Fi). Deployment is by moving the SD card to the development Mac.

Development machine:
- macOS.

Operating environment:
- OnionOS v4.3.1-1.
- Existing Linux environment.
- SDL version to be determined in Phase 0.

Hardware capabilities:
- Four front-facing action buttons (digital, so they carry no velocity information).
- D-pad.
- L/R shoulder buttons.
- Start and Select.
- Built-in display.
- Headphone output.
- SD card storage.

The application must use the existing audio output. No external hardware should be necessary.

#### Assumptions to verify in Phase 0

These come from memory and must not be treated as fact until confirmed:
- The stock environment is built around a customised SDL 1.2, with SDL2 available through community ports.
- Audio passes through a vendor audio layer and Onion's audio server.
- Onion intercepts the Menu button.
- Apps live in `/mnt/SDCARD/App/<name>/`.
- The LittleGPTracker Miyoo build is a useful reference for audio and packaging.

### 4. Technical Architecture

| Component | Approach |
|---|---|
| Language | Restrained C++17: no exceptions or allocation on the real-time path |
| Platform layer | Thin audio, input and video interfaces; SDL version decided in Phase 0 |
| Audio output | SDL audio callback or direct vendor API, decided by latency measurements |
| Sample format | 16-bit PCM WAV, mono or stereo |
| Sequencer | Custom, sample-accurate, runs inside the audio callback |
| Project format | Versioned JSON, using one vendored library |
| Targets | Miyoo Mini (cross-compiled), macOS (development), headless test build |

#### Application structure

```text
Gruvboy
├── core/                  Portable, no platform dependencies
│   ├── Engine             Called once per audio buffer
│   ├── Sequencer          Frame counter, patterns, swing, humanisation
│   ├── Voices             One active voice per track, plus preview
│   ├── Mixer              Track levels, mute/solo, soft limiter
│   └── Commands           Lock-free queues between threads
│
├── ui/
│   ├── GridPage
│   ├── SoundPage
│   └── HumanisePage
│
├── storage/
│   ├── WavLoader          Decode and resample outside the audio thread
│   ├── SampleLibrary
│   └── Project            Autosave with atomic writes
│
├── platform/
│   ├── miyoo/
│   └── desktop/
│
└── tests/
    └── Offline rendering to WAV and automated engine tests
```

#### Timing

The sequencer runs inside the audio callback and counts sample frames, rather than using a separate clock thread. This makes timing sample-accurate and independent of the UI. Swing and timing humanisation are frame offsets within this system.

Timing humanisation can move a hit earlier, so sequenced events are planned ahead by the maximum offset (around 10 ms).

#### Threads

| Thread | Responsibility |
|---|---|
| Audio | Sequencer, voices, mixing. Highest available priority |
| Input | Polls buttons independently of rendering and sends pad triggers straight to the audio queue |
| UI | Rendering and page logic. May drop frames, never affects audio |
| Loader | Sample loading and project saves |

#### Real-time rules

- No allocation, locks, logging or filesystem access in the audio callback.
- Communicate between threads through lock-free queues and atomics.
- A replaced sample is not freed until the audio thread confirms that no voice uses it.
- Pattern changes and transport stops must not interrupt the audio callback.

#### Voices and mixing

- Each track has one active voice: retriggering a track cuts its previous sound.
- A cut voice fades out over a few milliseconds to avoid clicks, so each track needs one extra slot for the fading tail.
- A separate voice handles sample preview.
- Velocity uses a 0–127 scale with a squared amplitude curve.
- The master output passes through a soft limiter, since four stacked hits would otherwise clip.

### 5. Sample Management

```text
/mnt/SDCARD/App/Gruvboy/      Application, launch script, logs
/mnt/SDCARD/Samples/
  Kicks/
  Snares/
  Hats/
  Percussion/
/mnt/SDCARD/Gruvboy/Projects/
  Default/
    project.json
```

Requirements:
- Support 16-bit PCM WAV, mono and stereo.
- Resample to the engine's sample rate on load.
- Load samples fully into RAM, never streaming during playback.
- Limit each sample to 30 seconds. Memory is otherwise not a concern, since 30 seconds of stereo audio is about 5 MB.
- Store sample paths in the project, relative to the Samples folder.
- A missing or invalid sample leaves its track silent and visibly marked. Playback and the rest of the project continue normally.

### 6. Sequencer

Specifications:
- Four tracks.
- 16 steps per pattern.
- 16 patterns per project.
- Adjustable BPM and swing.
- Per-step active state, velocity (default 100) and probability (default 100%).
- Per-track volume, mute and solo.
- Pattern changes are queued and take effect at the end of the bar.

Patterns and tracks are plain fixed-size data, so copying or editing them never requires allocation.

### 7. Humanisation

Humanisation is a core feature, not an afterthought.

| Parameter | Scope | Edited on |
|---|---|---|
| Velocity | Per step | Grid |
| Probability | Per step | Grid |
| Velocity randomisation | Per track | Humanise |
| Timing randomisation | Per track, up to around ±10 ms | Humanise |
| Swing | Global | Humanise |

**Velocity randomisation:** A base velocity of 80 with ±15 randomisation produces a velocity from 65 to 95, clamped to the valid range.

**Probability:** Each active step rolls against its probability every time it is reached.

**Randomness:** Use one continuously evolving random sequence with a small, fast generator. Random values must not repeat on every pattern loop.

**Swing:** Delay alternating 16th-note steps, from 50% (straight) to around 75%.

**Live pads:** The buttons have no velocity sensitivity, so pads use a fixed velocity of 100, modified by the track's velocity randomisation.

### 8. Interface & Controls

There are three pages: **Grid, Sound and Humanise**. The Grid shows all four tracks and 16 steps at once, along with pattern, BPM, playback position and live pad feedback.

Pads are mapped by physical position so that each pair can be rolled with a thumb:

| Button | Position | Track | Suggested use |
|---|---|---|---|
| B | Bottom | 1 | Kick |
| A | Right | 2 | Snare |
| Y | Left | 3 | Closed hat |
| X | Top | 4 | Percussion or open hat |

Global controls:

| Control | Function |
|---|---|
| Face button | Trigger its track |
| R + face button | Mute or unmute that track |
| L + R + face button | Solo or unsolo that track |
| Start | Play / Stop |
| L + Start | Toggle live recording (from version 0.2) |
| Select (tap) | Next page |
| Select (hold) + up/down | Change BPM |
| Select (hold) + left/right | Queue the previous or next pattern |
| Select + Start | Save and quit |

Modified button presses do not trigger the pad sound. Select acts as a page change on release, and only if the D-pad was not used while it was held.

Grid page:

| Control | Function |
|---|---|
| D-pad | Move the cursor |
| L + face button | Toggle that track's step in the cursor's column |
| R + up/down | Adjust velocity of the step under the cursor |
| R + left/right | Adjust probability of the step under the cursor |

Sound page:

| Control | Function |
|---|---|
| Up/down | Move through the file list |
| Right / left | Enter folder / go back |
| R (tap) | Preview the highlighted sample |
| L + face button | Assign the highlighted sample to that track |

The Sound page also shows per-track volume, adjusted with L + up/down for the most recently played track.

Humanise page:

| Control | Function |
|---|---|
| Up/down | Select a parameter |
| Left/right | Change its value |

Humanise parameters apply to the most recently played track, except for swing.

### 9. Development Workflow

Each hardware test requires moving the SD card, so:
- Develop and test the engine on macOS. The core must run there unchanged.
- Test sequencer behaviour automatically by rendering offline to WAV and checking event timing.
- Use a single command to build and copy the app to a mounted SD card.
- Make test builds configurable on the device, for example through a config file or on-screen controls for buffer size, to avoid rebuilds.
- Write logs and crash information to the SD card from outside the audio thread.
- End each phase with a short on-device checklist for the developer to run. The coding agent cannot test on hardware.

### 10. Development Roadmap

#### Phase 0: Platform Investigation

1. Confirm each assumption listed in section 3.
2. Set up the cross-compilation toolchain on macOS.
3. Identify how Onion apps are packaged and launched.
4. Study how these projects handle audio and packaging:
   - Little Piggy Tracker: https://github.com/djdiskmachine/LittleGPTracker
   - ChooChooTracker: https://github.com/paiheulevrai/Choochootracker
5. Check both licences before reusing any code. Use the projects as technical references unless a licence is explicitly accepted.
6. Build a test app that plays a click when a button is pressed, with an adjustable audio buffer size and on-screen underrun count.
7. Measure press-to-sound latency by recording the button's mechanical click and the audio output together with a phone, then measuring the gap between them.

**Go/no-go gate:**
- At or below 20 ms: proceed.
- Between 20 and 30 ms: decide together whether it feels playable.
- Above 30 ms: try the direct vendor audio API or other audio paths before building further.

**Deliverable:** A documented, working test app with verified audio and a measured latency.

#### Phase 1: Audio Engine

Implement WAV loading, resampling, per-track voices with declicking, mixer, soft limiter and live pad triggering. Samples are hard-coded or read from a config file at this stage.

**Acceptance criteria:**
- All four pads trigger their samples within the Phase 0 latency target.
- Different tracks overlap, and retriggering a track cuts it without a click.
- Four hits at full velocity do not clip harshly.
- No underruns during ten minutes of continuous pad playing.

#### Phase 2: Sequencer & Grid

Implement the sample-accurate sequencer, the Grid page, step editing, per-step velocity and probability, BPM, pattern switching, mute/solo and project autosave.

**Acceptance criteria:**
- Offline tests confirm that steps land on the exact expected frames.
- Tempo holds over ten minutes of playback on the device.
- UI load does not affect playback.
- All 16 patterns survive a restart, and a power loss during saving cannot corrupt the project.

#### Phase 3: Humanisation & Version 0.1

Implement swing, velocity randomisation, the Humanise page, the Sound page with the minimal sample picker, and per-track volume.

**Acceptance criteria:**
- Humanisation is musically useful, and changing parameters does not interrupt playback.
- A missing sample does not stop the app or the sequencer.
- All version 0.1 features in section 11 work on the device.

#### Phase 4: Live Performance (Version 0.2)

Implement live recording of pad presses, quantised to the nearest step and compensated for latency, along with timing humanisation.

**Acceptance criteria:**
- Pad presses recorded while the pattern plays land on the intended steps.
- Timing randomisation adds feel without sounding unstable.

#### Phase 5: Samples & Projects

Implement multiple projects, including creating, opening and copying them, plus a fuller sample browser and missing-sample relinking.

#### Phase 6: Polish & Performance

Implement improved UI feedback, performance testing, and memory and CPU optimisation.

### 11. Version 0.1 Scope

- Four sample slots, loaded from WAV files on the SD card.
- Minimal sample picker with preview, using the Sound page controls.
- Four immediately playable pads.
- 16-step sequencer with 16 patterns.
- BPM and swing.
- Per-step velocity and probability.
- Per-track velocity randomisation.
- Per-track volume, mute and solo.
- A single autosaved project.
- Stable headphone output.

### 12. Out of Scope

Excluded from version 0.1, and possible later:
- Pattern chaining, variations and fills.
- Sample start/end points and pitch adjustment.

Excluded from the project:
- Replacing or modifying OnionOS.
- Building a general-purpose tracker.
- USB audio interfaces.
- Audio recording. Live recording of pad presses as sequencer steps is in scope.
- MIDI sequencing.
- Synthesis and complex effects processing.
- Multi-track arrangement and song editing.

### 13. Instructions for the Coding Agent

Work incrementally.

Start with Phase 0 and establish a working development environment before making architectural commitments.

Before implementing each phase:
1. Investigate relevant existing code and platform constraints.
2. Propose a small implementation plan.
3. Identify technical risks.
4. Implement a vertical slice.
5. Test everything possible off-device, then provide a short on-device checklist for the developer.
6. Document findings and remaining limitations.

Prefer small, testable components over a large initial implementation.

Do not assume desktop Linux or SDL behaviour translates directly to OnionOS.

Prioritise reliable audio playback and responsive live pad triggering over visual polish.

The final result should feel like a small, immediate hardware drum machine: load samples, press pads, build grooves and play.
