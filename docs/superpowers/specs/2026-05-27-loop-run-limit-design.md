# Loop run-limit modes for the Macro Runner

**Date:** 2026-05-27
**Component:** `companion-app/macro_runner.py`
**Status:** Approved

## Goal

Let the user set how long a loop runs. Today a loop's **Mode** is only
"Repeat continuously" (forever until stopped) or "Run once". Add two bounded
modes: run for a wall-clock **time duration**, or run a fixed **number of
repetitions**, then auto-stop.

## Mode model

A loop has exactly one of four modes:

| Mode         | Behavior                                                        |
|--------------|-----------------------------------------------------------------|
| `once`       | Run one full pass through the steps, then stop. (existing)      |
| `continuous` | Repeat passes forever until Stop. (existing)                    |
| `count`      | Repeat passes until completed-pass count reaches N, then stop.  |
| `duration`   | Repeat passes until elapsed wall-clock time reaches the limit.  |

The termination check happens **at each iteration boundary** (after a full pass
through the steps completes), which gives "finish the current iteration"
behavior: a pass already in progress always runs to completion. The duration
limit is therefore a floor, not a hard cap — `duration` always runs at least
one full pass even if the limit is shorter than one pass.

The Stop button and global hotkey still interrupt instantly mid-step, with any
held key always released. That is independent of the mode and unchanged.

## UI / control layout

The current two-radio **Mode** row expands to four mutually-exclusive radios in
a single `QButtonGroup`, laid out on two rows to avoid excessive width:

```
Mode:  ( ) Run once   ( ) Repeat continuously
       ( ) Repeat [ 10 ]  times
       ( ) Repeat for [  5 ] [ Minutes v ]
```

- Count input: `QSpinBox`, range 1–100000.
- Duration input: `QSpinBox` (value, range 1–100000) + `QComboBox` unit
  selector with `Seconds` / `Minutes`.
- Each mode's input(s) are **enabled only when that mode's radio is selected**,
  greyed out otherwise, so the active input is always clear.

Chosen over a dropdown-plus-contextual-input because radios show all options at
a glance and match the existing radio-button style.

## Data model & persistence

`LoopCard.get_config()` replaces the legacy `"repeat": bool` field with:

```json
{
  "mode": "once" | "continuous" | "count" | "duration",
  "repeat_count": 10,
  "duration_value": 5,
  "duration_unit": "minutes"
}
```

`repeat_count`, `duration_value`, and `duration_unit` are always persisted
(whatever the inputs hold) so switching modes does not lose previously entered
values.

**Backward compatibility:** `set_config()` checks for `"mode"`. If absent (old
`macro_settings.json`), it derives the mode from the legacy `"repeat"` bool:
`true -> "continuous"`, `false -> "once"`. Existing saved loops load unchanged.

## Execution changes (`LoopPlayer`)

`LoopPlayer.__init__` takes the mode and its parameters instead of a single
`repeat` bool — e.g. `(steps, mode, repeat_count, duration_seconds)`. The
duration is pre-computed to seconds from value + unit by the caller.

`run()` loop, per iteration boundary:

- `once`: break after the first pass.
- `continuous`: never self-terminate.
- `count`: maintain a completed-pass counter; break when it reaches N.
- `duration`: record `time.monotonic()` before the first pass; after each pass,
  break if `monotonic() - start >= limit_seconds`.

Requires importing `time`.

## Edge cases

- Empty step list: existing "add at least one step first" guard applies.
- `count` with N = 1 behaves like `once`; both are allowed, no special-casing.
- `duration` shorter than one pass: still runs one full pass (see Mode model).
- Status bar message reflects the mode, e.g. `Playing: Loop 1 (for 5 min)`.

## Docs & testing

- Update README "PC Macro Runner" usage step 3 to describe all four modes.
- Manual verification:
  - Each mode plays and self-stops as specified (`count` stops after N passes;
    `duration` stops at the first boundary past the limit).
  - Loading an old settings file maps `repeat` true/false to
    `continuous`/`once`.
  - Selecting a mode enables only that mode's inputs.
  - Stop / hotkey still interrupts instantly mid-step in every mode.
