# Third-Party Notices & Licensing

Carta Studio is structured into two cleanly separated executables to respect the
exact license terms of every upstream dependency:

1. **Carta Studio (`Carta Studio.app/Contents/MacOS/Carta Studio`)** — Built with
   **JUCE 8** (GNU AGPLv3 open-source license, or Commercial JUCE 8 EULA) and the
   **Carta** ATRAC codecs (`carta1`, `carta3`, `carta5`, ISC License) compiled via
   **Porffor** (MIT License). `Carta Studio` links **zero** GPL-2.0 or `libusb` code.
2. **`carta-netmd` (`Carta Studio.app/Contents/Helpers/carta-netmd`)** — A
   standalone **GNU GPL-2.0** CLI executable that handles USB NetMD / Type-S /
   ForceTOCEdit protocol communication over standard CLI arguments, temporary
   binary files, and NDJSON `stdout` events. See [carta-netmd/LICENSE](carta-netmd/LICENSE)
   for the full GPL-2.0 license text.

---

## Third-Party Components in Carta Studio

### JUCE 8
- **Upstream**: https://github.com/juce-framework/JUCE
- **License**: GNU Affero General Public License v3.0 (AGPL-3.0) / JUCE 8 Commercial EULA
- **Usage**: GUI, audio engine, and cross-platform application framework for Carta Studio.

### Carta ATRAC Codecs (`carta1`, `carta3`, `carta5`)
- **License**: ISC License
- **Usage**: ATRAC1 (SP Stereo/Mono), ATRAC3 (LP2/LP4), and ATRAC3plus encoding and decoding.

### Porffor
- **Upstream**: https://github.com/CanadaHonk/porffor
- **License**: MIT License
- **Usage**: Ahead-of-time JavaScript-to-C compiler used to build native dynamic libraries
  for `carta1`, `carta3`, `carta5`, and `carta-netmd`.

### minimp3 (optional, Linux default)
- **Upstream**: https://github.com/lieff/minimp3
- **License**: CC0-1.0 (Public Domain)
- **Usage**: Streaming MPEG-1 MP3 import on Linux.

---

## Third-Party Components in `carta-netmd` (Standalone GPL-2.0 Helper)

### netmd-js
- **Upstream**: https://github.com/cybercase/netmd-js
- **Copyright**: Copyright (c) 2020 Stefano Peris & contributors (derived from linux-minidisc)
- **License**: GNU General Public License v2.0 (GPL-2.0)

### netmd-exploits
- **Upstream**: https://github.com/asivery/netmd-exploits
- **Fork (Type-R SPUpload)**: https://github.com/aynik/netmd-exploits
- **Copyright**: Copyright (c) asivery & contributors
- **License**: GNU General Public License v2.0 (GPL-2.0)

### netmd-tocmanip
- **Upstream**: https://github.com/asivery/netmd-tocmanip
- **Copyright**: Copyright (c) asivery & contributors
- **License**: GNU General Public License v2.0 (GPL-2.0)

### libusb-1.0
- **Upstream**: https://libusb.info / https://github.com/libusb/libusb
- **License**: GNU Lesser General Public License v2.1 or later (LGPL-2.1-or-later)
- **Usage**: Dynamically linked by `carta-netmd` (`Contents/Frameworks/libusb-1.0.0.dylib`
  on macOS) for USB transport. Users may replace the bundled `libusb-1.0` shared
  library with a compatible build per LGPL-2.1 Section 6.
