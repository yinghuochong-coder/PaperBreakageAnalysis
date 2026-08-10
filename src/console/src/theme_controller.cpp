#include "theme_controller.hpp"

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStyleHints>

#include <algorithm>
#include <array>
#include <cmath>

namespace paperbreak::console
{
namespace
{

struct ThemeColors final
{
    const char* window;
    const char* panel;
    const char* input;
    const char* text;
    const char* muted;
    const char* disabled_background;
    const char* disabled_text;
    const char* border;
    const char* header;
    const char* header_text;
    const char* header_muted;
    const char* navigation;
    const char* navigation_text;
    const char* selection;
    const char* selection_text;
    const char* banner;
    const char* banner_text;
    const char* preview;
    const char* preview_image;
};

constexpr ThemeColors light_colors{
    .window = "#f2f5f8",
    .panel = "#ffffff",
    .input = "#ffffff",
    .text = "#172033",
    .muted = "#526273",
    .disabled_background = "#e3e8ed",
    .disabled_text = "#5d6874",
    .border = "#9eacb9",
    .header = "#17324d",
    .header_text = "#f7fbff",
    .header_muted = "#c4d4e1",
    .navigation = "#223f59",
    .navigation_text = "#e7f0f7",
    .selection = "#1769aa",
    .selection_text = "#ffffff",
    .banner = "#fff4cf",
    .banner_text = "#604b00",
    .preview = "#132331",
    .preview_image = "#0a1118",
};

constexpr ThemeColors dark_colors{
    .window = "#121820",
    .panel = "#1b2530",
    .input = "#0f151c",
    .text = "#edf3f8",
    .muted = "#b8c4cf",
    .disabled_background = "#27323d",
    .disabled_text = "#aeb8c2",
    .border = "#607181",
    .header = "#0e2235",
    .header_text = "#f3f8fc",
    .header_muted = "#c5d6e4",
    .navigation = "#101e2b",
    .navigation_text = "#e5eef5",
    .selection = "#1768a6",
    .selection_text = "#ffffff",
    .banner = "#3a3215",
    .banner_text = "#ffeaa3",
    .preview = "#0d1720",
    .preview_image = "#070b10",
};

ThemeMode stored_mode(const QString& value)
{
    if (value == QStringLiteral("light"))
        return ThemeMode::light;
    if (value == QStringLiteral("dark"))
        return ThemeMode::dark;
    return ThemeMode::system;
}

QString mode_text(const ThemeMode mode)
{
    switch (mode)
    {
    case ThemeMode::system:
        return QStringLiteral("system");
    case ThemeMode::light:
        return QStringLiteral("light");
    case ThemeMode::dark:
        return QStringLiteral("dark");
    }
    return QStringLiteral("system");
}

bool effective_dark(const ThemeMode mode)
{
    if (mode == ThemeMode::dark)
        return true;
    if (mode == ThemeMode::light)
        return false;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

double linear_channel(const int channel)
{
    const double value = static_cast<double>(channel) / 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& color)
{
    return 0.2126 * linear_channel(color.red()) + 0.7152 * linear_channel(color.green()) +
           0.0722 * linear_channel(color.blue());
}

double contrast(const char* foreground, const char* background)
{
    const double first = luminance(QColor{foreground});
    const double second = luminance(QColor{background});
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

bool palette_contrast_passes(const ThemeColors& colors)
{
    const std::array pairs{
        std::pair{colors.text, colors.window},
        std::pair{colors.text, colors.panel},
        std::pair{colors.text, colors.input},
        std::pair{colors.muted, colors.panel},
        std::pair{colors.disabled_text, colors.disabled_background},
        std::pair{colors.header_text, colors.header},
        std::pair{colors.header_muted, colors.header},
        std::pair{colors.navigation_text, colors.navigation},
        std::pair{colors.selection_text, colors.selection},
        std::pair{colors.banner_text, colors.banner},
    };
    return std::ranges::all_of(
        pairs, [](const auto& pair) { return contrast(pair.first, pair.second) >= 4.5; });
}

QString style_sheet(const ThemeColors& c)
{
    return QString::fromLatin1(R"(
        QMainWindow, QWidget#app-central, QWidget#app-body, QStackedWidget#page-stack {
            background: %1; color: %4;
        }
        QWidget { color: %4; }
        QLabel { background: transparent; color: %4; }
        QWidget#status-header { background: %9; color: %10; }
        QWidget#status-header QLabel { color: %10; }
        QLabel[role="productTitle"] { font-size: 20px; font-weight: 600; color: %10; }
        QLabel[role="statusTitle"] { color: %11; font-size: 11px; }
        QLabel[role="statusValue"] { color: %10; font-weight: 600; }
        QLabel#connection-banner { background: %16; color: %17; padding: 7px; }
        QListWidget#navigation { background: %12; color: %13; border: 0; padding: 10px 6px; font-size: 14px; }
        QListWidget#navigation::item { color: %13; padding: 11px 14px; border-radius: 4px; }
        QListWidget#navigation::item:selected { background: %14; color: %15; }
        QLabel[role="pageTitle"] { font-size: 22px; font-weight: 600; color: %4; }
        QLabel[role="muted"] { color: %5; }
        QLabel[role="placeholder"] { color: %5; font-size: 16px; background: %2; border: 1px dashed %8; border-radius: 8px; padding: 28px; }
        QGroupBox { background: %2; color: %4; border: 1px solid %8; border-radius: 6px; margin-top: 8px; padding-top: 8px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; color: %4; }
        QGroupBox[role="cameraCard"] { min-height: 112px; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QListWidget, QAbstractItemView {
            background: %3; color: %4; border: 1px solid %8; border-radius: 4px; padding: 5px;
            selection-background-color: %14; selection-color: %15;
        }
        QComboBox QAbstractItemView { background: %3; color: %4; outline: 0; }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled,
        QListWidget:disabled { background: %6; color: %7; }
        QPushButton { background: %2; color: %4; border: 1px solid %8; border-radius: 4px; padding: 6px 10px; }
        QPushButton:hover { border-color: %14; }
        QPushButton:pressed { background: %14; color: %15; }
        QPushButton:disabled { background: %6; color: %7; border-color: %8; }
        QComboBox#theme-selector { background: %9; color: %10; border-color: %11; min-width: 92px; }
        QComboBox#theme-selector QAbstractItemView { background: %2; color: %4; }
        QScrollArea { background: transparent; border: 0; }
        QWidget#camera-page-header, QWidget#camera-action-bar {
            background: %2; border: 1px solid %8; border-radius: 6px;
        }
        QWidget#camera-editor { background: transparent; border: 0; }
        QWidget#page-camera-configuration QGroupBox[role="cameraPanel"] {
            background: %2; border-color: %8; border-radius: 6px; margin-top: 9px;
            padding-top: 10px;
        }
        QWidget#page-camera-configuration QGroupBox[role="cameraPanel"]::title {
            font-weight: 600; color: %4;
        }
        QWidget#page-camera-configuration QLineEdit,
        QWidget#page-camera-configuration QSpinBox,
        QWidget#page-camera-configuration QDoubleSpinBox,
        QWidget#page-camera-configuration QComboBox {
            min-height: 22px;
        }
        QWidget#page-camera-configuration QPushButton { min-height: 24px; }
        QWidget#page-camera-configuration QPushButton[role="primaryAction"] {
            background: %14; color: %15; border-color: %14; font-weight: 600;
            min-width: 128px;
        }
        QWidget#page-camera-configuration QPushButton[role="primaryAction"]:hover {
            border-color: %4;
        }
        QLabel#camera-operation-status { padding-right: 10px; }
        QListWidget#discovered-devices { background: %2; }
        QWidget[role="previewTile"] { background: %18; border-radius: 5px; }
        QLabel[role="previewImage"] { color: %11; background: %19; }
        QLabel[role="previewOverlay"] {
            color: %13; background: rgba(0, 0, 0, 150); border-radius: 3px;
            padding: 5px 8px; font-size: 11px;
        }
        QToolTip { background: %2; color: %4; border: 1px solid %8; }
    )")
        .arg(QString::fromLatin1(c.window), QString::fromLatin1(c.panel),
             QString::fromLatin1(c.input), QString::fromLatin1(c.text),
             QString::fromLatin1(c.muted), QString::fromLatin1(c.disabled_background),
             QString::fromLatin1(c.disabled_text), QString::fromLatin1(c.border),
             QString::fromLatin1(c.header), QString::fromLatin1(c.header_text),
             QString::fromLatin1(c.header_muted), QString::fromLatin1(c.navigation),
             QString::fromLatin1(c.navigation_text), QString::fromLatin1(c.selection),
             QString::fromLatin1(c.selection_text), QString::fromLatin1(c.banner),
             QString::fromLatin1(c.banner_text), QString::fromLatin1(c.preview),
             QString::fromLatin1(c.preview_image));
}

QPalette palette(const ThemeColors& c)
{
    QPalette result;
    result.setColor(QPalette::Window, QColor{c.window});
    result.setColor(QPalette::WindowText, QColor{c.text});
    result.setColor(QPalette::Base, QColor{c.input});
    result.setColor(QPalette::AlternateBase, QColor{c.panel});
    result.setColor(QPalette::Text, QColor{c.text});
    result.setColor(QPalette::Button, QColor{c.panel});
    result.setColor(QPalette::ButtonText, QColor{c.text});
    result.setColor(QPalette::Highlight, QColor{c.selection});
    result.setColor(QPalette::HighlightedText, QColor{c.selection_text});
    result.setColor(QPalette::ToolTipBase, QColor{c.panel});
    result.setColor(QPalette::ToolTipText, QColor{c.text});
    result.setColor(QPalette::PlaceholderText, QColor{c.muted});
    result.setColor(QPalette::Disabled, QPalette::WindowText, QColor{c.disabled_text});
    result.setColor(QPalette::Disabled, QPalette::Text, QColor{c.disabled_text});
    result.setColor(QPalette::Disabled, QPalette::ButtonText, QColor{c.disabled_text});
    result.setColor(QPalette::Disabled, QPalette::Base, QColor{c.disabled_background});
    result.setColor(QPalette::Disabled, QPalette::Button, QColor{c.disabled_background});
    return result;
}

} // namespace

ThemeController::ThemeController(QApplication& application, const bool persist_settings,
                                 const QString& settings_file)
    : application_(application)
{
    if (persist_settings)
    {
        settings_ = settings_file.isEmpty()
                        ? std::make_unique<QSettings>()
                        : std::make_unique<QSettings>(settings_file, QSettings::IniFormat);
        mode_ = stored_mode(
            settings_->value(QStringLiteral("ui/theme"), QStringLiteral("system")).toString());
    }
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == ThemeMode::system)
            apply();
    });
    apply();
}

ThemeController::~ThemeController() = default;

ThemeMode ThemeController::mode() const noexcept
{
    return mode_;
}

void ThemeController::set_mode(const ThemeMode mode)
{
    mode_ = mode;
    if (settings_)
    {
        settings_->setValue(QStringLiteral("ui/theme"), mode_text(mode));
        settings_->sync();
    }
    apply();
}

bool ThemeController::contrast_requirements_met() const noexcept
{
    return palette_contrast_passes(light_colors) && palette_contrast_passes(dark_colors);
}

void ThemeController::apply()
{
    const ThemeColors& colors = effective_dark(mode_) ? dark_colors : light_colors;
    application_.setPalette(palette(colors));
    application_.setStyleSheet(style_sheet(colors));
}

} // namespace paperbreak::console
