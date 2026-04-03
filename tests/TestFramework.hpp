#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>

namespace test
{

    struct Result
    {
        std::string name;
        bool passed;
        std::string failMsg;
    };

    struct TestCase
    {
        std::string name;
        std::function<void()> fn;
    };

    class TestRegistry
    {
    public:
        static TestRegistry &instance()
        {
            static TestRegistry inst;
            return inst;
        }

        void add(std::string name, std::function<void()> fn)
        {
            cases_.push_back({std::move(name), std::move(fn)});
        }

        // Returns exit code: 0 = all passed, 1 = some failed.
        static int run()
        {
            auto &reg = instance();
            int passed = 0, failed = 0;

            std::cout << "\n=== Running " << reg.cases_.size() << " tests ===\n\n";

            for (auto &tc : reg.cases_)
            {
                try
                {
                    tc.fn();
                    std::cout << "  \033[32m[PASS]\033[0m " << tc.name << "\n";
                    ++passed;
                }
                catch (const std::exception &ex)
                {
                    std::cout << "  \033[31m[FAIL]\033[0m " << tc.name
                              << "\n         " << ex.what() << "\n";
                    ++failed;
                }
            }

            std::cout << "\n=== Results: " << passed << " passed, "
                      << failed << " failed ===\n";
            return failed > 0 ? 1 : 0;
        }

    private:
        std::vector<TestCase> cases_;
    };

    struct AutoRegister
    {
        AutoRegister(const char *name, std::function<void()> fn)
        {
            TestRegistry::instance().add(name, std::move(fn));
        }
    };

    // Assertion that throws on failure (caught by run()).
    inline void assertEq(const char *expr, long long actual, long long expected,
                         const char *file, int line)
    {
        if (actual != expected)
        {
            throw std::runtime_error(
                std::string(file) + ":" + std::to_string(line) + " ASSERT_EQ(" + expr + "): got " + std::to_string(actual) + ", expected " + std::to_string(expected));
        }
    }

    inline void assertTrue(const char *expr, bool cond, const char *file, int line)
    {
        if (!cond)
        {
            throw std::runtime_error(
                std::string(file) + ":" + std::to_string(line) + " ASSERT_TRUE(" + expr + ") failed");
        }
    }

    inline void assertFalse(const char *expr, bool cond, const char *file, int line)
    {
        assertTrue(expr, !cond, file, line);
    }

}

#define CONCAT_INNER(a, b) a##b
#define CONCAT(a, b) CONCAT_INNER(a, b)

#define TEST(name)                                                                       \
    static void CONCAT(_test_fn_, __LINE__)();                                           \
    static test::AutoRegister CONCAT(_test_reg_, __LINE__)(name,                         \
                                                           CONCAT(_test_fn_, __LINE__)); \
    static void CONCAT(_test_fn_, __LINE__)()

#define ASSERT_EQ(actual, expected)                         \
    test::assertEq(#actual, static_cast<long long>(actual), \
                   static_cast<long long>(expected), __FILE__, __LINE__)

#define ASSERT_TRUE(cond) \
    test::assertTrue(#cond, (cond), __FILE__, __LINE__)

#define ASSERT_FALSE(cond) \
    test::assertFalse(#cond, (cond), __FILE__, __LINE__)

#define ASSERT_NE(actual, expected)          \
    test::assertEq(#actual " != " #expected, \
                   (actual) == (expected) ? 1 : 0, 0, __FILE__, __LINE__)