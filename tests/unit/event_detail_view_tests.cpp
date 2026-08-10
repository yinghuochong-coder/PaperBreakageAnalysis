#include "paperbreak/console/event_detail_view.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QTextEdit>

#include <gtest/gtest.h>

namespace paperbreak::console
{
namespace
{

void send_left_double_click(QWidget* widget)
{
    const QPointF center{static_cast<qreal>(widget->width()) / 2.0,
                         static_cast<qreal>(widget->height()) / 2.0};
    const QPointF global{widget->mapToGlobal(center.toPoint())};
    QMouseEvent event{QEvent::MouseButtonDblClick,
                      center,
                      center,
                      global,
                      Qt::LeftButton,
                      Qt::LeftButton,
                      Qt::NoModifier};
    static_cast<void>(QApplication::sendEvent(widget, &event));
    QApplication::processEvents();
}

TEST(EventDetailView, UsesThreeToOneMediaAndManifestLayout)
{
    EventDetailView view;
    auto* layout = view.findChild<QHBoxLayout*>(QStringLiteral("event-detail-layout"));
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->stretch(0), 3);
    EXPECT_EQ(layout->stretch(1), 1);

    auto* thumbnail = view.findChild<QLabel*>(QStringLiteral("event-thumbnail"));
    auto* manifest = view.findChild<QTextEdit*>(QStringLiteral("event-manifest"));
    ASSERT_NE(thumbnail, nullptr);
    ASSERT_NE(manifest, nullptr);

    view.resize(1000, 300);
    view.show();
    QApplication::processEvents();
    ASSERT_GT(manifest->width(), 0);
    const double ratio = static_cast<double>(thumbnail->width()) / manifest->width();
    EXPECT_GT(ratio, 2.8);
    EXPECT_LT(ratio, 3.2);
}

TEST(EventDetailView, DoubleClickEntersAndLeavesMediaFullScreen)
{
    EventDetailView view;
    QPixmap thumbnail{640, 360};
    thumbnail.fill(Qt::red);
    view.set_thumbnail(thumbnail);
    view.show();
    QApplication::processEvents();

    auto* normal = view.findChild<QLabel*>(QStringLiteral("event-thumbnail"));
    auto* full_screen = view.findChild<QLabel*>(QStringLiteral("event-fullscreen-thumbnail"));
    ASSERT_NE(normal, nullptr);
    ASSERT_NE(full_screen, nullptr);
    EXPECT_EQ(normal->pixmap().cacheKey(), full_screen->pixmap().cacheKey());

    send_left_double_click(normal);
    EXPECT_TRUE(view.media_full_screen_active());
    EXPECT_TRUE(full_screen->isVisible());

    send_left_double_click(full_screen);
    EXPECT_FALSE(view.media_full_screen_active());
    EXPECT_FALSE(full_screen->isVisible());
}

} // namespace
} // namespace paperbreak::console
