"""Exercise the installed PlanetPOI and EDMO APIs with synthetic navigation inputs."""
import collections
import json
import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
plugins = Path(os.environ["EDMO_PLUGINS"])
sys.path[:0] = [str(plugins / "EDMCModernOverlay"), str(plugins / "EDMC-PlanetPOI")]
from overlay_plugin import overlay_api
from PlanetPOI import overlay
from PlanetPOI.heading_guidance import HeadingGuidance

messages = []


def receive(payload):
    messages.append(dict(payload))
    return True


def counts():
    return dict(collections.Counter(p.get("shape", p["type"]) for p in messages))


overlay_api.register_publisher(receive)
try:
    overlay.set_overlay_settings(2, 500)
    overlay.show_poi_rows_with_colors([("317°/ 1.0km Canyon 01", True)])
    guide = HeadingGuidance(650, 56)
    guide.update(270, 317)
    for item_id in ("poi_0", "poi_1", "heading-arrow-shaft", "heading-arrow-head"):
        print(json.dumps(next(p for p in reversed(messages) if p["id"] == item_id), ensure_ascii=False))
    print("Row and turn", counts())
    assert all(p["event"] == "LegacyOverlay" for p in messages)
    messages.clear()
    guide.show_checkmark()
    print("Checkmark", counts())
finally:
    overlay_api.unregister_publisher()
