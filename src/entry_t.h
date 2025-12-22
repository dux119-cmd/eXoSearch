#ifndef ENTRY_T
#define ENTRY_T

#include <string>
#include <string_view>
#include <vector>

struct Entry {
	std::string key                = {};
	std::string content            = {};
	std::vector<std::string_view> words = {};
};

#endif
