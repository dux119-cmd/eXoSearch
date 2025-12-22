#ifndef TIMING_T
#define TIMING_T

#include <chrono>

namespace Timing {

using namespace std::chrono_literals;

constexpr auto SearchSleep           = 30ms;
constexpr auto IOSleep               = 50ms;
constexpr auto HeightCache           = 500ms;
constexpr auto InputTimeout          = 10ms;
constexpr auto IntraCharacterTimeout = 1ms;
} // namespace Timing

#endif
