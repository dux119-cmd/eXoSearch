#ifndef COMMAND_T
#define COMMAND_T

#include <variant>

namespace Display {
constexpr size_t MaxResults        = 10000;
constexpr size_t SeparatorLength   = 60;
constexpr size_t MaxPreviewLength  = 80;
constexpr size_t MinLinesPerResult = 3;
constexpr size_t MinVisibleResults = 2;
} // namespace Display

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

#endif