// SPDX-License-Identifier: MIT
#pragma once

#include "common/UiLanguage.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "util/Clipboard.hpp"

#include <QMenu>

namespace chatterino {

inline void addMessageCopyActions(QMenu &menu, MessageLayout &layout)
{
    QString text;
    QString full;
    layout.addSelectionText(text, 0, UINT32_MAX, CopyMode::OnlyTextAndEmotes);
    layout.addSelectionText(full, 0, UINT32_MAX, CopyMode::EverythingButReplies);
    if (!text.isEmpty())
    {
        menu.addAction(uiText("Copy &message"), [text] { crossPlatformCopy(text); });
    }
    if (!full.isEmpty() && full != text)
    {
        menu.addAction(uiText("Copy &full message"), [full] { crossPlatformCopy(full); });
    }
}

}  // namespace chatterino
