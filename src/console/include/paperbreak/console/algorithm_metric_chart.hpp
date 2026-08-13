#pragma once

#include <QWidget>

#include <QDateTime>
#include <QString>

#include <vector>

namespace paperbreak::console
{

struct AlgorithmChartPoint final
{
    QDateTime sample_time;
    double value{};
};

class AlgorithmMetricChart final : public QWidget
{
  public:
    explicit AlgorithmMetricChart(QWidget* parent = nullptr);

    void set_series(QString metric_name, QString unit, std::vector<AlgorithmChartPoint> points,
                    bool boolean_series = false);
    [[nodiscard]] std::size_t point_count() const noexcept;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    [[nodiscard]] int nearest_point_index(double x) const noexcept;

    QString metric_name_;
    QString unit_;
    std::vector<AlgorithmChartPoint> points_;
    bool boolean_series_{};
    int hovered_index_{-1};
};

} // namespace paperbreak::console
