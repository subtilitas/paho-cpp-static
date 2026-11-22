#include "test_harness.hpp"

namespace th {

TestCase* registry() noexcept
{
    static TestCase cases[kMaxTests];
    return cases;
}

size_t& test_count() noexcept
{
    static size_t n = 0;
    return n;
}

int& failures() noexcept
{
    static int n = 0;
    return n;
}

int& checks() noexcept
{
    static int n = 0;
    return n;
}

const char*& current_test() noexcept
{
    static const char* name = "";
    return name;
}

Registrar::Registrar(const char* name, TestFn fn) noexcept
{
    if (test_count() < kMaxTests)
    {
        registry()[test_count()] = TestCase{name, fn};
        ++test_count();
    }
}

int run_all() noexcept
{
    int failed_tests = 0;

    for (size_t i = 0; i < test_count(); ++i)
    {
        const int before = failures();
        current_test()   = registry()[i].name;

        std::cout << "[ RUN  ] " << registry()[i].name << '\n';
        registry()[i].fn();

        if (failures() > before)
        {
            ++failed_tests;
            std::cout << "[ FAIL ] " << registry()[i].name << '\n';
        }
        else
        {
            std::cout << "[  OK  ] " << registry()[i].name << '\n';
        }
    }

    std::cout << '\n'
              << test_count() << " tests, " << checks() << " checks, " << failures()
              << " failed checks in " << failed_tests << " tests\n";

    return (failed_tests == 0) ? 0 : 1;
}

}   // namespace th

int main() { return th::run_all(); }
