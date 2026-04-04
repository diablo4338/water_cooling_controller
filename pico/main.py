import math
import time
from machine import Pin, PWM, disable_irq, enable_irq

PWM_GPIO = 1
TACH_GPIO = 0

PWM_FREQ_HZ = 20000
TACH_PULSES_PER_REV = 2
TACH_STOP_TIMEOUT_US = 2_000_000

MAX_RPM = 5000
MIN_VALID_DT_US = int(60_000_000 / (MAX_RPM * TACH_PULSES_PER_REV))
MIN_VALID_DT_US = max(MIN_VALID_DT_US, 3000)

DT_BUF_SIZE = 7

_tach_last_edge_us = 0
_tach_last_valid_edge_us = 0

_tach_dt_buf = [0] * DT_BUF_SIZE
_tach_dt_count = 0
_tach_dt_index = 0


def _tach_irq(_pin):
    global _tach_last_edge_us
    global _tach_last_valid_edge_us
    global _tach_dt_count
    global _tach_dt_index

    now = time.ticks_us()
    last = _tach_last_edge_us
    _tach_last_edge_us = now

    if not last:
        return

    dt = time.ticks_diff(now, last)
    if dt <= 0:
        return

    if dt < MIN_VALID_DT_US:
        return

    last_valid = _tach_last_valid_edge_us
    if last_valid:
        valid_dt = time.ticks_diff(now, last_valid)
        if valid_dt < MIN_VALID_DT_US:
            return
    else:
        valid_dt = dt

    _tach_last_valid_edge_us = now
    _tach_dt_buf[_tach_dt_index] = valid_dt
    _tach_dt_index = (_tach_dt_index + 1) % DT_BUF_SIZE
    if _tach_dt_count < DT_BUF_SIZE:
        _tach_dt_count += 1


def _median(values):
    vals = sorted(values)
    n = len(vals)
    mid = n // 2
    if n % 2:
        return vals[mid]
    return (vals[mid - 1] + vals[mid]) / 2


def tach_get_rpm():
    irq_state = disable_irq()
    last_valid = _tach_last_valid_edge_us
    count = _tach_dt_count
    vals = _tach_dt_buf[:count]
    enable_irq(irq_state)

    if not last_valid:
        return math.nan

    if time.ticks_diff(time.ticks_us(), last_valid) > TACH_STOP_TIMEOUT_US:
        return 0.0

    if not vals:
        return math.nan

    dt_us = _median(vals)
    if dt_us <= 0:
        return math.nan

    rpm = 60_000_000.0 / (dt_us * TACH_PULSES_PER_REV)
    return rpm


def pwm_set_percent(pwm, percent):
    if percent < 0:
        percent = 0
    if percent > 100:
        percent = 100
    duty = int(65535 * percent / 100)
    pwm.duty_u16(duty)


def main():
    pwm = PWM(Pin(PWM_GPIO))
    pwm.freq(PWM_FREQ_HZ)
    pwm_set_percent(pwm, 10)

    tach = Pin(TACH_GPIO, Pin.IN, Pin.PULL_UP)
    tach.irq(trigger=Pin.IRQ_FALLING, handler=_tach_irq)

    pwm_percent = 10
    next_step = time.ticks_add(time.ticks_ms(), 3000)
    next_print = time.ticks_add(time.ticks_ms(), 500)

    while True:
        now_ms = time.ticks_ms()

        if time.ticks_diff(now_ms, next_step) >= 0:
            pwm_percent += 10
            if pwm_percent > 100:
                pwm_percent = 10
            pwm_set_percent(pwm, pwm_percent)
            next_step = time.ticks_add(next_step, 3000)

        if time.ticks_diff(now_ms, next_print) >= 0:
            rpm = tach_get_rpm()
            if math.isfinite(rpm):
                print("tach=%.1f rpm, pwm=%d%%" % (rpm, pwm_percent))
            else:
                print("tach=nan, pwm=%d%%" % pwm_percent)
            next_print = time.ticks_add(next_print, 500)

        time.sleep_ms(5)


if __name__ == "__main__":
    main()