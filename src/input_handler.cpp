#include "input_handler.h"
#include "exit_codes_t.h"
#include "timing_t.h"

// ============================================================================
// Input Handler
// ============================================================================

[[nodiscard]] bool InputHandler::kbhit() const
{
#ifdef _WIN32
	return _kbhit() != 0;
#else
	fd_set fds = {};
	const timeval tv{0, 0};
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	return select(STDIN_FILENO + 1,
	              &fds,
	              nullptr,
	              nullptr,
	              const_cast<timeval*>(&tv)) > 0;
#endif
}

[[nodiscard]] int InputHandler::getch() const
{
#ifdef _WIN32
	return _getch();
#else
	char c = {};
	return (read(STDIN_FILENO, &c, 1) > 0) ? c : -1;
#endif
}

void InputHandler::flush_input() const
{
#ifdef _WIN32
	while (_kbhit()) {
		_getch();
	}
#else
	tcflush(STDIN_FILENO, TCIFLUSH);
#endif
}

[[nodiscard]] int InputHandler::read_timeout(const int timeout_ms) const
{
#ifdef _WIN32
	const auto start   = std::chrono::steady_clock::now();
	const auto timeout = std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() - start < timeout) {
		if (_kbhit()) {
			return _getch();
		}
		std::this_thread::sleep_for(1ms);
	}
	return -1;
#else
	fd_set fds = {};
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);

	timeval tv{0, timeout_ms * 1000};

	if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
		unsigned char c = {};
		if (read(STDIN_FILENO, &c, 1) == 1) {
			return c;
		}
	}
	return -1;
#endif
}

InputHandler::InputHandler()
{
#ifndef _WIN32
	tcgetattr(STDIN_FILENO, &old_term_);
	termios new_term = old_term_;
	new_term.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
#endif
}

InputHandler::~InputHandler()
{
#ifndef _WIN32
	tcsetattr(STDIN_FILENO, TCSANOW, &old_term_);
#endif
}

[[nodiscard]] std::optional<Command> InputHandler::poll(std::string& query,
                                                        const SearchEngine& engine)
{
	if (!kbhit()) {
		return std::nullopt;
	}

	const auto c = getch();

	if (c == 0x03) { // Ctrl+C
		return Exit{ExitSuccess};
	}

	if (c == 0x09) { // Tab completion
		if (auto comp = engine.get_completion()) {
			query = *comp;
			return UpdateQuery{query};
		}
		return std::nullopt;
	}

	if (c == 127 || c == 8) { // Backspace
		if (!query.empty()) {
			query.pop_back();
			return UpdateQuery{query};
		}
		return std::nullopt;
	}

	if (c == '\r' || c == '\n') { // Enter
		return SelectResult{-1};
	}

	if (c == 0x1B) { // Escape sequence
		const int c1 = read_timeout(
		        static_cast<int>(Timing::InputTimeout.count()));
		if (c1 == -1) {
			return Exit{ExitSuccess};
		}
		if (c1 != '[') {
			flush_input();
			return std::nullopt;
		}

		const int c2 = read_timeout(
		        static_cast<int>(Timing::InputTimeout.count()));
		if (c2 == -1) {
			flush_input();
			return std::nullopt;
		}

		switch (c2) {
		case 'A': flush_input(); return MoveSelection{-1};
		case 'B': flush_input(); return MoveSelection{1};
		case '5': {
			const int c3 = read_timeout(
			        static_cast<int>(Timing::InputTimeout.count()));
			if (c3 == '~') {
				flush_input();
				return PageScroll{true};
			}
			break;
		}
		case '6': {
			const int c3 = read_timeout(
			        static_cast<int>(Timing::InputTimeout.count()));
			if (c3 == '~') {
				flush_input();
				return PageScroll{false};
			}
			break;
		}
		}
		return std::nullopt;
	}

	if (c >= 32 && c <= 126) { // Printable characters
		query += static_cast<char>(c);
		return UpdateQuery{query};
	}

	return std::nullopt;
}
