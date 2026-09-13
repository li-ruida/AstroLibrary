# Native backend dependencies

All third-party code is version-pinned and compiled into the executable. Building does not download dependencies. Runtime libraries (Foundation, AppKit, ImageIO, CoreGraphics, SQLite, zlib and CommonCrypto) are provided by macOS.

| Library | Version/source | License | SHA-256 of vendored header |
| --- | --- | --- | --- |
| nlohmann/json | [v3.12.0](https://github.com/nlohmann/json/tree/v3.12.0) | MIT, `vendor/json.LICENSE.MIT` | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| cpp-httplib | [v0.54.1](https://github.com/yhirose/cpp-httplib/tree/v0.54.1) | MIT, `vendor/httplib.LICENSE` | `5933c14b2d0f45212925ed18ca579841f5fce717f431fc20cec712423e905b10` |

Outbound HTTPS uses NSURLSession with system certificate verification and redirects disabled. cpp-httplib only serves loopback HTTP and does not require OpenSSL.
