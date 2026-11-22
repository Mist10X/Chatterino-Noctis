// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signalholder.hpp>

#include <functional>

class QDialog;
class QString;

class QWidget;

namespace chatterino {

/// When a newer Chatterino Noctis is out, a card at the top of @a window
/// says so (once per version: closing it hides it until the next version).
/// Clicking it opens showUpdatePrompt().
void initUpdateBanner(QWidget *window, pajlada::Signals::SignalHolder &holder);

/// What is new in the available version, with "Update now" / "Later".
void showUpdatePrompt(QWidget *parent);

// Presentation only. The supplied action is invoked exclusively by the
// primary button; the updater itself owns all download/installation logic.
QDialog *createUpdatePrompt(QWidget *parent, const QString &versionText,
                            const QString &notes, bool installed,
                            bool downloading, std::function<void()> update);

}  // namespace chatterino
