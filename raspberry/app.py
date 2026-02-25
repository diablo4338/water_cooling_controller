import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException

LOGGER = logging.getLogger("raspberry.app")

GPIO_PIN = 17  # BCM pin
LONG_PRESS_SECONDS = 5.0
SHORT_PRESS_SECONDS = 0.2

try:
    import RPi.GPIO as GPIO  # type: ignore

    _GPIO_AVAILABLE = True
except Exception as exc:  # pragma: no cover - only used on non-RPi hosts
    _GPIO_AVAILABLE = False

    class _GPIOStub:
        BCM = "BCM"
        OUT = "OUT"
        HIGH = 1
        LOW = 0

        def setmode(self, mode):
            LOGGER.warning("GPIO unavailable, using stub: setmode(%s)", mode)

        def setup(self, pin, mode, initial=None):
            LOGGER.warning(
                "GPIO unavailable, using stub: setup(pin=%s, mode=%s, initial=%s)",
                pin,
                mode,
                initial,
            )

        def output(self, pin, value):
            LOGGER.warning("GPIO unavailable, using stub: output(pin=%s, value=%s)", pin, value)

        def cleanup(self):
            LOGGER.warning("GPIO unavailable, using stub: cleanup()")

    GPIO = _GPIOStub()  # type: ignore
    LOGGER.warning("RPi.GPIO import failed: %s", exc)

@asynccontextmanager
async def _lifespan(_: FastAPI):
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(GPIO_PIN, GPIO.OUT, initial=GPIO.LOW)
    try:
        yield
    finally:
        GPIO.cleanup()


app = FastAPI(title="BLE Pairing Buttons", lifespan=_lifespan)
_lock = asyncio.Lock()


def _set_high():
    GPIO.output(GPIO_PIN, GPIO.HIGH)


def _set_low():
    GPIO.output(GPIO_PIN, GPIO.LOW)


async def _press(duration: float):
    if duration <= 0:
        raise ValueError("duration must be positive")

    async with _lock:
        _set_high()
        try:
            await asyncio.sleep(duration)
        finally:
            _set_low()


@app.get("/press/reset-long")
async def press_reset_long():
    try:
        await _press(LONG_PRESS_SECONDS)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"pin": GPIO_PIN, "duration_seconds": LONG_PRESS_SECONDS, "gpio": _GPIO_AVAILABLE}


@app.get("/press/pair")
async def press_pair_short():
    try:
        await _press(SHORT_PRESS_SECONDS)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"pin": GPIO_PIN, "duration_seconds": SHORT_PRESS_SECONDS, "gpio": _GPIO_AVAILABLE}

