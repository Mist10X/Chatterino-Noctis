// SPDX-License-Identifier: MIT
#pragma once

#include <QPoint>
#include <QRect>
#include <QString>

class QWidget;

namespace chatterino::noctistooltip {

// Handles QWidget and model tooltips without replacing custom emote/image
// previews. Safe to call again in test applications.
void install();

// Used by custom hover surfaces and brief action confirmations. A non-empty
// region describes the hovered area in owner-local coordinates. A positive
// duration is for brief feedback after a click, independent of mouse hover.
void showText(const QPoint &globalPosition, const QString &text, QWidget *owner,
              const QRect &region = {}, int durationMs = -1);
void hideText();

}  // namespace chatterino::noctistooltip
