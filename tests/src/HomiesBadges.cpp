// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/homies/HomiesBadges.hpp"

#include "common/Aliases.hpp"
#include "common/Literals.hpp"
#include "Test.hpp"

#include <QString>

using namespace chatterino;
using namespace literals;

TEST(HomiesBadges, EmptyAndUnknownUsersHaveNoBadges)
{
    HomiesBadges badges;
    for (const QString &user : {QString(), u"nonexistent"_s, u"123456"_s})
    {
        for (const auto &badge : badges.getBadges(user))
        {
            EXPECT_FALSE(badge);
        }
    }
}
