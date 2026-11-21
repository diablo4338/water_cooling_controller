from __future__ import annotations

import math
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from collections.abc import Callable, Iterable
from typing import Optional

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QFontMetrics, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import QSizePolicy, QWidget

from ..ui.theme import theme_color


_SERIES_APPEARANCE: tuple[tuple[str, str, Qt.PenStyle], ...] = (
    ("Fan output", "brand_foreground", Qt.PenStyle.SolidLine),
    ("Temp 4", "danger_foreground", Qt.PenStyle.DashLine),
    ("Temp 3", "warning_foreground", Qt.PenStyle.DotLine),
)


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
        self._latest_sample: MetricsChartPoint | None = None
        self._display_full_history = False
        self.setMinimumHeight(150)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_online_mode(self) -> None:
        self._display_full_history = False
        self._latest_sample = None
        self._prune(time.time())
        self.update()

    def set_offline_mode(self) -> None:
        self._display_full_history = True
        self._latest_sample = None
        self.update()

    def add_sample(
        self,
        fan_percent: Optional[float],
        temp4_c: Optional[float],
        temp3_c: Optional[float],
        timestamp: Optional[float] = None,
    ) -> None:
        now = time.time() if timestamp is None else timestamp
        point = MetricsChartPoint(
            timestamp=now,
            fan_percent=self._percent_or_none(fan_percent),
            temp4_c=self._finite_or_none(temp4_c),
            temp3_c=self._finite_or_none(temp3_c),
        )
        self._points.append(point)
        if not self._display_full_history:
            self._latest_sample = point
        self._prune(now)
        self.update()

    def set_points(self, points: Iterable[dict[str, float | None]], display_full_history: bool = False) -> None:
        self._points.clear()
        self._latest_sample = None
        for item in points:
            try:
                raw_timestamp = item["timestamp"]
                if raw_timestamp is None:
                    continue
                timestamp = float(raw_timestamp)
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

    def export_points(self) -> list[dict[str, float | None]]:
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
        self._latest_sample = None
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

    def paintEvent(self, _event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), theme_color("canvas_background"))

        metrics = QFontMetrics(painter.font())
        axis_margin = max(48, metrics.horizontalAdvance("0000") + 16)
        legend = self._legend_captions()
        available_width = self.width() - 2 * axis_margin
        preferred_column_width = metrics.horizontalAdvance("Fan output  100.0%") + 42
        columns = min(len(legend), max(1, available_width // preferred_column_width))
        column_width = min(available_width / columns, preferred_column_width)
        legend_rows = (len(legend) + columns - 1) // columns
        top = (legend_rows + 1) * metrics.height() + 8
        rect = QRectF(self.rect()).adjusted(axis_margin, top, -axis_margin, -metrics.height() - 16)
        if rect.width() <= 10 or rect.height() <= 10:
            return

        self._draw_legend(painter, rect, legend, columns, legend_rows, column_width)
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
            appearance=_SERIES_APPEARANCE[0],
        )
        self._draw_series(
            painter,
            rect,
            start_ts,
            end_ts,
            value_getter=lambda p: p.temp4_c,
            value_min=temp_min,
            value_max=temp_max,
            appearance=_SERIES_APPEARANCE[1],
        )
        self._draw_series(
            painter,
            rect,
            start_ts,
            end_ts,
            value_getter=lambda p: p.temp3_c,
            value_min=temp_min,
            value_max=temp_max,
            appearance=_SERIES_APPEARANCE[2],
        )

    def _legend_captions(self) -> tuple[str, ...]:
        names = tuple(name for name, _, _ in _SERIES_APPEARANCE)
        if self._display_full_history:
            return names
        latest = self._latest_sample
        fan = latest.fan_percent if latest else None
        temp4 = latest.temp4_c if latest else None
        temp3 = latest.temp3_c if latest else None
        readings = (
            f"{fan:.1f}%" if fan is not None else "—",
            f"{temp4:.2f} °C" if temp4 is not None else "—",
            f"{temp3:.2f} °C" if temp3 is not None else "—",
        )
        return tuple(f"{name}  {reading}" for name, reading in zip(names, readings))

    @staticmethod
    def _range_for(values: list[float], floor_zero: bool) -> tuple[float, float]:
        if not values:
            return 0.0, 1.0
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

    @staticmethod
    def _draw_frame(painter: QPainter, rect: QRectF) -> None:
        painter.setPen(QPen(theme_color("border_primary"), 1))
        painter.drawRect(rect)
        painter.setPen(QPen(theme_color("border_subtle"), 1))
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

    @staticmethod
    def _draw_empty_state(painter: QPainter, rect: QRectF) -> None:
        painter.setPen(theme_color("text_secondary"))
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, "No metric history yet")

    @staticmethod
    def _draw_y_axis(
        painter: QPainter,
        rect: QRectF,
        value_min: float,
        value_max: float,
        left: bool,
        label: str,
    ) -> None:
        painter.setPen(theme_color("text_secondary"))
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
        painter.setPen(theme_color("text_secondary"))
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
        value_getter: Callable[[MetricsChartPoint], float | None],
        value_min: float,
        value_max: float,
        appearance: tuple[str, str, Qt.PenStyle],
    ) -> None:
        _, alias, style = appearance
        painter.setPen(QPen(theme_color(alias), 2, style))
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

    @staticmethod
    def _draw_legend(
        painter: QPainter, rect: QRectF, captions: tuple[str, ...], columns: int, rows: int, column_width: float
    ) -> None:
        metrics = QFontMetrics(painter.font())
        for index, (label, (_, alias, style)) in enumerate(zip(captions, _SERIES_APPEARANCE)):
            row, column = divmod(index, columns)
            x = rect.left() + column * column_width
            y = rect.top() - (rows - row) * metrics.height() - 6
            painter.setPen(QPen(theme_color(alias), 2, style))
            painter.drawLine(QPointF(x, y), QPointF(x + 18, y))
            painter.setPen(theme_color("text_primary"))
            text = metrics.elidedText(label, Qt.TextElideMode.ElideRight, max(0, int(column_width) - 30))
            painter.drawText(QPointF(x + 24, y + metrics.ascent() / 2 - 2), text)
