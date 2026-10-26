// ============================================================================
//  DormProtocol.cpp
// ============================================================================
#include "DormProtocol.h"

#include <cwchar>

namespace protocol {

bool parse(const std::uint8_t *data, std::size_t len, Reading *out)
{
    if (!out) return false;
    *out = Reading{};
    if (!data || len < kMinPayload) return false;

    const std::int16_t raw_temp =
        static_cast<std::int16_t>(static_cast<std::uint16_t>(data[0]) |
                                  (static_cast<std::uint16_t>(data[1]) << 8));
    const std::uint16_t raw_volt =
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[3]) |
                                   (static_cast<std::uint16_t>(data[4]) << 8));

    out->temp  = raw_temp / 100.0;
    out->hum   = static_cast<double>(data[2]);
    out->volt  = raw_volt / 1000.0;
    out->valid = true;
    return true;
}

bool name_matches(const wchar_t *advertised, const wchar_t *wanted)
{
    if (!advertised || !wanted || !*advertised || !*wanted) return false;
    return _wcsicmp(advertised, wanted) == 0;
}

} // namespace protocol
