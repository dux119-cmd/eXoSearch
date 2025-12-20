#ifndef ENTRY_T
#define ENTRY_T

#include <string>
#include <vector>

struct Entry {
        std::string key                = {};
        std::string content            = {};
        std::vector<std::string> words = {};
};

#endif

