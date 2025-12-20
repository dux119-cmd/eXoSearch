#include "display_manager.h"
#include "utilities.h"
#include "timing_t.h"
#include "exit_codes_t.h"

// ============================================================================
// ANSI Color Codes
// ============================================================================

namespace Color {

    using namespace std::string_view_literals;

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
// Display Manager
// ============================================================================

[[nodiscard]] size_t DisplayManager::get_terminal_height_cached() const
{
    const auto now = std::chrono::steady_clock::now();
    if (cached_height_ == 0 || (now - last_check_) > Timing::HeightCache) {
        cached_height_ = Util::terminal_height();
        last_check_    = now;
    }
    return cached_height_;
}

[[nodiscard]] DisplayMetrics DisplayManager::measure_display(const DisplayMetrics& old_metrics) const
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

void DisplayManager::render_header(std::ostringstream& buf, const std::string& query,
                    const std::vector<std::string>& completions) const
{
using namespace std::string_view_literals;
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

void DisplayManager::render_result(std::ostringstream& buf, const SearchResult& result,
                    size_t display_index, bool selected) const
{
using namespace std::string_view_literals;
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

void DisplayManager::render_footer(std::ostringstream& buf, size_t scroll_offset,
                    size_t display_count, size_t total_results) const
{
using namespace std::string_view_literals;
    buf << Color::Reset << '\n'
        << Color::Bold << Color::Cyan << "Showing "sv
        << (scroll_offset + 1) << "-"sv
        << (scroll_offset + display_count) << " of "sv
        << total_results << " results"sv << Color::Reset << '\n'
        << Color::Dim
        << "↑/↓: Select | PgUp/PgDn: Scroll | Enter: Confirm | "
        << "Tab: Complete | Esc: Cancel"sv << Color::Reset << '\n';
}

DisplayManager::DisplayManager(const SearchEngine& engine) : engine_(engine) {}

[[nodiscard]] DisplayMetrics DisplayManager::render(DisplayState& state) const
{
using namespace std::string_view_literals;
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

[[nodiscard]] std::optional<int> DisplayManager::select(const int index) const
{
using namespace std::string_view_literals;
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
