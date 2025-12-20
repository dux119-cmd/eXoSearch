#ifndef APPLICATION_H
#define APPLICATION_H

#include "search_engine.h"
#include "display_manager.h"
#include "input_handler.h"
#include "safe_queue.h"
#include "exit_codes_t.h"

#include <atomic>
#include <string>

// ============================================================================
// Application
// ============================================================================

class Application {
	SearchEngine engine_;
	DisplayManager display_;
	InputHandler input_       = {};
	SafeQueue<Command> queue_ = {};

	DisplayState state_ = {};
	std::string query_  = {};
	std::atomic<bool> running_{true};
	std::atomic<int> exit_code_{ExitSuccess};

	void io_worker(const std::stop_token stoken);

	void handle_move(const int delta);

	void handle_page_scroll(const bool up);

	void handle_select(const int index);

public:
	explicit Application(std::vector<Entry> entries);

	[[nodiscard]] int run();
};

#endif
