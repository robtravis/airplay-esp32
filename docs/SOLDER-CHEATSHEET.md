# Vibe Radio — PCM5102A → T-Embed solder cheat sheet

For the LilyGO T-Embed ESP32-S3 build. The earlier sheet in the Arduino radio repo
is for the **T-Display-S3** (GPIO 18/17/21) — different pins, do not follow it here.

## Module jumpers (H / L pads)

| Jumper | Setting | Why |
|--------|---------|-----|
| FMT  | L | I2S format (not left-justified) |
| DEMP | L | De-emphasis off |
| FLT  | L | Normal latency filter |
| XSMT | H | Un-mute. **Low here means silence with everything else correct.** |

Bridging the pads and wiring the pins do the same thing — pick one.

## Wiring

| PCM5102A | T-Embed | Colour | Firmware key |
|----------|---------|--------|--------------|
| VCC  | 3V3 | red    | — |
| GND  | GND | black  | — |
| BCK  | **GPIO 38** | yellow | `CONFIG_I2S_BCK_IO` |
| LCK / WS | **GPIO 40** | white | `CONFIG_I2S_WS_IO` |
| DIN  | **GPIO 41** | green  | `CONFIG_I2S_DO_IO` |
| FMT  | GND | — | — |
| XSMT | 3V3 | — | — |
| SCK  | GND | — | `CONFIG_I2S_SCK_IO=-1` |

**VCC to 3V3, not 5V.** The module's regulator tolerates 5V, but nothing here needs
it and it puts 5V inside a 3.3V enclosure.

**SCK to GND.** The ESP32 sends no master clock — the PCM5102A runs its own internal
PLL, and grounding SCK is what selects that mode. Many breakouts ground it already;
if yours has an SCK pad floating and you get no audio, this is the first thing to
check.

Those GPIO numbers must match `sdkconfig.defaults.tembed`. If you wire differently,
change the config rather than the wires — the pins are configuration, not code, and
a rebuild is two minutes.

## Analog side

Keep the L / R / GND output run **short, twisted, and away from** the ribbon or
digital bundle, the display's SPI (GPIO 11/12/13) and the LED ring (GPIO 42/45).
Those are hard square waves at close range and the analog output is the only
noise-sensitive wiring in the box.

## Soldering

- Tin the pads before placing pins
- Flux pen before the iron touches anything
- Thin solder, 0.5mm
- Tin the tip lightly before each joint
- Touch → melt → remove quickly
- Inspect for bridges with a magnifier, especially BCK/DIN if they are adjacent
- Stranded silicone frays: tin or ferrule every end, or one stray strand bridges
- Hot glue over the joints once tested — with flying leads, mechanical load lands
  on the pads, and a lifted pad on the T-Embed is a bad repair
- Leave a slack service loop so nothing is taut when the case closes

## Verify before closing the case

Power up and watch the serial log. Working looks like:

```
radio: stream format: 44100 Hz, 2 ch
radio: took I2S ownership (AirPlay playback task stopped)
radio: decoded 400 frames, 0 resync bytes, 0 write errors
```

Then confirm the **visualiser bars move** — that proves the whole path from decode to
I2S, since the bars are tapped from the same PCM that feeds the DAC.

## If it does not work

| Symptom | Where to look |
|---------|---------------|
| Silence, but log shows frames decoded and `0 write errors` | DAC side: XSMT high? DIN on 41? VCC present? |
| Silence, and no `decoded` lines at all | Network or stream, not wiring |
| Garbled or static | Grounding first. Then confirm BCK/WS are not swapped |
| One channel only | Analog side — a broken output lead or speaker joint |
| Very quiet | Volume: default is 100%, but NVS may hold a lower saved value |

Two firmware traps that present as wiring faults, both already fixed — do not chase
them in hardware:

- Two writers on one I2S channel garbled audio until the radio took exclusive
  ownership (`audio_output_stop()` before writing).
- Archive episodes start with a ~2MB ID3 tag which read as static until it was
  skipped.

See `NOTES.md` in the parent directory for the full list.
