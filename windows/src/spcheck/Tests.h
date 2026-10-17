#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace spcheck {

const std::vector<std::pair<std::string, std::string>>& handTests();

/// Generates random, well-defined C programs (no undefined behaviour) that
/// print a trace of their values. Used for differential testing.
std::string generateProgram(uint64_t seed);

}  // namespace spcheck
