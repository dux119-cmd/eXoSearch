#ifndef XML_PARSER_H
#define XML_PARSER_H

#include "entry_t.h"

#include <string>
#include <map>
#include <set>
#include <optional>
#include <string_view>
#include <vector>

// ============================================================================
// XML Parser
// ============================================================================

namespace tinyxml2 { class XMLElement; }

class XMLParser {
	static constexpr auto get_text = [](const auto* parent, const char* tag) {
		const auto* elem = parent->FirstChildElement(tag);
		return elem ? elem->GetText() : nullptr;
	};

	[[nodiscard]] static std::map<std::string, std::set<std::string>>
	parse_alternate_names(const tinyxml2::XMLElement* root);

	[[nodiscard]] static std::vector<Entry> parse_games(
	        const tinyxml2::XMLElement* root,
	        const std::map<std::string, std::set<std::string>>& alt_names);

public:
	[[nodiscard]] static std::optional<std::vector<Entry>> parse(
	        const std::string_view filename);
};

#endif
