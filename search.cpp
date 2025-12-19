// A fast CLI search & launch tool for eXoDOS & eXoWin31
// Copyright (C) 2025 dux119-cmd <dux119-cmd@users.noreply.github.com>
// Licensed under GNU GPL v3+

#include "tinyxml2.h"
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
constexpr size_t HeaderLines      = 4;
constexpr size_t LinesPerResult   = 3;
} // namespace Display

namespace Timing {
constexpr auto SearchSleep = 10ms;
constexpr auto IOSleep     = 16ms;
} // namespace Timing

constexpr int MaxExitCode = 255;
constexpr int ExitSuccess = 0;
constexpr int ExitError   = 1;

// ============================================================================
// Data Structures
// ============================================================================

struct Entry {
	std::string key;
	std::string content;
	std::vector<std::string> words;
};

struct SearchResult {
	size_t index;
	int score;
};

struct DisplayState {
	size_t scroll_offset = 0;
	int selected_index   = -1;
};

// Commands for thread communication
struct RefreshDisplay {
	DisplayState state;
};
struct UpdateQuery {
	std::string query;
};
struct MoveSelection {
	int delta;
};
struct PageScroll {
	bool up;
};
struct SelectResult {
	int index;
};
struct Exit {
	int code;
};

using Command = std::variant<RefreshDisplay, UpdateQuery, MoveSelection,
                             PageScroll, SelectResult, Exit>;

// ============================================================================
// Utilities
// ============================================================================

namespace Util {
std::string to_lower(std::string_view s)
{
	std::string result;
	result.reserve(s.size());
	rng::transform(s, std::back_inserter(result), [](unsigned char c) {
		return std::tolower(c);
	});
	return result;
}

std::vector<std::string> tokenize(std::string_view text)
{
	std::vector<std::string> words;
	std::istringstream ss{std::string(text)};
	std::string word;

	while (ss >> word) {
		auto end = rng::remove_if(word, [](unsigned char c) {
			           return !std::isalnum(c);
		           }).begin();
		word.erase(end, word.end());

		if (!word.empty()) {
			words.push_back(to_lower(word));
		}
	}
	return words;
}

size_t terminal_height()
{
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
	struct winsize w;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	return w.ws_row;
#endif
}

void clear_screen()
{
#ifdef _WIN32
	std::system("cls");
#else
	std::cout << "\033[2J\033[1;1H";
#endif
}
} // namespace Util

// ============================================================================
// Thread-Safe Queue
// ============================================================================

template <typename T>
class SafeQueue {
	std::queue<T> queue_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<bool> running_{true};

public:
	void push(T item)
	{
		{
			std::scoped_lock lock(mutex_);
			queue_.push(std::move(item));
		}
		cv_.notify_one();
	}

	std::optional<T> pop(std::chrono::milliseconds timeout)
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
	static std::optional<std::vector<Entry>> parse(std::string_view filename)
	{
		try {
			tinyxml2::XMLDocument doc;
			if (doc.LoadFile(std::string(filename).c_str()) !=
			    tinyxml2::XML_SUCCESS) {
				std::cerr << "Error: Cannot open XML file "
				          << filename << '\n';
				return std::nullopt;
			}

			auto* root = doc.FirstChildElement("LaunchBox");
			if (!root) {
				std::cerr << "Error: No LaunchBox root element found\n";
				return std::nullopt;
			}

			auto alt_names = parse_alternate_names(root);
			return parse_games(root, alt_names);
		} catch (const std::exception& e) {
			std::cerr << "Error parsing XML: " << e.what() << '\n';
			return std::nullopt;
		} catch (...) {
			std::cerr << "Unknown error parsing XML\n";
			return std::nullopt;
		}
	}

private:
	static std::map<std::string, std::set<std::string>> parse_alternate_names(
	        tinyxml2::XMLElement* root)
	{
		std::map<std::string, std::set<std::string>> names;

		try {
			for (auto* elem = root->FirstChildElement("AlternateName");
			     elem;
			     elem = elem->NextSiblingElement("AlternateName")) {

				auto get_text = [](auto* parent,
				                   const char* tag) -> const char* {
					auto* e = parent->FirstChildElement(tag);
					return e ? e->GetText() : nullptr;
				};

				if (auto* id = get_text(elem, "GameId"); id) {
					if (auto* name = get_text(elem, "Name");
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

	static std::vector<Entry> parse_games(
	        tinyxml2::XMLElement* root,
	        const std::map<std::string, std::set<std::string>>& alt_names)
	{
		std::vector<Entry> entries;

		try {
			auto get_text = [](auto* parent,
			                   const char* tag) -> const char* {
				auto* e = parent->FirstChildElement(tag);
				return e ? e->GetText() : nullptr;
			};

			for (auto* game = root->FirstChildElement("Game"); game;
			     game       = game->NextSiblingElement("Game")) {

				try {
					Entry entry;

					// Key (RootFolder) - required
					if (auto* key = get_text(game, "RootFolder");
					    key) {
						entry.key = key;
					} else {
						continue;
					}

					// Content (Title) - required
					if (auto* title = get_text(game, "Title");
					    title) {
						entry.content = title;
					} else {
						continue;
					}

					// Add alternate names
					if (auto* id = get_text(game, "ID"); id) {
						if (auto it = alt_names.find(id);
						    it != alt_names.end()) {
							for (const auto& alt :
							     it->second) {
								entry.content += " " + alt;
							}
						}
					}

					// Add year if not already present
					if (auto* date = get_text(game, "ReleaseDate");
					    date) {
						size_t len = std::strlen(date);
						if (len >= 4) {
							std::string_view year(date, 4);
							if (entry.content.find(year) ==
							    std::string::npos) {
								entry.content +=
								        " " +
								        std::string(year);
							}
						}
					}

					// Add developer and publisher
					std::string dev_or_pub;
					if (auto* dev = get_text(game, "Developer");
					    dev) {
						dev_or_pub = dev;
						entry.content += " " + dev_or_pub;
					}
					if (auto* pub = get_text(game, "Publisher");
					    pub && dev_or_pub != pub) {
						entry.content += " " +
						                 std::string(pub);
					}

					entry.words = Util::tokenize(entry.content);
					entries.push_back(std::move(entry));
				} catch (...) {
					// Skip this entry if it fails to parse
					continue;
				}
			}

			std::cout << "Loaded " << entries.size()
			          << " game entries.\n";
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
	std::vector<Entry> entries_;
	std::atomic<std::vector<SearchResult>*> results_{nullptr};
	std::atomic<std::vector<std::string>*> completions_{nullptr};
	std::atomic<std::string*> query_{nullptr};
	std::atomic<bool> search_needed_{false};
	SafeQueue<Command>* queue_ = nullptr;

	static bool has_sequential_match(std::string_view text,
	                                 const std::vector<std::string>& words)
	{
		if (words.empty()) {
			return false;
		}

		auto lower = Util::to_lower(text);
		size_t pos = 0;

		for (const auto& word : words) {
			pos = lower.find(word, pos);
			if (pos == std::string::npos) {
				return false;
			}
			pos += word.length();
		}
		return true;
	}

	int score(const Entry& entry, std::string_view query) const
	{
		if (query.empty()) {
			return Score::Default;
		}

		auto query_words = Util::tokenize(query);
		if (query_words.empty()) {
			return Score::Default;
		}

		int result         = Score::None;
		auto lower_key     = Util::to_lower(entry.key);
		auto lower_content = Util::to_lower(entry.content);

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

			auto add_score = [&](int points) {
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

	std::vector<std::string> find_completions(std::string_view query) const
	{
		if (query.empty() || entries_.empty()) {
			return {};
		}

		std::string q{query};
		size_t last_space = q.find_last_of(" \t");
		std::string word  = (last_space == std::string::npos)
		                          ? q
		                          : (last_space + 1 < q.length()
		                                     ? q.substr(last_space + 1)
		                                     : "");

		if (word.empty()) {
			return {};
		}

		std::set<std::string> comps;
		auto lower_word = Util::to_lower(word);

		for (const auto& entry : entries_) {
			auto check_match = [&](const std::string& candidate) {
				if (candidate.empty()) {
					return;
				}
				auto lower = Util::to_lower(candidate);
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

	void search_worker(std::stop_token stoken)
	{
		while (!stoken.stop_requested()) {
			if (search_needed_.exchange(false)) {
				auto* qptr = query_.load(std::memory_order_acquire);
				if (!qptr) {
					std::this_thread::sleep_for(Timing::SearchSleep);
					continue;
				}

				std::string q = *qptr;

				auto new_results =
				        std::make_unique<std::vector<SearchResult>>();
				auto new_comps = std::make_unique<std::vector<std::string>>(
				        find_completions(q));

				for (size_t i = 0; i < entries_.size(); ++i) {
					if (int s = score(entries_[i], q);
					    s > Score::None) {
						new_results->emplace_back(i, s);
					}
				}

				rng::sort(*new_results,
				          rng::greater{},
				          &SearchResult::score);
				if (new_results->size() > Display::MaxResults) {
					new_results->resize(Display::MaxResults);
				}

				delete results_.exchange(new_results.release(),
				                         std::memory_order_acq_rel);
				delete completions_.exchange(new_comps.release(),
				                             std::memory_order_acq_rel);

				if (queue_) {
					queue_->push(RefreshDisplay{
					        {0, -1}
                                        });
				}
			}
			std::this_thread::sleep_for(Timing::SearchSleep);
		}
	}

public:
	SearchEngine(std::vector<Entry> entries)
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

	std::jthread start()
	{
		return std::jthread(
		        [this](std::stop_token st) { search_worker(st); });
	}

	void update_query(const std::string& q)
	{
		delete query_.exchange(new std::string(q), std::memory_order_acq_rel);
		search_needed_.store(true, std::memory_order_release);
	}

	std::string get_query() const
	{
		auto* qptr = query_.load(std::memory_order_acquire);
		return qptr ? *qptr : std::string();
	}

	std::optional<std::string> get_completion() const
	{
		auto* comps = completions_.load(std::memory_order_acquire);
		if (!comps || comps->empty()) {
			return std::nullopt;
		}

		std::string q = get_query();
		if (q.empty()) {
			return std::nullopt;
		}

		size_t last_space  = q.find_last_of(" \t");
		std::string prefix = (last_space == std::string::npos)
		                           ? ""
		                           : (last_space + 1 <= q.length()
		                                      ? q.substr(0, last_space + 1)
		                                      : q);
		std::string word   = (last_space == std::string::npos)
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

		auto lower_word = Util::to_lower(word);

		// Find common prefix among all completions
		for (size_t i = 1; i < comps->size() && !comp.empty(); ++i) {
			const auto& cand = (*comps)[i];
			if (cand.empty()) {
				continue;
			}

			auto lower_comp = Util::to_lower(comp);
			auto lower_cand = Util::to_lower(cand);

			size_t match_len = 0;
			size_t max_len   = std::min(lower_comp.length(),
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
			auto lower_comp = Util::to_lower(comp);
			if (lower_comp.starts_with(lower_word) &&
			    comp.length() > word.length()) {
				return prefix + comp;
			}
		}
		return std::nullopt;
	}

	const Entry& get_entry(size_t index) const
	{
		return entries_[index];
	}

	size_t get_entry_count() const
	{
		return entries_.size();
	}

	std::vector<SearchResult> get_results() const
	{
		auto* rptr = results_.load(std::memory_order_acquire);
		return rptr ? *rptr : std::vector<SearchResult>{};
	}

	std::vector<std::string> get_completions() const
	{
		auto* cptr = completions_.load(std::memory_order_acquire);
		return cptr ? *cptr : std::vector<std::string>{};
	}
};

// ============================================================================
// Display Manager
// ============================================================================

class DisplayManager {
	const SearchEngine& engine_;

public:
	explicit DisplayManager(const SearchEngine& engine) : engine_(engine) {}

	void render(const DisplayState& state) const
	{
		try {
			Util::clear_screen();

			std::string query = engine_.get_query();
			std::cout << "Search: " << query << "_\n";

			// Show completion hint
			auto comps = engine_.get_completions();
			if (!comps.empty() && !query.empty()) {
				if (auto hint = engine_.get_completion()) {
					size_t last_space = query.find_last_of(" \t");
					std::string preview =
					        (last_space == std::string::npos)
					                ? *hint
					                : (last_space + 1 < hint->length()
					                           ? hint->substr(last_space + 1)
					                           : "");
					if (!preview.empty()) {
						std::cout << "Tab: " << preview
						          << " ";
						if (comps.size() > 1) {
							std::cout << "("
							          << comps.size()
							          << " completions)";
						}
						std::cout << '\n';
					}
				}
			}

			std::cout << std::string(Display::SeparatorLength, '=')
			          << '\n';

			auto results = engine_.get_results();
			if (results.empty()) {
				if (!query.empty()) {
					std::cout << "No matches found.\n";
				}
				std::cout.flush();
				return;
			}

			size_t term_height = Util::terminal_height();
			size_t max_display = (term_height > Display::HeaderLines + 2)
			                           ? (term_height -
			                              Display::HeaderLines -
			                              2) / Display::LinesPerResult
			                           : 5;

			size_t display_count = std::min(max_display,
			                                results.size() -
			                                        state.scroll_offset);

			for (size_t i = 0; i < display_count; ++i) {
				size_t idx = state.scroll_offset + i;
				if (idx >= results.size()) {
					break;
				}

				const auto& result = results[idx];
				if (result.index >= engine_.get_entry_count()) {
					continue;
				}

				const auto& entry = engine_.get_entry(result.index);

				char marker = (static_cast<int>(idx) ==
				               state.selected_index)
				                    ? '>'
				                    : ' ';
				std::cout << marker << "[" << (idx + 1) << "] "
				          << entry.key << " (score: " << result.score
				          << ")\n    ";

				if (entry.content.length() >
				    Display::MaxPreviewLength) {
					size_t truncate_len = std::min(
					        Display::MaxPreviewLength - 3,
					        entry.content.length());
					std::cout << entry.content.substr(0, truncate_len)
					          << "...";
				} else {
					std::cout << entry.content;
				}
				std::cout << "\n\n";
			}

			std::cout << "\nShowing " << (state.scroll_offset + 1)
			          << "-" << (state.scroll_offset + display_count)
			          << " of " << results.size() << " results\n"
			          << "↑/↓: Select | PgUp/PgDn: Scroll | Enter: Confirm | Tab: Complete | Esc: Cancel\n";
			std::cout.flush();
		} catch (const std::exception& e) {
			std::cerr << "Display error: " << e.what() << '\n';
		} catch (...) {
			std::cerr << "Unknown display error\n";
		}
	}

	std::optional<int> select(int index) const
	{
		try {
			auto results = engine_.get_results();
			if (index >= 0 && index < static_cast<int>(results.size())) {
				if (results[index].index >=
				    engine_.get_entry_count()) {
					return std::nullopt;
				}
				const auto& entry = engine_.get_entry(
				        results[index].index);
				std::cout << "\n\nSelected: " << entry.key << '\n'
				          << entry.content << '\n';
				return std::min(static_cast<int>(results[index].index),
				                MaxExitCode);
			}
		} catch (const std::exception& e) {
			std::cerr << "Selection error: " << e.what() << '\n';
		} catch (...) {
			std::cerr << "Unknown selection error\n";
		}
		return std::nullopt;
	}
};

// ============================================================================
// Input Handler
// ============================================================================

class InputHandler {
#ifndef _WIN32
	termios old_term_;
#endif

	bool kbhit() const
	{
#ifdef _WIN32
		return _kbhit() != 0;
#else
		fd_set fds;
		timeval tv{0, 0};
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
#endif
	}

	char getch() const
	{
#ifdef _WIN32
		return _getch();
#else
		char c;
		read(STDIN_FILENO, &c, 1);
		return c;
#endif
	}

	int read_byte() const
	{
		unsigned char c;
		return (read(STDIN_FILENO, &c, 1) == 1) ? c : -1;
	}

public:
	InputHandler()
	{
#ifndef _WIN32
		termios new_term;
		tcgetattr(STDIN_FILENO, &old_term_);
		new_term = old_term_;
		new_term.c_lflag &= ~(ICANON | ECHO);
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

	std::optional<Command> poll(std::string& query, const SearchEngine& engine)
	{
		if (!kbhit()) {
			return std::nullopt;
		}

		char c = getch();

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
			auto results = engine.get_results();
			// Will be handled by application logic
			return SelectResult{-1}; // Signal to handle enter
		}

		// Escape sequences
		if (c == 0x1B) {
			int c1 = read_byte();
			if (c1 == -1) {
				return Exit{ExitSuccess};
			}
			if (c1 != '[') {
				return std::nullopt;
			}

			int c2 = read_byte();
			if (c2 == -1) {
				return std::nullopt;
			}

			switch (c2) {
			case 'A': return MoveSelection{-1};
			case 'B': return MoveSelection{1};
			case '5': {
				if (read_byte() == '~') {
					return PageScroll{true};
				}
				break;
			}
			case '6': {
				if (read_byte() == '~') {
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
	InputHandler input_;
	SafeQueue<Command> queue_;

	DisplayState display_state_;
	std::string query_;
	std::atomic<bool> running_{true};
	std::atomic<int> exit_code_{ExitSuccess};

	void io_worker(std::stop_token stoken)
	{
		while (!stoken.stop_requested() && running_) {
			try {
				auto cmd = queue_.pop(Timing::IOSleep);
				if (!cmd) {
					continue;
				}

				std::visit(
				        [this](auto&& arg) {
					        using T = std::decay_t<decltype(arg)>;

					        try {
						        if constexpr (std::is_same_v<T, RefreshDisplay>) {
							        display_state_ = arg.state;
							        display_.render(display_state_);
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
						        std::cerr << "Command processing error: "
						                  << e.what()
						                  << '\n';
					        } catch (...) {
						        std::cerr << "Unknown command processing error\n";
					        }
				        },
				        *cmd);
			} catch (const std::exception& e) {
				std::cerr << "IO worker error: " << e.what() << '\n';
			} catch (...) {
				std::cerr << "Unknown IO worker error\n";
			}
		}
	}

	void handle_move(int delta)
	{
		auto results = engine_.get_results();
		if (results.empty()) {
			return;
		}

		int result_count = static_cast<int>(results.size());

		if (delta < 0) { // Up
			if (display_state_.selected_index < 0) {
				display_state_.selected_index = 0;
			} else if (display_state_.selected_index > 0) {
				display_state_.selected_index--;
				if (static_cast<size_t>(display_state_.selected_index) <
				    display_state_.scroll_offset) {
					display_state_.scroll_offset =
					        display_state_.selected_index;
				}
			}
		} else { // Down
			if (display_state_.selected_index < 0) {
				display_state_.selected_index = 0;
			} else if (display_state_.selected_index < result_count - 1) {
				display_state_.selected_index++;
				size_t term_height = Util::terminal_height();
				size_t max_display = (term_height > 6)
				                           ? (term_height - 6) / 3
				                           : 5;
				if (static_cast<size_t>(display_state_.selected_index) >=
				    display_state_.scroll_offset + max_display) {
					display_state_.scroll_offset++;
				}
			}
		}
		display_.render(display_state_);
	}

	void handle_page_scroll(bool up)
	{
		auto results = engine_.get_results();
		if (results.empty()) {
			return;
		}

		size_t result_count = results.size();
		size_t term_height  = Util::terminal_height();
		size_t max_display = (term_height > 6) ? (term_height - 6) / 3 : 5;
		size_t page_size = std::max(size_t(1), max_display - 1);

		if (up) {
			if (display_state_.scroll_offset > page_size) {
				display_state_.scroll_offset -= page_size;
			} else {
				display_state_.scroll_offset = 0;
			}
			if (display_state_.selected_index >= 0) {
				display_state_.selected_index = static_cast<int>(
				        display_state_.scroll_offset);
			}
		} else {
			size_t max_offset = (result_count > max_display)
			                          ? result_count - max_display
			                          : 0;
			display_state_.scroll_offset = std::min(
			        display_state_.scroll_offset + page_size, max_offset);
			if (display_state_.selected_index >= 0) {
				size_t new_idx = std::min(display_state_.scroll_offset +
				                                  max_display - 1,
				                          result_count - 1);
				display_state_.selected_index = static_cast<int>(
				        new_idx);
			}
		}
		display_.render(display_state_);
	}

	void handle_select(int index)
	{
		auto results = engine_.get_results();

		// Determine which result to select
		int target_index = index;
		if (target_index < 0) {
			if (display_state_.selected_index >= 0) {
				target_index = display_state_.selected_index;
			} else if (results.size() == 1) {
				target_index = 0;
			} else if (results.size() > 1) {
				display_state_.selected_index = 0;
				display_.render(display_state_);
				return;
			}
		}

		if (auto code = display_.select(target_index)) {
			exit_code_ = *code;
			running_   = false;
		}
	}

public:
	Application(std::vector<Entry> entries)
	        : engine_(std::move(entries)),
	          display_(engine_)
	{
		engine_.set_queue(&queue_);
	}

	int run()
	{
		try {
			engine_.update_query("");

			auto search_thread = engine_.start();
			std::jthread io_thread(
			        [this](std::stop_token st) { io_worker(st); });

			while (running_) {
				try {
					if (auto cmd = input_.poll(query_, engine_)) {
						queue_.push(*cmd);
					}
					std::this_thread::sleep_for(Timing::IOSleep);
				} catch (const std::exception& e) {
					std::cerr << "Input error: " << e.what()
					          << '\n';
				} catch (...) {
					std::cerr << "Unknown input error\n";
				}
			}

			queue_.shutdown();
			std::cout << "\n\nSearch "
			          << (exit_code_ == ExitSuccess ? "terminated"
			                                        : "completed")
			          << ".\n";
			return exit_code_;
		} catch (const std::exception& e) {
			std::cerr << "Fatal error: " << e.what() << '\n';
			return ExitError;
		} catch (...) {
			std::cerr << "Unknown fatal error\n";
			return ExitError;
		}
	}
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[])
{
	try {
		if (argc != 2) {
			std::cout << "Usage: " << argv[0] << " <launchbox_xml_file>\n"
			          << "File format: LaunchBox XML with Game and AlternateName elements\n";
			return ExitError;
		}

		auto entries = XMLParser::parse(argv[1]);
		if (!entries) {
			return ExitError;
		}

		Application app(std::move(*entries));
		return app.run();
	} catch (const std::exception& e) {
		std::cerr << "Fatal error in main: " << e.what() << '\n';
		return ExitError;
	} catch (...) {
		std::cerr << "Unknown fatal error in main\n";
		return ExitError;
	}
}
