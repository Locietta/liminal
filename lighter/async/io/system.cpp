#include "system.h"

#if defined(__linux__)
#include <charconv>
#include <unistd.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

#include <lighter/types.hpp>
#include <lighter/async/detail/libuv_helper.h>
#if defined(__linux__)
#include <lighter/async/io/fs.h>
#endif

namespace lighter::sys {

i32 pid() noexcept { return static_cast<i32>(uv::os_getpid()); }

MemoryInfo memory() {
    MemoryInfo info;
    info.total = uv::get_total_memory();
    info.free = uv::get_free_memory();
    info.available = uv::get_available_memory();
    info.constrained = uv::get_constrained_memory();
    return info;
}

Result<usize> resident_memory() {
    usize rss = 0;
    if (auto err = uv::resident_set_memory(rss)) {
        return outcome_error(err);
    }
    return rss;
}

Result<std::vector<CpuCore>> cpu_cores() {
    uv_cpu_info_t *infos = nullptr;
    i32 count = 0;
    if (auto err = uv::cpu_info(infos, count)) {
        return outcome_error(err);
    }

    struct Guard {
        uv_cpu_info_t *p;
        i32 n;

        ~Guard() { uv::free_cpu_info(p, n); }
    } cleanup{infos, count};

    std::vector<CpuCore> result;
    result.reserve(static_cast<usize>(count));
    for (i32 i = 0; i < count; ++i) {
        auto &src = infos[i];
        CpuCore core;
        core.model = src.model ? src.model : "";
        core.speed_mhz = src.speed;
        core.times.user = std::chrono::milliseconds(src.cpu_times.user);
        core.times.nice = std::chrono::milliseconds(src.cpu_times.nice);
        core.times.sys = std::chrono::milliseconds(src.cpu_times.sys);
        core.times.idle = std::chrono::milliseconds(src.cpu_times.idle);
        core.times.irq = std::chrono::milliseconds(src.cpu_times.irq);
        result.push_back(std::move(core));
    }
    return result;
}

u32 parallelism() { return uv::available_parallelism(); }

Result<UnameInfo> uname() {
    uv_utsname_t buf{};
    if (auto err = uv::os_uname(buf)) {
        return outcome_error(err);
    }
    return UnameInfo{buf.sysname, buf.release, buf.version, buf.machine};
}

/// Helper: call a libuv string-returning Function with stack buffer,
/// retry with heap allocation on UV_ENOBUFS.
template <typename Fn> static Result<std::string> read_uv_string(Fn &&fn, usize initial_size) {
    std::string buf(initial_size, '\0');
    usize size = buf.size();
    auto err = fn(buf.data(), size);
    if (err == Error::k_no_buffer_space_available) {
        buf.resize(size);
        size = buf.size();
        err = fn(buf.data(), size);
    }
    if (err) {
        return outcome_error(err);
    }
    buf.resize(size);
    return buf;
}

Result<std::string> hostname() {
    return read_uv_string([](char *buf, usize &size) { return uv::os_gethostname(buf, size); }, 256);
}

Result<std::chrono::duration<f64>> uptime() {
    f64 value = 0;
    if (auto err = uv::uptime(value)) {
        return outcome_error(err);
    }
    return std::chrono::duration<f64>(value);
}

Result<std::string> home_directory() {
    return read_uv_string([](char *buf, usize &size) { return uv::os_homedir(buf, size); }, 1024);
}

Result<std::string> temp_directory() {
    return read_uv_string([](char *buf, usize &size) { return uv::os_tmpdir(buf, size); }, 1024);
}

Result<i32> priority(i32 pid) {
    i32 value = 0;
    if (auto err = uv::os_getpriority(static_cast<uv_pid_t>(pid), value)) {
        return outcome_error(err);
    }
    return value;
}

Error set_priority(i32 value, i32 pid) { return uv::os_setpriority(static_cast<uv_pid_t>(pid), value); }

Result<ProcessStat> process(i32 pid) {
    bool is_self = (pid == 0);
    if (is_self) {
        pid = sys::pid();
    }

    ProcessStat stat{};
    stat.pid = pid;

#if defined(__linux__)
    auto proc_path = "/proc/" + std::to_string(pid);

    auto skip_field = [](const char *&cur, const char *end) {
        while (cur < end && *cur == ' ') {
            ++cur;
        }
        while (cur < end && *cur != ' ') {
            ++cur;
        }
    };

    auto next_u64 = [](const char *&cur, const char *end) -> u64 {
        while (cur < end && *cur == ' ') {
            ++cur;
        }
        u64 val = 0;
        auto [ptr, ec] = std::from_chars(cur, end, val);
        cur = ptr;
        return (ec == std::errc{}) ? val : 0;
    };

    // Memory from /proc/<pid>/statm: "vsize_pages rss_pages ..." (all in pages).
    {
        auto content = fs::sync::read_to_string(proc_path + "/statm");
        if (!content) {
            return outcome_error(Error::k_no_such_process);
        }

        const char *cur = content->data();
        const char *end = cur + content->size();

        u64 vsize_pages = next_u64(cur, end);
        u64 rss_pages = next_u64(cur, end);

        auto page_size = static_cast<usize>(sysconf(_SC_PAGESIZE));
        stat.vsize = vsize_pages * page_size;
        stat.rss = rss_pages * page_size;
    }

    // CPU times and faults from /proc/<pid>/stat.
    // Format: "pid (comm) state ppid(4) ... minflt(10) cminflt(11)
    //          majflt(12) cmajflt(13) utime(14) stime(15) ..."
    {
        auto content = fs::sync::read_to_string(proc_path + "/stat");
        if (content) {
            auto rp = content->rfind(')');
            if (rp != std::string::npos && rp + 1 < content->size()) {
                const char *cur = content->data() + rp + 1;
                const char *end = content->data() + content->size();

                skip_field(cur, end); // state (3)
                for (i32 i = 0; i < 6 && cur < end; ++i) {
                    next_u64(cur, end); // ppid(4)..flags(9)
                }

                stat.minor_faults = next_u64(cur, end); // minflt (10)
                next_u64(cur, end);                     // cminflt (11)
                stat.major_faults = next_u64(cur, end); // majflt (12)
                next_u64(cur, end);                     // cmajflt (13)

                u64 utime_ticks = next_u64(cur, end); // (14)
                u64 stime_ticks = next_u64(cur, end); // (15)

                isize hz = sysconf(_SC_CLK_TCK);
                if (hz <= 0) {
                    hz = 100;
                }
                auto uhz = static_cast<u64>(hz);
                stat.user_time = std::chrono::microseconds(utime_ticks * 1'000'000ULL / uhz);
                stat.system_time = std::chrono::microseconds(stime_ticks * 1'000'000ULL / uhz);
            }
        }
    }

    if (is_self) {
        uv_rusage_t ru{};
        if (!uv::getrusage(ru)) {
            stat.max_rss = static_cast<usize>(ru.ru_maxrss) * 1024; // KB -> bytes
            stat.voluntary_context_switches = ru.ru_nvcsw;
            stat.involuntary_context_switches = ru.ru_nivcsw;
        }
    }

#elif defined(__APPLE__)
    struct proc_taskinfo pti{};
    i32 ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti));
    if (ret <= 0) {
        return outcome_error(Error::k_no_such_process);
    }

    stat.rss = pti.pti_resident_size;
    stat.vsize = pti.pti_virtual_size;
    stat.user_time = std::chrono::microseconds(pti.pti_total_user / 1000);
    stat.system_time = std::chrono::microseconds(pti.pti_total_system / 1000);
    stat.major_faults = pti.pti_pageins;
    stat.minor_faults = pti.pti_faults - pti.pti_pageins;
    stat.voluntary_context_switches = pti.pti_csw;

    if (is_self) {
        uv_rusage_t ru{};
        if (!uv::getrusage(ru)) {
            stat.max_rss = ru.ru_maxrss; // bytes on macOS
            stat.involuntary_context_switches = ru.ru_nivcsw;
        }
    }

#elif defined(_WIN32)
    HANDLE h;
    if (is_self) {
        h = GetCurrentProcess();
    } else {
        h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
        if (!h) {
            return outcome_error(Error::k_no_such_process);
        }
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
        stat.rss = pmc.WorkingSetSize;
        stat.vsize = pmc.PagefileUsage;
        stat.max_rss = pmc.PeakWorkingSetSize;
    }

    FILETIME creation, exit_time, kernel_time, user_time;
    if (GetProcessTimes(h, &creation, &exit_time, &kernel_time, &user_time)) {
        auto to_us = [](FILETIME ft) -> u64 {
            u64 t = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            return t / 10; // 100-ns intervals -> microseconds
        };
        stat.user_time = std::chrono::microseconds(to_us(user_time));
        stat.system_time = std::chrono::microseconds(to_us(kernel_time));
    }

    if (!is_self) {
        CloseHandle(h);
    }
#else
    return outcome_error(Error::k_function_not_implemented);
#endif

    return stat;
}

} // namespace lighter::sys
