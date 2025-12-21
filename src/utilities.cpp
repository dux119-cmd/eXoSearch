#include "utilities.h"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <string> // if std::string used here

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

// ============================================================================
// Utilities
// ============================================================================

namespace Util {

[[nodiscard]] std::string to_lower(const std::string_view s)
{
	std::string result = {};
	result.reserve(s.size());
	std::ranges::transform(s, std::back_inserter(result), [](const unsigned char c) {
		return std::tolower(c);
	});
	return result;
}

[[nodiscard]] std::vector<std::string> tokenize(const std::string_view text)
{
	std::vector<std::string> words = {};
	std::istringstream ss{std::string(text)};

	for (std::string word = {}; ss >> word;) {
		const auto end = std::ranges::remove_if(word, [](const unsigned char c) {
			                 return !std::isalnum(c);
		                 }).begin();

		word.erase(end, word.end());

		if (!word.empty()) {
			words.emplace_back(to_lower(word));
		}
	}
	return words;
}

[[nodiscard]] size_t terminal_height()
{
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
	winsize w = {};
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	return w.ws_row;
#endif
}

[[nodiscard]] std::pair<size_t, size_t> get_cursor_position()
{
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	return {static_cast<size_t>(csbi.dwCursorPosition.Y + 1),
	        static_cast<size_t>(csbi.dwCursorPosition.X + 1)};
#else
	std::cout << "\033[6n" << std::flush;

	char buf[32] = {};
	size_t i     = 0;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1 || buf[i] == 'R') {
			break;
		}
		++i;
	}

	size_t row = 1, col = 1;
	if (i > 0 && buf[0] == '\033' && buf[1] == '[') {
		if (sscanf(buf + 2, "%zu;%zu", &row, &col) != 2) {
			row = col = 1;
		}
	}

	return {row, col};
#endif
}

void move_cursor(const size_t row, const size_t col)
{
#ifdef _WIN32
	COORD coord = {static_cast<SHORT>(col - 1), static_cast<SHORT>(row - 1)};
	SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
#else
	std::cout << "\033[" << row << ";" << col << "H";
#endif
}

void clear_screen()
{
#ifdef _WIN32
	std::system("cls");
#else
	std::cout << "\033[H\033[J";
#endif
}

void clear_to_end_of_screen()
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
