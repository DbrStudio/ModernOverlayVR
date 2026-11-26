"""Shared display settings for EDMC, its renderer and the Steam launcher."""
import json
import math
import os
from pathlib import Path

DEFAULT = dict(enabled=True, anchor="room", shape="curved", resolution="1280x960",
               width=1.5, distance=1.3, arc=70, x=0, y=0)
CHOICES = dict(anchor=("room", "head"), shape=("curved", "flat"),
               resolution=("640x480", "960x720", "1280x960", "1920x1440"))
LIMITS = dict(width=(0.1, 5), distance=(0.1, 10), arc=(5, 180), x=(-5, 5), y=(-5, 5))


def number(key, raw):
    low, high = LIMITS[key]
    value = float(str(raw).replace(",", "."))
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"{key}: {low} … {high}")
    return value


def validate(values):
    settings = {**DEFAULT, **values}
    for key, choices in CHOICES.items():
        if settings[key] not in choices:
            raise ValueError(f"{key}: {choices}")
    if type(settings["enabled"]) is not bool:
        raise ValueError("enabled")
    for key in LIMITS:
        settings[key] = number(key, settings[key])
    return settings


def read(directory):
    try:
        return validate(json.loads((directory / "display.json").read_text()))
    except FileNotFoundError:
        return DEFAULT.copy()


def save(directory, settings):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f".display-{os.getpid()}.tmp"
    path.write_text(json.dumps(validate(settings), indent=2))
    path.replace(directory / "display.json")


def environment(directory, settings):
    return {**{f"EDMO_{k.upper()}": str(settings[k]) for k in (*LIMITS, *CHOICES)},
            "EDMO_XR_CHANNEL": str(directory / ("frame-" + settings["resolution"] + ".shm"))}
