#include "WorkerThread.hpp"

namespace engine
{

    void WorkerThread::assignInstrument(InstrumentContext *ctx)
    {
        instruments_.push_back(ctx);
        ctx->assignedWorker.store(workerId_, std::memory_order_relaxed);
    }

    void WorkerThread::start()
    {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread(&WorkerThread::run, this);
        pinThreadToCore(thread_, pinnedCore_);
    }

    void WorkerThread::stop()
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

    int WorkerThread::workerId() const noexcept { return workerId_; }

    int WorkerThread::pinnedCore() const noexcept { return pinnedCore_; }

    bool WorkerThread::isRunning() const noexcept
    {
        return running_.load(std::memory_order_relaxed);
    }

    void WorkerThread::run() noexcept
    {
        setThreadName("matcher-" + std::to_string(workerId_));

        uint64_t localSeq = 0;
        core::Command cmd;

        while (__builtin_expect(running_.load(std::memory_order_relaxed), 1))
        {
            for (auto *ctx : instruments_)
            {
                while (ctx->inputQueue.dequeue(cmd))
                {
                    std::visit(
                        [&](auto &book)
                        {
                            switch (cmd.type)
                            {
                            case core::CommandType::AddOrder:
                                return book.addOrder(cmd);
                            case core::CommandType::CancelOrder:
                                return book.cancelOrder(cmd);
                            case core::CommandType::ModifyOrder:
                                return book.modifyOrder(cmd);
                            default:
                                return;
                            }
                        },
                        ctx->book);
                }
            }
            _mm_pause();
        }
    }

    static void pinThreadToCore(std::thread &t, int core)
    {
#if defined(PLATFORM_LINUX)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);

        int rc = pthread_setaffinity_np(
            t.native_handle(),
            sizeof(cpu_set_t),
            &cpuset);

        if (rc != 0)
        {
            throw std::runtime_error(
                "Failed to pin to core " + std::to_string(core) + " (rc=" + std::to_string(rc) + ")";);
        }
#elif defined(PLATFORM_WINDOWS)
        if (core >= 64)
        {
            throw std::runtime_error(
                "Core " + std::to_string(core) + " exceeds Windows 64-core limit");
        }

        DWORD_PTR mask = DWORD_PTR(1) << core;
        DWORD_PTR result = SetThreadAffinityMask(t.native_handle(), mask);

        if (result == 0)
        {
            throw std::runtime_error(
                "SetThreadAffinityMask failed for core " + std::to_string(core));
        }
#elif defined(PLATFORM_MACOS)
        thread_affinity_policy_data_t policy;
        policy.affinity_tag = core;

        kern_return_t kr = thread_policy_set(
            pthread_mach_thread_np(t.native_handle()),
            THREAD_AFFINITY_POLICY,
            (thread_policy_t)&policy,
            THREAD_AFFINITY_POLICY_COUNT);

        if (kr != KERN_SUCCESS)
        {
            throw std::runtime_error(
                "Failed to set affinity tag " + std::to_string(core) +
                " (kr=" + std::to_string(kr) + ")");
        }
#endif
    }

    inline void setThreadName(const std::string &name)
    {
#if defined(PLATFORM_LINUX)
        pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#elif defined(PLATFORM_MACOS)
        pthread_setname_np(name.substr(0, 63).c_str());
#elif defined(PLATFORM_WINDOWS)
        std::wstring wname(name.begin(), name.end());
        SetThreadDescription(GetCurrentThread(), wname.c_str());
#endif
    }
}