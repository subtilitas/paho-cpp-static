// A dependency-free test harness.
//
// Deliberately not GoogleTest: the library is compiled -fno-exceptions and a
// framework that relies on throwing to report failures would defeat the point.
// Registration uses a fixed-size table so the harness itself never allocates,
// which matters for the zero-allocation test.

#ifndef MQTT_TEST_HARNESS_HPP
#define MQTT_TEST_HARNESS_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace th {

using TestFn = void (*)();

struct TestCase
{
    const char* name = nullptr;
    TestFn      fn   = nullptr;
};

constexpr size_t kMaxTests = 256;

TestCase* registry() noexcept;
size_t&   test_count() noexcept;
int&      failures() noexcept;
int&      checks() noexcept;
const char*& current_test() noexcept;

struct Registrar
{
    Registrar(const char* name, TestFn fn) noexcept;
};

inline void report_failure(const char* expr, const char* file, int line) noexcept
{
    ++failures();
    std::printf("  FAIL %s:%d\n    %s\n", file, line, expr);
}

inline bool check(bool cond, const char* expr, const char* file, int line) noexcept
{
    ++checks();
    if (!cond)
        report_failure(expr, file, line);
    return cond;
}

template <typename A, typename B>
inline bool check_eq(const A& a, const B& b, const char* expr, const char* file, int line) noexcept
{
    ++checks();
    if (!(a == b))
    {
        report_failure(expr, file, line);
        std::printf("    left  = %lld\n    right = %lld\n",
                    static_cast<long long>(a), static_cast<long long>(b));
        return false;
    }
    return true;
}

inline bool check_bytes(const uint8_t* got, size_t got_len,
                        const uint8_t* want, size_t want_len,
                        const char* expr, const char* file, int line) noexcept
{
    ++checks();
    if (got_len == want_len && std::memcmp(got, want, want_len) == 0)
        return true;

    report_failure(expr, file, line);
    std::printf("    got (%zu): ", got_len);
    for (size_t i = 0; i < got_len; ++i) std::printf("%02X ", got[i]);
    std::printf("\n    want(%zu): ", want_len);
    for (size_t i = 0; i < want_len; ++i) std::printf("%02X ", want[i]);
    std::printf("\n");
    return false;
}

int run_all() noexcept;

} // namespace th

#define TEST(name)                                            \
    static void name();                                       \
    static ::th::Registrar th_reg_##name(#name, name);        \
    static void name()

#define CHECK(cond)        ::th::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b)     ::th::check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_BYTES(g, gl, w, wl) \
    ::th::check_bytes((g), (gl), (w), (wl), "bytes match", __FILE__, __LINE__)

/// Abort the current test body when a precondition fails.
#define REQUIRE(cond)                                         \
    do { if (!CHECK(cond)) return; } while (false)

#endif // MQTT_TEST_HARNESS_HPP
