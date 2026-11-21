"""Fluent checkbox indicator, with Qt retaining layout and input handling."""

from typing import Protocol, cast

from PySide6.QtCore import QPointF, QRect, QRectF, Qt
from PySide6.QtGui import QColor, QFontMetrics, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QProxyStyle, QStyle, QStyleOption, QWidget


class _StyleOptionFields(Protocol):
    # PySide exposes these Qt fields at runtime but omits them from the stub.
    fontMetrics: QFontMetrics
    rect: QRect
    state: QStyle.StateFlag


class FluentControlStyle(QProxyStyle):
    def __init__(self, colors: dict[str, str]) -> None:
        super().__init__("Fusion")
        self.colors = colors

    def pixelMetric(
        self,
        metric: QStyle.PixelMetric,
        option: QStyleOption | None = None,
        widget: QWidget | None = None,
    ) -> int:
        if metric in (QStyle.PixelMetric.PM_IndicatorWidth, QStyle.PixelMetric.PM_IndicatorHeight):
            if option is None:
                return 20
            fields = cast(_StyleOptionFields, cast(object, option))
            return max(20, fields.fontMetrics.height() + 2)
        return super().pixelMetric(metric, option, widget)

    def drawPrimitive(
        self,
        element: QStyle.PrimitiveElement,
        option: QStyleOption,
        painter: QPainter,
        widget: QWidget | None = None,
    ) -> None:
        if element != QStyle.PrimitiveElement.PE_IndicatorCheckBox:
            super().drawPrimitive(element, option, painter, widget)
            return

        fields = cast(_StyleOptionFields, cast(object, option))
        state = QStyle.StateFlag
        checked = bool(fields.state & (state.State_On | state.State_NoChange))
        enabled = bool(fields.state & state.State_Enabled)
        fill = "brand_background" if checked else "canvas_background"
        border = "brand_foreground" if checked else "border_accessible"
        if not enabled:
            fill, border = "surface_disabled", "border_disabled"
        elif fields.state & state.State_Sunken:
            fill = "brand_background_pressed" if checked else "surface_background_pressed"
        elif fields.state & state.State_MouseOver:
            fill = "brand_background_hover" if checked else "surface_background_hover"

        rect = QRectF(fields.rect).adjusted(1, 1, -1, -1)
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QPen(QColor(self.colors[border]), 1.5))
        painter.setBrush(QColor(self.colors[fill]))
        painter.drawRoundedRect(rect, 4, 4)
        if checked:
            mark = "text_on_brand" if enabled else "text_secondary"
            painter.setPen(QPen(QColor(self.colors[mark]), 2, Qt.PenStyle.SolidLine,
                                Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
            if fields.state & state.State_NoChange:
                painter.drawLine(QPointF(rect.left() + 4, rect.center().y()),
                                 QPointF(rect.right() - 4, rect.center().y()))
            else:
                painter.drawPolyline(QPolygonF([
                    QPointF(rect.left() + rect.width() * 0.24, rect.top() + rect.height() * 0.51),
                    QPointF(rect.left() + rect.width() * 0.43, rect.top() + rect.height() * 0.70),
                    QPointF(rect.left() + rect.width() * 0.77, rect.top() + rect.height() * 0.34),
                ]))
        painter.restore()
