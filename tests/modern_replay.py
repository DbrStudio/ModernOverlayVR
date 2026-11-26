"""Replay installed PlanetPOI output through the installed EDMO broadcaster and VR renderer."""
import asyncio
import json
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "renderer"))
from modern import QGuiApplication, Scene, Publisher, consume
from overlay_plugin.overlay_socket_server import SocketBroadcaster


async def run(stream):
    scene = Scene(Publisher(stream))
    scene.clear()
    empty = scene.publisher.previous
    reader = asyncio.StreamReader()

    class Writer:
        def write(self, data):
            # TCP may split inside a Unicode code point or JSON token.
            reader.feed_data(data[:7])
            reader.feed_data(data[7:])

        async def drain(self):
            await asyncio.sleep(0)

    broadcaster = SocketBroadcaster()
    broadcaster._clients.add((reader, Writer()))
    task = asyncio.create_task(consume(reader, scene))
    probe = subprocess.check_output([sys.executable, root / "tests/planetpoi_probe.py"], text=True)
    rows = [json.loads(line) for line in probe.splitlines() if line.startswith("{")]
    reader.feed_data(b'not JSON\n')
    for row in rows:
        await broadcaster._broadcast(json.dumps(row))
    await asyncio.sleep(.05)
    assert scene.publisher.previous != empty
    assert {item.kind for item in scene.store.values()} == {"message", "rect", "vector"}
    for item_id in ("heading-arrow-head", "heading-arrow-shaft"):
        scene.receive(json.dumps(dict(event="LegacyOverlay", type="clear", id=item_id)))
    scene.receive(json.dumps(dict(rows[0], text="Updated route", ttl=1)))
    await asyncio.sleep(.05)
    assert scene.store.get("poi_0").data["text"] == "Updated route"
    await asyncio.sleep(1.1)
    assert scene.publisher.previous == empty
    reader.feed_eof()
    await task


app = QGuiApplication([])
with tempfile.TemporaryFile() as stream:
    asyncio.run(run(stream))
print("PlanetPOI → ModernOverlay broadcast framing → Qt → shared memory, updates/clear/TTL OK")
