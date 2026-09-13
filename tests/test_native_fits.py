"""Display behavior regressions exercised through the shipped native CLI."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from fixture_helpers import write_fits, png_pixels

BINARY = Path(__file__).resolve().parents[1] / 'build/native/astrolibrary'

class NativeDisplayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root/'test.fit'
        self.serial = 0

    def render(self, path, **options):
        self.serial += 1
        output = self.root/f'preview-{self.serial}.png'
        args = [BINARY, 'render', '--input', path, '--output', output]
        for name, value in options.items():
            if name == 'neutralize':
                if value:
                    args.append('--neutralize')
            else:
                args.extend(['--'+name, str(value)])
        run = subprocess.run(args, capture_output=True, text=True)
        if run.returncode:
            raise ValueError(run.stderr)
        return output.read_bytes()

    def test_rgb_planes_share_stretch(self):
        write_fits(self.path, [255] * 4 + [128] * 4 + [0] * 4, channels=3, bitpix=8)
        _, _, pixels = png_pixels(self.render(self.path))
        self.assertEqual(pixels[0], 255)
        self.assertEqual(pixels[2], 0)
        self.assertGreater(pixels[0], pixels[1])

    def test_bayer_patterns_and_offsets(self):
        for pattern in ("RGGB", "BGGR", "GBRG", "GRBG"):
            for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
                values = [{"R": 255, "G": 128, "B": 0}[pattern[((y + dy) % 2) * 2 + (x + dx) % 2]]
                          for y in range(4) for x in range(4)]
                write_fits(self.path, values, 4, 4, bitpix=8, extra={"BAYERPAT": repr(pattern), "XBAYROFF": str(dx), "YBAYROFF": str(dy)})
                w, h, pixels = png_pixels(self.render(self.path))
                self.assertEqual((w, h), (2, 2))
                self.assertEqual(pixels[0], 255)
                self.assertEqual(pixels[2], 0)

    def test_invalid_and_blank_pixels(self):
        write_fits(self.path, [float("nan"), float("inf"), 2, 3], bitpix=-32)
        self.assertEqual(png_pixels(self.render(self.path))[2][2:], b"\0\0")
        write_fits(self.path, [-32768, 1, 2, 3], extra={"BLANK": "-32768"})
        self.assertEqual(png_pixels(self.render(self.path))[2][2], 0)
        write_fits(self.path, [float("nan")] * 4, bitpix=-64)
        with self.assertRaisesRegex(ValueError, "有效像素"):
            self.render(self.path)

    def test_image_extension_and_long_headers(self):
        write_fits(self.path, [0, 1, 2, 3], extra={f"KEY{i}": "1" for i in range(320)})
        self.assertEqual(png_pixels(self.render(self.path))[:2], (2, 2))
        image = self.path.read_bytes().replace(b"SIMPLE  = T".ljust(80), b"XTENSION= 'IMAGE'".ljust(80), 1)
        primary = "".join(card.ljust(80) for card in ("SIMPLE  = T", "BITPIX  = 8", "NAXIS   = 0", "END")).encode().ljust(2880, b" ")
        self.path.write_bytes(primary + image)
        self.assertEqual(png_pixels(self.render(self.path))[:2], (2, 2))

    def test_linear_keeps_normalized_float_and_uint16_brightness(self):
        for bitpix, values, extra in ((-32, [0, .1, .5, 1], {}),
                                     (16, [-32768, -26214, 0, 32767], {"BZERO": "32768"})):
            write_fits(self.path, values, bitpix=bitpix, extra=extra)
            _, _, pixels = png_pixels(self.render(self.path, mode="linear"))
            self.assertEqual(pixels, bytes([128, 255, 0, 26]))

    def test_adaptive_background_is_dark_but_faint_signal_remains_visible(self):
        # Narrow, dim background plus a faint target and unsaturated bright core.
        values = [.02 + (i % 11 - 5) * .0002 for i in range(900)] + [.08] * 99 + [.8]
        write_fits(self.path, values, 100, 10, bitpix=-32)
        _, _, pixels = png_pixels(self.render(self.path))
        levels = sorted(pixels)
        self.assertTrue(20 <= levels[500] <= 32, levels[500])
        self.assertGreater(levels[990], levels[500] * 3)
        self.assertLess(levels[-1], 255, "do not percentile-clip the core to white")
        self.assertLess(sum(v == 0 for v in pixels), len(pixels) * .02)

    def test_dim_integer_counts_remain_visible_across_storage_depths(self):
        values = [1000 + (i % 11 - 5) * 5 for i in range(900)] + [6000] * 100
        for bitpix in (16, 32, 64):
            write_fits(self.path, values, 100, 10, bitpix=bitpix)
            pixels = sorted(png_pixels(self.render(self.path))[2])
            self.assertTrue(20 <= pixels[500] <= 32, (bitpix, pixels[500]))
            self.assertGreater(pixels[-1], 100)

    def test_stretch_history_prevents_double_stretch_without_using_filename(self):
        write_fits(self.path, [.01, .04, .1, .8], bitpix=-32)
        data = self.path.read_bytes().replace(b"END".ljust(80), b"HISTORY Autostretch (shadows: -2.80, target bg: 0.10, linked)".ljust(80) + b"END".ljust(80), 1)
        # Keep the header's 2880-byte block alignment.
        self.path.write_bytes(data[:2880] + data[2960:])
        auto = self.render(self.path)
        self.assertEqual(auto, self.render(self.path, mode="linear"))
        self.assertNotEqual(auto, self.render(self.path, mode="stretch"))

    def test_controls_change_only_preview_and_reset_is_deterministic(self):
        write_fits(self.path, [.02, .021, .023, .2], bitpix=-32)
        original = self.path.read_bytes()
        baseline = self.render(self.path)
        self.assertNotEqual(baseline, self.render(self.path, brightness=1))
        self.assertNotEqual(baseline, self.render(self.path, black=1))
        self.assertEqual(baseline, self.render(self.path, brightness=0, black=0))
        self.assertEqual(original, self.path.read_bytes())
        for options in ({"mode": "bad"}, {"black": "NaN"}, {"brightness": "inf"},
                        {"black": 4}, {"brightness": -3}):
            with self.assertRaises(ValueError):
                self.render(self.path, **options)

    def test_background_neutralization_is_optional(self):
        values = [v + offset for offset in (.003, 0, -.002) for v in [.02, .021, .019, .2]]
        write_fits(self.path, values, channels=3, bitpix=-32)
        default = self.render(self.path)
        self.assertEqual(default, self.render(self.path, neutralize=False))
        _, _, balanced = png_pixels(self.render(self.path, neutralize=True))
        self.assertNotEqual(default, self.render(self.path, neutralize=True))
        self.assertLessEqual(max(balanced[:3]) - min(balanced[:3]), 1)
