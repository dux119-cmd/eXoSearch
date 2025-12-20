#include "application.h"
#include "utilities.h"
#include "timing_t.h"
#include <iostream>

// ============================================================================
// Application
// ============================================================================

void Application::io_worker(const std::stop_token stoken)
{
	using namespace std::string_view_literals;

	while (!stoken.stop_requested() && running_) {
		try {
			const auto cmd = queue_.pop(Timing::IOSleep);
			if (!cmd) {
				continue;
			}

			std::visit(
					[this](auto&& arg) {
						using T = std::decay_t<decltype(arg)>;

						try {
							if constexpr (std::is_same_v<T, RefreshDisplay>) {
								state_.scroll_offset =
										arg.state
												.scroll_offset;
								state_.selected_index =
										arg.state
												.selected_index;
								state_.metrics.dirty = true;
								state_.metrics = display_.render(
										state_);
							} else if constexpr (std::is_same_v<T, UpdateQuery>) {
								query_ = std::move(
										arg.query);
								engine_.update_query(
										query_);
							} else if constexpr (std::is_same_v<T, MoveSelection>) {
								handle_move(arg.delta);
							} else if constexpr (std::is_same_v<T, PageScroll>) {
								handle_page_scroll(
										arg.up);
							} else if constexpr (std::is_same_v<T, SelectResult>) {
								handle_select(arg.index);
							} else if constexpr (std::is_same_v<T, Exit>) {
								exit_code_ = arg.code;
								running_ = false;
							}
						} catch (const std::exception& e) {
							std::cerr << "Command error: "sv
										<< e.what()
										<< '\n';
						} catch (...) {
							std::cerr << "Unknown command error\n"sv;
						}
					},
					*cmd);
		} catch (const std::exception& e) {
			std::cerr << "IO worker error: "sv << e.what()
						<< '\n';
		} catch (...) {
			std::cerr << "Unknown IO worker error\n"sv;
		}
	}
}

void  Application::handle_move(const int delta)
{
	const auto results = engine_.get_results();
	if (results.empty()) {
		return;
	}

	const int result_count = static_cast<int>(results.size());

	if (state_.selected_index < 0) {
		state_.selected_index = 0;
	} else {
		state_.selected_index = std::clamp(state_.selected_index + delta,
											0,
											result_count - 1);
	}

	const size_t selected = static_cast<size_t>(state_.selected_index);
	const size_t max_visible = state_.metrics.max_visible_results;

	if (max_visible > 0) {
		if (selected < state_.scroll_offset) {
			state_.scroll_offset = selected;
		} else if (selected >= state_.scroll_offset + max_visible) {
			state_.scroll_offset = selected - max_visible + 1;
		}
	}

	state_.metrics = display_.render(state_);
}

void  Application::handle_page_scroll(const bool up)
{
	const auto results = engine_.get_results();
	if (results.empty()) {
		return;
	}

	const size_t result_count = results.size();
	const size_t max_visible  = state_.metrics.max_visible_results;

	if (max_visible == 0) {
		return;
	}

	const size_t page_size = std::max(size_t(1), max_visible - 1);

	if (up) {
		const int new_selected = state_.selected_index -
									static_cast<int>(page_size);
		state_.selected_index = std::max(0, new_selected);
	} else {
		const int new_selected = state_.selected_index +
									static_cast<int>(page_size);
		state_.selected_index = std::min(
				new_selected, static_cast<int>(result_count) - 1);
	}

	const size_t selected = static_cast<size_t>(state_.selected_index);

	if (selected < state_.scroll_offset) {
		state_.scroll_offset = selected;
	} else if (selected >= state_.scroll_offset + max_visible) {
		state_.scroll_offset = selected - max_visible + 1;
	}

	state_.metrics = display_.render(state_);
}

void  Application::handle_select(const int index)
{
	const auto results = engine_.get_results();

	int target_index = index;
	if (target_index < 0) {
		if (state_.selected_index >= 0) {
			target_index = state_.selected_index;
		} else if (results.size() == 1) {
			target_index = 0;
		} else if (results.size() > 1) {
			state_.selected_index = 0;
			state_.metrics        = display_.render(state_);
			return;
		}
	}

	if (const auto code = display_.select(target_index)) {
		exit_code_ = *code;
		running_   = false;
	}
}


Application::Application(std::vector<Entry> entries)
		: engine_(std::move(entries)),
			display_(engine_)
{
	engine_.set_queue(&queue_);
}

[[nodiscard]] int  Application::run()
{
	using namespace std::string_view_literals;

	try {
		Util::clear_screen();
		engine_.update_query("");

		auto search_thread = engine_.start();
		std::jthread io_thread([this](const std::stop_token st) {
			io_worker(st);
		});

		while (running_) {
			try {
				if (const auto cmd = input_.poll(query_,
													engine_)) {
					queue_.emplace(*cmd);
				}
				std::this_thread::sleep_for(Timing::IOSleep);
			} catch (const std::exception& e) {
				std::cerr << "Input error: "sv
							<< e.what() << '\n';
			} catch (...) {
				std::cerr << "Unknown input error\n"sv;
			}
		}

		queue_.shutdown();
		std::cout << "\n\nSearch "sv
					<< (exit_code_ == ExitSuccess ? "terminated"sv
												: "completed"sv)
					<< ".\n"sv;
		return exit_code_;
	} catch (const std::exception& e) {
		std::cerr << "Fatal error: "sv << e.what() << '\n';
		return ExitError;
	} catch (...) {
		std::cerr << "Unknown fatal error\n"sv;
		return ExitError;
	}
}
