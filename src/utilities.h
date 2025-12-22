#ifndef UTILITIES_H
#define UTILITIES_H

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// Utilities
// ============================================================================

namespace Util {

[[nodiscard]] std::string to_lower(const std::string_view s);

[[nodiscard]] std::vector<std::string_view> tokenize(const std::string_view text);

[[nodiscard]] size_t terminal_height();

[[nodiscard]] std::pair<size_t, size_t> get_cursor_position();

void move_cursor(const size_t row, const size_t col);

void clear_screen();

void clear_to_end_of_screen();

} // namespace Util

#endif
