#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "search_engine.h"

#include <vector>
#include <string>
#include <optional>
#include <chrono>

// ============================================================================
// Display Manager
// ============================================================================

class DisplayManager {
	const SearchEngine& engine_;
	mutable size_t cached_height_                             = 0;
	mutable std::chrono::steady_clock::time_point last_check_ = {};

	[[nodiscard]] size_t get_terminal_height_cached() const;

	[[nodiscard]] DisplayMetrics measure_display(const DisplayMetrics& old_metrics) const;

	void render_header(std::ostringstream& buf, const std::string& query,
	                   const std::vector<std::string>& completions) const;

	void render_result(std::ostringstream& buf, const SearchResult& result,
	                   size_t display_index, bool selected) const;

	void render_footer(std::ostringstream& buf, size_t scroll_offset,
	                   size_t display_count, size_t total_results) const;

public:
	explicit DisplayManager(const SearchEngine& engine);

	[[nodiscard]] DisplayMetrics render(DisplayState& state) const;

	[[nodiscard]] std::optional<int> select(const int index) const;
};

#endif