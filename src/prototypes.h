#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace rng = std::ranges;
namespace vws = std::views;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

// ============================================================================
// Constants
// ============================================================================

// ============================================================================
// Data Structures
// ============================================================================

using Command = std::variant<RefreshDisplay, UpdateQuery, MoveSelection,
                             PageScroll, SelectResult, Exit>;
