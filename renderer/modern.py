"""Render the installed ModernOverlay broadcast into the active XR live panel."""
import asyncio
import json
import os
import sys
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QPainter, QPen
from channel import RESOLUTIONS, DEFAULT_CHANNEL, HEIGHT, WIDTH, Publisher

PLUGIN = Path(os.environ.get("EDMO_PLUGINS", Path(__file__).resolve().parents[2])) / "EDMCModernOverlay"
sys.dont_write_bytecode = True
sys.path.insert(0, str(PLUGIN))
from overlay_client.legacy_processor import process_legacy_payload
from overlay_client.legacy_store import LegacyItemStore
from overlay_client.vector_renderer import render_vector
from overlay_client.viewport_helper import BASE_HEIGHT, BASE_WIDTH


class Painter:
    def __init__(self, painter):
        self.p = painter

    def set_pen(self, color, width=None):
        self.p.setPen(QPen(QColor(str(color)), max(1, min(int(width or 1), 128))))

    def draw_line(self, x1, y1, x2, y2):
        self.p.drawLine(x1, y1, x2, y2)

    def draw_circle_marker(self, x, y, radius, color):
        self.set_pen(color)
        self.p.drawEllipse(QPoint(x, y), radius, radius)

    def draw_cross_marker(self, x, y, size, color):
        self.set_pen(color)
        self.p.drawLine(x-size, y, x+size, y)
        self.p.drawLine(x, y-size, x, y+size)

    def draw_text(self, x, y, text, color, *, text_size=None):
        font = QFont("DejaVu Sans")
        font.setPixelSize({"small": 14, "large": 22, "huge": 30}.get(text_size, 18))
        self.p.setFont(font)
        self.set_pen(color)
        self.p.drawText(x, y + self.p.fontMetrics().ascent(), str(text))

    def item(self, item):
        d = item.data
        if item.kind == "vector":
            self.p.setBrush(Qt.BrushStyle.NoBrush)
            render_vector(self, d, 1, 1)
        elif item.kind == "message":
            self.draw_text(d["x"], d["y"], d["text"], d["color"], text_size=d["size"])
        elif item.kind in ("circle", "rect"):
            self.set_pen(d["color"], d.get("thickness"))
            self.p.setBrush(QColor(str(d["fill"])))
            if item.kind == "circle":
                self.p.drawEllipse(QPoint(d["x"], d["y"]), d["radius"], d["radius"])
            else:
                self.p.drawRect(d["x"], d["y"], d["w"], d["h"])


def raster(store, width=WIDTH, height=HEIGHT):
    canvas = QImage(width, height, QImage.Format.Format_RGBA8888)
    canvas.fill(Qt.GlobalColor.transparent)
    with QPainter(canvas) as p:
        p.scale(width / BASE_WIDTH, height / BASE_HEIGHT)
        adapter = Painter(p)
        for item in store.values():
            try:
                adapter.item(item)
            except (OverflowError, TypeError, ValueError):
                continue
    return bytes(canvas.constBits())


class Scene:
    def __init__(self, publisher):
        self.publisher = publisher
        self.store = LegacyItemStore()
        self.timer = None

    def receive(self, line):
        try:
            payload = json.loads(line)
            if isinstance(payload, dict) and payload.get("event") == "LegacyOverlay":
                if process_legacy_payload(self.store, payload):
                    # Collapse bursts of PlanetPOI updates to a single paint.
                    if self.timer is None or self.timer.when() > asyncio.get_running_loop().time() + 0.017:
                        self.schedule(0.016)
        except (ValueError, TypeError, KeyError, OverflowError, RecursionError):
            pass

    def schedule(self, delay):
        if self.timer:
            self.timer.cancel()
        self.timer = asyncio.get_running_loop().call_later(delay, self.paint)

    def paint(self):
        if self.timer:
            self.timer.cancel()
            self.timer = None
        self.store.purge_expired(time.monotonic())
        self.publisher.publish(raster(self.store, self.publisher.width, self.publisher.height))
        expiry = min((item.expiry for item in self.store.values() if item.expiry is not None), default=None)
        if expiry is not None:
            self.schedule(max(0, expiry - time.monotonic()))

    def clear(self):
        self.store.clear()
        self.paint()


async def consume(reader, scene):
    while line := await reader.readline():
        scene.receive(line)


async def listen(scene):
    scene.clear()
    last_status = None
    while True:
        writer = None
        try:
            port = json.loads((PLUGIN / "port.json").read_text())["port"]
            if type(port) is not int or not 0 < port < 65536:
                raise ValueError("Invalid ModernOverlay port")
            reader, writer = await asyncio.open_connection("127.0.0.1", port, limit=1024*1024)
            print(f"Connected to ModernOverlay on 127.0.0.1:{port}", flush=True)
            last_status = None
            await consume(reader, scene)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            if str(exc) != last_status:
                print(f"Waiting for ModernOverlay: {exc}", flush=True)
                last_status = str(exc)
        finally:
            if writer:
                writer.close()
                scene.clear()
        await asyncio.sleep(1)


def open_channel(path, width, height):
    path.parent.mkdir(exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    stream = os.fdopen(fd, "r+b")
    try:
        return Publisher(stream, width, height)
    except BaseException:
        stream.close()
        raise


def close_channel(publisher):
    publisher.memory.close()
    publisher.stream.close()


async def watch_resolution(scene, directory):
    while True:
        await asyncio.sleep(.25)
        try:
            resolution = json.loads((directory / "display.json").read_text())["resolution"]
            width, height = RESOLUTIONS[resolution]
            old = scene.publisher
            if (width, height) == (old.width, old.height):
                continue
            scene.publisher = open_channel(directory / f"frame-{resolution}.shm", width, height)
            close_channel(old)
            scene.paint()
        except (OSError, KeyError, ValueError, TypeError) as exc:
            print(f"Cannot update display resolution: {exc}", flush=True)


async def run(scene, directory):
    if not os.environ.get("EDMO_EDMC_MANAGED"):
        await listen(scene)
        return
    loop = asyncio.get_running_loop()
    watcher = asyncio.create_task(watch_resolution(scene, directory))
    task = asyncio.create_task(listen(scene))

    def parent_pipe_ready():
        if not os.read(0, 4096):
            task.cancel()

    loop.add_reader(0, parent_pipe_ready)
    try:
        await task
    except asyncio.CancelledError:
        pass
    finally:
        loop.remove_reader(0)
        watcher.cancel()
        await asyncio.gather(watcher, return_exceptions=True)


def main():
    app = QGuiApplication(sys.argv)
    path = Path(os.environ.get("EDMO_XR_CHANNEL", DEFAULT_CHANNEL))
    scene = Scene(open_channel(path, WIDTH, HEIGHT))
    try:
        asyncio.run(run(scene, path.parent))
    except KeyboardInterrupt:
        pass
    finally:
        close_channel(scene.publisher)


if __name__ == "__main__":
    main()
