// Minimal test harness: CHECK records a failure and keeps going, so one run
// reports every broken expectation; main() returns nonzero if any failed.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace adw_test {

inline int& failures() {
  static int n = 0;
  return n;
}

inline std::vector<std::pair<const char*, std::function<void()>>>& registry() {
  static std::vector<std::pair<const char*, std::function<void()>>> r;
  return r;
}

struct Register {
  Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};

inline int run_all(const char* filter = nullptr) {
  int ran = 0;
  for (auto& [name, fn] : registry()) {
    if (filter && std::string(name).find(filter) == std::string::npos) continue;
    int before = failures();
    fn();
    ran++;
    fprintf(stderr, "%s %s\n", failures() == before ? "PASS" : "FAIL", name);
  }
  fprintf(stderr, "%d test(s), %d failed check(s)\n", ran, failures());
  // A filter that matches nothing must not read as a pass.
  if (ran == 0) {
    fprintf(stderr, "no test matched%s%s\n", filter ? " filter " : "", filter ? filter : "");
    return 1;
  }
  return failures() ? 1 : 0;
}

}  // namespace adw_test

#define TEST(name)                                                      \
  static void test_##name();                                            \
  static adw_test::Register reg_##name(#name, test_##name);             \
  static void test_##name()

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      adw_test::failures()++;                                                          \
      fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                                          \
  do {                                                                                          \
    auto _va = (a);                                                                             \
    auto _vb = (b);                                                                             \
    if (!(_va == _vb)) {                                                                        \
      adw_test::failures()++;                                                                   \
      fprintf(stderr, "  %s:%d: CHECK_EQ failed: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #a, \
              #b, adw_test::show(_va).c_str(), adw_test::show(_vb).c_str());                   \
    }                                                                                           \
  } while (0)

namespace adw_test {
template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_convertible_v<T, std::string>) {
    return "\"" + std::string(v) + "\"";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(v);
  } else {
    return "?";
  }
}
}  // namespace adw_test
