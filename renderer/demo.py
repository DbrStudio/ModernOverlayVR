#!/usr/bin/env python3
"""Phase 3 headless renderer. Can be started or stopped during a live session."""
import os
import signal
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from channel import DEFAULT_CHANNEL, HEIGHT, WIDTH, Publisher
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QPainter

def draw(seconds):
    image = QImage(WIDTH, HEIGHT, QImage.Format.Format_RGBA8888)
    image.fill(Qt.GlobalColor.transparent)
    with QPainter(image) as painter:
        painter.fillRect(2, 2, WIDTH - 4, HEIGHT - 4, QColor(12, 16, 24, 160))
        painter.fillRect(3, HEIGHT - 5, 1 + seconds % (WIDTH - 6), 2, QColor("#ff78be"))
        font = QFont("DejaVu Sans Mono")
        font.setPixelSize(16)
        painter.setFont(font)
        painter.setPen(QColor("#65efe3"))
        painter.drawText(image.rect(), Qt.AlignmentFlag.AlignCenter, f"EDMO LIVE {seconds:04d}")
    return bytes(image.constBits())


def main():
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    app = QGuiApplication(sys.argv)
    channel = Path(os.environ.get("EDMO_XR_CHANNEL", DEFAULT_CHANNEL))
    channel.parent.mkdir(exist_ok=True)
    fd = os.open(channel, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "r+b") as stream:
        publisher = Publisher(stream)

        def tick():
            publisher.publish(draw(publisher.sequence % 10000))

        tick()
        timer = QTimer()
        timer.timeout.connect(tick)
        timer.start(1000)
        print(f"EDMO LIVE → {channel}", flush=True)
        return app.exec()


if __name__ == "__main__":
    sys.exit(main())
