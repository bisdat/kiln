// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string_view>
#include <mutex>
namespace kvx {
// Writers serialize file replacement and receipt bookkeeping; snapshot readers never take this mutex.
std::mutex& pending_io_mutex();
// Remove only this transaction's bytes, without following a redirected parent/file. I/O: call outside ledger locks.
bool discard_pending(const std::filesystem::path& file, std::string_view expected) noexcept;
}
