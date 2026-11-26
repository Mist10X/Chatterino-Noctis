#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace chatterino
{
// Largest remainder rounding: displayed shares add up to 100 without
// altering the underlying counts. Ties retain outcome order.
template <typename Range, typename Projection>
int outcomePercentage(const Range &outcomes, size_t index, Projection count)
{
    long double total = 0;
    for (const auto &outcome : outcomes)
    {
        total += std::max<long double>(0, count(outcome));
    }
    if (total <= 0 || index >= outcomes.size())
    {
        return 0;
    }
    struct Share
    {
        size_t index;
        long double remainder;
    };
    std::vector<Share> shares;
    std::vector<int> percentages;
    int remaining = 100;
    for (const auto &outcome : outcomes)
    {
        const auto exact = std::max<long double>(0, count(outcome)) * 100 / total;
        const auto rounded = static_cast<int>(exact);
        shares.push_back({percentages.size(), exact - rounded});
        percentages.push_back(rounded);
        remaining -= rounded;
    }
    std::stable_sort(shares.begin(), shares.end(), [](const auto &a, const auto &b) {
        return a.remainder > b.remainder;
    });
    for (int i = 0; i < remaining && i < static_cast<int>(shares.size()); ++i)
    {
        ++percentages[shares[i].index];
    }
    return percentages[index];
}
}  // namespace chatterino
