#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <tinyxml2.h>
#pragma GCC diagnostic pop

#include "xml_parser.h"

#include "utilities.h"

#include <iostream>
#include <string_view>

// ============================================================================
// XML Parser
// ============================================================================

[[nodiscard]] std::map<std::string, std::set<std::string>> XMLParser::parse_alternate_names(
        const tinyxml2::XMLElement* root)
{
	std::map<std::string, std::set<std::string>> names = {};

	try {
		for (const auto* elem = root->FirstChildElement("AlternateName"); elem;
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

[[nodiscard]] std::vector<Entry> XMLParser::parse_games(
        const tinyxml2::XMLElement* root,
        const std::map<std::string, std::set<std::string>>& alt_names)
{
	std::vector<Entry> entries = {};

	try {
		for (const auto* game = root->FirstChildElement("Game"); game;
		     game             = game->NextSiblingElement("Game")) {

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
						for (const auto& alt : it->second) {
							entry.content += " " + alt;
						}
					}
				}

				// Add year if not present
				if (const auto* date = get_text(game, "ReleaseDate")) {
					const size_t len = std::strlen(date);
					if (len >= 4) {
						const std::string_view year(date, 4);
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

		std::cout << "Loaded " << entries.size() << " game entries.\n";
	} catch (...) {
	}

	return entries;
}

[[nodiscard]] std::optional<std::vector<Entry>> XMLParser::parse(const std::string_view filename)
{
	try {
		tinyxml2::XMLDocument doc = {};
		if (doc.LoadFile(std::string(filename).c_str()) !=
		    tinyxml2::XML_SUCCESS) {
			std::cerr << "Error: Cannot open XML file " << filename
			          << '\n';
			return std::nullopt;
		}

		const auto* root = doc.FirstChildElement("LaunchBox");
		if (!root) {
			std::cerr << "Error: No LaunchBox root element found\n";
			return std::nullopt;
		}

		return parse_games(root, parse_alternate_names(root));
	} catch (const std::exception& e) {
		std::cerr << "Error parsing XML: " << e.what() << '\n';
		return std::nullopt;
	} catch (...) {
		std::cerr << "Unknown error parsing XML\n";
		return std::nullopt;
	}
}
