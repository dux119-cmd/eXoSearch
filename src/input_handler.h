#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include "search_engine.h"

#include <optional>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

// ============================================================================
// Input Handler
// ============================================================================

class InputHandler {
#ifndef _WIN32
	termios old_term_ = {};
#endif

	[[nodiscard]] bool kbhit() const;

	[[nodiscard]] int getch() const;

	void flush_input() const;

	[[nodiscard]] int read_timeout(const int timeout_ms) const;

public:
	InputHandler();

	~InputHandler();

	InputHandler(const InputHandler&)            = delete;
	InputHandler& operator=(const InputHandler&) = delete;

	[[nodiscard]] std::optional<Command> poll(std::string& query,
	                                          const SearchEngine& engine);
};

#endif
