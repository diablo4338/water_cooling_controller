from __future__ import annotations

from .base import BleCoreBaseMixin
from .control import BleCoreControlMixin
from .metrics import BleCoreMetricsMixin
from .pairing import BleCorePairingMixin


class BleAppCore(
    BleCoreBaseMixin,
    BleCorePairingMixin,
    BleCoreMetricsMixin,
    BleCoreControlMixin,
):
    """
    PUBLIC API used by both the GUI and integration tests.
    Do not change behavior without adjusting tests.
    """
