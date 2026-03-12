import asyncio
import logging
from contextlib import asynccontextmanager
import RPi.GPIO as GPIO
from fastapi import FastAPI, HTTPException

LOGGER = logging.getLogger("raspberry.app")

GPIO_PIN = 17  # BCM pin
LONG_PRESS_SECONDS = 5.0


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
    return {"pin": GPIO_PIN}
