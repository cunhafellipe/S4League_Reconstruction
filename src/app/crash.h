// Crash reporter: whatever kills the process (an uncaught C++ exception, an
// access violation, a CRT invalid-parameter / abort) is written to
// s4_crash.log (what, where, memory in use) with a minidump s4_crash.dmp
// next to the exe, so a "random" crash can be traced afterwards.
#pragma once

namespace crash {
void Install();
// One line with the process' memory (working set / private bytes / address
// space used), also appended to s4_debug.log.
void LogMemory(const char* tag);
}
