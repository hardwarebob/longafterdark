// Control defaults of an AD4 module, from its type-1000 resources (ABI.md
// §2.10.2): the values AFTERDAR.SCR wrote to AD_MODULE32 +0x40 when the
// registry held nothing for the module. SET/ADCVSET values override them.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace adw::pe32 {

// The value slot `rec` (a type-1000 record) starts with: kind 1 (string
// slider) → the value of the last stop whose value ≤ the default, after the
// host's prepend rule; kind 2 → the default clamped to [min, max]; kind 3 →
// the item index clamped to the list; kind 5 → 0/1; kinds 0/4 → 0. A record
// too short for its kind yields 0.
int32_t control_default(std::string_view rec);

}  // namespace adw::pe32
