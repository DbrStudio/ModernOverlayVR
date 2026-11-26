"""Coordinate renderer crashes and restarts with the mock XR runtime."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

demo = Path(__file__).resolve().parents[1] / "renderer" / "demo.py"
with tempfile.TemporaryDirectory() as tmp:
    env = dict(os.environ, EDMO_XR_CHANNEL=f"{tmp}/renderer.shm", QT_QPA_PLATFORM="offscreen")
    if os.getenv("EDMO_TEST_SETTINGS"):
        config = dict(enabled=False, anchor="head", shape="flat", resolution="1280x960",
                      width=1.5, distance=1.3, arc=70, x=0, y=0)
        path = Path(tmp) / "display.json"
        env.update(EDMO_DISPLAY_FILE=str(path), EDMO_XR_CHANNEL=f"{tmp}/frame-1280x960.shm")
        def save(**changes):
            config.update(changes)
            temp = path.with_suffix(".tmp")
            temp.write_text(json.dumps(config))
            temp.replace(path)
        save()
    renderer = None
    with subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          text=True, env=env) as test:
        try:
            for line in test.stdout:
                if line == "renderer enable\n":
                    save(enabled=True)
                elif line == "renderer geometry\n":
                    save(shape="curved", anchor="room", width=2, distance=2, arc=90, x=.3, y=-.2)
                elif line == "renderer resize\n":
                    save(resolution="640x480")
                    env.update(EDMO_RESOLUTION="640x480", EDMO_XR_CHANNEL=f"{tmp}/frame-640x480.shm")
                elif line == "renderer disable\n":
                    save(enabled=False)
                elif line == "renderer start\n":
                    assert renderer is None
                    renderer = subprocess.Popen([sys.executable, demo], env=env)
                elif line == "renderer kill\n":
                    assert renderer.poll() is None
                    renderer.kill()
                    renderer.wait()
                    renderer = None
                else:
                    print(line, end="")
                    continue
                test.stdin.write("\n")
                test.stdin.flush()
        finally:
            if renderer:
                renderer.kill()
                renderer.wait()
        sys.exit(test.wait())
