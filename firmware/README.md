# Firmware

`ESP-IDF` firmware for the `ESP32-C3` controller. It implements the BLE protocol, pairing and authorization, temperature acquisition from 4 channels, and control for 4 fans.

## Structure
- `src/ble_protocol/` - BLE services, GATT, pairing, control, and application logic.
- `src/metrics/` - temperature metric acquisition.
- `src/i2c_bus/` - shared I2C bus support.
- `tests/` - firmware tests enabled with `PAIR_RUN_TESTS=1`.
- `include/` - shared top-level headers if they need to be moved out of component-local structure.
- `sdkconfig`, `sdkconfig.defaults` - build configuration.

## Build and flash
From the repository root:

```bash
make fw
```

Manual `ESP-IDF` invocation:

```bash
. ~/esp/esp-idf/export.sh
idf.py -C firmware fullclean build flash
```

Test firmware:

```bash
make fw-test
```

This build enables `PAIR_RUN_TESTS=1`.

## Release output
[`scripts/build_firmware.sh`](../scripts/build_firmware.sh) builds and places the following into `dist/`:
- the main `.bin`;
- `.elf`;
- `.map`, if generated;
- `bootloader.bin`;
- `partition-table.bin`;
- a flashing note file.

## Related docs
- [`../docs/firmware.md`](../docs/firmware.md) - short firmware summary.
- [`../docs/architecture.md`](../docs/architecture.md) - architecture notes and protocol details.
