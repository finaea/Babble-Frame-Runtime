<p align="center"><img src="runtime/resources/icons/bfr_logo_128.png" alt="Babble Frame Runtime" width="128"></p>

# Babble Frame Runtime

Eye and mouth expression tracking that runs on the Steam Frame itself.

Babble Frame Runtime reads the Steam Frame's eye cameras, and a wired Babble face tracker for the mouth, and runs a set of optimized neural networks to output expression values (eye openness, widening, pupil dilation; jaw, lips, cheeks and
tongue), which it sends to VRCFaceTracking on your PC.

It is installed as a SteamVR driver for settings integration. The driver starts the runtime as a separate process and restarts it if it stops,
so a problem in the runtime never affects SteamVR.

## About this fork

This is a fork of [Project-Babble/Babble-Frame-Runtime](https://github.com/Project-Babble/Babble-Frame-Runtime), currently
based on Babble's V1.2 Release. It keeps Babble's models and outputs, and adds:

- **Squint** from eyecam's live engine (sasaken1102r/frameeyeosc, MIT, in `third_party/eyecam`), run on the runtime's
  own eye frames, with eyecam's guided squint calibration on the Babble tab. An existing `~/.config/eyecam/calib.json`
  is imported on first start.
- **Gaze smoothing** from frameeyeosc (One Euro filter, deadzone, gaze held while the eyes are shut), in four levels:
  Off (Valve's raw gaze), Low (the default), Medium and High.
- **Blink hold**, off by default: keeps a short blink closed long enough for other players to see it.
- **Manual calibration** in the Baballonia style (a minimum and a maximum per value) for eyelids, widen, squint, pupils,
  gaze and the 45 mouth shapes. The eyes keep the runtime's automatic calibration underneath; the mouth uses the manual
  ranges by default.
- **Any USB webcam as the mouth camera:** supports any standard UVC cameras, bulk or isochronous, in MJPEG, YUY2, UYVY, NV12 or
  grey, with a resolution and frame rate picker (tested working with a GC0308 and Logitech C615 webcam).
- **Any MJPEG stream over HTTP as the mouth camera:** supports Stream URL mode that reads any stream that carries JPEG frames, multipart or plain
  back-to-back JPEGs, chunked or not. If a stream requires a login that takes `username` and `password`, they can be configured under
  `sources.face` in `settings.json` (Stream parsing and login contributed by [grzesjam](https://github.com/grzesjam)).
- **Brightness and tilt** on the Mouth page's Image tab: brightness for a camera that is too dark or too bright, and a
  tilt of up to 45° either way for a camera that isn't square to the face. The tab shows exactly what the mouth model
  sees.
- **ESP32 serial boards:** Supports ETVR serial firmware (such as `babble_wrooms_s3_usb` on the ventiFBT mouth tracker,
  USB `303a:1001`) for mouth tracking by reading over raw USB. **Untested**
- **Mouth from Baballonia on the PC:** set Camera to PC App on the Mouth page. The runtime then opens no mouth camera
  and runs no mouth model, but with Mouth Tracking on it still tells the VRCFaceTracking module to claim face tracking,
  so the module takes the mouth from Baballonia and the eyes from the headset.

Updates come from this fork's releases. Its versions keep Babble's format, so the build and the updater accept them,
with the release number in its own range: the hundreds are the Babble release it is based on, the last two digits the
fork's build. V1.2 Release 101 is the first fork build on Babble's V1.2 Release, and V1.2 Release 201 would be the
first on a Babble V1.2 Release 2. Official Babble releases never reach these numbers, and a fork release always counts
as newer than the Babble release it is based on.

## Features

- **Eye expressions:** openness, widen and pupil dilation per eye.
- **Mouth tracking:** 43 blendshapes from a wired Babble face tracker plugged into the headset.
- **Gaze:** each eye's own gaze from the headset's eye service, or SteamVR's combined gaze.
- **Presence:** tracking pauses while the headset is off the head (proximity sensor), and the cameras turn off.
- **SteamVR Settings:** an Expression Tracking section to turn mouth and eye tracking on and off and set the eye
  tracking rate.
- **Outputs:**
  - OSC in the Baballonia format for VRCFaceTracking.
  - A plugin interface for other outputs.
- **Dataset capture:** an optional guided recording app, installed from the overlay's Diagnostics page. 

Work in progress. The mouth and eye models are prototypes; the runtime keeps models swappable so better ones can be
dropped in.

## Install

On the Steam Frame, switch to Desktop Mode, open Konsole and paste:

```
curl -fsSL https://raw.githubusercontent.com/finaea/Babble-Frame-Runtime/main/tools/get.sh | BABBLE_REPO=finaea/Babble-Frame-Runtime bash
```

It downloads the newest release, checks it against the release's SHA-256 checksum, installs it in your home folder and restarts SteamVR. Its settings are in SteamVR Settings, under Expression Tracking. The Babble tab in the SteamVR dashboard is on after you install, and updates appear there next to the version. To hide the tab, turn off Show App Window.
To uninstall, run `~/.local/share/babble-frame-runtime/uninstall.sh`.

## VRCFT Module

Until further notice, this [VRCFT Module](https://cdn.discordapp.com/attachments/1279163592440283240/1561139947597992016/VRCFaceTracking.Baballonia.zip?ex=6ade7ee0&is=6add2d60&hm=92b516967feec073c70adf231a947d09ca4caf820d78a6a73157607361b80b13&) is required 

## Building from source

1. Get the source on a PC and copy it to the headset.
2. Build ExecuTorch once, then build the driver, on the headset. The squint engine is Rust, so the build also needs a
   stable Rust toolchain (`cargo`); the build fetches its one crate (`libc`).
3. Register the driver with `vrpathreg.sh`, then restart SteamVR.


## Build profiles

`-DBABBLE_PROFILE=release` (what releases ship) builds the USB camera or a stream URL as the mouth; it leaves out the
Frame's front-port mouth cameras and their camera arbiter, the visualizer launcher, the camera-stream switch and the
unused models. Every profile has all three outputs: VRCFaceTracking, VRChat Native and Babble OSC. The default, `dev`, builds everything including unfinished features. 
## License

Licensed under the [GNU General Public License v3.0](LICENSE). Third-party code in `third_party/` keeps its own license.
