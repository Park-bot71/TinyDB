#pragma once

#include <stdexcept>
#include <string>

namespace tinydb {
enum class ErrorCode {
  Io, Corruption, InvalidArgument, NotFound, PageFull, Busy, Closed,
  LockTimeout, TransactionAborted, InvalidTransactionState, DuplicateKey, TransactionFailed
};

class Error : public std::runtime_error {
 public:
  Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
 private:
  ErrorCode code_;
};
}  // namespace tinydb
