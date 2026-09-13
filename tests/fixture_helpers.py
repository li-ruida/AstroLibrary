"""Synthetic FIT writer and decoder for the filter-zero PNG test format."""
import struct
import zlib

def write_fits(path, values, width=2, height=2, bitpix=16, channels=1, extra=None):
    header = {"SIMPLE": "T", "BITPIX": str(bitpix), "NAXIS": "3" if channels > 1 else "2",
              "NAXIS1": str(width), "NAXIS2": str(height)}
    if channels > 1:
        header["NAXIS3"] = str(channels)
    header.update(extra or {})
    cards = "".join(f"{key:8}= {value}".ljust(80) for key, value in header.items()) + "END".ljust(80)
    data = struct.pack(">" + {8:"B",16:"h",32:"i",64:"q",-32:"f",-64:"d"}[bitpix] * len(values), *values)
    path.write_bytes(cards.encode().ljust((len(cards) + 2879) // 2880 * 2880, b" ") + data)

def png_pixels(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    width, height, _, kind = struct.unpack(">IIBB", data[16:26])
    assert data[24] == 8 and kind in (0, 2), "Expected 8-bit grayscale or RGB PNG"
    pos, compressed = 8, b""
    while pos < len(data):
        size = struct.unpack(">I", data[pos:pos + 4])[0]
        if data[pos + 4:pos + 8] == b"IDAT":
            compressed += data[pos + 8:pos + 8 + size]
        pos += size + 12
    rows = zlib.decompress(compressed)
    stride = width * (3 if kind == 2 else 1)
    assert len(rows) == height * (stride + 1)
    assert all(rows[y * (stride + 1)] == 0 for y in range(height)), "Expected PNG filter zero"
    return width, height, b"".join(rows[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(height))
