#include "paperbreak/console/algorithm_metric_chart.hpp"

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>
#include <QPalette>

#include <vector>

namespace
{

TEST(AlgorithmMetricChart, RendersEmptySingleConstantAndBooleanSeries)
{
    paperbreak::console::AlgorithmMetricChart chart;
    chart.resize(640, 260);
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
    const std::vector<std::vector<paperbreak::console::AlgorithmChartPoint>> cases{
        {},
        {{.sample_time = start, .value = 42.0}},
        {{.sample_time = start, .value = 5.0}, {.sample_time = start.addSecs(5), .value = 5.0}},
        {{.sample_time = start, .value = 0.0},
         {.sample_time = start.addSecs(5), .value = 1.0},
         {.sample_time = start.addSecs(10), .value = 0.0}}};
    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        chart.set_series(QStringLiteral("测试曲线"), QStringLiteral("单位"), cases[index],
                         index == 3U);
        EXPECT_EQ(chart.point_count(), cases[index].size());
        QImage image{chart.size(), QImage::Format_ARGB32_Premultiplied};
        image.fill(Qt::transparent);
        QPainter painter{&image};
        chart.render(&painter);
        EXPECT_FALSE(image.isNull());
    }

    QPalette dark_palette = chart.palette();
    dark_palette.setColor(QPalette::Base, QColor{31, 34, 39});
    dark_palette.setColor(QPalette::Text, QColor{235, 238, 242});
    dark_palette.setColor(QPalette::Mid, QColor{91, 97, 105});
    dark_palette.setColor(QPalette::Highlight, QColor{71, 139, 255});
    chart.setPalette(dark_palette);
    chart.set_series(QStringLiteral("布尔状态"), QStringLiteral("状态"), cases.back(), true);
    QImage dark_image{chart.size(), QImage::Format_ARGB32_Premultiplied};
    dark_image.fill(Qt::transparent);
    QPainter dark_painter{&dark_image};
    chart.render(&dark_painter);
    EXPECT_EQ(dark_image.pixelColor(0, 0), dark_palette.color(QPalette::Base));
}

} // namespace
