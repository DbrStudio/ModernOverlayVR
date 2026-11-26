"""EDMC entry points for ModernOverlay VR."""
import logging
import os
import subprocess
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

import myNotebook as nb
import vr_settings as vr

__version__ = "0.1.0"
NAME = "ModernOverlay VR"
LOG = logging.getLogger("EDMarketConnector.ModernOverlayVR")
LABELS = dict(anchor="Anchor", shape="Display shape", resolution="Resolution (pixels)",
              width="Width / arc length (m)", distance="Distance (m)", arc="Arc angle (°)",
              x="Horizontal offset (m)", y="Vertical offset (m)")
NAMES = {"room": "Room-locked (follows Elite recenter)", "head": "Head-locked", "curved": "Curved", "flat": "Flat"}
process = None
fields = {}
cancel_preview = None
folder = data = settings = None


def stop_renderer():
    global process
    if process is None:
        return
    # Closing the pipe also reaches a host renderer launched through flatpak-spawn.
    process.stdin.close()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    process = None


def start_renderer():
    global process
    if not settings["enabled"]:
        return
    env = vr.environment(data, settings)
    env["EDMO_EDMC_MANAGED"] = "1"
    cmd = ["/usr/bin/python3", "-u", str(folder / "renderer/modern.py")]
    if Path("/.flatpak-info").exists():
        cmd = ["flatpak-spawn", "--host", "--watch-bus",
               *(f"--env={k}={v}" for k, v in env.items()), *cmd]
    with (data / "renderer.log").open("a") as log:
        # flatpak-spawn forwards cwd to the host; EDMC itself runs under /app.
        process = subprocess.Popen(cmd, cwd=folder, stdin=subprocess.PIPE, stdout=log, stderr=log,
                                   env={**os.environ, **env})


def plugin_start3(plugin_dir):
    global folder, data, settings
    folder = Path(plugin_dir).resolve()
    data = folder.parent.parent / "edmo-vr"
    settings = vr.read(data)
    vr.save(data, settings)
    try:
        start_renderer()
    except OSError:
        LOG.exception("Could not start the renderer")
    return NAME


def plugin_stop():
    stop_renderer()


def numeric_controls(frame, row, key, text, changed):
    low, high = vr.LIMITS[key]
    step = 1 if key == "arc" else 0.01
    position = tk.DoubleVar(frame, vr.number(key, text.get()))

    def drag(raw):
        value = round(max(low, min(high, round(float(raw) / step) * step)), 2)
        text.set(f"{value:g}")
        changed(key, value)

    def edit(*_):
        try:
            position.set(vr.number(key, text.get()))
        except ValueError:
            pass  # Allow intermediate input such as a minus sign.

    text.trace_add("write", edit)
    scale = ttk.Scale(frame, from_=low, to=high, variable=position, command=drag)
    scale.grid(row=row, column=1, sticky="ew", padx=8)
    ttk.Entry(frame, textvariable=text, width=10).grid(row=row, column=2, padx=8)


def plugin_prefs(parent, cmdr, is_beta):
    global cancel_preview
    if cancel_preview:
        cancel_preview()
    frame = nb.Frame(parent)
    frame.columnconfigure(1, weight=1)
    preview = tk.BooleanVar(frame, False)
    pending = {}
    timer = None

    def flush():
        nonlocal timer
        global settings
        timer = None
        updated = vr.validate({**settings, **pending})
        pending.clear()
        if updated != settings:
            vr.save(data, updated)
            settings = updated

    def cancel():
        nonlocal timer
        if timer is not None:
            frame.after_cancel(timer)
            timer = None
        pending.clear()

    def moved(key, value):
        nonlocal timer
        if preview.get():
            pending[key] = value
            if timer is None:
                timer = frame.after(200, flush)

    cancel_preview = cancel
    frame.bind("<Destroy>", lambda event: cancel() if event.widget is frame else None)
    fields.clear()
    fields["enabled"] = tk.BooleanVar(frame, settings["enabled"])
    nb.Checkbutton(frame, text="Start VR overlay with EDMC", variable=fields["enabled"]).grid(
        row=0, columnspan=2, sticky="w", padx=8, pady=6)
    for row, (key, label) in enumerate(LABELS.items(), 1):
        nb.Label(frame, text=label).grid(row=row, column=0, sticky="w", padx=8, pady=3)
        value = settings[key]
        var = fields[key] = tk.StringVar(frame, NAMES.get(value, str(value)))
        if key in vr.CHOICES:
            widget = ttk.Combobox(frame, state="readonly", textvariable=var,
                                  values=[NAMES.get(v,v) for v in vr.CHOICES[key]])
        else:
            numeric_controls(frame, row, key, var, moved)
            continue
        widget.grid(row=row, column=1, sticky="ew", padx=8)
    nb.Checkbutton(frame, text="Live preview while moving sliders", variable=preview,
                   command=cancel).grid(row=9, columnspan=3, sticky="w", padx=8)

    def renderer_status():
        return "Renderer running" if process and process.poll() is None else "Renderer stopped"

    status = tk.StringVar(frame, renderer_status())
    nb.Label(frame, textvariable=status).grid(row=10, column=0, sticky="w", padx=8, pady=8)

    def apply():
        apply_settings()
        status.set(renderer_status())

    nb.Button(frame, text="Apply", command=apply).grid(row=10, column=1, sticky="e", padx=8, pady=8)
    return frame


def apply_settings():
    global settings
    if cancel_preview:
        cancel_preview()
    if not fields:
        return
    reverse = {label: key for key, label in NAMES.items()}
    values = {k: reverse.get(v.get(), v.get()) for k,v in fields.items()}
    try:
        updated = vr.validate(values)
    except (TypeError, ValueError) as exc:
        messagebox.showerror(NAME, f"Invalid value: {exc}")
        return
    if updated != settings:
        vr.save(data, updated)
    restart = settings["enabled"] != updated["enabled"]
    settings = updated
    if restart or process is None or process.poll() is not None:
        stop_renderer()
        try:
            start_renderer()
        except OSError:
            LOG.exception("Could not start the renderer")


def prefs_changed(cmdr, is_beta):
    apply_settings()
