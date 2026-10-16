#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace btc::events {
using Millis = std::int64_t;
struct Event {
    Millis time = 0;
    char kind = 0; // S spot reference, Q perp BBO, T perp trade, F funding, R prior schedule seed.
    double price = 0, quantity = 0, bid = 0, ask = 0;
    bool buyer_maker = false;
    double funding_rate = 0, interval_hours = 8;
};
void read(const std::string& path, const std::function<void(const Event&,bool)>& feed);
} // namespace btc::events
