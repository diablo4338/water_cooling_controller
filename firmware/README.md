# Firmware

ESP-IDF firmware project for the BLE controller.

## Layout
- `src/` - ESP-IDF components
- `tests/` - firmware test sources enabled with `PAIR_RUN_TESTS=1`
- `include/` - reserved for shared top-level headers

## Build
```bash
idf.py -C firmware build
```
