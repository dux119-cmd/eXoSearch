// A fast CLI search & launch tool for eXoDOS & eXoWin31
// Copyright (C) 2025 dux119-cmd <dux119-cmd@users.noreply.github.com>
// Licensed under GNU GPL v3+

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <tinyxml2.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace rng = std::ranges;
namespace vws = std::views;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

// ============================================================================
// Constants
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
}

namespace Display {
constexpr size_t MaxResults        = 10000;
constexpr size_t SeparatorLength   = 60;
constexpr size_t MaxPreviewLength  = 80;
constexpr size_t MinLinesPerResult = 3;
constexpr size_t MinVisibleResults = 2;
}

namespace Timing {
constexpr auto SearchSleep  = 30ms;
constexpr auto IOSleep      = 50ms;
constexpr auto HeightCache  = 500ms;
constexpr auto InputTimeout = 10ms;
}

constexpr int MaxExitCode = 255;
constexpr int ExitSuccess = 0;
constexpr int ExitError   = 1;

// ============================================================================
// ANSI Color Codes
// ============================================================================

namespace Color {
constexpr auto Reset      = "\033[0m"sv;
constexpr auto Bold       = "\033[1m"sv;
constexpr auto Dim        = "\033[2m"sv;
constexpr auto Cyan       = "\033[96m"sv;
constexpr auto Green      = "\033[92m"sv;
constexpr auto Yellow     = "\033[93m"sv;
constexpr auto Gray       = "\033[90m"sv;
constexpr auto SelectedBg = "\033[48;5;24m\033[97m"sv;
}

// ============================================================================
// Data Structures
// ============================================================================

struct Entry {
	std::string key                = {};
	std::string content            = {};
	std::vector<std::string> words = {};
};

struct SearchResult {
	size_t index = {};
	int score    = {};
};

struct DisplayMetrics {
	size_t terminal_height     = 0;
	size_t header_lines        = 0;
	size_t footer_lines        = 0;
	size_t available_lines     = 0;
	size_t lines_per_result    = Display::MinLinesPerResult;
	size_t max_visible_results = 0;
	bool dirty                 = true;
};

struct DisplayState {
	size_t scroll_offset        = 0;
	int selected_index          = -1;
	DisplayMetrics metrics      = {};
	size_t last_terminal_height = 0;
};

struct RefreshDisplay {
	DisplayState state = {};
};
struct UpdateQuery {
	std::string query = {};
};
struct MoveSelection {
	int delta = {};
};
struct PageScroll {
	bool up = {};
};
struct SelectResult {
	int index = {};
};
struct Exit {
	int code = {};
};

using Command = std::variant<RefreshDisplay, UpdateQuery, MoveSelection,
                             PageScroll, SelectResult, Exit>;

// ============================================================================
// Utilities
// ============================================================================

namespace Util {

[[nodiscard]] constexpr std::string to_lower(const std::string_view s)
{
	std::string result = {};
	result.reserve(s.size());
	rng::transform(s, std::back_inserter(result), [](const unsigned char c) {
		return std::tolower(c);
	});
	return result;
}

[[nodiscard]] constexpr std::vector<std::string> tokenize(const std::string_view text)
{
	std::vector<std::string> words = {};
	std::istringstream ss{std::string(text)};

	for (std::string word = {}; ss >> word;) {
		const auto end = rng::remove_if(word, [](const unsigned char c) {
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

constexpr void clear_screen()
{
#ifdef _WIN32
	std::system("cls");
#else
	std::cout << "\033[H\033[J"sv;
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
	std::cout << "\033[J"sv;
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

} // namespace Util

// ============================================================================
// Thread-Safe Queue
// ============================================================================

template <typename T>
class SafeQueue {
	std::queue<T> queue_;
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<bool> running_{true};

public:
	// 1. Perfect forwarding - constructs T in-place
	template<typename... Args>
	void emplace(Args&&... args)
	{
		{
			std::scoped_lock lock(mutex_);
			queue_.emplace(std::forward<Args>(args)...);
		}
		cv_.notify_one();
	}

	// 2. Universal reference for efficient push
	template<typename U>
	void push(U&& item)
	{
		{
			std::scoped_lock lock(mutex_);
			queue_.push(std::forward<U>(item));
		}
		cv_.notify_one();
	}

	// 3. Non-blocking try_pop (more efficient when timeout not needed)
	[[nodiscard]] std::optional<T> try_pop()
	{
		std::scoped_lock lock(mutex_);
		if (queue_.empty()) {
			return std::nullopt;
		}
		T item = std::move(queue_.front());
		queue_.pop();
		return item;
	}

	// 4. Blocking pop with timeout
	[[nodiscard]] std::optional<T> pop(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(mutex_);
		if (!cv_.wait_for(lock, timeout, [this] {
			return !queue_.empty() || !running_.load(std::memory_order_relaxed);
		})) {
			return std::nullopt;
		}

		if (queue_.empty()) {
			return std::nullopt;
		}

		T item = std::move(queue_.front());
		queue_.pop();
		return item;
	}

	// 5. Blocking pop without timeout
	[[nodiscard]] std::optional<T> pop()
	{
		std::unique_lock lock(mutex_);
		cv_.wait(lock, [this] {
			return !queue_.empty() || !running_.load(std::memory_order_relaxed);
		});

		if (queue_.empty()) {
			return std::nullopt;
		}

		T item = std::move(queue_.front());
		queue_.pop();
		return item;
	}

	void shutdown()
	{
		running_.store(false, std::memory_order_relaxed);
		cv_.notify_all();
	}

	// 6. Thread-safe size check
	[[nodiscard]] size_t size() const
	{
		std::scoped_lock lock(mutex_);
		return queue_.size();
	}

	// 7. Thread-safe empty check
	[[nodiscard]] bool empty() const
	{
		std::scoped_lock lock(mutex_);
		return queue_.empty();
	}
};

// ============================================================================
// XML Parser
// ============================================================================

class XMLParser {
	static constexpr auto get_text = [](const auto* parent, const char* tag) {
		const auto* elem = parent->FirstChildElement(tag);
		return elem ? elem->GetText() : nullptr;
	};

	[[nodiscard]] static std::map<std::string, std::set<std::string>>
	parse_alternate_names(const tinyxml2::XMLElement* root)
	{
		std::map<std::string, std::set<std::string>> names = {};

		try {
			for (const auto* elem =
			             root->FirstChildElement("AlternateName");
			     elem;
			     elem = elem->NextSiblingElement("AlternateName")) {

				const auto* id   = get_text(elem, "GameId");
				const auto* name = get_text(elem, "Name");

				if (id && name) {
					names[id].insert(name);
				}
			}
		} catch (...) {
		}

		return names;
	}

	[[nodiscard]] static std::vector<Entry> parse_games(
	        const tinyxml2::XMLElement* root,
	        const std::map<std::string, std::set<std::string>>& alt_names)
	{
		std::vector<Entry> entries = {};

		try {
			for (const auto* game = root->FirstChildElement("Game"); game;
			     game = game->NextSiblingElement("Game")) {

				try {
					const auto* key = get_text(game, "RootFolder");
					const auto* title = get_text(game, "Title");

					if (!key || !title) {
						continue;
					}

					Entry entry = {.key = key, .content = title};

					// Add alternate names
					if (const auto* id = get_text(game, "ID")) {
						if (const auto it = alt_names.find(id);
						    it != alt_names.end()) {
							for (const auto& alt :
							     it->second) {
								entry.content += " " + alt;
							}
						}
					}

					// Add year if not present
					if (const auto* date = get_text(game, "ReleaseDate")) {
						const size_t len = std::strlen(date);
						if (len >= 4) {
							const std::string_view year(
							        date, 4);
							if (entry.content.find(year) ==
							    std::string::npos) {
								entry.content += " ";
								entry.content += year;
							}
						}
					}

					// Add developer and publisher
					const auto* dev = get_text(game, "Developer");
					const auto* pub = get_text(game, "Publisher");

					if (dev) {
						entry.content += " ";
						entry.content += dev;
					}
					if (pub && (!dev || std::string(dev) != pub)) {
						entry.content += " ";
						entry.content += pub;
					}

					entry.words = Util::tokenize(entry.content);
					entries.emplace_back(std::move(entry));
				} catch (...) {
					continue;
				}
			}

			std::cout << "Loaded "sv << entries.size()
			          << " game entries.\n"sv;
		} catch (...) {
		}

		return entries;
	}

public:
	[[nodiscard]] static std::optional<std::vector<Entry>> parse(
	        const std::string_view filename)
	{
		try {
			tinyxml2::XMLDocument doc = {};
			if (doc.LoadFile(std::string(filename).c_str()) !=
			    tinyxml2::XML_SUCCESS) {
				std::cerr << "Error: Cannot open XML file "sv
				          << filename << '\n';
				return std::nullopt;
			}

			const auto* root = doc.FirstChildElement("LaunchBox");
			if (!root) {
				std::cerr << "Error: No LaunchBox root element found\n"sv;
				return std::nullopt;
			}

			return parse_games(root, parse_alternate_names(root));
		} catch (const std::exception& e) {
			std::cerr << "Error parsing XML: "sv << e.what() << '\n';
			return std::nullopt;
		} catch (...) {
			std::cerr << "Unknown error parsing XML\n"sv;
			return std::nullopt;
		}
	}
};

// ============================================================================
// Search Engine
// ============================================================================

class SearchEngine {
	std::vector<Entry> entries_                         = {};
	std::atomic<std::vector<SearchResult>*> results_    = nullptr;
	std::atomic<std::vector<std::string>*> completions_ = nullptr;
	std::atomic<std::string*> query_                    = nullptr;
	std::atomic<bool> search_needed_{false};
	SafeQueue<Command>* queue_ = nullptr;

	[[nodiscard]] static bool has_sequential_match(
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

	[[nodiscard]] int score(const Entry& entry, const std::string_view query) const
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
					word_score = std::max(word_score,
					                      Score::WordPrefix);
				} else if (eword.find(qword) != std::string::npos) {
					word_score = std::max(word_score,
					                      Score::WordContains);
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

	[[nodiscard]] std::vector<std::string> find_completions(const std::string_view query) const
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
			if (lower.starts_with(lower_word) &&
			    lower.length() > word.length()) {
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

	void search_worker(const std::stop_token stoken)
	{
		while (!stoken.stop_requested()) {
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

			rng::sort(*new_results, [this](const auto& a, const auto& b) {
				return (a.score != b.score)
				             ? (a.score > b.score)
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

public:
	explicit SearchEngine(std::vector<Entry> entries)
	        : entries_(std::move(entries)),
	          results_(new std::vector<SearchResult>()),
	          completions_(new std::vector<std::string>()),
	          query_(new std::string())
	{}

	~SearchEngine()
	{
		delete results_.load();
		delete completions_.load();
		delete query_.load();
	}

	void set_queue(SafeQueue<Command>* q)
	{
		queue_ = q;
	}

	[[nodiscard]] std::jthread start()
	{
		return std::jthread([this](const std::stop_token st) {
			search_worker(st);
		});
	}

	void update_query(const std::string& q)
	{
		delete query_.exchange(new std::string(q), std::memory_order_acq_rel);
		search_needed_.store(true, std::memory_order_release);
	}

	[[nodiscard]] std::string get_query() const
	{
		const auto* qptr = query_.load(std::memory_order_acquire);
		return qptr ? *qptr : std::string();
	}

	[[nodiscard]] std::optional<std::string> get_completion() const
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

	[[nodiscard]] const Entry& get_entry(const size_t idx) const
	{
		return entries_[idx];
	}

	[[nodiscard]] size_t get_entry_count() const
	{
		return entries_.size();
	}

	[[nodiscard]] std::vector<SearchResult> get_results() const
	{
		const auto* rptr = results_.load(std::memory_order_acquire);
		return rptr ? *rptr : std::vector<SearchResult>{};
	}

	[[nodiscard]] std::vector<std::string> get_completions() const
	{
		const auto* cptr = completions_.load(std::memory_order_acquire);
		return cptr ? *cptr : std::vector<std::string>{};
	}
};

// ============================================================================
// Display Manager
// ============================================================================

class DisplayManager {
	const SearchEngine& engine_;
	mutable size_t cached_height_                             = 0;
	mutable std::chrono::steady_clock::time_point last_check_ = {};

	[[nodiscard]] size_t get_terminal_height_cached() const
	{
		const auto now = std::chrono::steady_clock::now();
		if (cached_height_ == 0 || (now - last_check_) > Timing::HeightCache) {
			cached_height_ = Util::terminal_height();
			last_check_    = now;
		}
		return cached_height_;
	}

	[[nodiscard]] DisplayMetrics measure_display(const DisplayMetrics& old_metrics) const
	{
		const size_t current_height = get_terminal_height_cached();

		// Reuse if unchanged
		if (!old_metrics.dirty &&
		    old_metrics.terminal_height == current_height &&
		    current_height > 0) {
			return old_metrics;
		}

		DisplayMetrics metrics = {.terminal_height = current_height,
		                          .dirty           = false};

		constexpr size_t min_footer = 3;
		constexpr size_t header     = 3;
		constexpr size_t min_space  = Display::MinVisibleResults *
		                             Display::MinLinesPerResult;

		if (current_height > min_footer + header + min_space) {
			metrics.footer_lines     = min_footer;
			metrics.header_lines     = header;
			metrics.lines_per_result = Display::MinLinesPerResult;

			const size_t used           = header + min_footer;
			metrics.available_lines     = (current_height > used)
			                                    ? (current_height - used)
			                                    : min_space;
			metrics.max_visible_results = metrics.available_lines /
			                              metrics.lines_per_result;

			if (metrics.max_visible_results < Display::MinVisibleResults) {
				metrics.max_visible_results = Display::MinVisibleResults;
			}
		} else {
			// Fallback for tiny terminals
			metrics.header_lines     = header;
			metrics.footer_lines     = min_footer;
			metrics.lines_per_result = Display::MinLinesPerResult;
			metrics.available_lines  = min_space;
			metrics.max_visible_results = Display::MinVisibleResults;
		}

		return metrics;
	}

	void render_header(std::ostringstream& buf, const std::string& query,
	                   const std::vector<std::string>& completions) const
	{
		buf << Color::Bold << Color::Cyan << "Search: "sv << Color::Reset
		    << query << Color::Cyan << "_"sv << Color::Reset << '\n';

		if (!completions.empty() && !query.empty()) {
			if (const auto hint = engine_.get_completion()) {
				const size_t last_space = query.find_last_of(" \t");
				const std::string preview =
				        (last_space == std::string::npos)
				                ? *hint
				                : (last_space + 1 < hint->length()
				                           ? hint->substr(last_space + 1)
				                           : "");

				if (!preview.empty()) {
					buf << Color::Dim << "Tab: "sv
					    << Color::Reset << Color::Green
					    << preview << Color::Reset;

					if (completions.size() > 1) {
						buf << Color::Dim << " "sv
						    << Color::Reset << Color::Gray
						    << "("sv << Color::Yellow
						    << completions.size()
						    << " completions)"sv;
					}
					buf << '\n';
				}
			}
		}

		buf << Color::Reset << Color::Gray
		    << std::string(Display::SeparatorLength, '=') << '\n';
	}

	void render_result(std::ostringstream& buf, const SearchResult& result,
	                   size_t display_index, bool selected) const
	{
		if (result.index >= engine_.get_entry_count()) {
			return;
		}

		const auto& entry = engine_.get_entry(result.index);

		if (selected) {
			buf << Color::SelectedBg;
		}

		buf << (selected ? '>' : ' ') << Color::Bold << "["sv
		    << (display_index + 1) << "] "sv << Color::Reset;

		if (selected) {
			buf << Color::SelectedBg;
		}

		buf << entry.key << Color::Dim << " (score: "sv << result.score
		    << ")"sv << Color::Reset << "\n    "sv;

		if (entry.content.length() > Display::MaxPreviewLength) {
			const size_t truncate_len =
			        std::min(Display::MaxPreviewLength - 3,
			                 entry.content.length());
			buf << entry.content.substr(0, truncate_len) << "..."sv;
		} else {
			buf << entry.content;
		}
		buf << "\n\n"sv;
	}

	void render_footer(std::ostringstream& buf, size_t scroll_offset,
	                   size_t display_count, size_t total_results) const
	{
		buf << Color::Reset << '\n'
		    << Color::Bold << Color::Cyan << "Showing "sv
		    << (scroll_offset + 1) << "-"sv
		    << (scroll_offset + display_count) << " of "sv
		    << total_results << " results"sv << Color::Reset << '\n'
		    << Color::Dim
		    << "↑/↓: Select | PgUp/PgDn: Scroll | Enter: Confirm | "
		    << "Tab: Complete | Esc: Cancel"sv << Color::Reset << '\n';
	}

public:
	explicit DisplayManager(const SearchEngine& engine) : engine_(engine) {}

	[[nodiscard]] DisplayMetrics render(DisplayState& state) const
	{
		try {
			std::ostringstream buf;
			buf << "\033[2J\033[H"sv; // Clear screen and home

			const std::string query = engine_.get_query();
			const auto results      = engine_.get_results();
			const auto completions  = engine_.get_completions();

			render_header(buf, query, completions);

			DisplayMetrics metrics = measure_display(state.metrics);

			// Detect terminal resize
			const size_t current_height = get_terminal_height_cached();
			if (state.last_terminal_height != current_height) {
				state.last_terminal_height = current_height;
				state.metrics.dirty        = true;
				metrics = measure_display(state.metrics);
			}

			if (results.empty()) {
				if (!query.empty()) {
					buf << "No matches found.\n"sv;
				}
				std::cout << buf.str() << std::flush;
				return metrics;
			}

			const size_t display_count =
			        std::min(metrics.max_visible_results,
			                 results.size() - state.scroll_offset);

			for (size_t i = 0; i < display_count; ++i) {
				const size_t idx = state.scroll_offset + i;
				if (idx >= results.size()) {
					break;
				}

				const bool selected = (static_cast<int>(idx) ==
				                       state.selected_index);
				render_result(buf, results[idx], idx, selected);
			}

			render_footer(buf,
			              state.scroll_offset,
			              display_count,
			              results.size());

			std::cout << buf.str() << std::flush;
			return metrics;
		} catch (const std::exception& e) {
			std::cerr << "Display error: "sv << e.what() << '\n';
			return {};
		} catch (...) {
			std::cerr << "Unknown display error\n"sv;
			return {};
		}
	}

	[[nodiscard]] std::optional<int> select(const int index) const
	{
		try {
			const auto results = engine_.get_results();
			const auto i       = static_cast<size_t>(index);

			if (index >= 0 && i < results.size() &&
			    results[i].index < engine_.get_entry_count()) {

				const auto& entry = engine_.get_entry(results[i].index);
				std::cout << "\n\nSelected: "sv << entry.key << '\n'
				          << entry.content << '\n';
				return std::min(static_cast<int>(results[i].index),
				                MaxExitCode);
			}
		} catch (const std::exception& e) {
			std::cerr << "Selection error: "sv << e.what() << '\n';
		} catch (...) {
			std::cerr << "Unknown selection error\n"sv;
		}
		return std::nullopt;
	}
};

// ============================================================================
// Input Handler
// ============================================================================

class InputHandler {
#ifndef _WIN32
	termios old_term_ = {};
#endif

	[[nodiscard]] bool kbhit() const
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

	[[nodiscard]] char getch() const
	{
#ifdef _WIN32
		return _getch();
#else
		char c = {};
		return (read(STDIN_FILENO, &c, 1) > 0) ? c : -1;
#endif
	}

	void flush_input() const
	{
#ifdef _WIN32
		while (_kbhit()) {
			_getch();
		}
#else
		tcflush(STDIN_FILENO, TCIFLUSH);
#endif
	}

	[[nodiscard]] int read_timeout(const int timeout_ms) const
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

public:
	InputHandler()
	{
#ifndef _WIN32
		tcgetattr(STDIN_FILENO, &old_term_);
		termios new_term = old_term_;
		new_term.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
#endif
	}

	~InputHandler()
	{
#ifndef _WIN32
		tcsetattr(STDIN_FILENO, TCSANOW, &old_term_);
#endif
	}

	InputHandler(const InputHandler&)            = delete;
	InputHandler& operator=(const InputHandler&) = delete;

	[[nodiscard]] std::optional<Command> poll(std::string& query,
	                                          const SearchEngine& engine)
	{
		if (!kbhit()) {
			return std::nullopt;
		}

		const char c = getch();

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
				const int c3 = read_timeout(static_cast<int>(
				        Timing::InputTimeout.count()));
				if (c3 == '~') {
					flush_input();
					return PageScroll{true};
				}
				break;
			}
			case '6': {
				const int c3 = read_timeout(static_cast<int>(
				        Timing::InputTimeout.count()));
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
			query += c;
			return UpdateQuery{query};
		}

		return std::nullopt;
	}
};

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

	void io_worker(const std::stop_token stoken)
	{
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

	void handle_move(const int delta)
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

	void handle_page_scroll(const bool up)
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

	void handle_select(const int index)
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

public:
	explicit Application(std::vector<Entry> entries)
	        : engine_(std::move(entries)),
	          display_(engine_)
	{
		engine_.set_queue(&queue_);
	}

	[[nodiscard]] int run()
	{
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
};

// ============================================================================
// Main
// ============================================================================

int main(const int argc, char* const argv[])
{
	try {
		if (argc != 2) {
			std::cout << "Usage: "sv << argv[0]
			          << " <launchbox_xml_file>\n"sv
			          << "File format: LaunchBox XML with Game and "
			          << "AlternateName elements\n"sv;
			return ExitError;
		}

		auto entries = XMLParser::parse(argv[1]);
		if (!entries) {
			return ExitError;
		}

		Application app(std::move(*entries));
		return app.run();
	} catch (const std::exception& e) {
		std::cerr << "Fatal error in main: "sv << e.what() << '\n';
		return ExitError;
	} catch (...) {
		std::cerr << "Unknown fatal error in main\n"sv;
		return ExitError;
	}
}