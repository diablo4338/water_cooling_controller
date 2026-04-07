import math
import threading
import time

import pigpio
import RPi.GPIO as GPIO

# BCM pin numbers. Adjust to match your wiring.
PWM_GPIO = 18
TACH_GPIO = 23
AUX_GPIO = 24

PWM_FREQ_HZ = 20000
TACH_PULSES_PER_REV = 2
TACH_STOP_TIMEOUT_US = 2_000_000

MAX_RPM = 5000
MIN_VALID_DT_US = int(60_000_000 / (MAX_RPM * TACH_PULSES_PER_REV))
MIN_VALID_DT_US = max(MIN_VALID_DT_US, 3000)

DT_BUF_SIZE = 7

_tach_lock = threading.Lock()
_tach_last_edge_ns = 0
_tach_last_valid_edge_ns = 0

_tach_dt_buf = [0] * DT_BUF_SIZE
_tach_dt_count = 0
_tach_dt_index = 0


def _tach_irq(_pin):
    global _tach_last_edge_ns
    global _tach_last_valid_edge_ns
    global _tach_dt_count
    global _tach_dt_index

    now_ns = time.monotonic_ns()
    with _tach_lock:
        last = _tach_last_edge_ns
        _tach_last_edge_ns = now_ns

        if not last:
            return

        dt_ns = now_ns - last
        if dt_ns <= 0:
            return

        dt_us = dt_ns // 1000
        if dt_us < MIN_VALID_DT_US:
            return

        last_valid = _tach_last_valid_edge_ns
        if last_valid:
            valid_dt_us = (now_ns - last_valid) // 1000
            if valid_dt_us < MIN_VALID_DT_US:
                return
        else:
            valid_dt_us = dt_us

        _tach_last_valid_edge_ns = now_ns
        _tach_dt_buf[_tach_dt_index] = valid_dt_us
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
    with _tach_lock:
        last_valid = _tach_last_valid_edge_ns
        count = _tach_dt_count
        vals = _tach_dt_buf[:count]

    if not last_valid:
        return math.nan

    if (time.monotonic_ns() - last_valid) > (TACH_STOP_TIMEOUT_US * 1000):
        return 0.0

    if not vals:
        return math.nan

    dt_us = _median(vals)
    if dt_us <= 0:
        return math.nan

    rpm = 60_000_000.0 / (dt_us * TACH_PULSES_PER_REV)
    return rpm


def pwm_set_percent(pi, percent):
    if percent < 0:
        percent = 0
    if percent > 100:
        percent = 100
    pi.set_PWM_dutycycle(PWM_GPIO, int(percent))


def main():
    pi = pigpio.pi()
    if not pi.connected:
        raise RuntimeError("pigpio daemon is not running (start with: sudo pigpiod)")

    GPIO.setmode(GPIO.BCM)
    GPIO.setup(TACH_GPIO, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    GPIO.setup(AUX_GPIO, GPIO.OUT, initial=GPIO.HIGH)

    pi.set_PWM_frequency(PWM_GPIO, PWM_FREQ_HZ)
    pi.set_PWM_range(PWM_GPIO, 100)
    pwm_set_percent(pi, 0)

    GPIO.add_event_detect(TACH_GPIO, GPIO.FALLING, callback=_tach_irq)

    step_seconds = 2.0
    step_sequence = [(pwm, GPIO.HIGH) for pwm in range(0, 101, 10)]
    step_sequence.append((100, GPIO.LOW))
    step_index = 0
    pwm_percent, aux_state = step_sequence[step_index]
    GPIO.output(AUX_GPIO, aux_state)
    pwm_set_percent(pi, pwm_percent)

    next_step = time.monotonic() + step_seconds
    next_print = time.monotonic() + 0.5

    try:
        while True:
            now = time.monotonic()

            if now >= next_step:
                step_index = (step_index + 1) % len(step_sequence)
                pwm_percent, aux_state = step_sequence[step_index]
                GPIO.output(AUX_GPIO, aux_state)
                pwm_set_percent(pi, pwm_percent)
                next_step += step_seconds

            if now >= next_print:
                rpm = tach_get_rpm()
                aux_label = "HIGH" if aux_state == GPIO.HIGH else "LOW"
                phase = "hold_low" if aux_state == GPIO.LOW else "normal"
                if math.isfinite(rpm):
                    print(
                        "tach=%.1f rpm, pwm=%d%%, aux=%s, phase=%s"
                        % (rpm, pwm_percent, aux_label, phase)
                    )
                else:
                    print(
                        "tach=nan, pwm=%d%%, aux=%s, phase=%s"
                        % (pwm_percent, aux_label, phase)
                    )
                next_print += 0.5

            time.sleep(0.005)
    finally:
        GPIO.remove_event_detect(TACH_GPIO)
        pi.set_PWM_dutycycle(PWM_GPIO, 0)
        pi.stop()
        GPIO.cleanup()


if __name__ == "__main__":
    main()
