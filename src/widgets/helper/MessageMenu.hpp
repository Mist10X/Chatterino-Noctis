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
    layout.addSelectionText(text, 0, UINT32_MAX, CopyMode::OnlyTextAndEmotes);
    if (!text.isEmpty())
    {
        menu.addAction(uiText("Copy &message"), [text] { crossPlatformCopy(text); });
    }
}

}  // namespace chatterino
