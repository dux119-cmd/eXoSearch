#include "search_engine.h"
#include "timing_t.h"
#include "utilities.h"

#include <algorithm>
#include <ranges>
#include <set>

// ============================================================================
// Search Engine
// ============================================================================

namespace Score {
constexpr int SequentialKey     = 5000;
constexpr int SequentialContent = 3000;
constexpr int KeyPrefix         = 2000;
constexpr int KeyContains       = 1000;
constexpr int WordPrefix        = 100;
constexpr int WordContains      = 50;
constexpr int Content           = 10;
constexpr int Default           = 1;
constexpr int None              = 0;
} // namespace Score

[[nodiscard]] bool SearchEngine::has_sequential_match(
        const std::string_view text, const std::vector<std::string>& words)
{
	if (words.empty()) {
		return false;
	}

	const auto lower = Util::to_lower(text);
	size_t pos       = 0;

	for (const auto& word : words) {
		pos = lower.find(word, pos);
		if (pos == std::string::npos) {
			return false;
		}
		pos += word.length();
	}
	return true;
}

[[nodiscard]] int SearchEngine::score(const Entry& entry,
                                      const std::string_view query) const
{
	if (query.empty()) {
		return Score::Default;
	}

	const auto query_words = Util::tokenize(query);
	if (query_words.empty()) {
		return Score::Default;
	}

	int result               = Score::None;
	const auto lower_key     = Util::to_lower(entry.key);
	const auto lower_content = Util::to_lower(entry.content);

	// Sequential matching bonus
	if (query_words.size() > 1) {
		if (has_sequential_match(entry.key, query_words)) {
			result += Score::SequentialKey;
		} else if (has_sequential_match(entry.content, query_words)) {
			result += Score::SequentialContent;
		}
	}

	// Per-word matching
	for (const auto& qword : query_words) {
		int word_score = Score::None;

		// Check key matches
		if (lower_key.starts_with(qword)) {
			word_score = Score::KeyPrefix;
		} else if (lower_key.find(qword) != std::string::npos) {
			word_score = Score::KeyContains;
		}

		// Check entry word matches
		for (const auto& eword : entry.words) {
			if (eword.starts_with(qword)) {
				word_score = std::max(word_score, Score::WordPrefix);
			} else if (eword.find(qword) != std::string::npos) {
				word_score = std::max(word_score, Score::WordContains);
			}
		}

		// Check content match
		if (lower_content.find(qword) != std::string::npos) {
			word_score = std::max(word_score, Score::Content);
		}

		if (word_score == Score::None) {
			return Score::None;
		}
		result += word_score;
	}
	return result;
}

[[nodiscard]] std::vector<std::string> SearchEngine::find_completions(
        const std::string_view query) const
{
	if (query.empty() || entries_.empty()) {
		return {};
	}

	const std::string q{query};
	const size_t last_space = q.find_last_of(" \t");
	const std::string word  = (last_space == std::string::npos)
	                                ? q
	                                : (last_space + 1 < q.length()
	                                           ? q.substr(last_space + 1)
	                                           : "");

	if (word.empty()) {
		return {};
	}

	std::set<std::string> completions;
	const auto lower_word = Util::to_lower(word);

	const auto check_candidate = [&](const std::string& candidate) {
		if (candidate.empty()) {
			return;
		}
		const auto lower = Util::to_lower(candidate);
		if (lower.starts_with(lower_word) && lower.length() > word.length()) {
			completions.insert(candidate);
		}
	};

	for (const auto& entry : entries_) {
		check_candidate(entry.key);
		for (const auto& w : entry.words) {
			check_candidate(w);
		}
	}

	return {completions.begin(), completions.end()};
}

void SearchEngine::search_worker(std::atomic<bool>& stop_flag)
{
	while (!stop_flag.load(std::memory_order_acquire)) {
		if (!search_needed_.exchange(false)) {
			std::this_thread::sleep_for(Timing::SearchSleep);
			continue;
		}

		const auto* qptr = query_.load(std::memory_order_acquire);
		if (!qptr) {
			std::this_thread::sleep_for(Timing::SearchSleep);
			continue;
		}

		const std::string q = *qptr;

		auto new_results = std::make_unique<std::vector<SearchResult>>();
		auto new_comps = std::make_unique<std::vector<std::string>>(
		        find_completions(q));

		for (size_t i = 0; i < entries_.size(); ++i) {
			const int s = score(entries_[i], q);
			if (s > Score::None) {
				new_results->emplace_back(i, s);
			}
		}

		std::ranges::sort(*new_results, [this](const auto& a, const auto& b) {
			return (a.score != b.score) ? (a.score > b.score)
			                            : (entries_[a.index].content <
			                               entries_[b.index].content);
		});

		if (new_results->size() > Display::MaxResults) {
			new_results->resize(Display::MaxResults);
		}

		delete results_.exchange(new_results.release(),
		                         std::memory_order_acq_rel);
		delete completions_.exchange(new_comps.release(),
		                             std::memory_order_acq_rel);

		if (queue_) {
			queue_->emplace(RefreshDisplay{
			        {0, -1, {}}
                        });
		}
	}
}

SearchEngine::SearchEngine(std::vector<Entry> entries)
        : entries_(std::move(entries)),
          results_(new std::vector<SearchResult>()),
          completions_(new std::vector<std::string>()),
          query_(new std::string())
{}

SearchEngine::~SearchEngine()
{
	delete results_.load();
	delete completions_.load();
	delete query_.load();
}

void SearchEngine::set_queue(SafeQueue<Command>* q)
{
	queue_ = q;
}

[[nodiscard]] std::thread SearchEngine::start(std::atomic<bool>& stop_flag)
{
	return std::thread([this, &stop_flag]() { search_worker(stop_flag); });
}

void SearchEngine::update_query(const std::string& q)
{
	delete query_.exchange(new std::string(q), std::memory_order_acq_rel);
	search_needed_.store(true, std::memory_order_release);
}

[[nodiscard]] std::string SearchEngine::get_query() const
{
	const auto* qptr = query_.load(std::memory_order_acquire);
	return qptr ? *qptr : std::string();
}

[[nodiscard]] std::optional<std::string> SearchEngine::get_completion() const
{
	const auto* comps = completions_.load(std::memory_order_acquire);
	if (!comps || comps->empty()) {
		return std::nullopt;
	}

	const std::string q = get_query();
	if (q.empty()) {
		return std::nullopt;
	}

	const size_t last_space  = q.find_last_of(" \t");
	const std::string prefix = (last_space == std::string::npos)
	                                 ? ""
	                                 : q.substr(0, last_space + 1);
	const std::string word   = (last_space == std::string::npos)
	                                 ? q
	                                 : (last_space + 1 < q.length()
	                                            ? q.substr(last_space + 1)
	                                            : "");

	if (word.empty() || comps->empty()) {
		return std::nullopt;
	}

	std::string comp = (*comps)[0];
	if (comp.empty()) {
		return std::nullopt;
	}

	const auto lower_word = Util::to_lower(word);

	// Find common prefix among all completions
	for (size_t i = 1; i < comps->size() && !comp.empty(); ++i) {
		const auto& cand = (*comps)[i];
		if (cand.empty()) {
			continue;
		}

		const auto lower_comp = Util::to_lower(comp);
		const auto lower_cand = Util::to_lower(cand);

		size_t match_len     = 0;
		const size_t max_len = std::min(lower_comp.length(),
		                                lower_cand.length());
		while (match_len < max_len &&
		       lower_comp[match_len] == lower_cand[match_len]) {
			++match_len;
		}

		if (match_len < comp.length()) {
			comp.resize(match_len);
		}
	}

	if (!comp.empty()) {
		const auto lower_comp = Util::to_lower(comp);
		if (lower_comp.starts_with(lower_word) &&
		    comp.length() > word.length()) {
			return prefix + comp;
		}
	}
	return std::nullopt;
}

[[nodiscard]] const Entry& SearchEngine::get_entry(const size_t idx) const
{
	return entries_[idx];
}

[[nodiscard]] size_t SearchEngine::get_entry_count() const
{
	return entries_.size();
}

[[nodiscard]] std::vector<SearchResult> SearchEngine::get_results() const
{
	const auto* rptr = results_.load(std::memory_order_acquire);
	return rptr ? *rptr : std::vector<SearchResult>{};
}

[[nodiscard]] std::vector<std::string> SearchEngine::get_completions() const
{
	const auto* cptr = completions_.load(std::memory_order_acquire);
	return cptr ? *cptr : std::vector<std::string>{};
}
