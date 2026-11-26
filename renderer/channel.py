"""Version 1 of the shared RGBA channel between the renderer and XR layer."""
import fcntl
import mmap
import os
import secrets
import struct
import sys
from pathlib import Path

RESOLUTIONS = {"640x480": (640, 480), "960x720": (960, 720),
               "1280x960": (1280, 960), "1920x1440": (1920, 1440)}
WIDTH, HEIGHT = RESOLUTIONS[os.environ.get("EDMO_RESOLUTION", "1280x960")]
HEADER = struct.Struct("<4sIIIQQ")
FRAME_BYTES = WIDTH * HEIGHT * 4
DEFAULT_CHANNEL = Path(__file__).resolve().parents[1] / "logs" / "renderer-hd.shm"


class Publisher:
    def __init__(self, stream, width=WIDTH, height=HEIGHT):
        self.width, self.height = width, height
        self.frame_bytes = width * height * 4
        self.stream = stream
        self.generation = secrets.randbits(64) or 1
        self.sequence = 0
        self.previous = None
        # Readers cannot observe an old frame with the new writer's lifetime lock.
        fcntl.flock(stream, fcntl.LOCK_EX)
        try:
            try:
                fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB, 1)
            except BlockingIOError:
                sys.exit("A renderer already owns this channel")
            size = os.fstat(stream.fileno()).st_size
            if size == 0:
                stream.truncate(HEADER.size + self.frame_bytes)
            elif size != HEADER.size + self.frame_bytes:
                sys.exit("Incompatible renderer channel size")
            self.memory = mmap.mmap(stream.fileno(), HEADER.size + self.frame_bytes)
            self.memory[:HEADER.size] = bytes(HEADER.size)
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)

    def publish(self, pixels):
        if len(pixels) != self.frame_bytes:
            raise ValueError("Invalid RGBA frame size")
        if pixels == self.previous:
            return
        self.sequence += 1
        fcntl.flock(self.stream, fcntl.LOCK_EX)
        try:
            self.memory[HEADER.size:] = pixels
            HEADER.pack_into(self.memory, 0, b"EDM3", 1, self.width, self.height, self.generation, self.sequence)
        finally:
            fcntl.flock(self.stream, fcntl.LOCK_UN)
        self.previous = pixels
