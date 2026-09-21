# FxMultiHost 7-Path

Experimental Windows 11 x64 audio processor using the open-source FxSound `DfxDsp`
engine with seven independent WASAPI loopback paths.

## What it does

The intended topology is:

Windows application
  -> one VB-Matrix VAIO playback endpoint
  -> WASAPI loopback capture
  -> one independent FxSound DfxDsp instance
  -> one or more physical Windows playback devices

Default mapping:

1. VAIO1 -> Philips
2. VAIO2 -> HP
3. VAIO3 -> Azona
4. VAIO4 -> Philips + HP
5. VAIO5 -> Philips + Azona
6. VAIO6 -> HP + Azona
7. VAIO7 -> Philips + HP + Azona

The application uses seven independent DfxDsp objects. Each path processes its own
audio stream. For combination paths, the same processed stream is rendered to two or
three physical outputs in shared WASAPI mode.

## Important architecture detail

The program does NOT render processed audio back into the same VAIO endpoint it
captures. That would create an audio feedback loop.

Instead:

VAIO playback endpoint -> loopback capture -> FxSound DSP -> physical speaker endpoint(s)

This means VB-Matrix's direct VAIO-to-WIN output routes for these seven VAIOs should
be disabled while FxMultiHost is running. Otherwise you will hear both the original
and processed signal.

VB-Matrix can continue to provide the VAIO playback devices to applications. The
VB-Matrix VAIO driver supports Windows audio applications and up to eight channels
per VAIO.

## Current prototype limitations

- Windows 11 x64.
- Seven paths are fixed in `app/main.cpp`.
- Device matching is by case-insensitive substring:
  `VAIO1`, `Philips`, `HP`, `Azona`, etc.
- Input/output sample rate and channel count must match for a path. If they do not,
  that physical output is skipped.
- The prototype uses each path's input format and converts 16-bit FxSound DSP
  samples to/from common float32/16/32-bit WASAPI shared formats.
- There is no GUI yet.
- There is no per-app routing database inside FxMultiHost. Windows itself chooses
  which VAIO playback endpoint an application uses.
- The DSP is initialized with FxSound processing enabled and EQ enabled. A future
  version can add preset loading and a GUI.

## Recommended Windows/VB-Matrix configuration

1. Keep VAIO1..VAIO7 available as Windows playback devices.
2. Set the VAIO endpoints to Stereo and 48,000 Hz if possible.
3. Set Philips, HP and Azona playback devices to Stereo and 48,000 Hz if possible.
4. In VB-Matrix, disable direct VAIO -> Philips/HP/Azona routes for the seven paths.
5. Start FxMultiHost7.
6. In Windows 11:
   Settings -> System -> Sound -> Volume mixer.
7. Assign applications to the desired VAIO:
   - application -> VAIO1 = Philips
   - application -> VAIO2 = HP
   - application -> VAIO3 = Azona
   - application -> VAIO4 = Philips + HP
   - application -> VAIO5 = Philips + Azona
   - application -> VAIO6 = HP + Azona
   - application -> VAIO7 = Philips + HP + Azona

## Building without Visual Studio on your PC

This repository contains a GitHub Actions workflow.

1. Create a private GitHub repository.
2. Upload this entire project to it.
3. Open the repository's Actions tab.
4. Select `Build FxMultiHost 7-Path`.
5. Click `Run workflow`.
6. Wait for the Windows 2022 runner to compile the project.
7. Download the artifact named `FxMultiHost7-Windows-x64`.
8. Extract `FxMultiHost7.exe`.
9. Run it on Windows 11.

The workflow clones the official FxSound source at build time, so the package does
not contain a second copy of the upstream repository.

## First test

Start only one application with:

application -> VAIO1

and run FxMultiHost7.

You should see:

[PATH 1] VAIO... -> FxDSP -> Philips...

Then verify audio in the Philips speaker.

After that, test VAIO2 and VAIO3. Finally test the combination VAIO4..VAIO7.

If an endpoint is not found, the program prints the active Windows playback devices
so the matching strings can be corrected in `app/main.cpp`.

## Safety / rollback

FxMultiHost does not install a driver. It uses existing Windows WASAPI endpoints.

To stop processing, press ENTER in the console window or close the process. Then
restore your normal VB-Matrix VAIO routes.

Do not run FxMultiHost and a second processed return path for the same VAIO at the
same time until the routing is understood, because duplicate paths can cause
double audio or feedback.

## Licensing

The DfxDsp source is part of FxSound and is licensed under AGPL-3.0. See LICENSE.txt
and the official FxSound repository.
