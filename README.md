# Sculpt

An instrument plugin for Akai MPC standalone hardware, by Lightlike.

_Describe what the plugin does here. This file is included in the distributable zip._

## Build

```
python tools/build.py plugins/Sculpt
python tools/package.py plugins/Sculpt
```

## Project layout

- `plugin.json` - manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/Sculpt.cpp` - the plugin source (parameters, MIDI handling, DSP).
- `ui.json` - which parameters appear on the MPC screen, in which order.
- `build/` - build output (the deployable `Lightlike - VST - Sculpt` folder lives here).
- `dist/` - zips produced by `package.py`.
