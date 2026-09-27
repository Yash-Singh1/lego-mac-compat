#!/usr/bin/env python3
"""Build a separate MW2 compatibility app from Aspyr's original Mac release."""
import argparse
import ctypes
import os
import stat
import plistlib
import re
import shutil
import tempfile
from pathlib import Path

from bundle_cod4 import ditto, macho_slice, run

MODES = {'sp': ('COD_MW2_SP', 'MW2', 10180), 'mp': ('COD_MW2_MP', 'MW2MP', 10190)}
LIBRARIES = ('libBinkMacx86.dylib', 'libMilesX86.dylib')


def steam_source(mode, steam=None):
    steam = steam or Path.home() / 'Library/Application Support/Steam'
    libraries = [steam]
    folders = steam / 'steamapps/libraryfolders.vdf'
    if folders.is_file():
        libraries += [Path(p.replace('\\\\', '\\')) for p in re.findall(r'"path"\s+"([^"\n]+)"', folders.read_text())]
    pending = False
    for library in dict.fromkeys(libraries):
        manifest = library / f'steamapps/appmanifest_{MODES[mode][2]}.acf'
        if not manifest.is_file():
            continue
        text = manifest.read_text()
        state = re.search(r'"StateFlags"\s+"(\d+)"', text)
        if not state or int(state[1]) != 4:
            pending = True
            continue
        folder = re.search(r'"installdir"\s+"([^"\n]+)"', text)
        if folder and folder[1] not in ('.', '..') and '/' not in folder[1] and '\\' not in folder[1]:
            source = library / 'steamapps/common' / folder[1] / (MODES[mode][0] + '.app')
            if source.is_dir():
                return source.resolve()
    if pending:
        raise ValueError('MW2 is still downloading or updating in Steam. Let Steam finish before converting.')
    raise ValueError(f"Steam's MW2 {mode} Mac app was not found. Install it or pass SOURCE_APP=/path/to/{MODES[mode][0]}.app")


def validate_source(source, mode):
    name, _, _ = MODES[mode]
    source = source.resolve()
    if source.suffix != '.app':
        source = source / (name + '.app')
    # A staging tree may contain preallocated, incomplete game files.
    if 'steamapps' in source.parts and 'downloading' in source.parts:
        raise ValueError('MW2 is still downloading in Steam. Choose the completed installation.')
    common = source.parent.parent
    if common.name == 'common' and common.parent.name == 'steamapps':
        manifest = common.parent / f'appmanifest_{MODES[mode][2]}.acf'
        if manifest.is_file():
            state = re.search(r'"StateFlags"\s+"(\d+)"', manifest.read_text())
            if not state or int(state[1]) != 4:
                raise ValueError('MW2 is still downloading or updating in Steam. Let Steam finish before converting.')
    contents = source / 'Contents'
    info_path = contents / 'Info.plist'
    if not info_path.is_file():
        raise ValueError(f'Missing MW2 Mac app Info.plist: {info_path}')
    info = plistlib.loads(info_path.read_bytes())
    if info.get('LP32GeneratedGame'):
        raise ValueError('Choose the original MW2 app, not a converted copy')
    if info.get('CFBundleExecutable') != name:
        raise ValueError(f'Choose the original MW2 {mode} app ({name}.app)')
    # The plist executable is Game Guide. The sub executable contains the game.
    image = contents / 'MacOS' / (name + 'sub')
    data = source.parent / 'GameData'
    required = [image, data / 'main/iw_00.iwd', contents / 'MacOS/libsteam_api.dylib']
    required += [contents / 'MacOS' / library for library in LIBRARIES]
    for path in required:
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Incomplete MW2 installation: missing {path}')
    macho_slice(image.read_bytes(), 7)
    for library in LIBRARIES:
        macho_slice((contents / 'MacOS' / library).read_bytes(), 7)
    macho_slice((contents / 'MacOS/libsteam_api.dylib').read_bytes(), 0x01000007)
    return source, image, data, info


def validate_destination(source, data, bundle):
    destination = bundle.resolve()
    for original in (source.resolve(), data.resolve()):
        if original == destination or original in destination.parents or destination in original.parents:
            raise ValueError('Build destination must be separate from the original MW2 installation')
    if bundle.exists():
        raise ValueError('The output already exists. Choose a new BUNDLE path to preserve its data and saves.')


def validate_tree(root):
    if not stat.S_ISDIR(root.lstat().st_mode):
        raise ValueError(f'The installation contains a link or unsupported file: {root}')
    for directory, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            path = Path(directory) / name
            kind = path.lstat().st_mode
            if not (stat.S_ISDIR(kind) or stat.S_ISREG(kind)):
                raise ValueError(f'The installation contains a link or unsupported file: {path}')


def publish(staged, bundle):
    # macOS RENAME_EXCL also protects an empty directory created by another
    # process between validation and publication.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renamex_np
    rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(os.fsencode(staged), os.fsencode(bundle), 4):
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(bundle))


def build(source, mode, loader, bundle, inactive=False):
    source, image, data, info = validate_source(source, mode)
    validate_destination(source, data, bundle)
    validate_tree(source)
    validate_tree(data)
    if not loader.is_file():
        raise ValueError(f'Missing loader: {loader}')
    _, stem, app_id = MODES[mode]
    original = source / 'Contents'
    bundle.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.mw2-stage-', dir=bundle.parent) as tmp:
        staged = Path(tmp) / bundle.name
        contents = staged / 'Contents'
        for directory in ('MacOS', 'Resources', 'SharedSupport'):
            (contents / directory).mkdir(parents=True)
        ditto(original / 'Resources', contents / 'Resources')
        ditto(data, contents / 'GameData')
        shutil.copy2(loader, contents / f'MacOS/{stem}Compat')
        (contents / f'SharedSupport/{stem}.image').write_bytes(macho_slice(image.read_bytes(), 7))
        for library in LIBRARIES:
            shutil.copy2(original / 'MacOS' / library, contents / 'SharedSupport' / library)
        # Keep Miles' guest PE codecs with its guest libraries, outside host code.
        for file in (original / 'MacOS').iterdir():
            if file.suffix in ('.asi', '.flt', '.mix'):
                shutil.copy2(file, contents / 'SharedSupport' / file.name)
        steam = contents / 'Resources/libsteam_api.dylib'
        steam.write_bytes(macho_slice((original / 'MacOS/libsteam_api.dylib').read_bytes(), 0x01000007))
        run('codesign', '--force', '--sign', '-', steam)
        info.update(CFBundleExecutable=f'{stem}Compat',
                    CFBundleIdentifier=f'org.32bitgoofy.{stem.lower()}.compat',
                    CFBundleName=f'Modern Warfare 2{(" Multiplayer" if mode == "mp" else "")} (Compatibility)',
                    CFBundleDisplayName=f'Modern Warfare 2{(" Multiplayer" if mode == "mp" else "")} (Compatibility)',
                    LSMinimumSystemVersion='11.0', NSHighResolutionCapable=False,
                    LP32GeneratedGame='mw2', LP32SteamAppID=app_id,
                    LP32ContinueWhenInactive=bool(inactive))
        # The original nib and GameGuide settings belong to its launcher.
        for key in ('NSMainNibFile', 'GameGuide', 'DefaultChildApp'):
            info.pop(key, None)
        (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
        run('codesign', '--force', '--sign', '-', staged)
        run('codesign', '--verify', '--strict', staged)
        validate_source(source, mode)
        if bundle.exists():
            raise ValueError('The output appeared during conversion. Choose a new BUNDLE path.')
        publish(staged, bundle)
    print(f'MW2 {mode} compatibility bundle: {bundle}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-app', default='')
    parser.add_argument('--mode', choices=MODES, default='sp')
    parser.add_argument('--loader', type=Path, default=Path('build/game_loader'))
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--continue-when-inactive', type=int, choices=(0, 1), default=0)
    args = parser.parse_args()
    try:
        source = Path(args.source_app) if args.source_app else steam_source(args.mode)
        bundle = args.bundle or Path('build') / (MODES[args.mode][1] + '-Compat.app')
        build(source, args.mode, args.loader.resolve(), bundle.absolute(), args.continue_when_inactive)
    except (ValueError, OSError, plistlib.InvalidFileException) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
