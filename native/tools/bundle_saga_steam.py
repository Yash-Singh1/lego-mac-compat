#!/usr/bin/env python3
"""Package the Steam app and its sibling data without altering the installation."""
import argparse
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile


def ditto(source, destination):
    subprocess.run(["ditto", "--noextattr", "--noqtn", str(source), str(destination)],
                   check=True)


def install_directory(staged, destination):
    """Replace only a generated directory, restoring it if the rename fails."""
    with tempfile.TemporaryDirectory(prefix=".saga-previous-", dir=destination.parent) as tmp:
        previous = Path(tmp) / destination.name
        existed = destination.exists()
        if existed:
            destination.rename(previous)
        try:
            staged.rename(destination)
        except BaseException:
            if existed:
                previous.rename(destination)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source_app", "source_data", "loader", "bundle", "template"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    source = args.source_app.resolve()
    data = args.source_data.resolve()
    bundle = args.bundle.absolute()
    destination_data = bundle.parent / "LEGOStarWarsSagaData"
    # Refuse layouts that could replace the Steam installation or a parent of it.
    for destination in (bundle.resolve(), destination_data.resolve()):
        for original in (source, data):
            if destination == original or destination in original.parents or original in destination.parents:
                parser.error("build destinations must be separate from the source app and data")
    contents = source / "Contents"
    required = [contents / "Info.plist", contents / "MacOS/LEGO Star Wars Saga",
                contents / "Resources/GameIcon.icns", contents / "Frameworks/libsteam_api.dylib",
                args.loader, args.template, data / "game.dat", data / "savegame.zip"]
    required += [data / f"episode_{episode}.dat" for episode in ("i", "ii", "iii", "iv", "v", "vi")]
    missing = [str(path) for path in required if not path.is_file()]
    missing += [str(data / name) for name in ("audio", "movies", "feralshaders", "stuff")
                if not (data / name).is_dir()]
    if missing:
        parser.error("missing Steam files: " + ", ".join(missing))
    info = plistlib.loads((contents / "Info.plist").read_bytes())
    if str(info.get("FeralAppID")) != "32440" or info.get("CFBundleShortVersionString") != "1.2.1":
        parser.error("expected the Steam Complete Saga 1.2.1 app (FeralAppID 32440)")
    subprocess.run(["lipo", str(contents / "Frameworks/libsteam_api.dylib"), "-verify_arch", "x86_64"],
                   check=True)
    bundle.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".saga-steam-", dir=bundle.parent) as tmp:
        staged = Path(tmp) / bundle.name
        staged_contents = staged / "Contents"
        for directory in ("MacOS", "SharedSupport"):
            (staged_contents / directory).mkdir(parents=True, exist_ok=True)
        ditto(contents / "Resources", staged_contents / "Resources")
        ditto(contents / "Frameworks", staged_contents / "Frameworks")
        # Steam's depot omits the conventional versioned-framework symlinks.
        # Reconstruct them in the output so dyld and strict codesign can use it.
        quincy = staged_contents / "Frameworks/QuincyKit.framework"
        if (quincy / "Versions/A/QuincyKit").is_file():
            for link, target in (("Versions/Current", "A"),
                                 ("QuincyKit", "Versions/Current/QuincyKit"),
                                 ("Resources", "Versions/Current/Resources")):
                path = quincy / link
                if not path.exists() and not path.is_symlink():
                    path.symlink_to(target)
        # Modern codesign rejects this SDK's legacy i386 slice. The host bridge
        # uses x86_64; retain the publisher's native code, removing only i386
        # from the staged copy. The original Steam installation is untouched.
        steam_library = staged_contents / "Frameworks/libsteam_api.dylib"
        steam_library.unlink()
        subprocess.run(["lipo", str(contents / "Frameworks/libsteam_api.dylib"),
                        "-thin", "x86_64", "-output", str(steam_library)], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(steam_library)], check=True)
        # The shared Steam bridge loads the publisher's native slice here.
        shutil.copy2(steam_library,
                     staged_contents / "Resources/libsteam_api.dylib")
        shutil.copy2(args.loader, staged_contents / "MacOS/LEGOCompleteSagaCompat")
        image = staged_contents / "SharedSupport/LEGOCompleteSaga.image"
        shutil.copy2(contents / "MacOS/LEGO Star Wars Saga", image)
        image.chmod(0o644)
        # Steam's public app ID lets the SDK identify this relocated build.
        # Initialization and ownership checks remain in the original Steam SDK.
        (staged_contents / "MacOS/steam_appid.txt").write_text("32440\n")
        info.update(plistlib.loads(args.template.read_bytes()))
        info.update(CFBundleIconFile="GameIcon.icns",
                    CFBundleIdentifier="com.feralinteractive.saga.steam.compat",
                    CFBundleName="LEGO Complete Saga Steam (Compatibility)",
                    CFBundleShortVersionString="1.2.1-compat")
        (staged_contents / "Info.plist").write_bytes(plistlib.dumps(info))
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(staged)], check=True)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(staged)], check=True)
        staged_data = Path(tmp) / destination_data.name
        ditto(data, staged_data)
        install_directory(staged_data, destination_data)
        install_directory(staged, bundle)
    print(f"Steam Saga bundle: {bundle}")
    print(f"Keep its game data beside it: {destination_data}")


if __name__ == "__main__":
    main()
