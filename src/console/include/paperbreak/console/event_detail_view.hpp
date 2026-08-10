#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPixmap;
class QTextEdit;

namespace paperbreak::console
{

class EventDetailView final : public QWidget
{
  public:
    explicit EventDetailView(QWidget* parent = nullptr);

    void set_thumbnail(const QPixmap& thumbnail,
                       const QString& unavailable_text = QStringLiteral("缩略图不可用"));
    void set_manifest_text(const QString& manifest);

    [[nodiscard]] bool media_full_screen_active() const noexcept;

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void enter_media_full_screen();
    void leave_media_full_screen();

    QLabel* thumbnail_{};
    QTextEdit* manifest_{};
    QWidget* full_screen_window_{};
    QLabel* full_screen_thumbnail_{};
    bool media_full_screen_active_{};
};

} // namespace paperbreak::console
