from __future__ import annotations

import math
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from typing import Iterable, Optional

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFontMetrics, QPainter, QPen
from PySide6.QtWidgets import QSizePolicy, QWidget


@dataclass(frozen=True)
class MetricsChartPoint:
    timestamp: float
    fan_percent: Optional[float]
    temp4_c: Optional[float]
    temp3_c: Optional[float]


class MetricsHistoryChart(QWidget):
    WINDOW_SECONDS = 30 * 60
    SERIES_GAP_SECONDS = 30.0

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._points: deque[MetricsChartPoint] = deque()
        self._display_full_history = False
        self.setMinimumHeight(180)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set_online_mode(self) -> None:
        self._display_full_history = False
        self._prune(time.time())
        self.update()

    def set_offline_mode(self) -> None:
        self._display_full_history = True
        self.update()

    def add_sample(
        self,
        fan_percent: Optional[float],
        temp4_c: Optional[float],
        temp3_c: Optional[float],
        timestamp: Optional[float] = None,
    ) -> None:
        now = time.time() if timestamp is None else timestamp
        self._points.append(
            MetricsChartPoint(
                timestamp=now,
                fan_percent=self._percent_or_none(fan_percent),
                temp4_c=self._finite_or_none(temp4_c),
                temp3_c=self._finite_or_none(temp3_c),
            )
        )
        self._prune(now)
        self.update()

    def set_points(self, points: Iterable[dict], display_full_history: bool = False) -> None:
        self._points.clear()
        for item in points:
            try:
                timestamp = float(item["timestamp"])
            except (KeyError, TypeError, ValueError):
                continue
            self._points.append(
                MetricsChartPoint(
                    timestamp=timestamp,
                    fan_percent=self._percent_or_none(item.get("fan_percent")),
                    temp4_c=self._finite_or_none(item.get("temp4_c")),
                    temp3_c=self._finite_or_none(item.get("temp3_c")),
                )
            )
        self._points = deque(sorted(self._points, key=lambda point: point.timestamp))
        self._display_full_history = display_full_history
        if not self._display_full_history:
            self._prune(time.time())
        self.update()

    def export_points(self) -> list[dict]:
        self._prune(time.time())
        return [
            {
                "timestamp": point.timestamp,
                "fan_percent": point.fan_percent,
                "temp4_c": point.temp4_c,
                "temp3_c": point.temp3_c,
            }
            for point in self._points
        ]

    def clear(self) -> None:
        self._points.clear()
        self.update()

    def _prune(self, now: float) -> None:
        min_ts = now - self.WINDOW_SECONDS
        while self._points and self._points[0].timestamp < min_ts:
            self._points.popleft()

    @staticmethod
    def _finite_or_none(value: Optional[float]) -> Optional[float]:
        if value is None or not math.isfinite(value):
            return None
        return value

    @classmethod
    def _percent_or_none(cls, value: Optional[float]) -> Optional[float]:
        value = cls._finite_or_none(value)
        if value is None:
            return None
        return min(100.0, max(0.0, value))

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor("#ffffff"))

        rect = self.rect().adjusted(56, 18, -58, -36)
        if rect.width() <= 10 or rect.height() <= 10:
            return

        self._draw_frame(painter, rect)
        if not self._points:
            self._draw_empty_state(painter, rect)
            return

        start_ts, end_ts = self._time_bounds()

        percent_min, percent_max = 0.0, 100.0
        temp_values = [
            value
            for p in self._points
            for value in (p.temp4_c, p.temp3_c)
            if value is not None
        ]
        temp_min, temp_max = self._range_for(temp_values, floor_zero=False)

        self._draw_y_axis(painter, rect, percent_min, percent_max, left=True, label="%")
        self._draw_y_axis(painter, rect, temp_min, temp_max, left=False, label="deg C")
        self._draw_time_axis(painter, rect, start_ts, end_ts)

        self._draw_series(
            painter,
            rect,
            start_ts,
            end_ts,
            value_getter=lambda p: p.fan_percent,
            value_min=percent_min,
            value_max=percent_max,
            color=QColor("#2563eb"),
        )
        self._draw_series(
            painter,
            rect,
            start_ts,
            end_ts,
            value_getter=lambda p: p.temp4_c,
            value_min=temp_min,
            value_max=temp_max,
            color=QColor("#dc2626"),
        )
        self._draw_series(
            painter,
            rect,
            start_ts,
            end_ts,
            value_getter=lambda p: p.temp3_c,
            value_min=temp_min,
            value_max=temp_max,
            color=QColor("#f6ad55"),
        )
        self._draw_legend(painter, rect)

    @staticmethod
    def _range_for(values: list[float], floor_zero: bool) -> tuple[float, float]:
        if not values:
            return (0.0, 1.0)
        min_value = min(values)
        max_value = max(values)
        if floor_zero:
            min_value = min(0.0, min_value)
        if math.isclose(min_value, max_value):
            padding = max(abs(max_value) * 0.1, 1.0)
        else:
            padding = (max_value - min_value) * 0.12
        return min_value - padding, max_value + padding

    @staticmethod
    def _map_x(rect: QRectF, timestamp: float, start_ts: float, end_ts: float) -> float:
        span = max(end_ts - start_ts, 1.0)
        return rect.left() + ((timestamp - start_ts) / span) * rect.width()

    @staticmethod
    def _map_y(rect: QRectF, value: float, value_min: float, value_max: float) -> float:
        span = max(value_max - value_min, 1.0)
        return rect.bottom() - ((value - value_min) / span) * rect.height()

    def _draw_frame(self, painter: QPainter, rect: QRectF) -> None:
        painter.setPen(QPen(QColor("#d1d5db"), 1))
        painter.drawRect(rect)
        painter.setPen(QPen(QColor("#eef2f7"), 1))
        for idx in range(1, 4):
            y = rect.top() + rect.height() * idx / 4
            painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y))

    def _time_bounds(self) -> tuple[float, float]:
        if not self._points:
            now = time.time()
            return now - self.WINDOW_SECONDS, now
        end_ts = self._points[-1].timestamp
        if self._display_full_history:
            start_ts = self._points[0].timestamp
            if math.isclose(start_ts, end_ts):
                start_ts -= 1.0
                end_ts += 1.0
            return start_ts, end_ts
        return end_ts - self.WINDOW_SECONDS, end_ts

    def _draw_empty_state(self, painter: QPainter, rect: QRectF) -> None:
        painter.setPen(QColor("#6b7280"))
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, "Waiting for notify data")

    def _draw_y_axis(
        self,
        painter: QPainter,
        rect: QRectF,
        value_min: float,
        value_max: float,
        left: bool,
        label: str,
    ) -> None:
        painter.setPen(QColor("#374151"))
        metrics = QFontMetrics(painter.font())
        x_label = rect.left() - 46 if left else rect.right() + 8
        painter.drawText(QPointF(x_label, rect.top() - 4), label)
        for idx in range(5):
            ratio = idx / 4
            value = value_max - ((value_max - value_min) * ratio)
            text = f"{value:.0f}"
            y = rect.top() + rect.height() * ratio + metrics.ascent() / 2 - 2
            if left:
                x = rect.left() - metrics.horizontalAdvance(text) - 8
            else:
                x = rect.right() + 8
            painter.drawText(QPointF(x, y), text)

    def _draw_time_axis(self, painter: QPainter, rect: QRectF, start_ts: float, end_ts: float) -> None:
        painter.setPen(QColor("#374151"))
        metrics = QFontMetrics(painter.font())
        for idx in range(4):
            timestamp = start_ts + (end_ts - start_ts) * idx / 3
            text = datetime.fromtimestamp(max(0.0, timestamp)).strftime("%H:%M")
            x = self._map_x(rect, timestamp, start_ts, end_ts)
            painter.drawText(
                QPointF(x - metrics.horizontalAdvance(text) / 2, rect.bottom() + metrics.height() + 4),
                text,
            )

    def _draw_series(
        self,
        painter: QPainter,
        rect: QRectF,
        start_ts: float,
        end_ts: float,
        value_getter,
        value_min: float,
        value_max: float,
        color: QColor,
    ) -> None:
        painter.setPen(QPen(color, 2))
        current_segment: list[QPointF] = []
        previous_timestamp: Optional[float] = None
        for point in self._points:
            value = value_getter(point)
            if value is None:
                if len(current_segment) > 1:
                    painter.drawPolyline(current_segment)
                current_segment = []
                previous_timestamp = None
                continue
            if previous_timestamp is not None and point.timestamp - previous_timestamp > self.SERIES_GAP_SECONDS:
                if len(current_segment) > 1:
                    painter.drawPolyline(current_segment)
                elif len(current_segment) == 1:
                    painter.drawPoint(current_segment[0])
                current_segment = []
            x = self._map_x(rect, point.timestamp, start_ts, end_ts)
            y = self._map_y(rect, value, value_min, value_max)
            current_segment.append(QPointF(x, y))
            previous_timestamp = point.timestamp
        if len(current_segment) > 1:
            painter.drawPolyline(current_segment)
        elif len(current_segment) == 1:
            painter.drawPoint(current_segment[0])

    def _draw_legend(self, painter: QPainter, rect: QRectF) -> None:
        items = (("Fan %", QColor("#2563eb")), ("Temp 4", QColor("#dc2626")), ("Temp 3", QColor("#f6ad55")))
        x = rect.left()
        y = rect.top() - 6
        metrics = QFontMetrics(painter.font())
        for label, color in items:
            painter.setPen(QPen(color, 3))
            painter.drawLine(QPointF(x, y), QPointF(x + 18, y))
            painter.setPen(QColor("#374151"))
            painter.drawText(QPointF(x + 24, y + metrics.ascent() / 2 - 2), label)
            x += 86
