#ifndef UTILITIES_H
#define UTILITIES_H

#include <vector>
#include <string>
#include <string_view>
#include <iostream>

// ============================================================================
// Utilities
// ============================================================================

namespace Util {

[[nodiscard]] std::string to_lower(const std::string_view s);

[[nodiscard]] std::vector<std::string> tokenize(const std::string_view text);

[[nodiscard]] size_t terminal_height();

[[nodiscard]] std::pair<size_t, size_t> get_cursor_position();

void move_cursor(const size_t row, const size_t col);

constexpr void clear_screen()
{
#ifdef _WIN32
	std::system("cls");
#else
	std::cout << "\033[H\033[J";
#endif
}

constexpr void clear_to_end_of_screen()
{
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
		const COORD start = csbi.dwCursorPosition;
		const DWORD size  = (csbi.dwSize.X * csbi.dwSize.Y) -
		                   (start.Y * csbi.dwSize.X + start.X);
		DWORD written = 0;
		FillConsoleOutputCharacter(GetStdHandle(STD_OUTPUT_HANDLE),
		                           ' ',
		                           size,
		                           start,
		                           &written);
		SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), start);
	}
#else
	std::cout << "\033[J";
#endif
}

} // namespace Util

#endif
