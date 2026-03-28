#pragma once
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32) || defined(_WIN64)
#define PLATFORM_WINDOWS
#elif defined(__APPLE__) && defined(__MACH__)
#define PLATFORM_MACOS
#elif defined(__linux__)
#define PLATFORM_LINUX
#else
#define PLATFORM_UNKNOWN
#endif

#if defined(PLATFORM_LINUX)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(PLATFORM_MACOS)
#include <pthread.h>
#include <mach/mach.h>
#elif defined(PLATFORM_WINDOWS)
#include <windows.h>
#endif

namespace engine
{

    void pinThreadToCore(std::thread &t, int core);

    void setThreadName(const std::string &name);

}