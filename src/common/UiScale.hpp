// SPDX-License-Identifier: MIT

#pragma once

namespace chatterino {

// Keep compact layouts distinct below 100%, with room for legible text.
inline constexpr float contentUiScale(float configuredScale)
{
    return configuredScale < 1.F ? 0.6F + 0.4F * configuredScale : configuredScale;
}

}  // namespace chatterino
