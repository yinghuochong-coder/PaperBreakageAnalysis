#include "paperbreak/console/event_detail_view.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QSizePolicy>
#include <QTextEdit>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace paperbreak::console
{
namespace
{

template <typename T, typename... Arguments>
T* make_child(QWidget* parent, Arguments&&... arguments)
{
    auto child = std::make_unique<T>(std::forward<Arguments>(arguments)...);
    child->setParent(parent);
    T* result = child.get();
    static_cast<void>(child.release());
    return result;
}

template <typename T, typename... Arguments>
T* make_layout(QWidget* parent, Arguments&&... arguments)
{
    auto layout = std::make_unique<T>(parent, std::forward<Arguments>(arguments)...);
    T* result = layout.get();
    static_cast<void>(layout.release());
    return result;
}

bool is_left_button_double_click(const QEvent* event)
{
    if (event->type() != QEvent::MouseButtonDblClick)
        return false;
    return static_cast<const QMouseEvent*>(event)->button() == Qt::LeftButton;
}

} // namespace

EventDetailView::EventDetailView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("event-detail-view"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* layout = make_layout<QHBoxLayout>(this);
    layout->setObjectName(QStringLiteral("event-detail-layout"));
    layout->setContentsMargins(0, 0, 0, 0);

    thumbnail_ = make_child<QLabel>(this, QStringLiteral("选择事件加载缩略图"));
    thumbnail_->setObjectName(QStringLiteral("event-thumbnail"));
    thumbnail_->setMinimumSize(240, 140);
    thumbnail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    thumbnail_->setAlignment(Qt::AlignCenter);
    thumbnail_->setScaledContents(true);
    thumbnail_->setToolTip(QStringLiteral("双击全屏查看，再次双击或按 Esc 恢复"));
    thumbnail_->installEventFilter(this);

    manifest_ = make_child<QTextEdit>(this);
    manifest_->setObjectName(QStringLiteral("event-manifest"));
    manifest_->setReadOnly(true);
    manifest_->setPlaceholderText(QStringLiteral("选择事件查看已校验 manifest"));

    layout->addWidget(thumbnail_, 3);
    layout->addWidget(manifest_, 1);

    auto full_screen = std::make_unique<QWidget>(this, Qt::Window | Qt::FramelessWindowHint);
    full_screen_window_ = full_screen.get();
    full_screen_window_->setObjectName(QStringLiteral("event-media-fullscreen"));
    full_screen_window_->setStyleSheet(QStringLiteral("background: black;"));
    full_screen_window_->installEventFilter(this);
    auto* full_screen_layout = make_layout<QVBoxLayout>(full_screen_window_);
    full_screen_layout->setContentsMargins(0, 0, 0, 0);
    full_screen_thumbnail_ = make_child<QLabel>(full_screen_window_);
    full_screen_thumbnail_->setObjectName(QStringLiteral("event-fullscreen-thumbnail"));
    full_screen_thumbnail_->setAlignment(Qt::AlignCenter);
    full_screen_thumbnail_->setScaledContents(true);
    full_screen_thumbnail_->setFocusPolicy(Qt::StrongFocus);
    full_screen_thumbnail_->setToolTip(QStringLiteral("双击或按 Esc 恢复"));
    full_screen_thumbnail_->installEventFilter(this);
    full_screen_layout->addWidget(full_screen_thumbnail_);
    static_cast<void>(full_screen.release());
}

void EventDetailView::set_thumbnail(const QPixmap& thumbnail, const QString& unavailable_text)
{
    if (thumbnail.isNull())
    {
        thumbnail_->setPixmap({});
        thumbnail_->setText(unavailable_text);
        full_screen_thumbnail_->setPixmap({});
        full_screen_thumbnail_->setText(unavailable_text);
        return;
    }

    thumbnail_->setPixmap(thumbnail);
    full_screen_thumbnail_->setPixmap(thumbnail);
}

void EventDetailView::set_manifest_text(const QString& manifest)
{
    manifest_->setPlainText(manifest);
}

bool EventDetailView::media_full_screen_active() const noexcept
{
    return media_full_screen_active_;
}

bool EventDetailView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == thumbnail_ && is_left_button_double_click(event))
    {
        enter_media_full_screen();
        return true;
    }
    if (watched == full_screen_thumbnail_ && is_left_button_double_click(event))
    {
        leave_media_full_screen();
        return true;
    }
    if (media_full_screen_active_ &&
        (watched == full_screen_thumbnail_ || watched == full_screen_window_) &&
        event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
    {
        leave_media_full_screen();
        return true;
    }
    if (watched == full_screen_window_ && event->type() == QEvent::Close)
        media_full_screen_active_ = false;
    return QWidget::eventFilter(watched, event);
}

void EventDetailView::enter_media_full_screen()
{
    if (media_full_screen_active_)
        return;
    media_full_screen_active_ = true;
    full_screen_window_->showFullScreen();
    full_screen_thumbnail_->setFocus(Qt::MouseFocusReason);
}

void EventDetailView::leave_media_full_screen()
{
    if (!media_full_screen_active_)
        return;
    media_full_screen_active_ = false;
    full_screen_window_->hide();
}

} // namespace paperbreak::console
