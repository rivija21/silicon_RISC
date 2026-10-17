// The ten stops on the way from C source to silicon, and the built-in examples.
#pragma once

#include <string>
#include <vector>

namespace sp {

enum class Station { Source, Tokens, Tree, Symbols, IR, Asm, Machine, Link, Pipeline, Memory };

constexpr int stationCount = 10;
inline Station stationAt(int k) { return (Station)k; }
const char* stationTitle(Station s);
const char* stationLayer(Station s);
/// Hand-written assembly skips the compiler stations.
inline bool stationNeedsC(Station s) {
    return s == Station::Tokens || s == Station::Tree || s == Station::Symbols || s == Station::IR;
}

struct Example {
    std::string name;
    std::string goal;
    std::string source;
    bool isAssembly = false;
    Station station = Station::IR;
};

namespace Examples {
const std::vector<Example>& all();
}

}  // namespace sp
