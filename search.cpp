
// A fast CLI search & launch tool for eXoDOS & eXoWin31

// Copyright (C) 2025 dux119-cmd <dux119-cmd@users.noreply.github.com>

// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.

// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.

#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <ranges>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cctype>
#include <set>
#include <optional>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <variant>
#include "tinyxml2.h"

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#endif

namespace rng = std::ranges;
namespace vws = std::views;
using namespace std::chrono_literals;

// Scoring constants
constexpr int ScoreSequentialKeyMatch = 5000;
constexpr int ScoreSequentialContentMatch = 3000;
constexpr int ScoreKeyPrefixMatch = 2000;
constexpr int ScoreKeyContainsMatch = 1000;
constexpr int ScoreWordPrefixMatch = 100;
constexpr int ScoreWordContainsMatch = 50;
constexpr int ScoreContentMatch = 10;
constexpr int ScoreDefaultMatch = 1;
constexpr int ScoreNoMatch = 0;

// Display constants
constexpr size_t MaxResultsToKeep = 10000;
constexpr size_t SeparatorLineLength = 60;
constexpr size_t MaxPreviewLength = 80;
constexpr size_t PreviewEllipsisLength = 3;

// Timing constants
constexpr auto SearchWorkerSleepDuration = 10ms;
constexpr auto IOThreadSleepDuration = 16ms;

// Keyboard input constants
constexpr char KeyBackspace1 = 127;
constexpr char KeyBackspace2 = 8;
constexpr char KeyCtrlC = 3;
constexpr char KeyEscape = 27;
constexpr char KeyTab1 = '\t';
constexpr char KeyTab2 = 9;
constexpr char KeyEnter1 = '\n';
constexpr char KeyEnter2 = '\r';
constexpr char KeyPrintableMin = 32;
constexpr char KeyPrintableMax = 126;

// Arrow key codes
constexpr char KeyArrowPrefix = 27;
constexpr char KeyArrowBracket = '[';
constexpr char KeyArrowUp = 'A';
constexpr char KeyArrowDown = 'B';
constexpr char KeyPageUp1 = '5';
constexpr char KeyPageDown1 = '6';

// Process exit code constants
constexpr int MaxExitCode = 255;
constexpr int ExitCodeError = 1;
constexpr int ExitCodeSuccess = 0;

struct Entry {
    std::string key = {};
    std::string content = {};
    std::vector<std::string> words = {};
};

struct SearchResult {
    size_t index = {};
    int score = {};
};

// Display state
struct DisplayState {
    size_t scroll_offset = 0;
    int selected_index = -1;
};

// I/O Command types
struct RefreshDisplayCommand { DisplayState state; };
struct UpdateQueryCommand { std::string query; };
struct MoveSelectionCommand { int delta; };
struct PageScrollCommand { bool page_up; };
struct SelectResultCommand { int index; };
struct ExitCommand { int exit_code; };

using IOCommand = std::variant<RefreshDisplayCommand, UpdateQueryCommand, MoveSelectionCommand,
                               PageScrollCommand, SelectResultCommand, ExitCommand>;

// Helper function to get terminal height
size_t get_terminal_height() {
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

class IOQueue {
private:
    std::queue<IOCommand> commands;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> running{true};

public:
    void push(IOCommand cmd) {
        {
            std::scoped_lock lock(mutex);
            commands.push(std::move(cmd));
        }
        cv.notify_one();
    }

    std::optional<IOCommand> pop(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);

        if (cv.wait_for(lock, timeout, [this] { return !commands.empty() || !running; })) {
            if (!commands.empty()) {
                IOCommand cmd = std::move(commands.front());
                commands.pop();
                return cmd;
            }
        }
        return std::nullopt;
    }

    void shutdown() {
        running = false;
        cv.notify_all();
    }
};

class IncrementalSearchEngine {
private:
    std::vector<Entry> entries;
    std::atomic<std::vector<SearchResult>*> results_ptr{nullptr};
    std::atomic<std::vector<std::string>*> completions_ptr{nullptr};
    std::atomic<std::string*> current_query_ptr{nullptr};

    std::atomic<bool> search_needed{false};
    std::atomic<bool> running{true};
    std::jthread search_thread;

    IOQueue* io_queue = nullptr;

    static constexpr std::string to_lower(const std::string_view s) {
        std::string result;
        result.reserve(s.size());
        rng::transform(s, std::back_inserter(result),
            [](const unsigned char c) { return std::tolower(c); });
        return result;
    }

    static std::vector<std::string> tokenize(const std::string_view text) {
        std::vector<std::string> words;
        std::istringstream ss{std::string(text)};
        std::string word;

        while (ss >> word) {
            const auto end = rng::remove_if(word, [](const unsigned char c) {
                return !std::isalnum(c);
            }).begin();
            word.erase(end, word.end());

            if (!word.empty()) {
                words.push_back(to_lower(word));
            }
        }
        return words;
    }

    static bool has_sequential_match(const std::string_view text, const std::vector<std::string>& query_words) {
        if (query_words.empty()) return false;

        const auto lower_text = to_lower(text);
        size_t search_pos = 0;

        for (const auto& query_word : query_words) {
            const size_t found_pos = lower_text.find(query_word, search_pos);
            if (found_pos == std::string::npos) {
                return false;
            }
            search_pos = found_pos + query_word.length();
        }

        return true;
    }

    int calculate_score(const Entry& entry, const std::string_view query) const {
        if (query.empty()) return ScoreDefaultMatch; // Show all entries when no query

        const auto query_words = tokenize(query);
        if (query_words.empty()) return ScoreDefaultMatch;

        int score = ScoreNoMatch;
        const auto lower_key = to_lower(entry.key);
        const auto lower_content = to_lower(entry.content);

        bool has_sequential_key = false;
        bool has_sequential_content = false;

        if (query_words.size() > 1) {
            has_sequential_key = has_sequential_match(entry.key, query_words);
            has_sequential_content = has_sequential_match(entry.content, query_words);

            if (has_sequential_key) {
                score += ScoreSequentialKeyMatch;
            } else if (has_sequential_content) {
                score += ScoreSequentialContentMatch;
            }
        }

        for (const auto& query_word : query_words) {
            bool word_matched = false;

            if (lower_key.starts_with(query_word)) {
                score += ScoreKeyPrefixMatch;
                word_matched = true;
            } else if (lower_key.find(query_word) != std::string::npos) {
                score += ScoreKeyContainsMatch;
                word_matched = true;
            }

            for (const auto& entry_word : entry.words) {
                if (entry_word.starts_with(query_word)) {
                    score += ScoreWordPrefixMatch;
                    word_matched = true;
                } else if (entry_word.find(query_word) != std::string::npos) {
                    score += ScoreWordContainsMatch;
                    word_matched = true;
                }
            }

            if (lower_content.find(query_word) != std::string::npos) {
                score += ScoreContentMatch;
                word_matched = true;
            }

            if (!word_matched) {
                return ScoreNoMatch;
            }
        }

        return score;
    }

    std::vector<std::string> find_completions(const std::string_view query) const {
        if (query.empty()) return {};

        const std::string query_str{query};
        const size_t last_space_pos = query_str.find_last_of(" \t");
        const std::string current_word =
            (last_space_pos == std::string::npos)
                ? query_str
                : query_str.substr(last_space_pos + 1);

        if (current_word.empty()) return {};

        std::set<std::string> completion_set;
        const auto lower_word = to_lower(current_word);

        for (const auto& entry : entries) {
            const auto lower_key = to_lower(entry.key);
            if (lower_key.starts_with(lower_word) && lower_key.length() > current_word.length()) {
                completion_set.insert(entry.key);
            }

            for (const auto& word : entry.words) {
                if (word.starts_with(lower_word) && word.length() > current_word.length()) {
                    completion_set.insert(word);
                }
            }
        }

        return {completion_set.begin(), completion_set.end()};
    }

    void search_worker(const std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            if (search_needed.exchange(false)) {
                // Get current query atomically
                std::string* query_ptr = current_query_ptr.load(std::memory_order_acquire);
                if (!query_ptr) {
                    std::this_thread::sleep_for(SearchWorkerSleepDuration);
                    continue;
                }

                const std::string query = *query_ptr;

                // Perform search
                auto new_results = std::make_unique<std::vector<SearchResult>>();
                auto new_completions = std::make_unique<std::vector<std::string>>(find_completions(query));

                for (size_t i = 0; i < entries.size(); ++i) {
                    if (const int score = calculate_score(entries[i], query); score > ScoreNoMatch) {
                        new_results->emplace_back(i, score);
                    }
                }

                rng::sort(*new_results, rng::greater{}, &SearchResult::score);

                if (new_results->size() > MaxResultsToKeep) {
                    new_results->resize(MaxResultsToKeep);
                }

                // Atomically swap results and completions
                auto* old_results = results_ptr.exchange(new_results.release(), std::memory_order_acq_rel);
                auto* old_completions = completions_ptr.exchange(new_completions.release(), std::memory_order_acq_rel);

                delete old_results;
                delete old_completions;

                // Notify I/O thread to refresh display with reset state
                if (io_queue) {
                    io_queue->push(RefreshDisplayCommand{{0, -1}});
                }
            }
            std::this_thread::sleep_for(SearchWorkerSleepDuration);
        }
    }

public:
    IncrementalSearchEngine()
        : search_thread([this](const std::stop_token st) { search_worker(st); }) {
        // Initialize with empty containers
        results_ptr.store(new std::vector<SearchResult>(), std::memory_order_release);
        completions_ptr.store(new std::vector<std::string>(), std::memory_order_release);
        current_query_ptr.store(new std::string(), std::memory_order_release);
    }

    ~IncrementalSearchEngine() {
        delete results_ptr.load();
        delete completions_ptr.load();
        delete current_query_ptr.load();
    }

    void set_io_queue(IOQueue* queue) {
        io_queue = queue;
    }

    bool load_xml_file(const std::string_view filename) {
        tinyxml2::XMLDocument doc;
        if (doc.LoadFile(std::string(filename).c_str()) != tinyxml2::XML_SUCCESS) {
            std::cerr << "Error: Cannot open XML file " << filename << '\n';
            return false;
        }

        tinyxml2::XMLElement* root = doc.FirstChildElement("LaunchBox");
        if (!root) {
            std::cerr << "Error: No LaunchBox root element found\n";
            return false;
        }

        // First pass: collect all alt_names indexed by game_id
        std::map<std::string, std::set<std::string>> alt_names;

        for (tinyxml2::XMLElement* alt_element = root->FirstChildElement("AlternateName");
             alt_element != nullptr;
             alt_element = alt_element->NextSiblingElement("AlternateName")) {

            const char* game_id = nullptr;
            const char* name = nullptr;

            if (auto* e = alt_element->FirstChildElement("GameId")) {
                game_id = e->GetText();
            }
            if (auto* e = alt_element->FirstChildElement("Name")) {
                name = e->GetText();
            }

            if (game_id && name) {
                alt_names[game_id].insert(name);
            }
        }

        // Second pass: process Game elements
        for (tinyxml2::XMLElement* game_element = root->FirstChildElement("Game");
             game_element != nullptr;
             game_element = game_element->NextSiblingElement("Game")) {

            Entry entry;
            std::string developer_or_publisher;

            // Extract RootFolder (key)
            if (auto* e = game_element->FirstChildElement("RootFolder")) {
                if (e->GetText()) {
                    entry.key = e->GetText();
                }
            }

            // Skip entries without root_folder
            if (entry.key.empty()) continue;

            // Extract Title
            if (auto* e = game_element->FirstChildElement("Title")) {
                if (e->GetText()) {
                    entry.content = e->GetText();
                }
            }
            // Skip entries without titles
            if (entry.content.empty()) continue;

            // Add alt_names for this game
            if (auto* e = game_element->FirstChildElement("ID")) {
                if (const char* game_id = e->GetText()) {
                    if (auto it = alt_names.find(game_id); it != alt_names.end()) {
                        for (const auto& alt_name : it->second) {
                            entry.content += " " + alt_name;
                        }
                    }
                }
            }

            // Extract ReleaseDate as year only, and only add it if it's not already in the title
            if (auto* e = game_element->FirstChildElement("ReleaseDate")) {
                const auto buf = e->GetText();
                if (buf) {
                    const std::string_view release_year(buf, std::min(std::strlen(buf), 4lu));
                    if (entry.content.rfind(release_year) == std::string::npos) {
                        entry.content += " ";
                        entry.content += release_year;
                    }
                }
            }

            // Extract Developer
            if (auto* e = game_element->FirstChildElement("Developer")) {
                if (e->GetText()) {
                    developer_or_publisher = e->GetText();
                    entry.content += " " + developer_or_publisher;
                }
            }

            // Extract Publisher
            if (auto* e = game_element->FirstChildElement("Publisher")) {
                if (e->GetText() && developer_or_publisher != e->GetText()) {
                    developer_or_publisher = e->GetText();
                    entry.content += " " + developer_or_publisher;
                }
            }

            // Build the entry
            entry.words = tokenize(entry.content);
            entries.push_back(std::move(entry));
        }

        std::cout << "Loaded " << entries.size() << " game entries.\n";
        return true;
    }

    void update_query(const std::string& new_query) {
        std::string* old_query = current_query_ptr.load(std::memory_order_acquire);
        auto* new_query_ptr = new std::string(new_query);

        current_query_ptr.store(new_query_ptr, std::memory_order_release);
        delete old_query;

        search_needed.store(true, std::memory_order_release);
    }

    std::string get_current_query() const {
        std::string* query_ptr = current_query_ptr.load(std::memory_order_acquire);
        return query_ptr ? *query_ptr : std::string();
    }

    std::optional<std::string> get_tab_completion() const {
        std::vector<std::string>* comps = completions_ptr.load(std::memory_order_acquire);
        if (!comps || comps->empty()) return std::nullopt;

        const std::string query = get_current_query();
        if (query.empty()) return std::nullopt;

        const size_t last_space_pos = query.find_last_of(" \t");
        const std::string prefix =
            (last_space_pos == std::string::npos)
                ? ""
                : query.substr(0, last_space_pos + 1);
        const std::string current_word =
            (last_space_pos == std::string::npos)
                ? query
                : query.substr(last_space_pos + 1);

        if (current_word.empty()) return std::nullopt;

        std::string completion = (*comps)[0];
        const auto lower_current = to_lower(current_word);

        for (const auto& candidate : *comps | vws::drop(1)) {
            const auto lower_comp = to_lower(completion);
            const auto lower_cand = to_lower(candidate);

            const auto [it1, it2] = rng::mismatch(lower_comp, lower_cand);
            completion.resize(std::distance(lower_comp.begin(), it1));
        }

        const auto lower_completion = to_lower(completion);
        if (lower_completion.starts_with(lower_current) && completion.length() > current_word.length()) {
            return prefix + completion;
        }

        return std::nullopt;
    }

    void display_results(const DisplayState& state) const {
        const std::string query = get_current_query();
        std::vector<SearchResult>* results = results_ptr.load(std::memory_order_acquire);
        std::vector<std::string>* comps = completions_ptr.load(std::memory_order_acquire);

        #ifdef _WIN32
        std::system("cls");
        #else
        std::cout << "\033[2J\033[1;1H";
        #endif

        std::cout << "Search: " << query << "_\n";

        if (comps && !comps->empty() && !query.empty()) {
            if (const auto hint = get_tab_completion(); hint.has_value()) {
                const auto& full_completion = hint.value();
                const size_t last_space = query.find_last_of(" \t");
                const std::string completion_preview =
                    (last_space == std::string::npos)
                        ? full_completion
                        : full_completion.substr(last_space + 1);

                std::cout << "Tab: " << completion_preview << " ";
            }
            if (comps->size() > 1) {
                std::cout << "(" << comps->size() << " completions)";
            }
            std::cout << '\n';
        }

        std::cout << std::string(SeparatorLineLength, '=') << '\n';

        if (!results || results->empty()) {
            if (!query.empty()) {
                std::cout << "No matches found.\n";
            }
        } else {
            const size_t term_height = get_terminal_height();
            const size_t header_lines = 4;
            const size_t lines_per_result = 3;
            const size_t max_display = (term_height > header_lines + 2)
                ? (term_height - header_lines - 2) / lines_per_result
                : 5;

            const size_t total_results = results->size();
            const size_t display_count = std::min(max_display, total_results - state.scroll_offset);

            for (size_t i = 0; i < display_count; ++i) {
                const size_t result_idx = state.scroll_offset + i;
                const auto& result = (*results)[result_idx];
                const auto& entry = entries[result.index];

                const bool is_selected = (static_cast<int>(result_idx) == state.selected_index);
                const char marker = is_selected ? '>' : ' ';

                std::cout << marker << "[" << (result_idx + 1) << "] " << entry.key
                         << " (score: " << result.score << ")\n";

                const std::string_view preview = entry.content;
                if (preview.length() > MaxPreviewLength) {
                    const auto truncated_length = MaxPreviewLength - PreviewEllipsisLength;
                    std::cout << "    " << preview.substr(0, truncated_length) << "...\n\n";
                } else {
                    std::cout << "    " << preview << "\n\n";
                }
            }

            std::cout << "\nShowing " << (state.scroll_offset + 1) << "-"
                     << (state.scroll_offset + display_count) << " of " << total_results
                     << " results\n";
            std::cout << "↑/↓: Select | PgUp/PgDn: Scroll | Enter: Confirm | Tab: Complete | Esc: Cancel\n";
        }
        std::cout.flush();
    }

    std::optional<int> select_result(const int index) const {
        std::vector<SearchResult>* results = results_ptr.load(std::memory_order_acquire);
        if (results && index >= 0 && index < static_cast<int>(results->size())) {
            const auto& entry = entries[(*results)[index].index];
            std::cout << "\n\nSelected: " << entry.key << '\n' << entry.content << '\n';
            return static_cast<int>((*results)[index].index);
        }
        return std::nullopt;
    }

    size_t get_results_count() const {
        std::vector<SearchResult>* results = results_ptr.load(std::memory_order_acquire);
        return results ? results->size() : 0;
    }
};

class KeyboardInput {
private:
    #ifndef _WIN32
    termios old_term;
    #endif

public:
    KeyboardInput() {
        #ifndef _WIN32
        termios new_term;
        tcgetattr(STDIN_FILENO, &old_term);
        new_term = old_term;
        new_term.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
        #endif
    }

    ~KeyboardInput() {
        #ifndef _WIN32
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        #endif
    }

    KeyboardInput(const KeyboardInput&) = delete;
    KeyboardInput& operator=(const KeyboardInput&) = delete;
    KeyboardInput(KeyboardInput&&) = delete;
    KeyboardInput& operator=(KeyboardInput&&) = delete;

    bool kbhit() const {
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

    char getch() const {
        #ifdef _WIN32
        return _getch();
        #else
        char c;
        read(STDIN_FILENO, &c, 1);
        return c;
        #endif
    }
};

class SearchApplication {
private:
    IncrementalSearchEngine engine;
    KeyboardInput kb;
    IOQueue io_queue;
    std::jthread io_thread;

    DisplayState display_state;
    std::string query;
    std::atomic<bool> running{true};
    std::atomic<int> exit_code{ExitCodeSuccess};

    void io_worker(const std::stop_token stoken) {
        while (!stoken.stop_requested() && running) {
            auto cmd = io_queue.pop(IOThreadSleepDuration);

            if (cmd.has_value()) {
                std::visit([this](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;

                    if constexpr (std::is_same_v<T, RefreshDisplayCommand>) {
                        display_state = arg.state;
                        engine.display_results(display_state);
                    }
                    else if constexpr (std::is_same_v<T, UpdateQueryCommand>) {
                        query = std::move(arg.query);
                        engine.update_query(query);
                        // Don't display here - wait for search to complete
                    }
                    else if constexpr (std::is_same_v<T, MoveSelectionCommand>) {
                        const size_t result_count = engine.get_results_count();
                        if (result_count == 0) return;

                        if (arg.delta < 0) { // Up
                            if (display_state.selected_index < 0) {
                                display_state.selected_index = 0;
                            } else if (display_state.selected_index > 0) {
                                display_state.selected_index--;
                                if (static_cast<size_t>(display_state.selected_index) < display_state.scroll_offset) {
                                    display_state.scroll_offset = display_state.selected_index;
                                }
                            }
                        } else { // Down
                            if (display_state.selected_index < 0) {
                                display_state.selected_index = 0;
                            } else if (display_state.selected_index < static_cast<int>(result_count) - 1) {
                                display_state.selected_index++;
                                const size_t term_height = get_terminal_height();
                                const size_t max_display = (term_height - 6) / 3;
                                if (static_cast<size_t>(display_state.selected_index) >= display_state.scroll_offset + max_display) {
                                    display_state.scroll_offset++;
                                }
                            }
                        }
                        engine.display_results(display_state);
                    }
                    else if constexpr (std::is_same_v<T, PageScrollCommand>) {
                        const size_t result_count = engine.get_results_count();
                        if (result_count == 0) return;

                        const size_t term_height = get_terminal_height();
                        const size_t max_display = (term_height - 6) / 3;
                        const size_t page_size = std::max(size_t(1), max_display - 1);

                        if (arg.page_up) {
                            if (display_state.scroll_offset > 0) {
                                display_state.scroll_offset = (display_state.scroll_offset > page_size)
                                    ? display_state.scroll_offset - page_size : 0;
                            }
                            if (display_state.selected_index >= 0) {
                                display_state.selected_index = display_state.scroll_offset;
                            }
                        } else {
                            const size_t max_offset = (result_count > max_display) ? result_count - max_display : 0;
                            display_state.scroll_offset = std::min(display_state.scroll_offset + page_size, max_offset);
                            if (display_state.selected_index >= 0) {
                                display_state.selected_index = std::min(
                                    display_state.scroll_offset + max_display - 1,
                                    static_cast<size_t>(result_count - 1)
                                );
                            }
                        }
                        engine.display_results(display_state);
                    }
                    else if constexpr (std::is_same_v<T, SelectResultCommand>) {
                        if (auto idx = engine.select_result(arg.index); idx.has_value()) {
                            exit_code.store(std::min(idx.value(), MaxExitCode));
                            running = false;
                        }
                    }
                    else if constexpr (std::is_same_v<T, ExitCommand>) {
                        exit_code.store(arg.exit_code);
                        running = false;
                    }
                }, *cmd);
            }
        }
    }

    int readByte() {
        unsigned char c;
        return (read(STDIN_FILENO, &c, 1) == 1) ? c : -1;
    }

    void handle_input(char c) {

        if (c == -1) return;

        switch (c) {
        case 0x03:                          // Ctrl+C
            io_queue.push(ExitCommand{ExitCodeSuccess});
            break;
        case 0x09:                          // Tab
            if (auto completion = engine.get_tab_completion(); completion.has_value()) {
                io_queue.push(UpdateQueryCommand{std::move(completion.value())});
            }
            break;

        case KeyBackspace1:
        case KeyBackspace2:
            if (!query.empty()) {
                query.pop_back();
                io_queue.push(UpdateQueryCommand{query});
            }
            break;

        case '\r':
        case '\n':                          // Enter/Return
            {
                const size_t result_count = engine.get_results_count();

                if (display_state.selected_index >= 0) {
                    io_queue.push(SelectResultCommand{display_state.selected_index});
                } else if (result_count == 1) {
                    io_queue.push(SelectResultCommand{0});
                } else if (result_count > 1) {
                    display_state.selected_index = 0;
                    io_queue.push(RefreshDisplayCommand{display_state});
                }
            }
            break;
        case 0x1B: {                        // ESC or escape sequence
            int c1 = readByte();
            if (c1 == -1) { io_queue.push(ExitCommand{ExitCodeSuccess}); break; }

            if (c1 != '[') {                // lone ESC or Alt+key (treat as Escape then push back c1 by handling it)
                std::cout << "Escape\n";
                // c1 is next input; handle it in next loop iteration by placing it into variable:
                // simple approach: process c1 immediately if printable
                if (c1 >= 32 && c1 <= 126) std::cout << "Char: " << static_cast<char>(c1) << '\n';
                break;
            }

            int c2 = readByte();
            if (c2 == -1) break;

            switch (c2) {
            case 'A':  io_queue.push(MoveSelectionCommand{-1}); break;
            case 'B':  io_queue.push(MoveSelectionCommand{1}); break;
            case '5': {                        // ESC [ 5 ~  => Page Up
                int c3 = readByte(); if (c3 == '~') io_queue.push(PageScrollCommand{true}); break;
            }
            case '6': {                        // ESC [ 6 ~  => Page Down
                int c3 = readByte(); if (c3 == '~') io_queue.push(PageScrollCommand{false}); break;
            }
            default:
                // consume potential trailing "~" for other sequences, or ignore
                if (c2 >= '0' && c2 <= '9') {
                    // read until '~' (simple bounded consume)
                    for (int i = 0; i < 4; ++i) { int x = readByte(); if (x == '~' || x == -1) break; }
                }
                break;
            }
            break;
        }
        default:
            if (c >= 32 && c <= 126) {
                query += c;
                io_queue.push(UpdateQueryCommand{query});
            }
            else std::cout << "Ctrl-[" << c << "]\n";
            break;
        }
    };

public:
    SearchApplication()
        : io_thread([this](const std::stop_token st) { io_worker(st); }) {
        engine.set_io_queue(&io_queue);
    }

    bool initialize(const std::string_view filename) {
        if (!engine.load_xml_file(filename)) {
            return false;
        }
        // Trigger initial search to show all entries
        engine.update_query("");
        return true;
    }

    int run() {
        while (running) {
            if (kb.kbhit()) {
                const char ch = kb.getch();
                handle_input(ch);
            }
            std::this_thread::sleep_for(IOThreadSleepDuration);
        }

        io_queue.shutdown();

        std::cout << "\n\nSearch " << (exit_code.load() == ExitCodeSuccess ? "terminated" : "completed") << ".\n";
        return exit_code.load();
    }
};

int main(const int argc, char* const argv[]) {
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <launchbox_xml_file>\n"
                  << "File format: LaunchBox XML with Game and AlternateName elements\n";
        return ExitCodeError;
    }

    SearchApplication app;

    if (!app.initialize(argv[1])) {
        return ExitCodeError;
    }

    return app.run();
}
