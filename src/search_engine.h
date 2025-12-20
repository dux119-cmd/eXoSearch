#ifndef SEARCH_ENGINE_H
#define SEARCH_ENGINE_H

#include "safe_queue.h"
#include "command_t.h"
#include "entry_t.h"

#include <thread>
#include <vector>
#include <string>
#include <string_view>
#include <optional>
#include <atomic>

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
	        const std::string_view text, const std::vector<std::string>& words);

	[[nodiscard]] int score(const Entry& entry, const std::string_view query) const;

	[[nodiscard]] std::vector<std::string> find_completions(const std::string_view query) const;

	void search_worker(const std::stop_token stoken);

public:
	explicit SearchEngine(std::vector<Entry> entries);

	~SearchEngine();

	void set_queue(SafeQueue<Command>* q);

	[[nodiscard]] std::jthread start();

	void update_query(const std::string& q);

	[[nodiscard]] std::string get_query() const;

	[[nodiscard]] std::optional<std::string> get_completion() const;

	[[nodiscard]] const Entry& get_entry(const size_t idx) const;

	[[nodiscard]] size_t get_entry_count() const;

	[[nodiscard]] std::vector<SearchResult> get_results() const;

	[[nodiscard]] std::vector<std::string> get_completions() const;
};

#endif
