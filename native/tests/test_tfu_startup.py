#!/usr/bin/env python3
"""Launch TFU behind other apps; exercise its dialogs and window presentation.

Requires a built TFU bundle and an already accepted first-launch license.
Uses a temporary preferences domain and isolated save directory. No desktop
input is injected, and no normal game settings or saves are changed.
"""
import ctypes
import datetime
import json
import os
from pathlib import Path
import plistlib
import subprocess
import uuid

NATIVE = Path(__file__).resolve().parents[1]
BUNDLE = NATIVE / "build/TFU-Compat.app"
OUTPUT = NATIVE / "build/test-tfu-startup"
OUTPUT.mkdir(parents=True, exist_ok=True)


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class Size(ctypes.Structure):
    _fields_ = [("width", ctypes.c_double), ("height", ctypes.c_double)]


class Rect(ctypes.Structure):
    _fields_ = [("origin", Point), ("size", Size)]


cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
cg.CGDisplayBounds.argtypes = [ctypes.c_uint32]
cg.CGDisplayBounds.restype = Rect
ids = (ctypes.c_uint32 * 32)()
count = ctypes.c_uint32()
assert cg.CGGetActiveDisplayList(32, ids, ctypes.byref(count)) == 0
displays = list(ids[:count.value])
assert displays
primary = cg.CGMainDisplayID()
desktop_top = cg.CGDisplayBounds(primary).size.height
original = subprocess.check_output(["defaults", "export", "com.aspyr.SWTFU", "-"])
preferences = plistlib.loads(original)
assert preferences.get("AspyrEulaAccepted"), "Accept the game's license before running this test."
domain = "com.aspyr.SWTFU.compat-startup-test-" + uuid.uuid4().hex
loader = Path(os.environ.get("LP32_TEST_LOADER", str(NATIVE / "build/game_loader"))).resolve()

try:
    for display, fullscreen in [(d, True) for d in displays] + [(primary, False)]:
        name = f"display-{display}-{'fullscreen' if fullscreen else 'windowed'}"
        case = OUTPUT / name
        case.mkdir(exist_ok=True)
        settings = dict(preferences, RegistrationRequestDate=datetime.datetime(3999, 9, 4),
                        PrefsDialogAlways=True, GameDisplayID=display, GameDisplayMode=int(fullscreen),
                        GameDisplayWidth=1920 if fullscreen else 1280,
                        GameDisplayHeight=1200 if fullscreen else 800,
                        GameDisplayDepth=32, GameDisplayFrequency=0,
                        IsKBAndMouse="TRUE", LowDetail="0")
        subprocess.run(["defaults", "import", domain, "-"], input=plistlib.dumps(settings), check=True)
        for artifact in ("windows.json", "frame.ppm", "dialog.png"):
            (case / artifact).unlink(missing_ok=True)
        env = dict(os.environ, LP32_BACKGROUND_TEST="1", LP32_MUTE_AUDIO="1",
                   LP32_GUEST_RUNTIME_DIR=str(NATIVE / "build/guest-runtime"),
                   LP32_TFU_PREFERENCES_ID=domain, LP32_TFU_USER_DATA_ROOT=str(case / "user-data"),
                   LP32_CARBON_TEST_COMMANDS="Moninot!Moniok  ok  " if len(displays) > 1 else "ok  ",
                   LP32_CARBON_CAPTURE_WINDOW=str(case / "dialog.png"),
                   LP32_CARBON_WINDOW_REPORT=str(case / "windows.json"),
                   LP32_AGL_CAPTURE_FRAME=str(case / "frame.ppm"))
        for key in ("LP32_HEADLESS", "LP32_BUILD_UI_ONLY", "LP32_CARBON_TEST_PANE_POINT",
                    "LP32_TRACE_GUEST_ADDRESS", "LP32_TRACE_CARBON_NATIVE", "LP32_TRACE_AGL"):
            env.pop(key, None)
        with (case / "run.log").open("w") as log:
            process = subprocess.Popen(["arch", "-x86_64", str(loader),
                                        str(BUNDLE / "Contents/SharedSupport/TFU.image")],
                                       cwd=NATIVE, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                status = process.wait(timeout=25)
                raise AssertionError(f"{name}: exited early ({status}); see {case / 'run.log'}")
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        log = (case / "run.log").read_text(errors="replace")
        assert "trapped import" not in log and "compat32: signal" not in log, name
        assert "TFU legacy HID enumeration disabled" in log, name
        assert "TFU GameController bridge installed" in log, name
        if len(displays) > 1:
            assert log.count("dialog test command Moni") == 2, name
            assert "dialog test command not!" in log, name
        windows = json.loads((case / "windows.json").read_text())
        visible = [w for w in windows if w["visible"]]
        assert len(visible) == 1 and not visible[0]["backdrop"], windows
        window = visible[0]
        assert window["fullscreen"] == fullscreen, windows
        assert not any(w["visible"] for w in windows if w["backdrop"]), windows
        assert not any(w["key"] or w["main"] for w in windows), "Background test took window focus"
        if fullscreen:
            bounds = cg.CGDisplayBounds(display)
            expected = [bounds.origin.x, desktop_top - bounds.origin.y - bounds.size.height,
                        bounds.size.width, bounds.size.height]
            assert window["style"] == 0 and window["display"] == display, windows
            assert all(abs(a-b) < 0.5 for a, b in zip(window["frame"], expected)), windows
            assert window["surface"] == [1920, 1200], windows
        else:
            assert window["style"] & 1, windows  # NSWindowStyleMaskTitled
        frame = (case / "frame.ppm").read_bytes()
        magic, dimensions, maximum, pixels = frame.split(b"\n", 3)
        assert magic == b"P6" and maximum == b"255", name
        width, height = map(int, dimensions.split())
        if fullscreen:
            assert [width, height] == window["surface"], (
                "Intro viewport must use the selected render resolution, not desktop points", name,
                [width, height], window["surface"])
        assert len(pixels) == width * height * 3 and len(set(pixels[::101])) > 32, name
        print(f"PASS {name}: chooser cancel/OK, startup, one game window, rendered {width}x{height}", flush=True)
finally:
    subprocess.run(["defaults", "delete", domain], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert plistlib.loads(subprocess.check_output(["defaults", "export", "com.aspyr.SWTFU", "-"])) == plistlib.loads(original), "Normal preferences changed during test"
