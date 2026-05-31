# SuperCollider (Supersonic) support in Sushi

Sushi can host a SuperCollider [`scsynth`](https://supercollider.github.io/) synthesis
engine via the embedded, **JUCE-free** [Supersonic](https://github.com/samaaron/supersonic)
core. Enable it at configure time:

```bash
cmake -DSUSHI_WITH_SUPERCOLLIDER=ON ...
```

This compiles the same source set that Supersonic's WebAssembly target uses
(scsynth server + common + UGen plugins + oscpack + tlsf), but natively, as a
static library (`supersonic_core`). No JUCE, no audio device backends and no UDP
server are pulled in — audio and OSC flow directly through Supersonic's
in-process ring buffer.

## How it works

The `supercollider` plugin is a normal Sushi processor on a track:

- Its audio output is scsynth's **master output bus (bus 0)**, copied into the
  track buffer. Track input is fed to scsynth's input bus, so `In.ar`-based
  effects work too.
- scsynth runs in **non-realtime (NRT) mode**: OSC commands are processed inline
  inside the audio callback, so no extra engine thread is needed.
- The scsynth control block size is pinned to Sushi's compile-time
  `SUSHI_AUDIO_BUFFER_SIZE`.

## Control surface

| Field / property | Purpose |
|------------------|---------|
| `path` (config)  | Path to a compiled SynthDef (`.scsyndef`), loaded via `/d_recv` at startup. |
| `source_code` (config) | OSC commands run once at startup, one per line (`#` = comment). |
| `osc` (property) | Send an arbitrary OSC command at runtime (gRPC/OSC `set_property`). |
| `synthdef` (property) | Load another `.scsyndef` file at runtime. |
| `status` (property, read-only) | Last engine/compile status. |

OSC commands are written as plain text, e.g.:

```
/s_new sine -1 0 0 freq 440 amp 0.2
/n_set 1000 freq 660
/n_free 1000
```

Argument types are inferred: integers → `int32`, values containing `.`/`e` →
`float`, anything else → `string`.

## Producing a `.scsyndef`

Sushi does not embed the SuperCollider *language* (sclang), so SynthDefs must be
compiled ahead of time. From the SuperCollider IDE / sclang:

```supercollider
SynthDef(\sine, { |out=0, freq=440, amp=0.2|
    Out.ar(out, SinOsc.ar(freq, 0, amp).dup)
}).writeDefFile("misc/config_files/supercollider/");
```

This writes `sine.scsyndef`, which the example `play_supercollider.json` loads.

## Limitations

- **Single instance per process.** scsynth keeps process-global state, so only
  one `supercollider` plugin can exist per Sushi instance; a second one fails to
  initialise.
- **Stereo in / stereo out.** The hosted World is built as stereo.
- **No sample file loading.** The core is built with `NO_LIBSNDFILE`, so
  `/b_allocRead` is unavailable. Buffers can still be created and filled over
  OSC (`/b_alloc`, `/b_setn`, …).
- **No GUI editor.**
- Bundle timetags are interpreted against an engine clock that starts at 0 and
  advances per block; use immediate messages or near-future bundles.
