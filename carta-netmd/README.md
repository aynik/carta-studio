# carta-netmd

Standalone command-line USB NetMD & Type-S / Hi-MD protocol helper used by Carta Studio over an out-of-process NDJSON / CLI boundary.

## License

`carta-netmd` is licensed under the **GNU General Public License v2.0** (see [`LICENSE`](LICENSE)).

It incorporates and credits:
- **[netmd-js](https://github.com/cybercase/netmd-js)** (GPL-2.0) — Stefano Brilli & Asivery
- **[netmd-exploits](https://github.com/asivery/netmd-exploits)** (GPL-2.0) — Asivery
- **[netmd-tocmanip](https://github.com/asivery/netmd-tocmanip)** (GPL-2.0) — Asivery
- **[Porffor](https://github.com/CanadaHonk/porffor)** (MIT) — ahead-of-time JavaScript-to-C runtime
- **[libusb-1.0](https://libusb.info/)** (LGPL-2.1-or-later) — dynamically linked USB transport

## Standalone Usage

```sh
carta-netmd list
carta-netmd probe usb:054c:0188:p1
carta-netmd read-utoc usb:054c:0188:p1 --out sectors5.bin
carta-netmd erase usb:054c:0188:p1 --confirm-hardware
carta-netmd write-utoc usb:054c:0188:p1 --in sectors5.bin --mask 23 --confirm-hardware
carta-netmd upload usb:054c:0188:p1 --mode 0 --in span.bin --hw-title "Track 1" --confirm-hardware
```

All commands emit optional newline-delimited JSON progress lines (`{"event":"progress",...}`) followed by a single final result line (`{"event":"result","ok":true|false,...}`) on `stdout`.
