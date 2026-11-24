from __future__ import annotations

import math
import time
from collections import deque
from collections.abc import Iterable
from dataclasses import dataclass
from datetime import datetime

from PySide6.QtCore import QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QFontMetrics, QIcon, QPainter, QPaintEvent, QPen, QPixmap, QResizeEvent
from PySide6.QtWidgets import QHBoxLayout, QPushButton, QSizePolicy, QVBoxLayout, QWidget

from ..core_logic.models import MetricsSnapshot
from ..ui.theme import theme_color


@dataclass(frozen=True)
class MetricSeries:
    name: str
    unit: str
    color: str
    style: Qt.PenStyle

    def pen(self, color: str | None = None) -> QPen:
        return QPen(theme_color(color or self.color), 2, self.style)


_CHANNEL_COLORS = ("danger_foreground", "warning_foreground", "success_foreground", "brand_foreground")
_CHANNEL_STYLES = (Qt.PenStyle.SolidLine, Qt.PenStyle.DashLine, Qt.PenStyle.DotLine, Qt.PenStyle.DashDotLine)
_SERIES = {
    "fan_percent": MetricSeries("Fan output", "%", "text_secondary", Qt.PenStyle.DashDotDotLine),
    **{
        f"temp{index}_c": MetricSeries(f"Temp {index}", "°C", color, style)
        for index, (color, style) in enumerate(zip(_CHANNEL_COLORS, _CHANNEL_STYLES), 1)
    },
    **{
        f"fan{index}_rpm": MetricSeries(f"Fan {index}", "RPM", color, style)
        for index, (color, style) in enumerate(zip(_CHANNEL_COLORS, reversed(_CHANNEL_STYLES)), 1)
    },
}
_DISPLAY_PRECISION = {"°C": 2, "RPM": 0, "%": 1}


@dataclass(frozen=True)
class MetricsChartPoint:
    timestamp: float
    values: dict[str, float]


class MetricsHistoryChart(QWidget):
    WINDOW_SECONDS = 30 * 60
    SERIES_GAP_SECONDS = 30.0

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._points: deque[MetricsChartPoint] = deque()
        self._last_nonzero: dict[str, float] = {}
        self._latest_sample: MetricsChartPoint | None = None
        self._display_full_history = False
        self._active_keys: tuple[str, ...] = ()

        self._legend = QWidget()
        self._legend.hide()
        self._legend_layout = QHBoxLayout(self._legend)
        self._legend_layout.setContentsMargins(0, 0, 0, 0)
        self._legend_layout.setSpacing(4)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._legend)
        layout.addSpacing(150)
        layout.addStretch()
        self._buttons: dict[str, QPushButton] = {}
        for key, series in _SERIES.items():
            button = QPushButton(series.name, self._legend)
            button.setProperty("uiRole", "chartLegend")
            button.setAccessibleName(f"Show {series.name} on chart")
            button.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
            button.setCheckable(True)
            button.setChecked(True)
            button.setIconSize(QSize(20, 8))
            button.hide()
            button.toggled.connect(lambda _checked, key=key: self._update_legend_swatch(key))
            self._buttons[key] = button
        self.refresh_theme()

    def set_online_mode(self) -> None:
        self._display_full_history = False
        self._latest_sample = None
        self._prune(time.time())
        self._refresh_legend()

    def set_offline_mode(self) -> None:
        self._display_full_history = True
        self._latest_sample = None
        self._refresh_legend()

    def add_sample(self, snapshot: MetricsSnapshot, timestamp: float | None = None) -> None:
        now = time.time() if timestamp is None else timestamp
        values = {
            **{f"temp{index}_c": value for index, value in enumerate(snapshot.temperatures, 1)},
            **{f"fan{index}_rpm": value for index, value in enumerate(snapshot.fan_speeds, 1)},
            "fan_percent": snapshot.fan_percent,
        }
        point = self._append_point(now, values)
        if not self._display_full_history:
            self._latest_sample = point
            self._prune(now)
        self._refresh_legend()

    def _append_point(self, timestamp: float, values: dict) -> MetricsChartPoint:
        readings: dict[str, float] = {}
        for key in _SERIES:
            value = values.get(key)
            if not isinstance(value, (float, int)) or not math.isfinite(value):
                continue
            if key == "fan_percent":
                value = min(100.0, max(0.0, value))
            readings[key] = value
            if value != 0:
                self._last_nonzero[key] = timestamp
        point = MetricsChartPoint(timestamp, readings)
        self._points.append(point)
        return point

    def set_points(self, points: Iterable[dict], display_full_history: bool = False) -> None:
        self._points.clear()
        self._last_nonzero.clear()
        self._latest_sample = None
        samples = []
        for item in points:
            try:
                timestamp = float(item["timestamp"])
            except (KeyError, TypeError, ValueError):
                continue
            if math.isfinite(timestamp):
                samples.append((timestamp, item))
        for timestamp, item in sorted(samples, key=lambda sample: sample[0]):
            self._append_point(timestamp, item)
        self._display_full_history = display_full_history
        if not self._display_full_history:
            self._prune(time.time())
        self._refresh_legend()

    def export_points(self) -> list[dict[str, float]]:
        min_ts = time.time() - self.WINDOW_SECONDS
        return [
            {"timestamp": point.timestamp, **point.values}
            for point in self._points
            if point.timestamp >= min_ts
        ]

    def clear(self) -> None:
        self._points.clear()
        self._last_nonzero.clear()
        self._latest_sample = None
        self._refresh_legend()

    def _prune(self, now: float) -> None:
        min_ts = now - self.WINDOW_SECONDS
        while self._points and self._points[0].timestamp < min_ts:
            self._points.popleft()
        self._last_nonzero = {
            key: timestamp for key, timestamp in self._last_nonzero.items() if timestamp >= min_ts
        }

    def _refresh_legend(self) -> None:
        active_keys = tuple(key for key in _SERIES if key in self._last_nonzero)
        if active_keys != self._active_keys:
            for key in self._active_keys:
                button = self._buttons[key]
                self._legend_layout.removeWidget(button)
                if key not in active_keys:
                    button.hide()
            for key in active_keys:
                button = self._buttons[key]
                self._legend_layout.addWidget(button, 1)
                button.show()
            self._active_keys = active_keys
            self._legend.setVisible(bool(active_keys))

        self._refresh_legend_captions()
        self.update()

    def _refresh_legend_captions(self) -> None:
        if not self._active_keys:
            return
        count = len(self._active_keys)
        cell_width = (self.contentsRect().width() - (count - 1) * self._legend_layout.spacing()) // count
        for key in self._active_keys:
            series = _SERIES[key]
            button = self._buttons[key]
            caption = series.name
            if not self._display_full_history:
                value = self._latest_sample.values.get(key) if self._latest_sample else None
                reading = f"{value:.{_DISPLAY_PRECISION[series.unit]}f} {series.unit}" if value else "—"
                caption = f"{caption}  {reading}"
            text_width = max(0, cell_width - button.iconSize().width() - 20)
            metrics = button.fontMetrics()
            short_caption = caption if metrics.horizontalAdvance(caption) <= text_width else series.name
            button.setText(metrics.elidedText(short_caption, Qt.TextElideMode.ElideRight, text_width))
            button.setToolTip(caption)

    def resizeEvent(self, event: QResizeEvent) -> None:
        super().resizeEvent(event)
        self._refresh_legend_captions()

    def refresh_theme(self) -> None:
        for key in self._buttons:
            self._update_legend_swatch(key)
        self._refresh_legend_captions()

    def _update_legend_swatch(self, key: str) -> None:
        button = self._buttons[key]
        swatch = QPixmap(button.iconSize())
        swatch.fill(Qt.GlobalColor.transparent)
        painter = QPainter(swatch)
        painter.setPen(_SERIES[key].pen(None if button.isChecked() else "border_accessible"))
        painter.drawLine(1, 4, 19, 4)
        painter.end()
        button.setIcon(QIcon(swatch))
        self.update()

    def paintEvent(self, _event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), theme_color("canvas_background"))

        metrics = QFontMetrics(painter.font())
        axis_margin = max(48, metrics.horizontalAdvance("-00.00") + 16)
        visible_keys = [key for key in self._active_keys if self._buttons[key].isChecked()]
        units = {_SERIES[key].unit for key in visible_keys}
        rpm_offset = axis_margin if {"%", "RPM"} <= units else 0
        legend_height = self._legend.geometry().bottom() + 1 if self._active_keys else 0
        top = legend_height + metrics.height() + 10
        rect = QRectF(self.rect()).adjusted(axis_margin + rpm_offset, top,
                                            -axis_margin, -metrics.height() - 16)
        if rect.width() <= 10 or rect.height() <= 10:
            return

        self._draw_frame(painter, rect)
        if not self._active_keys:
            self._draw_empty_state(painter, rect)
            return

        start_ts, end_ts = self._time_bounds()
        ranges = {}
        for unit in ("%", "RPM", "°C"):
            if unit not in units:
                continue
            if unit == "%":
                bounds = (0.0, 100.0)
            else:
                keys = [key for key in visible_keys if _SERIES[key].unit == unit]
                values = [point.values[key] for point in self._points for key in keys if point.values.get(key)]
                if unit == "RPM":
                    bounds = (min(0.0, min(values) * 1.12), max(1.0, max(values) * 1.12))
                else:
                    bounds = self._range_for(values)
            ranges[unit] = bounds
            self._draw_y_axis(painter, rect, *bounds, left=unit != "°C", label=unit,
                              offset=rpm_offset if unit == "RPM" else 0)

        self._draw_time_axis(painter, rect, start_ts, end_ts)
        for key in visible_keys:
            self._draw_series(painter, rect, start_ts, end_ts, key, *ranges[_SERIES[key].unit])

    @staticmethod
    def _range_for(values: list[float]) -> tuple[float, float]:
        min_value = min(values)
        max_value = max(values)
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
        span = value_max - value_min
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
        offset: float = 0,
    ) -> None:
        painter.setPen(theme_color("text_secondary"))
        metrics = QFontMetrics(painter.font())
        x_label = rect.left() - offset - metrics.horizontalAdvance(label) - 8 if left else rect.right() + 8
        painter.drawText(QPointF(x_label, rect.top() - metrics.height()), label)
        decimals = 2 if label == "°C" and value_max - value_min < 4 else 0
        for idx in range(5):
            ratio = idx / 4
            value = value_max - ((value_max - value_min) * ratio)
            text = f"{value:.{decimals}f}"
            y = rect.top() + rect.height() * ratio + metrics.ascent() / 2 - 2
            if left:
                x = rect.left() - offset - metrics.horizontalAdvance(text) - 8
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
        key: str,
        value_min: float,
        value_max: float,
    ) -> None:
        painter.setPen(_SERIES[key].pen())
        segment: list[QPointF] = []
        previous_timestamp = None
        for point in self._points:
            value = point.values.get(key)
            gap = previous_timestamp is not None and point.timestamp - previous_timestamp > self.SERIES_GAP_SECONDS
            if not value or gap:
                self._draw_segment(painter, segment)
                segment.clear()
            if value:
                x = self._map_x(rect, point.timestamp, start_ts, end_ts)
                y = self._map_y(rect, value, value_min, value_max)
                segment.append(QPointF(x, y))
            previous_timestamp = point.timestamp
        self._draw_segment(painter, segment)

    @staticmethod
    def _draw_segment(painter: QPainter, segment: list[QPointF]) -> None:
        if len(segment) > 1:
            painter.drawPolyline(segment)
        elif segment:
            painter.drawPoint(segment[0])
