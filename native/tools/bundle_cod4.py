#!/usr/bin/env python3
"""Build a COD4 compatibility app from a 32-bit Mac installation."""
import argparse
import plistlib
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

EXECUTABLES = {"sp": "Call of Duty 4", "mp": "Call of Duty 4 Multiplayer"}


def macho_slice(data, cpu):
    thin_magic = b"\xce\xfa\xed\xfe" if cpu == 7 else b"\xcf\xfa\xed\xfe"
    header_size = 28 if cpu == 7 else 32
    if len(data) >= header_size and data[:4] == thin_magic and struct.unpack_from('<I', data, 4)[0] == cpu:
        return data
    if len(data) < 8 or data[:4] != b"\xca\xfe\xba\xbe":
        raise ValueError("The library or game executable has no required Intel Mach-O architecture")
    count = struct.unpack_from('>I', data, 4)[0]
    if count > (len(data) - 8) // 20:
        raise ValueError("Invalid universal Mach-O architecture table")
    for index in range(count):
        kind, _, offset, size, _ = struct.unpack_from('>IIIII', data, 8 + index * 20)
        if kind != cpu:
            continue
        if offset < 8 + count * 20 or size < header_size or offset > len(data) or size > len(data) - offset:
            raise ValueError("Invalid Intel Mach-O slice")
        return macho_slice(data[offset:offset + size], cpu)
    raise ValueError("The library or game executable has no required Intel Mach-O architecture")


def game_data(source):
    for path in (source / 'Contents/Call of Duty 4 Data',
                 source / 'Contents/Resources/Call of Duty 4 Data',
                 source.parent / 'Call of Duty 4 Data'):
        if (path / 'main/iw_00.iwd').is_file():
            return path
    raise ValueError("Missing Call of Duty 4 Data/main/iw_00.iwd")


def library(contents, name):
    return next((contents / directory / name for directory in ('MacOS', 'Frameworks', 'Resources')
                 if (contents / directory / name).is_file()), None)


def steam_source(steam=None):
    steam = steam or Path.home() / "Library/Application Support/Steam"
    libraries = [steam]
    folders = steam / "steamapps/libraryfolders.vdf"
    if folders.is_file():
        libraries += [Path(p.replace("\\\\", "\\")) for p in
                      re.findall(r'"path"\s+"([^"\n]+)"', folders.read_text())]
    for library in dict.fromkeys(libraries):
        manifest = library / "steamapps/appmanifest_7940.acf"
        if not manifest.is_file():
            continue
        match = re.search(r'"installdir"\s+"([^"\n]+)"', manifest.read_text())
        if match and Path(match[1]).name == match[1]:
            source = library / "steamapps/common" / match[1] / "Call of Duty 4.app"
            if source.is_dir():
                return source.resolve()
    raise ValueError("Steam's Mac installation of app 7940 was not found; pass SOURCE_APP=/path/to/Call of Duty 4.app")


def validate_source(source, mode):
    contents = source / "Contents"
    selected = contents if mode == "sp" or source.name == "Call of Duty 4 Multiplayer.app" else contents / "Call of Duty 4 Multiplayer.app/Contents"
    info_path = selected / "Info.plist"
    if not info_path.is_file():
        raise ValueError(f"Missing Mac app Info.plist: {info_path}")
    info = plistlib.loads(info_path.read_bytes())
    name = info.get('CFBundleExecutable')
    if not isinstance(name, str) or name in ('', '.', '..') or '/' in name or '\\' in name:
        raise ValueError("Invalid CFBundleExecutable in the Mac app Info.plist")
    if info.get('LP32GeneratedGame') == 'cod4':
        raise ValueError("Choose the original Mac game app, not a converted copy")
    image = selected / "MacOS" / name
    required = [image, game_data(source) / 'main/iw_00.iwd']
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise ValueError("Missing Mac game files: " + ", ".join(missing))
    macho_slice(image.read_bytes(), 7)
    return selected, image, info


def validate_destination(source, bundle):
    source, destination = source.resolve(), bundle.resolve()
    if (source.name == 'Call of Duty 4 Multiplayer.app' and source.parent.name == 'Contents'
            and source.parent.parent.suffix == '.app'):
        source = source.parent.parent
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError("Build destination must be separate from the Steam source installation")
    if bundle.exists():
        info_path = bundle / "Contents/Info.plist"
        info = plistlib.loads(info_path.read_bytes()) if info_path.is_file() else {}
        if info.get("LP32GeneratedGame") != "cod4":
            raise ValueError("Refusing to replace a directory that is not a generated COD4 compatibility app")


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def ditto(source, target):
    run("ditto", "--noextattr", "--noqtn", source, target)


def build(source, mode, loader, bundle, inactive, runtime):
    selected, image, info = validate_source(source, mode)
    validate_destination(source, bundle)
    if not loader.is_file():
        raise ValueError(f"Missing loader: {loader}")
    from prepare_guest_runtime import LIBRARIES, sha256
    for name, (_, digest) in LIBRARIES.items():
        if not (runtime / name).is_file() or sha256(runtime / name) != digest:
            raise ValueError("Missing or unrecognized private C++ runtime; run make cod4-runtime")
    steam_library = library(selected, 'libsteam_api.dylib')
    steam_image = macho_slice(steam_library.read_bytes(), 0x01000007) if steam_library else None
    bink_source = library(selected, 'libBinkMachOx86.dylib')
    data_source = game_data(source)
    suffix = "MP" if mode == "mp" else ""
    bundle.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".cod4-stage-", dir=bundle.parent) as tmp:
        staged = Path(tmp) / bundle.name
        contents = staged / "Contents"
        for name in ("MacOS", "SharedSupport", "Resources"):
            (contents / name).mkdir(parents=True)
        resources = selected / "Resources"
        if resources.is_dir():
            if data_source.parent == resources:
                for item in resources.iterdir():
                    if item != data_source:
                        ditto(item, contents / "Resources" / item.name)
            else:
                ditto(resources, contents / "Resources")
        ditto(data_source, contents / "Call of Duty 4 Data")
        shutil.copy2(loader, contents / f"MacOS/COD4{suffix}Compat")
        target = contents / f"SharedSupport/COD4{suffix}.image"
        target.write_bytes(macho_slice(image.read_bytes(), 7))
        target.chmod(0o644)
        # The original i386 Bink runs as guest code; the Steam SDK runs through
        # the host bridge using the publisher's unmodified x86_64 slice.
        if bink_source:
            shutil.copy2(bink_source, contents / "SharedSupport/libBinkMachOx86.dylib")
        ditto(runtime, contents / "SharedSupport/compat-runtime")
        steam = contents / "Resources/libsteam_api.dylib"
        if steam_image:
            if steam.exists():
                steam.unlink()
            steam.write_bytes(steam_image)
            run("codesign", "--force", "--sign", "-", steam)
        source_id = info.get('CFBundleIdentifier')
        if not isinstance(source_id, str) or not source_id:
            source_id = f'org.32bitgoofy.cod4.{mode}'
        info.update(CFBundleExecutable=f"COD4{suffix}Compat",
                    CFBundleIdentifier=source_id + '.compat',
                    CFBundleName=f"Call of Duty 4{(' Multiplayer' if suffix else '')} (Compatibility)",
                    CFBundleDisplayName=f"Call of Duty 4{(' Multiplayer' if suffix else '')} (Compatibility)",
                    LSMinimumSystemVersion="11.0", NSHighResolutionCapable=False,
                    LP32GeneratedGame="cod4", LP32ContinueWhenInactive=bool(inactive))
        if mode == "mp":
            # The original NSURLDownload code fetches server-selected mods
            # over HTTP. Linking our host against a modern SDK otherwise adds
            # ATS restrictions and fails these downloads with error -1022.
            info["NSAppTransportSecurity"] = {"NSAllowsArbitraryLoads": True}
        (contents / "Info.plist").write_bytes(plistlib.dumps(info))
        run("codesign", "--force", "--sign", "-", staged)
        run("codesign", "--verify", "--strict", staged)
        previous = Path(tmp) / "previous.app"
        if bundle.exists():
            bundle.rename(previous)
        try:
            staged.rename(bundle)
        except BaseException:
            if previous.exists():
                previous.rename(bundle)
            raise
    print(f"COD4 {mode} compatibility bundle: {bundle}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-app", default="")
    parser.add_argument("--mode", choices=EXECUTABLES, default="sp")
    parser.add_argument("--loader", type=Path, default=Path("build/game_loader"))
    parser.add_argument("--bundle", type=Path, default=Path("build/COD4-Compat.app"))
    parser.add_argument("--runtime", type=Path, default=Path("build/guest-runtime"))
    parser.add_argument("--continue-when-inactive", type=int, choices=(0, 1), default=0)
    args = parser.parse_args()
    try:
        source = Path(args.source_app).resolve() if args.source_app else steam_source()
        build(source, args.mode, args.loader.resolve(), args.bundle.absolute(), args.continue_when_inactive, args.runtime.resolve())
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
