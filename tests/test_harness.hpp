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
#include <iomanip>
#include <iostream>

#include <etl/algorithm.h>
#include <etl/string_view.h>

namespace th {

using TestFn = void (*)();

struct TestCase
{
    const char* name = nullptr;
    TestFn      fn   = nullptr;
};

constexpr size_t kMaxTests = 256;

TestCase*    registry() noexcept;
size_t&      test_count() noexcept;
int&         failures() noexcept;
int&         checks() noexcept;
const char*& current_test() noexcept;

struct Registrar
{
    Registrar(const char* name, TestFn fn) noexcept;
};

/// Two null-terminated strings hold the same characters. Null equals only null.
inline bool same_text(const char* a, const char* b) noexcept
{
    if ((a == nullptr) || (b == nullptr))
        return a == b;
    return etl::string_view(a) == etl::string_view(b);
}

inline void report_failure(const char* expr, const char* file, int line) noexcept
{
    ++failures();
    std::cout << "  FAIL " << file << ':' << line << "\n    " << expr << '\n';
}

/// A view written to a stream by length, not up to a terminator.
struct View
{
    etl::string_view text;
};

inline std::ostream& operator<<(std::ostream& os, View v)
{
    return os.write(v.text.data(), static_cast<std::streamsize>(v.text.size()));
}

/// Write `len` bytes as space-separated two-digit upper-case hex, and leave
/// the stream's flags and fill character as they were.
inline void print_hex(std::ostream& os, const uint8_t* bytes, size_t len) noexcept
{
    const std::ios_base::fmtflags flags = os.flags();
    const char                    fill  = os.fill();

    os << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        os << std::setw(2) << static_cast<unsigned>(bytes[i]) << ' ';

    os.flags(flags);
    os.fill(fill);
}

inline bool check(bool cond, const char* expr, const char* file, int line) noexcept
{
    ++checks();
    if (!cond)
        report_failure(expr, file, line);
    return cond;
}

template <typename A, typename B>
inline bool check_eq(const A& a, const B& b, const char* expr, const char* file,
                     int line) noexcept
{
    ++checks();
    if (!(a == b))
    {
        report_failure(expr, file, line);
        std::cout << "    left  = " << static_cast<long long>(a)
                  << "\n    right = " << static_cast<long long>(b) << '\n';
        return false;
    }
    return true;
}

inline bool check_bytes(const uint8_t* got, size_t got_len, const uint8_t* want,
                        size_t want_len, const char* expr, const char* file, int line) noexcept
{
    ++checks();
    if (got_len == want_len && etl::equal(got, got + got_len, want))
        return true;

    report_failure(expr, file, line);
    std::cout << "    got (" << got_len << "): ";
    print_hex(std::cout, got, got_len);
    std::cout << "\n    want(" << want_len << "): ";
    print_hex(std::cout, want, want_len);
    std::cout << '\n';
    return false;
}

int run_all() noexcept;

}   // namespace th

#define TEST(name)                                     \
    static void            name();                     \
    static ::th::Registrar th_reg_##name(#name, name); \
    static void            name()

#define CHECK(cond) ::th::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::th::check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_BYTES(g, gl, w, wl) \
    ::th::check_bytes((g), (gl), (w), (wl), "bytes match", __FILE__, __LINE__)

/// Abort the current test body when a precondition fails.
#define REQUIRE(cond)     \
    do                    \
    {                     \
        if (!CHECK(cond)) \
            return;       \
    } while (false)

#endif   // MQTT_TEST_HARNESS_HPP
