// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

namespace chatterino {

/// How many installations of Chatterino Noctis exist: each copy adds one
/// to a public counter once, on its first start, and sends nothing else (no
/// name, no account, no ID). Described in the README and the guide; off with
/// the "Count this installation" setting.
void countInstallOnce();

}  // namespace chatterino
