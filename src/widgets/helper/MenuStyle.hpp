// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QMenu;

namespace chatterino {

/// Apply the Noctis surface to menus created by the client or by Qt itself.
void installMenuStyle();

/// Prepare a menu immediately if its geometry is needed before Qt polishes it.
void styleMenu(QMenu *menu);

}  // namespace chatterino
