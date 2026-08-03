#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QApplication;
class QSettings;

namespace paperbreak::console
{

enum class ThemeMode
{
    system,
    light,
    dark,
};

class ThemeController final : public QObject
{
  public:
    explicit ThemeController(QApplication& application, bool persist_settings = true,
                             const QString& settings_file = {});
    ~ThemeController() override;

    [[nodiscard]] ThemeMode mode() const noexcept;
    void set_mode(ThemeMode mode);
    [[nodiscard]] bool contrast_requirements_met() const noexcept;

  private:
    void apply();

    QApplication& application_;
    std::unique_ptr<QSettings> settings_;
    ThemeMode mode_{ThemeMode::system};
};

} // namespace paperbreak::console
