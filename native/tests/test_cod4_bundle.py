#!/usr/bin/env python3
"""Source discovery, edition checks and protections for Steam conversions."""
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import bundle_cod4 as cod


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cod4-bundle-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'Steam library/steamapps/common/Call of Duty 4/Call of Duty 4.app'
        self.contents = self.source / 'Contents'
        self.contents.mkdir(parents=True)

    def fixture(self, mode='sp'):
        contents = self.contents if mode == 'sp' else self.contents / 'Call of Duty 4 Multiplayer.app/Contents'
        name = cod.EXECUTABLES[mode]
        icon = 'Game.icns' if mode == 'sp' else 'Game_mp.icns'
        files = [contents / 'MacOS' / name, contents / 'MacOS/libsteam_api.dylib',
                 contents / 'MacOS/libBinkMachOx86.dylib', contents / 'Resources' / icon,
                 self.contents / 'Call of Duty 4 Data/main/iw_00.iwd']
        for p in files:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b'fixture')
        (contents / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': f'com.aspyr.callofduty4.{mode}.steam',
            'CFBundleShortVersionString': '1.7.2', 'CFBundleIconFile': icon,
            'CFBundleExecutable': name}))
        return contents

    def test_external_steam_library(self):
        steam = self.root / 'Steam'
        (steam / 'steamapps').mkdir(parents=True)
        library = self.root / 'Steam library'
        (steam / 'steamapps/libraryfolders.vdf').write_text(f'"libraryfolders" {{ "1" {{ "path" "{library}" }} }}')
        (library / 'steamapps/appmanifest_7940.acf').write_text('"AppState" { "installdir" "Call of Duty 4" }')
        self.assertEqual(cod.steam_source(steam), self.source.resolve())

    def test_discovery_rejects_manifest_traversal(self):
        steam = self.root / 'Steam'
        (steam / 'steamapps').mkdir(parents=True)
        (steam / 'steamapps/appmanifest_7940.acf').write_text('"installdir" "../../Call of Duty 4"')
        with self.assertRaisesRegex(ValueError, 'not found'):
            cod.steam_source(steam)

    def test_mac_images_and_mode_layouts(self):
        for mode in ('sp', 'mp'):
            contents = self.fixture(mode)
            name = cod.EXECUTABLES[mode]
            image = contents / 'MacOS' / name
            image.write_bytes(bytes.fromhex('cefaedfe07000000') + bytes(20))
            selected, found, _ = cod.validate_source(self.source, mode)
            self.assertEqual(selected, contents)
            self.assertEqual(found, image)
            image.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'Mach-O'):
                cod.validate_source(self.source, mode)

    def test_other_edition_and_incomplete_data(self):
        self.fixture()
        original = self.contents / 'MacOS/Call of Duty 4'
        image = self.contents / 'MacOS/Retail COD4'
        original.rename(image)
        image.write_bytes(bytes.fromhex('cefaedfe07000000') + bytes(20))
        (self.contents / 'MacOS/libsteam_api.dylib').unlink()
        (self.contents / 'MacOS/libBinkMachOx86.dylib').unlink()
        info = self.contents / 'Info.plist'
        info.write_bytes(plistlib.dumps({'CFBundleIdentifier': 'retail', 'CFBundleShortVersionString': '1.0',
                                        'CFBundleExecutable': 'Retail COD4'}))
        self.assertEqual(cod.validate_source(self.source, 'sp')[1], image)
        self.assertIsNone(cod.library(self.contents, 'libsteam_api.dylib'))
        (self.contents / 'Call of Duty 4 Data/main/iw_00.iwd').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing Call of Duty 4 Data'):
            cod.validate_source(self.source, 'sp')

    def test_source_overlap_and_symlink_protection(self):
        for path in (self.source, self.source.parent, self.contents / 'Output.app'):
            with self.assertRaisesRegex(ValueError, 'separate'):
                cod.validate_destination(self.source, path)
        link = self.root / 'Alias.app'
        link.symlink_to(self.source, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'separate'):
            cod.validate_destination(self.source, link)

    def test_existing_output_must_be_generated(self):
        output = self.root / 'Output.app'
        (output / 'Contents').mkdir(parents=True)
        (output / 'personal.txt').write_text('keep me')
        with self.assertRaisesRegex(ValueError, 'Refusing'):
            cod.validate_destination(self.source, output)
        self.assertEqual((output / 'personal.txt').read_text(), 'keep me')
        (output / 'Contents/Info.plist').write_bytes(plistlib.dumps({'LP32GeneratedGame': 'cod4'}))
        cod.validate_destination(self.source, output)


if __name__ == '__main__':
    unittest.main()
