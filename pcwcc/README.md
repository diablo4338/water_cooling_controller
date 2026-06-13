# PCWCC

Python desktop application for the BLE water-cooling controller.

## Layout
- `src/pcwcc/` - application package
- `tests/` - pytest suite
- `packaging/` - packaging helpers/stubs

## Run
```bash
PYTHONPATH=src python -m pcwcc.main
```

## Test
```bash
PYTHONPATH=src python -m pytest -c pytest.ini -q -m "not integration"
```
