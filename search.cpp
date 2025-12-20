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
} // namespace Score

namespace Display {
constexpr size_t MaxResults       = 10000;
constexpr size_t SeparatorLength  = 60;
constexpr size_t MaxPreviewLength = 80;
constexpr size_t MinLinesPerResult = 3;  // Minimum lines needed per result
constexpr size_t MinVisibleResults = 2;  // Try to show at least 2 results
} // namespace Display

namespace Timing {
constexpr auto SearchSleep = 30ms;
constexpr auto IOSleep     = 50ms;
} // namespace Timing

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
} // namespace Color

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
	size_t terminal_height = 0;
	size_t header_lines = 0;
	size_t footer_lines = 0;
	size_t available_lines = 0;
	size_t lines_per_result = Display::MinLinesPerResult;
	size_t max_visible_results = 0;
	bool dirty = true;  // Need to recalculate
};

struct DisplayState {
	size_t scroll_offset = 0;
	int selected_index   = -1;
	DisplayMetrics metrics = {};
	size_t last_terminal_height = 0;  // For detecting resize
};

// Commands for thread communication
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
	std::string word = {};

	while (ss >> word) {
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
	// Query cursor position using ANSI escape code
	std::cout << "\033[6n" << std::flush;

	char buf[32] = {};
	size_t i = 0;

	// Read response: ESC [ row ; col R
	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) {
			break;
		}
		if (buf[i] == 'R') {
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
	// Move to home and clear from cursor to end of screen
	std::cout << "\033[H\033[J"sv;
#endif
}

constexpr void clear_to_end_of_screen()
{
#ifdef _WIN32
	// Windows doesn't have a simple equivalent, use spaces
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
		const COORD start = csbi.dwCursorPosition;
		const DWORD size = (csbi.dwSize.X * csbi.dwSize.Y) -
		                   (start.Y * csbi.dwSize.X + start.X);
		DWORD written = 0;
		FillConsoleOutputCharacter(GetStdHandle(STD_OUTPUT_HANDLE),
		                          ' ', size, start, &written);
		SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), start);
	}
#else
	std::cout << "\033[J"sv;  // Clear from cursor to end
#endif
}

void move_cursor(const size_t row, const size_t col)
{
#ifdef _WIN32
	COORD coord = {};
	coord.X = static_cast<SHORT>(col - 1);
	coord.Y = static_cast<SHORT>(row - 1);
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
	std::queue<T> queue_        = {};
	std::mutex mutex_           = {};
	std::condition_variable cv_ = {};
	std::atomic<bool> running_{true};

public:
	void push(T item)
	{
		{
			const std::scoped_lock lock(mutex_);
			queue_.push(std::move(item));
		}
		cv_.notify_one();
	}

	[[nodiscard]] std::optional<T> pop(const std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(mutex_);
		if (cv_.wait_for(lock, timeout, [this] {
			    return !queue_.empty() || !running_;
		    })) {
			if (!queue_.empty()) {
				T item = std::move(queue_.front());
				queue_.pop();
				return item;
			}
		}
		return std::nullopt;
	}

	void shutdown()
	{
		running_ = false;
		cv_.notify_all();
	}
};

// ============================================================================
// XML Parser
// ============================================================================

class XMLParser {
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

			const auto* const root = doc.FirstChildElement("LaunchBox");
			if (!root) {
				std::cerr << "Error: No LaunchBox root element found\n"sv;
				return std::nullopt;
			}

			const auto alt_names = parse_alternate_names(root);
			return parse_games(root, alt_names);
		} catch (const std::exception& e) {
			std::cerr << "Error parsing XML: "sv << e.what() << '\n';
			return std::nullopt;
		} catch (...) {
			std::cerr << "Unknown error parsing XML\n"sv;
			return std::nullopt;
		}
	}

private:
	[[nodiscard]] static std::map<std::string, std::set<std::string>>
	parse_alternate_names(const tinyxml2::XMLElement* const root)
	{
		std::map<std::string, std::set<std::string>> names = {};

		try {
			constexpr auto get_text =
			        [](const auto* const parent,
			           const char* const tag) -> const char* {
				const auto* const e = parent->FirstChildElement(tag);
				return e ? e->GetText() : nullptr;
			};

			for (const auto* elem =
			             root->FirstChildElement("AlternateName");
			     elem;
			     elem = elem->NextSiblingElement("AlternateName")) {

				if (const auto* const id = get_text(elem, "GameId");
				    id) {
					if (const auto* const name = get_text(elem, "Name");
					    name) {
						names[id].insert(name);
					}
				}
			}
		} catch (...) {
			// Return partial results if parsing fails
		}
		return names;
	}

	[[nodiscard]] static std::vector<Entry> parse_games(
	        const tinyxml2::XMLElement* const root,
	        const std::map<std::string, std::set<std::string>>& alt_names)
	{
		std::vector<Entry> entries = {};

		try {
			constexpr auto get_text =
			        [](const auto* const parent,
			           const char* const tag) -> const char* {
				const auto* const e = parent->FirstChildElement(tag);
				return e ? e->GetText() : nullptr;
			};

			for (const auto* game = root->FirstChildElement("Game"); game;
			     game = game->NextSiblingElement("Game")) {

				try {
					Entry entry = {};

					// Key (RootFolder) - required
					if (const auto* const key = get_text(game, "RootFolder");
					    key) {
						entry.key = key;
					} else {
						continue;
					}

					// Content (Title) - required
					if (const auto* const title = get_text(game, "Title");
					    title) {
						entry.content = title;
					} else {
						continue;
					}

					// Add alternate names
					if (const auto* const id = get_text(game, "ID");
					    id) {
						if (const auto it = alt_names.find(id);
						    it != alt_names.end()) {
							for (const auto& alt :
							     it->second) {
								entry.content += " " + alt;
							}
						}
					}

					// Add year if not already present
					if (const auto* const date =
					            get_text(game, "ReleaseDate");
					    date) {
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
					std::string dev_or_pub;
					if (const auto* const dev = get_text(game, "Developer");
					    dev) {
						dev_or_pub = dev;
						entry.content += " " + dev_or_pub;
					}
					if (const auto* const pub = get_text(game, "Publisher");
					    pub && dev_or_pub != pub) {
						entry.content += " ";
						entry.content += pub;
					}

					entry.words = Util::tokenize(entry.content);
					entries.emplace_back(std::move(entry));
				} catch (...) {
					// Skip this entry if it fails to parse
					continue;
				}
			}

			std::cout << "Loaded "sv << entries.size()
			          << " game entries.\n"sv;
		} catch (...) {
			// Return partial results if parsing fails
		}

		return entries;
	}
};

// ============================================================================
// Search Engine
// ============================================================================

class SearchEngine {
	std::vector<Entry> entries_ = {};
	std::atomic<std::vector<SearchResult>*> results_{nullptr};
	std::atomic<std::vector<std::string>*> completions_{nullptr};
	std::atomic<std::string*> query_{nullptr};
	std::atomic<bool> search_needed_{false};
	SafeQueue<Command>* queue_ = nullptr;

	[[nodiscard]] static constexpr bool has_sequential_match(
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
			bool matched = false;

			const auto add_score = [&](const int points) {
				result += points;
				matched = true;
			};

			if (lower_key.starts_with(qword)) {
				add_score(Score::KeyPrefix);
			} else if (lower_key.find(qword) != std::string::npos) {
				add_score(Score::KeyContains);
			}

			for (const auto& eword : entry.words) {
				if (eword.starts_with(qword)) {
					add_score(Score::WordPrefix);
				} else if (eword.find(qword) != std::string::npos) {
					add_score(Score::WordContains);
				}
			}

			if (lower_content.find(qword) != std::string::npos) {
				add_score(Score::Content);
			}

			if (!matched) {
				return Score::None;
			}
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

		std::set<std::string> comps;
		const auto lower_word = Util::to_lower(word);

		for (const auto& entry : entries_) {
			const auto check_match = [&](const std::string& candidate) {
				if (candidate.empty()) {
					return;
				}
				const auto lower = Util::to_lower(candidate);
				if (lower.starts_with(lower_word) &&
				    lower.length() > word.length()) {
					comps.insert(candidate);
				}
			};

			check_match(entry.key);
			for (const auto& w : entry.words) {
				check_match(w);
			}
		}

		return {comps.begin(), comps.end()};
	}

	void search_worker(const std::stop_token stoken)
	{
		while (!stoken.stop_requested()) {
			if (search_needed_.exchange(false)) {
				const auto* const qptr = query_.load(
				        std::memory_order_acquire);
				if (!qptr) {
					std::this_thread::sleep_for(Timing::SearchSleep);
					continue;
				}

				const std::string q = *qptr;

				auto new_results =
				        std::make_unique<std::vector<SearchResult>>();
				auto new_comps = std::make_unique<std::vector<std::string>>(
				        find_completions(q));

				for (size_t i = 0; i < entries_.size(); ++i) {
					if (const int s = score(entries_[i], q);
					    s > Score::None) {
						new_results->emplace_back(i, s);
					}
				}

				rng::sort(*new_results, [this](const SearchResult& a, const SearchResult& b) {
					return (a.score != b.score)
					             ? (a.score > b.score)
					             : (entries_[a.index]
					                        .content.compare(
					                                entries_[b.index]
					                                        .content) <
					                0);
				});

				if (new_results->size() > Display::MaxResults) {
					new_results->resize(Display::MaxResults);
				}

				delete results_.exchange(new_results.release(),
				                         std::memory_order_acq_rel);

				delete completions_.exchange(new_comps.release(),
				                             std::memory_order_acq_rel);

				if (queue_) {
					queue_->push(RefreshDisplay{
					        {0, -1, {}}
                                        });
				}
			}
			std::this_thread::sleep_for(Timing::SearchSleep);
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

	void set_queue(SafeQueue<Command>* const q)
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
		const auto* const qptr = query_.load(std::memory_order_acquire);
		return qptr ? *qptr : std::string();
	}

	[[nodiscard]] std::optional<std::string> get_completion() const
	{
		const auto* const comps = completions_.load(std::memory_order_acquire);
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
		                                 : (last_space + 1 <= q.length()
		                                            ? q.substr(0, last_space + 1)
		                                            : q);
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

	[[nodiscard]] const Entry& get_entry(const size_t index) const
	{
		return entries_[index];
	}

	[[nodiscard]] size_t get_entry_count() const
	{
		return entries_.size();
	}

	[[nodiscard]] std::vector<SearchResult> get_results() const
	{
		const auto* const rptr = results_.load(std::memory_order_acquire);
		return rptr ? *rptr : std::vector<SearchResult>{};
	}

	[[nodiscard]] std::vector<std::string> get_completions() const
	{
		const auto* const cptr = completions_.load(std::memory_order_acquire);
		return cptr ? *cptr : std::vector<std::string>{};
	}
};

// ============================================================================
// Display Manager
// ============================================================================

class DisplayManager {
	const SearchEngine& engine_;
	mutable size_t cached_terminal_height_ = 0;
	mutable std::chrono::steady_clock::time_point last_height_check_ = {};

	[[nodiscard]] size_t get_terminal_height_cached() const
	{
		// Only check terminal height every 500ms to avoid excessive syscalls
		const auto now = std::chrono::steady_clock::now();
		if (cached_terminal_height_ == 0 ||
		    (now - last_height_check_) > 500ms) {
			cached_terminal_height_ = Util::terminal_height();
			last_height_check_ = now;
		}
		return cached_terminal_height_;
	}

	[[nodiscard]] DisplayMetrics measure_display(const size_t result_count,
	                                             const DisplayMetrics& old_metrics) const
	{
		const size_t current_height = get_terminal_height_cached();

		// If terminal size hasn't changed and metrics exist, reuse them
		if (!old_metrics.dirty &&
		    old_metrics.terminal_height == current_height &&
		    old_metrics.terminal_height > 0) {
			return old_metrics;
		}

		DisplayMetrics metrics = {};
		metrics.terminal_height = current_height;
		metrics.dirty = false;

		// Reserve minimum space for footer (status + controls = 2 lines minimum)
		constexpr size_t min_footer_lines = 3;
		constexpr size_t header_lines = 3;  // Search line + completion + separator

		// Calculate available space more conservatively
		if (metrics.terminal_height > min_footer_lines + header_lines +
		    Display::MinVisibleResults * Display::MinLinesPerResult) {
			metrics.footer_lines = min_footer_lines;
			metrics.header_lines = header_lines;
			metrics.lines_per_result = Display::MinLinesPerResult;

			// Available lines = terminal height - header - footer
			size_t used_lines = header_lines + metrics.footer_lines;
			if (metrics.terminal_height > used_lines) {
				metrics.available_lines = metrics.terminal_height - used_lines;
			} else {
				metrics.available_lines = Display::MinVisibleResults * Display::MinLinesPerResult;
			}

			metrics.max_visible_results = metrics.available_lines / metrics.lines_per_result;

			// Ensure at least minimum visible results
			if (metrics.max_visible_results < Display::MinVisibleResults) {
				metrics.max_visible_results = Display::MinVisibleResults;
			}
		} else {
			// Fallback for very small terminals
			metrics.header_lines = header_lines;
			metrics.footer_lines = min_footer_lines;
			metrics.lines_per_result = Display::MinLinesPerResult;
			metrics.available_lines = Display::MinVisibleResults * Display::MinLinesPerResult;
			metrics.max_visible_results = Display::MinVisibleResults;
		}

		return metrics;
	}

public:
	explicit DisplayManager(const SearchEngine& engine) : engine_(engine) {}

	[[nodiscard]] DisplayMetrics render(DisplayState& state) const
	{
		try {
			// Build entire output in a buffer for atomic display
			std::ostringstream buffer;

			// Clear entire screen and move to home - prevents remnants from previous renders
			buffer << "\033[2J\033[H"sv;

			const std::string query = engine_.get_query();
			buffer << Color::Bold << Color::Cyan << "Search: "sv
			       << Color::Reset << query << Color::Cyan
			       << "_"sv << Color::Reset << '\n';

			// Show completion hint
			const auto comps = engine_.get_completions();
			if (!comps.empty() && !query.empty()) {
				if (const auto hint = engine_.get_completion()) {
					const size_t last_space = query.find_last_of(" \t");
					const std::string preview =
					        (last_space == std::string::npos)
					                ? *hint
					                : (last_space + 1 < hint->length()
					                           ? hint->substr(last_space + 1)
					                           : "");
					if (!preview.empty()) {
						buffer << Color::Dim << "Tab: "sv
						       << Color::Reset << Color::Green
						       << preview << Color::Reset
						       << Color::Dim << " "sv;
						if (comps.size() > 1) {
							buffer << Color::Reset << Color::Gray
							       << "("sv << Color::Yellow
							       << comps.size()
							       << " completions)"sv;
						}
						buffer << '\n';
					}
				}
			}

			buffer << Color::Reset << Color::Gray
			       << std::string(Display::SeparatorLength, '=')
			       << '\n';

			const auto results = engine_.get_results();

			// Measure display space with caching
			DisplayMetrics metrics = measure_display(results.size(), state.metrics);

			// Detect terminal resize
			const size_t current_height = get_terminal_height_cached();
			if (state.last_terminal_height != current_height) {
				state.last_terminal_height = current_height;
				state.metrics.dirty = true;
				metrics = measure_display(results.size(), state.metrics);
			}

			if (results.empty()) {
				if (!query.empty()) {
					buffer << "No matches found.\n"sv;
				}
				std::cout << buffer.str() << std::flush;
				return metrics;
			}

			const size_t display_count = std::min(
			        metrics.max_visible_results,
			        results.size() - state.scroll_offset);

			for (size_t i = 0; i < display_count; ++i) {
				const size_t idx = state.scroll_offset + i;
				if (idx >= results.size()) {
					break;
				}

				const auto& result = results[idx];
				if (result.index >= engine_.get_entry_count()) {
					continue;
				}

				const auto& entry = engine_.get_entry(result.index);

				const bool is_selected = (static_cast<int>(idx) ==
				                          state.selected_index);

				if (is_selected) {
					buffer << Color::SelectedBg;
				}

				buffer << (is_selected ? '>' : ' ')
				       << Color::Bold << "["sv << (idx + 1)
				       << "] "sv << Color::Reset;

				if (is_selected) {
					buffer << Color::SelectedBg;
				}

				buffer << entry.key << Color::Dim
				       << " (score: "sv << result.score
				       << ")"sv << Color::Reset << "\n    "sv;

				if (entry.content.length() > Display::MaxPreviewLength) {
					const size_t truncate_len = std::min(
					        Display::MaxPreviewLength - 3,
					        entry.content.length());
					buffer << entry.content.substr(0, truncate_len)
					       << "..."sv;
				} else {
					buffer << entry.content;
				}
				buffer << "\n\n"sv;
			}

			buffer << Color::Reset << '\n'
			       << Color::Bold << Color::Cyan << "Showing "sv
			       << (state.scroll_offset + 1) << "-"sv
			       << (state.scroll_offset + display_count)
			       << " of "sv << results.size() << " results"sv
			       << Color::Reset << '\n'
			       << Color::Dim
			       << "↑/↓: Select | PgUp/PgDn: Scroll | Enter: Confirm | "
			       << "Tab: Complete | Esc: Cancel"sv
			       << Color::Reset << '\n';

			// Write entire buffer at once for smooth rendering
			std::cout << buffer.str() << std::flush;

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
			if (const auto i = static_cast<size_t>(index);
			    index >= 0 && i < results.size()) {
				if (results[i].index >= engine_.get_entry_count()) {
					return std::nullopt;
				}
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

	[[nodiscard]] int read_byte() const
	{
		unsigned char c = {};
		return (read(STDIN_FILENO, &c, 1) == 1) ? c : -1;
	}

	void flush_input_buffer() const
	{
#ifdef _WIN32
		while (_kbhit()) {
			_getch();
		}
#else
		tcflush(STDIN_FILENO, TCIFLUSH);
#endif
	}

	[[nodiscard]] int read_byte_timeout(const int timeout_ms) const
	{
#ifdef _WIN32
		const auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout_ms)) {
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

		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = timeout_ms * 1000;

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

		// Ctrl+C or Escape
		if (c == 0x03) {
			return Exit{ExitSuccess};
		}

		// Tab completion
		if (c == 0x09) {
			if (auto comp = engine.get_completion()) {
				query = *comp;
				return UpdateQuery{query};
			}
			return std::nullopt;
		}

		// Backspace
		if (c == 127 || c == 8) {
			if (!query.empty()) {
				query.pop_back();
				return UpdateQuery{query};
			}
			return std::nullopt;
		}

		// Enter
		if (c == '\r' || c == '\n') {
			return SelectResult{-1};
		}

		// Escape sequences
		if (c == 0x1B) {
			// Read with timeout to handle incomplete sequences
			const int c1 = read_byte_timeout(10);
			if (c1 == -1) {
				return Exit{ExitSuccess};
			}
			if (c1 != '[') {
				// Not a standard escape sequence, discard
				flush_input_buffer();
                return std::nullopt;
			}

			const int c2 = read_byte_timeout(10);
			if (c2 == -1) {
				// Incomplete sequence, discard
				flush_input_buffer();
                return std::nullopt;
			}

			switch (c2) {
			case 'A':
				flush_input_buffer();  // Clear any queued repeats
				return MoveSelection{-1};
			case 'B':
				flush_input_buffer();  // Clear any queued repeats
				return MoveSelection{1};
			case '5': {
				if (read_byte_timeout(10) == '~') {
					flush_input_buffer();  // Clear any queued repeats
					return PageScroll{true};
				}
				break;
			}
			case '6': {
				if (read_byte_timeout(10) == '~') {
					flush_input_buffer();  // Clear any queued repeats
					return PageScroll{false};
				}
				break;
			}
			}
			return std::nullopt;
		}

		// Printable characters
		if (c >= 32 && c <= 126) {
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

	DisplayState display_state_ = {};
	std::string query_          = {};
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
							        display_state_.scroll_offset = arg.state.scroll_offset;
							        display_state_.selected_index = arg.state.selected_index;
							        display_state_.metrics.dirty = true;
							        display_state_.metrics = display_.render(display_state_);
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
						        std::cerr << "Command processing error: "sv
						                  << e.what()
						                  << '\n';
					        } catch (...) {
						        std::cerr << "Unknown command processing error\n"sv;
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

		// Initialize or update selection
		if (display_state_.selected_index < 0) {
			display_state_.selected_index = 0;
		} else {
			display_state_.selected_index =
			        std::clamp(display_state_.selected_index + delta,
			                   0,
			                   result_count - 1);
		}

		// Ensure selected item is visible using measured metrics
		const size_t selected = static_cast<size_t>(display_state_.selected_index);
		const size_t max_visible = display_state_.metrics.max_visible_results;

		if (max_visible > 0) {
			// If selected is above visible window, scroll up
			if (selected < display_state_.scroll_offset) {
				display_state_.scroll_offset = selected;
			}
			// If selected is below visible window, scroll down
			else if (selected >= display_state_.scroll_offset + max_visible) {
				display_state_.scroll_offset = selected - max_visible + 1;
			}
		}

		display_state_.metrics = display_.render(display_state_);
	}

	void handle_page_scroll(const bool up)
	{
		const auto results = engine_.get_results();
		if (results.empty()) {
			return;
		}

		const size_t result_count = results.size();
		const size_t max_visible = display_state_.metrics.max_visible_results;

		if (max_visible == 0) {
			return;
		}

		// Calculate page size (typically one screen minus one for overlap)
		const size_t page_size = std::max(size_t(1), max_visible - 1);

		// Move selection by page_size
		if (up) {
			const int new_selected = display_state_.selected_index - static_cast<int>(page_size);
			display_state_.selected_index = std::max(0, new_selected);
		} else {
			const int new_selected = display_state_.selected_index + static_cast<int>(page_size);
			display_state_.selected_index = std::min(new_selected,
			                                         static_cast<int>(result_count) - 1);
		}

		// Adjust scroll to keep selection visible
		const size_t selected = static_cast<size_t>(display_state_.selected_index);

		if (selected < display_state_.scroll_offset) {
			display_state_.scroll_offset = selected;
		} else if (selected >= display_state_.scroll_offset + max_visible) {
			display_state_.scroll_offset = selected - max_visible + 1;
		}

		display_state_.metrics = display_.render(display_state_);
	}

	void handle_select(const int index)
	{
		const auto results = engine_.get_results();

		// Determine which result to select
		int target_index = index;
		if (target_index < 0) {
			if (display_state_.selected_index >= 0) {
				target_index = display_state_.selected_index;
			} else if (results.size() == 1) {
				target_index = 0;
			} else if (results.size() > 1) {
				display_state_.selected_index = 0;
				display_state_.metrics = display_.render(display_state_);
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
			// Initial clear on startup
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
						queue_.push(*cmd);
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
			          << "File format: LaunchBox XML with Game and AlternateName elements\n"sv;
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
