// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ChannelView.hpp"

#include "mocks/BaseApplication.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/Scrollbar.hpp"

#include <algorithm>

using namespace chatterino;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args, this->paths_, this->settings, this->theme,
                        this->fonts)
    {
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    WindowManager windowManager;
};

}  // namespace

TEST(ChannelViewTest, GoToBottomGeometryUpdatesWhenScaleChanges)
{
    MockApplication mockApplication;

    ChannelView view(nullptr);
    view.resize(320, 180);

    const auto widgets = view.findChildren<QWidget *>();
    const auto it =
        std::find_if(widgets.begin(), widgets.end(), [](QWidget *w) {
            return dynamic_cast<Button *>(w) != nullptr &&
                   !w->accessibleName().isEmpty();
        });
    auto *button =
        it == widgets.end() ? nullptr : dynamic_cast<Button *>(*it);
    ASSERT_NE(button, nullptr);

    auto *scrollbar = view.scrollbar();
    ASSERT_NE(scrollbar, nullptr);

    const auto initialHeight = button->height();
    const auto initialScrollbarWidth = scrollbar->width();
    const auto initialLabelFontHeight = button->fontMetrics().height();

    view.setOverrideScale(1.5F);

    EXPECT_GT(button->height(), initialHeight);
    EXPECT_GT(scrollbar->width(), initialScrollbarWidth);
    EXPECT_GT(button->fontMetrics().height(), initialLabelFontHeight);
    EXPECT_GE(button->font().weight(), QFont::DemiBold);
    EXPECT_GE(button->height(), qRound(view.scale() * 26));
    EXPECT_GE(button->height(), button->fontMetrics().height() + qRound(view.scale() * 8));
    EXPECT_EQ(button->geometry().bottom(), view.rect().bottom() - qRound(view.scale() * 5));
    EXPECT_LT(button->geometry().right(), scrollbar->geometry().left());
    EXPECT_LT(button->width(), view.width());
    EXPECT_EQ(scrollbar->width(), int(view.scale() * 16));
}
