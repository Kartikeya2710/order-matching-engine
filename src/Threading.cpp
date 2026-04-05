#include "Threading.hpp"

namespace engine
{
    void pinThreadToCore(std::thread &t, int core)
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
            std::cerr << "Warning: Could not pin thread to core " << core
                      << " (rc=" << rc << "), continuing unpinned\n";
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
            if (kr == 46)
            {
                // Apple Silicon does not support strict pinning.
                std::cerr << "Warning: Could not pin thread to core " << core << " (macOS kr=46)" << std::endl;
            }
            else
            {
                throw std::runtime_error(
                    "Failed to set affinity tag " + std::to_string(core) +
                    " (kr=" + std::to_string(kr) + ")");
            }
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

#else
        (void)t;
        (void)core;
#endif
    }

    void setThreadName(const std::string &name)
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