// CancelToken — stops an import, a download or a cover fetch from another
// thread, at once (internal to the importer and its windows).
//
// A progress callback that returns false is noticed only at the next report,
// and a download blocked in WinHTTP (connecting, waiting for a response or
// for the next bytes) reports nothing for up to its timeout, 30-60 s. The
// token closes what such a call is waiting on: whoever blocks registers an
// abort for as long as it might block (Scope), and cancel() runs every
// registered abort; the blocked call then fails and its caller sees
// cancelled() and reports Status::cancelled.
//
// Aborts run under the token's lock, on the cancelling thread, and must be
// quick and must not touch the token. A Scope's destructor waits for an abort
// of its own that is running, so what the abort closes can be freed right
// after the Scope ends.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace adw::import {

class CancelToken {
 public:
  CancelToken() = default;
  CancelToken(const CancelToken&) = delete;
  CancelToken& operator=(const CancelToken&) = delete;

  // Sets the flag and runs every registered abort once. Any thread; repeated
  // calls do nothing more.
  void cancel() const {
    std::lock_guard<std::mutex> lock(m_);
    if (cancelled_.exchange(true)) return;
    for (auto& [id, abort] : aborts_) abort();
  }
  bool cancelled() const { return cancelled_.load(); }
  // Clears the flag for the next operation (the caller knows none is running).
  void reset() const {
    std::lock_guard<std::mutex> lock(m_);
    cancelled_ = false;
  }

  // While it lives, `abort` runs when the token is cancelled (at once when it
  // already is). A null token makes it a no-op.
  class Scope {
   public:
    Scope(const CancelToken* t, std::function<void()> abort) : t_(t) {
      if (!t_) return;
      std::lock_guard<std::mutex> lock(t_->m_);
      if (t_->cancelled_) {
        abort();
        t_ = nullptr;
        return;
      }
      id_ = ++t_->next_;
      t_->aborts_.emplace(id_, std::move(abort));
    }
    ~Scope() {
      if (!t_) return;
      std::lock_guard<std::mutex> lock(t_->m_);
      t_->aborts_.erase(id_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    const CancelToken* t_;
    uint64_t id_ = 0;
  };

 private:
  mutable std::mutex m_;
  mutable std::atomic<bool> cancelled_{false};
  mutable std::map<uint64_t, std::function<void()>> aborts_;
  mutable uint64_t next_ = 0;
};

inline bool is_cancelled(const CancelToken* t) { return t && t->cancelled(); }

}  // namespace adw::import
