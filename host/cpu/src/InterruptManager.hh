// Vendored from resource_dasm: https://github.com/swannman/resource_dasm
// (branch afterdark-perf, commit 02d8ea9a58eaf559a9194601b33d98723c9d4f60,
// file src/Emulators/InterruptManager.hh), itself a fork of fuzziqersoftware/resource_dasm.
// MIT, (c) Martin Michelsen; modified for Long After Dark.
// See ../LICENSE.resource_dasm for the license text.

#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>

namespace adw::cpu {

class InterruptManager {
public:
  InterruptManager();
  ~InterruptManager() = default;

  struct PendingCall {
    std::shared_ptr<PendingCall> next;
    uint64_t at_cycle_count;
    bool canceled;
    bool completed;
    std::function<void()> fn;

    inline void cancel() {
      this->canceled = true;
    }
  };

  std::shared_ptr<PendingCall> add(uint64_t cycle_count, std::function<bool()> fn);

  void on_cycle_start();

  uint64_t cycles() const;

protected:
  uint64_t cycle_count;
  std::shared_ptr<PendingCall> head;
};

} // namespace adw::cpu
