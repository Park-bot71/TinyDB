#include "cli/shell.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
volatile std::sig_atomic_t wakeup_fd = -1;
extern "C" void stop(int signal) {
  const int saved = errno;
  interrupted = signal;
  if (wakeup_fd >= 0) {
    const char byte = 1;
    // Nonblocking and async-signal-safe. A full pipe already wakes the reader.
    (void)::write(wakeup_fd, &byte, 1);
  }
  errno = saved;
}
struct WakeupPipe {
  std::array<int, 2> descriptors{};
  WakeupPipe() {
    if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) < 0)
      throw std::runtime_error("unable to create shutdown wakeup pipe");
    wakeup_fd = descriptors[1];
  }
  ~WakeupPipe() { wakeup_fd = -1; for (int fd : descriptors) ::close(fd); }
};
// Polling a self-pipe avoids a lost signal between checking a flag and entering
// a blocking input read. No stream/database API runs inside the signal handler.
class InputBuffer : public std::streambuf {
 public:
  explicit InputBuffer(int wakeup) : wakeup_(wakeup) {}
 protected:
  int_type underflow() override {
    for (;;) {
      if (interrupted) return traits_type::eof();
      std::array<pollfd, 2> descriptors{{{STDIN_FILENO, POLLIN, 0}, {wakeup_, POLLIN, 0}}};
      const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
      if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("input poll failed"); }
      if (interrupted || descriptors[1].revents) return traits_type::eof();
      const auto size = ::read(STDIN_FILENO, bytes_.data(), bytes_.size());
      if (size < 0) { if (errno == EINTR || errno == EAGAIN) continue; throw std::runtime_error("input read failed"); }
      if (size == 0) return traits_type::eof();
      setg(bytes_.data(), bytes_.data(), bytes_.data() + size);
      return traits_type::to_int_type(*gptr());
    }
  }
 private:
  int wakeup_;
  std::array<char, 4096> bytes_{};
};
}
int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: tinydb DATABASE_DIRECTORY [--batch]\nType HELP for commands.\n";
    return 0;
  }
  if (argc < 2 || argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--batch")) {
    std::cerr << "Usage: tinydb DATABASE_DIRECTORY [--batch]\n";
    return 2;
  }
  try {
    WakeupPipe wakeup;
    struct sigaction action {};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0)
      throw std::runtime_error("unable to install shutdown signal handlers");
    InputBuffer buffer(wakeup.descriptors[0]);
    std::istream input(&buffer);
    return tinydb::run_shell(argv[1], input, std::cout, std::cerr,
                            argc == 2 && ::isatty(STDIN_FILENO), &interrupted);
  } catch (const std::exception& error) {
    std::cerr << "FATAL: " << error.what() << '\n'; return 1;
  }
}
