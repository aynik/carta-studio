# carta-himd

Standalone command-line Sony Hi-MD UMSC / SCSI protocol helper used by Carta Studio over an out-of-process NDJSON / CLI boundary.

## License

`carta-himd` is licensed under the **GNU General Public License v2.0** (see [`LICENSE`](LICENSE)).

It incorporates and credits:
- **[himd-js](https://github.com/asivery/himd-js)** (GPL-2.0) — Asivery
- **[nufatfs](https://github.com/asivery/nufatfs)** (GPL-2.0) — Asivery
- **[node-mass-storage](https://github.com/asivery/node-mass-storage)** (GPL-2.0) — Asivery
- **[Porffor](https://github.com/CanadaHonk/porffor)** (MIT) — ahead-of-time JavaScript-to-C runtime

## Standalone Usage

```sh
carta-himd list
carta-himd probe mock:mz-rh1
carta-himd upload mock:mz-rh1 --codec hi-sp --in track.at3p --title "Track 1" --artist "Artist" --album "Album" --confirm-hardware
carta-himd dump mock:mz-rh1 --track 0 --out track0.oma
carta-himd rename-disc mock:mz-rh1 --title "My Hi-MD" --confirm-hardware
carta-himd rename-track mock:mz-rh1 --track 0 --title "New Title" --confirm-hardware
carta-himd move-track mock:mz-rh1 --from 0 --to 1 --confirm-hardware
carta-himd set-groups mock:mz-rh1 --groups-json '[{"title":"Group A","indices":[0,1]}]' --confirm-hardware
carta-himd delete-tracks mock:mz-rh1 --tracks 0,1 --confirm-hardware
carta-himd wipe mock:mz-rh1 --confirm-hardware
carta-himd selftest-stack
```

All commands emit optional newline-delimited JSON progress lines (`{"event":"progress",...}`) followed by a single final result line (`{"event":"result","ok":true|false,...}`) on `stdout`.

## Native OS SCSI & USB BOT Transport

`carta-himd` enumerates connected physical Sony / Buffalo Hi-MD devices across OS SCSI pass-through and USB Mass Storage Bulk-Only Transport (`libusb-1.0`), ignoring NetMD-only hardware:

- **macOS**: Inspects `IOUSBHostDevice` / `IOUSBDevice` entries (`macos:<vid>:<pid>:<locationID>`), unmounts any mounted whole BSD disk via `DiskArbitration`, uses IOKit `SCSITaskDeviceInterface` (`SCSITaskUserClient`) when present, and falls back to `libusb-1.0` BOT (reporting clear `IOUSBMassStorageDriver` ownership diagnostics if exclusive USB claim is blocked).
- **Linux**: Enumerates `/sys/class/scsi_generic/sg*` (`sg:/dev/sgN`) via `SG_IO` (blocking writes while the sibling `/dev/sdX*` is mounted in `/proc/mounts`) and falls back to `libusb-1.0` BOT (`usb:bus:addr`).
- **Windows**: Enumerates `\\.\PhysicalDriveN` (`spti:PhysicalDriveN`) via `IOCTL_SCSI_PASS_THROUGH_DIRECT` (locking & dismounting logical volumes before mutating commands) with `libusb-1.0` WinUSB BOT fallback.
