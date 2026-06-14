# Raspberry button test helper

A small `FastAPI` service for Raspberry Pi that simulates a long hardware-button press over GPIO. It is used by integration tests when the controller must be switched into pairing mode without manual intervention.

## Contents
- `app.py` - HTTP endpoint `GET /press/reset-long` that drives the GPIO pin.
- `requirements.txt` - Raspberry Pi dependencies: `fastapi`, `uvicorn`, `RPi.GPIO`.

## Wiring
- GPIO mode: `BCM`;
- output pin in code: `GPIO_PIN = 17`;
- Raspberry Pi physical header pin for `BCM 17`: `11`;
- signal level: `3.3V`;
- common `GND` between the Raspberry Pi and the target board is required.

The service holds `BCM 17` at `HIGH` for `5.0` seconds and then returns the line to `LOW`.

## Run
From the repository root:

```bash
uvicorn tests.raspberry.app:app --host 0.0.0.0 --port 8001
```

or:

```bash
make run-buttons
```

## Constraints
- `RPi.GPIO` only works on Raspberry Pi with GPIO access;
- if the target board input is not `3.3V` tolerant, use proper level shifting or interface circuitry.
