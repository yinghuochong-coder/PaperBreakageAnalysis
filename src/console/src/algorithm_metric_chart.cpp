#include "paperbreak/console/algorithm_metric_chart.hpp"

#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <limits>

namespace paperbreak::console
{
namespace
{
constexpr int left_margin = 62;
constexpr int right_margin = 20;
constexpr int top_margin = 34;
constexpr int bottom_margin = 36;

QString value_text(const double value, const bool boolean_series)
{
    if (boolean_series)
        return value >= 0.5 ? QStringLiteral("是") : QStringLiteral("否");
    return QString::number(value, 'g', 8);
}
} // namespace

AlgorithmMetricChart::AlgorithmMetricChart(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(230);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void AlgorithmMetricChart::set_series(QString metric_name, QString unit,
                                      std::vector<AlgorithmChartPoint> points,
                                      const bool boolean_series)
{
    metric_name_ = std::move(metric_name);
    unit_ = std::move(unit);
    points_ = std::move(points);
    boolean_series_ = boolean_series;
    hovered_index_ = -1;
    update();
}

std::size_t AlgorithmMetricChart::point_count() const noexcept
{
    return points_.size();
}

void AlgorithmMetricChart::paintEvent(QPaintEvent*)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette().brush(QPalette::Base));

    const QRectF plot{static_cast<qreal>(left_margin), static_cast<qreal>(top_margin),
                      static_cast<qreal>(std::max(1, width() - left_margin - right_margin)),
                      static_cast<qreal>(std::max(1, height() - top_margin - bottom_margin))};
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRectF{8.0, 5.0, static_cast<qreal>(width() - 16), 24.0},
                     Qt::AlignLeft | Qt::AlignVCenter,
                     metric_name_.isEmpty() ? QStringLiteral("运行指标曲线") : metric_name_);
    painter.setPen(QPen{palette().color(QPalette::Mid), 1.0});
    for (int line = 0; line <= 4; ++line)
    {
        const qreal y = plot.top() + plot.height() * line / 4.0;
        painter.drawLine(QPointF{plot.left(), y}, QPointF{plot.right(), y});
    }

    if (points_.empty())
    {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("暂无有效采样"));
        return;
    }

    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    for (const auto& point : points_)
    {
        minimum = std::min(minimum, point.value);
        maximum = std::max(maximum, point.value);
    }
    if (boolean_series_)
    {
        minimum = 0.0;
        maximum = 1.0;
    }
    else if (std::abs(maximum - minimum) <=
             std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(maximum)))
    {
        const double padding = std::max(1.0, std::abs(maximum) * 0.05);
        minimum -= padding;
        maximum += padding;
    }

    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRectF{0.0, plot.top() - 8.0, left_margin - 8.0, 20.0},
                     Qt::AlignRight | Qt::AlignVCenter, value_text(maximum, boolean_series_));
    painter.drawText(QRectF{0.0, plot.bottom() - 10.0, left_margin - 8.0, 20.0},
                     Qt::AlignRight | Qt::AlignVCenter, value_text(minimum, boolean_series_));
    if (!unit_.isEmpty())
        painter.drawText(QRectF{plot.left(), plot.bottom() + 10.0, plot.width(), 20.0},
                         Qt::AlignRight | Qt::AlignVCenter, unit_);

    const auto point_position = [&](const std::size_t index) {
        const qreal x = points_.size() == 1U
                            ? plot.center().x()
                            : plot.left() + plot.width() * static_cast<qreal>(index) /
                                                static_cast<qreal>(points_.size() - 1U);
        const qreal normalized = (points_[index].value - minimum) / (maximum - minimum);
        return QPointF{x, plot.bottom() - normalized * plot.height()};
    };

    QPainterPath path;
    path.moveTo(point_position(0U));
    for (std::size_t index = 1U; index < points_.size(); ++index)
        path.lineTo(point_position(index));
    const QColor accent = palette().color(QPalette::Highlight);
    painter.setPen(QPen{accent, 2.0});
    painter.drawPath(path);
    painter.setBrush(accent);
    painter.setPen(Qt::NoPen);
    if (points_.size() == 1U)
        painter.drawEllipse(point_position(0U), 4.0, 4.0);
    if (hovered_index_ >= 0 && static_cast<std::size_t>(hovered_index_) < points_.size())
    {
        painter.setBrush(palette().color(QPalette::Base));
        painter.setPen(QPen{accent, 2.0});
        painter.drawEllipse(point_position(static_cast<std::size_t>(hovered_index_)), 5.0, 5.0);
    }
}

void AlgorithmMetricChart::mouseMoveEvent(QMouseEvent* event)
{
    const int next = nearest_point_index(event->position().x());
    if (next != hovered_index_)
    {
        hovered_index_ = next;
        update();
    }
    if (hovered_index_ >= 0)
    {
        const auto& point = points_[static_cast<std::size_t>(hovered_index_)];
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1\n采样：%2\n当前值：%3 %4")
                               .arg(metric_name_,
                                    point.sample_time.toLocalTime().toString(
                                        QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                    value_text(point.value, boolean_series_), unit_),
                           this);
    }
    QWidget::mouseMoveEvent(event);
}

void AlgorithmMetricChart::leaveEvent(QEvent* event)
{
    hovered_index_ = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}

int AlgorithmMetricChart::nearest_point_index(const double x) const noexcept
{
    if (points_.empty() || x < left_margin || x > width() - right_margin)
        return -1;
    if (points_.size() == 1U)
        return 0;
    const double plot_width = std::max(1, width() - left_margin - right_margin);
    const double position = std::clamp((x - left_margin) / plot_width, 0.0, 1.0);
    return static_cast<int>(std::lround(position * static_cast<double>(points_.size() - 1U)));
}

} // namespace paperbreak::console
