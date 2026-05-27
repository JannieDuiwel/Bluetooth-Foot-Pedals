from loop_logic import (
    duration_to_seconds,
    loop_should_continue,
    normalize_mode_config,
)


# --- duration_to_seconds ---

def test_duration_seconds_unit():
    assert duration_to_seconds(30, "seconds") == 30


def test_duration_minutes_unit():
    assert duration_to_seconds(5, "minutes") == 300


def test_duration_unknown_unit_treated_as_seconds():
    assert duration_to_seconds(10, "bogus") == 10


# --- loop_should_continue (evaluated AFTER a full pass completes) ---

def test_once_never_continues():
    assert loop_should_continue("once", 1, 10, 0.0, 0.0) is False


def test_continuous_always_continues():
    assert loop_should_continue("continuous", 999, 0, 0.0, 0.0) is True


def test_count_continues_until_reached():
    assert loop_should_continue("count", 1, 3, 0.0, 0.0) is True
    assert loop_should_continue("count", 2, 3, 0.0, 0.0) is True
    assert loop_should_continue("count", 3, 3, 0.0, 0.0) is False


def test_duration_continues_until_limit():
    assert loop_should_continue("duration", 1, 0, 4.0, 5.0) is True
    assert loop_should_continue("duration", 1, 0, 5.0, 5.0) is False
    assert loop_should_continue("duration", 1, 0, 6.0, 5.0) is False


def test_unknown_mode_stops():
    assert loop_should_continue("bogus", 1, 10, 0.0, 0.0) is False


# --- normalize_mode_config ---

def test_legacy_repeat_true_to_continuous():
    assert normalize_mode_config({"repeat": True})["mode"] == "continuous"


def test_legacy_repeat_false_to_once():
    assert normalize_mode_config({"repeat": False})["mode"] == "once"


def test_legacy_missing_repeat_defaults_continuous():
    assert normalize_mode_config({})["mode"] == "continuous"


def test_explicit_mode_preserved():
    out = normalize_mode_config({"mode": "count", "repeat_count": 7})
    assert out["mode"] == "count"
    assert out["repeat_count"] == 7


def test_defaults_filled():
    out = normalize_mode_config({"mode": "duration"})
    assert out["repeat_count"] == 10
    assert out["duration_value"] == 5
    assert out["duration_unit"] == "minutes"


def test_invalid_mode_falls_back_to_legacy():
    out = normalize_mode_config({"mode": "bogus", "repeat": False})
    assert out["mode"] == "once"


def test_invalid_unit_falls_back_to_minutes():
    out = normalize_mode_config({"mode": "duration", "duration_unit": "hours"})
    assert out["duration_unit"] == "minutes"
