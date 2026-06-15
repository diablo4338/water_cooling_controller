# PC Water Cooling Controller

An end-to-end fan controller solution built around a custom PCB and an `ESP32-C3` controller. The repository covers the full product scope: device firmware, a desktop application for Windows and Linux, PCB manufacturing files, and the bill of materials.

The controller is designed for:
- up to 4 fans;
- up to 4 `10k` thermistors.

The desktop application works over BLE and supports pairing mode, controller connection, telemetry monitoring, and cooling parameter updates. The hardware lives in `hardware/`, and the release pipeline builds a unified set of artifacts through [`.github/workflows/release.yml`](.github/workflows/release.yml).

## Repository structure
- [`firmware/`](firmware/README.md) - `ESP-IDF` firmware for `ESP32-C3`.
- [`pcwcc/`](pcwcc/README.md) - desktop GUI for Windows and Linux.
- [`hardware/`](hardware/README.md) - KiCad project, BOM, and production exports for the PCB.
- [`tests/raspberry/`](tests/raspberry/README.md) - Raspberry Pi helper for hardware integration tests.
- [`docker/`](docker/README.md) - local Docker environment for release-build verification.
- [`docs/`](docs/) - additional notes on architecture, CLI, release flow, and hardware.

## Release artifacts
The `scripts/make_release.sh` script and the `Release` workflow produce:
- firmware for `ESP32-C3`;
- a Linux `AppImage` for the desktop client;
- a production archive with `Gerber` and `drill` files for PCB manufacturing;
- checksums;
- the BOM is stored separately in [`hardware/production/bom/main.csv`](hardware/production/bom/main.csv).

Build details and structure notes are kept in the `README` files of the corresponding directories.

## License

This project is licensed for non-commercial use only.

Commercial use is not permitted without prior written permission from the author.

See [LICENSE](LICENSE) for details.

## Safety disclaimer

This is an experimental DIY hardware/software project. Use it at your own risk.
Incorrect wiring, firmware, configuration, or installation may cause hardware
damage, overheating, fire, or injury.

See [DISCLAIMER.md](DISCLAIMER.md).