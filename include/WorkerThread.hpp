#include "InstrumentContext.hpp"

#include <stdexcept>
#include <string>
#include <thread>
#include <emmintrin.h>

namespace engine
{

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
#elif defined(PLATFORM_WINDOWS)
#include <windows.h>
#elif defined(PLATFORM_MACOS)
#include <pthread.h>
#include <mach/mach.h>
#endif

    class WorkerThread
    {
    private:
        int workerId_;
        int pinnedCore_;
        std::vector<InstrumentContext *> instruments_;
        std::thread thread_;
        alignas(64) std::atomic<bool> running_{false};

        void run() noexcept;

    public:
        explicit WorkerThread(int workerId, int pinnedCore) : workerId_(workerId), pinnedCore_(pinnedCore)
        {
        }

        WorkerThread(const WorkerThread &) = delete;
        WorkerThread &operator=(const WorkerThread &) = delete;
        WorkerThread(WorkerThread &&) = default;
        WorkerThread &operator=(WorkerThread &&) = default;

        ~WorkerThread()
        {
            if (thread_.joinable())
                stop();
        }

        void assignInstrument(InstrumentContext *ctx);

        void start();

        void stop();

        int workerId() const noexcept;

        int pinnedCore() const noexcept;

        bool isRunning() const noexcept;
    };
}