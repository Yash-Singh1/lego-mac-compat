#!/usr/bin/env python3
"""Embed opt-in runtime settings before the generated bundle is signed."""
import argparse
import plistlib
from pathlib import Path


def configure(path, continue_when_inactive=False):
    info = plistlib.loads(path.read_bytes())
    info["LP32ContinueWhenInactive"] = bool(continue_when_inactive)
    path.write_bytes(plistlib.dumps(info))
    if str(info.get("FeralAppID")) == "32440":
        install_saga_controller_profile(path.parent / "Resources")


def install_saga_controller_profile(resources):
    """Install the DS4 revision and a virtual profile for semantic gamepads."""
    profiles = resources / "InputDevices/AnalogTriggers"
    source = profiles / "PS4Dualshock.plist"
    target = profiles / "PS4DualshockV2.plist"
    generic = profiles / "LP32StandardGamepad.plist"
    if not source.is_file():
        return
    profile = plistlib.loads(source.read_bytes())
    if profile.get("VendorID") != 1356 or profile.get("ProductID") != 1476:
        return
    if not target.exists():
        v2 = dict(profile)
        v2.update(ProductID=2508, CGPDisplayNameOvr="DualShock 4 (v2)",
                  ButtonBack="9:9")  # Share; the older profile uses touchpad.
        target.write_bytes(plistlib.dumps(v2))
    if not generic.exists():
        standard = dict(profile)
        standard.update(VendorID=0x7F32, ProductID=0x7F32,
                        CGPDisplayNameOvr="Standard Gamepad",
                        CGPDeviceType="Xbox", ButtonBack="9:9")
        generic.write_bytes(plistlib.dumps(standard))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--continue-when-inactive", choices=("0", "1"), default="0")
    args = parser.parse_args()
    configure(args.bundle / "Contents/Info.plist", args.continue_when_inactive == "1")
