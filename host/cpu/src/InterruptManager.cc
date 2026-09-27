// Vendored from resource_dasm: https://github.com/swannman/resource_dasm
// (branch afterdark-perf, commit 02d8ea9a58eaf559a9194601b33d98723c9d4f60,
// file src/Emulators/InterruptManager.cc), itself a fork of fuzziqersoftware/resource_dasm.
// MIT, (c) Martin Michelsen; modified for Long After Dark.
// See ../LICENSE.resource_dasm for the license text.

#include "InterruptManager.hh"

namespace adw::cpu {

InterruptManager::InterruptManager() : cycle_count(0) {}

std::shared_ptr<InterruptManager::PendingCall> InterruptManager::add(uint64_t after_cycles, std::function<bool()> fn) {
  auto ret = std::make_shared<PendingCall>();
  ret->at_cycle_count = this->cycle_count + after_cycles;
  ret->canceled = false;
  ret->completed = false;
  ret->fn = std::move(fn);

  if (!this->head.get()) {
    this->head = ret;
  } else {
    if (ret->at_cycle_count < this->head->at_cycle_count) {
      ret->next = this->head;
      this->head = ret;
    } else {
      std::shared_ptr<PendingCall> prev = this->head;
      std::shared_ptr<PendingCall> next = this->head->next;
      while (next.get() && next->at_cycle_count < ret->at_cycle_count) {
        prev = next;
        next = next->next;
      }
      ret->next = next;
      prev->next = ret;
    }
  }

  return ret;
}

void InterruptManager::on_cycle_start() {
  this->cycle_count++;

  while (this->head.get() && (this->head->at_cycle_count <= this->cycle_count)) {
    std::shared_ptr<PendingCall> c = this->head;
    this->head = c->next;
    if (!c->canceled) {
      c->fn();
    }
    c->completed = true;
  }
}

uint64_t InterruptManager::cycles() const {
  return this->cycle_count;
}

} // namespace adw::cpu
