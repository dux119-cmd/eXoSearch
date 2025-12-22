#ifndef SEARCH_ENGINE_H
#define SEARCH_ENGINE_H

#include "command_t.h"
#include "entry_t.h"
#include "safe_queue.h"

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ============================================================================
// Search Engine
// ============================================================================

struct SearchResult {
	size_t index = {};
	int score    = {};
};

class SearchEngine {
	std::vector<Entry> entries_                         = {};
	std::atomic<std::vector<SearchResult>*> results_    = nullptr;
	std::atomic<std::vector<std::string>*> completions_ = nullptr;
	std::atomic<std::string*> query_                    = nullptr;
	std::atomic<bool> search_needed_{false};
	SafeQueue<Command>* queue_ = nullptr;

	[[nodiscard]] static bool has_sequential_match(
	        const std::string_view text,
	        const std::vector<std::string_view>& words);

	[[nodiscard]] int score(const Entry& entry, const std::string_view query) const;

	[[nodiscard]] std::vector<std::string> find_completions(
	        const std::string_view query) const;

	void search_worker(std::atomic<bool>& stop_flag);

public:
	explicit SearchEngine(std::vector<Entry> entries);

	~SearchEngine();

	void set_queue(SafeQueue<Command>* q);

	[[nodiscard]] std::thread start(std::atomic<bool>& stop_flag);

	void update_query(const std::string& q);

	[[nodiscard]] std::string get_query() const;

	[[nodiscard]] std::optional<std::string> get_completion() const;

	[[nodiscard]] const Entry& get_entry(const size_t idx) const;

	[[nodiscard]] size_t get_entry_count() const;

	[[nodiscard]] std::vector<SearchResult> get_results() const;

	[[nodiscard]] std::vector<std::string> get_completions() const;
};

#endif
