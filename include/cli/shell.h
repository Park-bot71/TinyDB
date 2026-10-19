#pragma once

#include <csignal>
#include <filesystem>
#include <iosfwd>

namespace tinydb {
// One fixed table, accounts. EOF/EXIT explicitly roll back unfinished work and
// close the database. Returns 1 if any command failed, 128+signal on interruption.
int run_shell(const std::filesystem::path& directory, std::istream& input,
              std::ostream& output, std::ostream& errors, bool prompts = false,
              const volatile std::sig_atomic_t* interrupted = nullptr);
}
