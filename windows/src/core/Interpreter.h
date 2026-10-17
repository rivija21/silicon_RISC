// The reference interpreter: runs a checked C program straight from its syntax tree.
#pragma once

#include "Machine.h"

namespace sp {

/// Runs a checked C program directly from its syntax tree, without compiling it.
///
/// This is the *reference*: the compiled RISC-V program must print exactly what
/// the interpreter prints. Every variable lives in memory here (no registers),
/// and the arithmetic comes from `RV`, so it agrees with the hardware on corner cases.
namespace Interpreter {
ProgramOutput run(const Program& prog, const SemaResult& sema, int stepLimit = 20000000);
}

}  // namespace sp
