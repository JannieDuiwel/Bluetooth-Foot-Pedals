"""Pure, GUI-free helpers for Macro Runner loop run-limit logic.

Kept separate from macro_runner.py (which imports PyQt6 and `keyboard`) so the
termination logic and legacy-config mapping are unit-testable without a display
or keyboard hook.
"""

VALID_MODES = ("once", "continuous", "count", "duration")
VALID_UNITS = ("seconds", "minutes")

DEFAULT_MODE = "continuous"
DEFAULT_REPEAT_COUNT = 10
DEFAULT_DURATION_VALUE = 5
DEFAULT_DURATION_UNIT = "minutes"


def duration_to_seconds(value, unit):
    """Convert a duration value + unit to seconds.

    'minutes' multiplies by 60; anything else is treated as seconds.
    """
    if unit == "minutes":
        return value * 60
    return value


def loop_should_continue(mode, completed_passes, repeat_count,
                         elapsed_seconds, duration_seconds):
    """Decide whether to run another pass, evaluated AFTER a full pass completes.

    once       -> never (a single pass only)
    continuous -> always
    count      -> until completed_passes reaches repeat_count
    duration   -> until elapsed_seconds reaches duration_seconds
    unknown    -> stop (safe default)
    """
    if mode == "continuous":
        return True
    if mode == "count":
        return completed_passes < repeat_count
    if mode == "duration":
        return elapsed_seconds < duration_seconds
    # "once" and any unrecognised mode
    return False


def normalize_mode_config(cfg):
    """Return {mode, repeat_count, duration_value, duration_unit} from a loop
    config dict.

    Maps the legacy boolean 'repeat' field onto the new modes and fills defaults
    for any missing or invalid values, so old macro_settings.json files load
    unchanged.
    """
    mode = cfg.get("mode")
    if mode not in VALID_MODES:
        # Legacy config: derive from the old 'repeat' bool (default True).
        mode = "continuous" if cfg.get("repeat", True) else "once"
    unit = cfg.get("duration_unit", DEFAULT_DURATION_UNIT)
    if unit not in VALID_UNITS:
        unit = DEFAULT_DURATION_UNIT
    return {
        "mode": mode,
        "repeat_count": cfg.get("repeat_count", DEFAULT_REPEAT_COUNT),
        "duration_value": cfg.get("duration_value", DEFAULT_DURATION_VALUE),
        "duration_unit": unit,
    }
