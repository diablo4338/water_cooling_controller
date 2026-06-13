# Raspberry button test helper

This folder contains a small FastAPI service for Raspberry Pi that simulates a long button press through a GPIO pin. The integration tests call this service over HTTP to put the controller into pairing mode.

## Files
- `app.py` - exposes `GET /press/reset-long` and drives the GPIO pin.
- `requirements.txt` - Python dependencies for Raspberry Pi: `fastapi`, `uvicorn`, `RPi.GPIO`.

## Wiring
- GPIO mode: `BCM`
- Output pin in code: `GPIO_PIN = 17`
- Raspberry Pi physical header pin for `BCM 17`: pin `11`
- Signal level: `3.3V`
- Common ground between Raspberry Pi and the target board is required.

The script sets `BCM 17` to `HIGH` for `5.0` seconds and then returns it to `LOW`.

## Run
Install dependencies from `requirements.txt`, then start the service from the repository root:

```bash
uvicorn tests.raspberry.app:app --host 0.0.0.0 --port 8001
```

Or use:

```bash
make run-buttons
```

## Notes
- `RPi.GPIO` works only on Raspberry Pi with access to GPIO.
- If the target input is not 3.3V-tolerant, connect through proper level shifting or interface circuitry.
