#!/usr/bin/env python3
"""MW2/MW3 mode selection, pending Steam downloads and MW1/source preservation."""
import plistlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import bundle_mw2 as mw2


def image(cpu=7):
    magic = 'cefaedfe' if cpu == 7 else 'cffaedfe'
    return bytes.fromhex(magic) + cpu.to_bytes(4, 'little') + bytes(24)


class MW2BundleTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='mw2-test-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.steam = self.root / 'Steam library'
        self.install = self.steam / 'steamapps/common/Call of Duty Modern Warfare 2'
        self.data = self.install / 'GameData'
        (self.data / 'main').mkdir(parents=True)
        (self.data / 'main/iw_00.iwd').write_bytes(b'original assets')

    def fixture(self, mode='sp', state=4, game='mw2'):
        name, _, appid = mw2.GAMES[game][2][mode]
        app = self.install / (name + '.app')
        macos = app / 'Contents/MacOS'
        macos.mkdir(parents=True)
        (app / 'Contents/Resources').mkdir()
        (macos / name).write_bytes(b'launcher must not be used')
        (macos / (name + 'sub')).write_bytes(image())
        for lib in mw2.libraries(game, mode):
            (macos / lib).write_bytes(image())
        (macos / 'libsteam_api.dylib').write_bytes(image(0x01000007))
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': name}))
        (self.steam / f'steamapps/appmanifest_{appid}.acf').write_text(
            f'"StateFlags" "{state}" "installdir" "{self.install.name}"')
        return app

    def test_modes_select_game_not_launcher(self):
        for mode in mw2.MODES:
            app = self.fixture(mode)
            self.assertEqual(mw2.steam_source(mode, self.steam), app)
            source, executable, data, _ = mw2.validate_source(app, mode)
            self.assertEqual(executable.name, mw2.MODES[mode][0] + 'sub')
            self.assertEqual(source, app)
            self.assertEqual(data, self.data)
            self.assertEqual(mw2.validate_source(self.install, mode)[0], app)
            with self.assertRaisesRegex(ValueError, 'Choose the original'):
                mw2.validate_source(app, 'mp' if mode == 'sp' else 'sp')

    def test_mw3_modes_are_separate_from_mw2(self):
        mw2_app = self.fixture('sp')
        for mode in mw2.MODES:
            app = self.fixture(mode, game='mw3')
            self.assertEqual(mw2.steam_source(mode, self.steam, game='mw3'), app)
            source, executable, data, _ = mw2.validate_source(app, mode, 'mw3')
            self.assertEqual(executable.name, mw2.GAMES['mw3'][2][mode][0] + 'sub')
            self.assertEqual(data, self.data)
            with self.assertRaisesRegex(ValueError, 'Choose the original MW2'):
                mw2.validate_source(app, mode, 'mw2')
        with self.assertRaisesRegex(ValueError, 'Choose the original MW3'):
            mw2.validate_source(mw2_app, 'sp', 'mw3')
        self.assertEqual(mw2.steam_source('sp', self.steam), mw2_app)
        (self.steam / 'steamapps/appmanifest_42690.acf').write_text(
            f'"StateFlags" "68" "installdir" "{self.install.name}"')
        with self.assertRaisesRegex(ValueError, 'MW3 is still downloading'):
            mw2.steam_source('mp', self.steam, game='mw3')
        # MW3 multiplayer links Bink 2; the Bink 1 library is not required.
        (self.steam / 'steamapps/appmanifest_42690.acf').write_text(
            f'"StateFlags" "4" "installdir" "{self.install.name}"')
        mp = self.install / 'COD_MW3_MP.app/Contents/MacOS'
        self.assertFalse((mp / 'libBinkMacx86.dylib').exists())
        (mp / 'libBink2Macx86.dylib').unlink()
        with self.assertRaisesRegex(ValueError, 'libBink2Macx86'):
            mw2.validate_source(mp.parent.parent, 'mp', 'mw3')

    def test_external_library(self):
        app = self.fixture()
        steam = self.root / 'Default Steam'
        (steam / 'steamapps').mkdir(parents=True)
        (steam / 'steamapps/libraryfolders.vdf').write_text(f'"path" "{self.steam}"')
        self.assertEqual(mw2.steam_source('sp', steam), app)

    def test_pending_download(self):
        app = self.fixture(state=1026)
        with self.assertRaisesRegex(ValueError, 'still downloading'):
            mw2.steam_source('sp', self.steam)
        with self.assertRaisesRegex(ValueError, 'still downloading'):
            mw2.validate_source(app, 'sp')
        with self.assertRaisesRegex(ValueError, 'still downloading'):
            mw2.validate_source(self.steam / 'steamapps/downloading/10180/COD_MW2_SP.app', 'sp')

    def test_incomplete_and_wrong_architecture(self):
        app = self.fixture()
        lib = app / 'Contents/MacOS/libMilesX86.dylib'
        lib.write_bytes(image(0x01000007))
        with self.assertRaisesRegex(ValueError, 'Mach-O'):
            mw2.validate_source(app, 'sp')
        lib.unlink()
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            mw2.validate_source(app, 'sp')

    def test_preserves_mw1_and_sources(self):
        app = self.fixture()
        for output in (app, app.parent, app / 'output.app', self.data, self.data / 'output.app'):
            with self.assertRaisesRegex(ValueError, 'separate'):
                mw2.validate_destination(app, self.data, output)
        alias = self.root / 'Alias.app'
        alias.symlink_to(app)
        with self.assertRaisesRegex(ValueError, 'separate'):
            mw2.validate_destination(app, self.data, alias)
        output = self.root / 'COD4-Compat.app'
        (output / 'Contents').mkdir(parents=True)
        sentinel = output / 'Contents/Info.plist'
        original = plistlib.dumps({'LP32GeneratedGame': 'cod4'})
        sentinel.write_bytes(original)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            mw2.build(app, 'sp', self.root / 'missing-loader', output)
        self.assertEqual(sentinel.read_bytes(), original)

    def test_links_and_atomic_publication(self):
        app = self.fixture()
        (app / 'Contents/Resources/link').symlink_to(self.data)
        with self.assertRaisesRegex(ValueError, 'link or unsupported'):
            mw2.validate_tree(app)
        staged = self.root / 'staged.app'
        staged.mkdir()
        (staged / 'sentinel').write_text('staged')
        destination = self.root / 'destination.app'
        destination.mkdir()
        with self.assertRaises(FileExistsError):
            mw2.publish(staged, destination)
        self.assertEqual((staged / 'sentinel').read_text(), 'staged')
        self.assertEqual(list(destination.iterdir()), [])
        destination.rmdir()
        mw2.publish(staged, destination)
        self.assertEqual((destination / 'sentinel').read_text(), 'staged')

    def test_failed_build_does_not_publish_or_modify_source(self):
        app = self.fixture()
        loader = self.root / 'loader'
        loader.write_bytes(b'loader')
        output = self.root / 'MW2-Compat.app'
        before = {p.relative_to(self.install): p.read_bytes() for p in self.install.rglob('*') if p.is_file()}
        with patch.object(mw2, 'ditto', side_effect=RuntimeError('copy failed')):
            with self.assertRaisesRegex(RuntimeError, 'copy failed'):
                mw2.build(app, 'sp', loader, output)
        self.assertFalse(output.exists())
        self.assertFalse(list(self.root.glob('.mw2-stage-*')))
        self.assertEqual(before, {p.relative_to(self.install): p.read_bytes() for p in self.install.rglob('*') if p.is_file()})


if __name__ == '__main__':
    unittest.main()
