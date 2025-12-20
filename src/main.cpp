// A fast CLI search & launch tool for eXoDOS & eXoWin31
// Copyright (C) 2025 dux119-cmd <dux119-cmd@users.noreply.github.com>
// Licensed under GNU GPL v3+

#include "xml_parser.h"
#include "application.h"

#include <iostream>

// ============================================================================
// Main
// ============================================================================

int main(const int argc, char* const argv[])
{
	try {
		if (argc != 2) {
			std::cout << "Usage: " << argv[0]
			          << " <launchbox_xml_file>\n"
			          << "File format: LaunchBox XML with Game and "
			          << "AlternateName elements\n";
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
