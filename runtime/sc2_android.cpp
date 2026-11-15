// SPDX-License-Identifier: GPL-3.0-or-later
// Android: the game is a library (libsoulcalibur2.so) that its activity (android/, GameActivity.java) loads in a
// process of its own and calls the main of, with the launcher's arguments. What a program started elsewhere gets from
// the launcher, the activity gives as variables: RECOMP_FOLDER, the player's folder for the game (the current folder
// elsewhere), and RECOMP_LOG, the file its output goes to (logs/game.log). Without a log the output goes to the
// system's log (adb logcat -s soulcalibur2). Taken from cps2recomp's port/android.cpp.
#ifdef __ANDROID__
#include "sc2_android.h"
#include "sc2_android_telemetry.h"
#include "sc2_cpu_affinity.h"
#include "sc2_perf_metrics.h"
#include "sc2_settings.h"
#include "sc2_touch.h"
#include "d3d_vblank.h"
#include "host_call.h"
#include "audio_output.h"

#include <SDL3/SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <signal.h>
#include <unwind.h>
#include <sched.h>

#include <algorithm>
#include <vector>
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

// The standard output and error, a line at a time to the system's log
void toSystemLog()
{
    static int pipes[2];
    if (pipe(pipes) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(pipes[1], 1);
    dup2(pipes[1], 2);
    std::thread([] {
        std::string line;
        char buffer[512];
        ssize_t got;
        while ((got = read(pipes[0], buffer, sizeof buffer)) > 0) {
            for (ssize_t i = 0; i < got; i++) {
                if (buffer[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, "soulcalibur2", line.c_str());
                    line.clear();
                } else {
                    line += buffer[i];
                }
            }
        }
    }).detach();
}

} // namespace

// SDL's failed checks: written to the log (SDL has already said which) and passed over, not shown in a dialog, which
// would be made through Java from a guest fiber's stack and stop the game there (host_call.h)
static SDL_AssertState SDLCALL passAssertion(const SDL_AssertData *data, void *)
{
    std::fprintf(stderr, "SDL check failed (passed over): %s at %s:%d\n", data->condition, data->filename,
        data->linenum);
    return SDL_ASSERTION_IGNORE;
}

// A crash's signal and where it was, written to the game's log (the system's own report goes only to adb logcat),
// then the system's handling as before
struct CrashWalk { int count; };
static _Unwind_Reason_Code crashFrame(struct _Unwind_Context *context, void *argument)
{
    CrashWalk *walk = static_cast<CrashWalk *>(argument);
    if (walk->count++ >= 40) return _URC_END_OF_STACK;
    const uintptr_t pc = _Unwind_GetIP(context);
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_fname) {
        const char *file = std::strrchr(info.dli_fname, '/');
        dprintf(2, "  #%02d %s+0x%zx %s\n", walk->count - 1, file ? file + 1 : info.dli_fname,
            static_cast<size_t>(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)), info.dli_sname ? info.dli_sname : "");
    } else {
        dprintf(2, "  #%02d 0x%zx\n", walk->count - 1, static_cast<size_t>(pc));
    }
    return _URC_NO_REASON;
}

static void crashed(int signal, siginfo_t *info, void *)
{
    dprintf(2, "soulcalibur2: crashed, signal %d at address %p\n", signal, info ? info->si_addr : nullptr);
    CrashWalk walk{0};
    _Unwind_Backtrace(crashFrame, &walk);
    ::signal(signal, SIG_DFL);
    raise(signal);
}

static long readSysfsLong(const char *path)
{
    long value = 0;
    if (std::FILE *file = std::fopen(path, "r")) {
        if (std::fscanf(file, "%ld", &value) != 1) value = 0;
        std::fclose(file);
    }
    return value;
}

static std::vector<sc2::ClusterFreqSample> readClusterFreqs()
{
    std::vector<sc2::ClusterFreqSample> samples;
    const int count = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    if (count <= 0) return samples;
    long previous_max = -1;
    long previous_cur = -1;
    for (int cpu = 0; cpu < count; ++cpu) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
        const long max_khz = readSysfsLong(path);
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        const long cur_khz = readSysfsLong(path);
        if (max_khz > 0) {
            if (max_khz == previous_max) continue;
            previous_max = max_khz;
        } else if (cur_khz <= 0 || cur_khz == previous_cur) {
            continue;
        }
        previous_cur = cur_khz;
        samples.push_back({cpu, max_khz, cur_khz});
    }
    return samples;
}

// The cores' speed caps now (a warm phone lowers them), for the speed line
static std::string coreCaps()
{
    return sc2::FormatClusterCaps(readClusterFreqs());
}

static std::string readThermalStatus()
{
    std::vector<sc2::ThermalZoneSample> zones;
    for (int i = 0; i < 64; ++i) {
        char temp_path[96];
        std::snprintf(temp_path, sizeof temp_path, "/sys/class/thermal/thermal_zone%d/temp", i);
        std::FILE *temp_file = std::fopen(temp_path, "r");
        if (temp_file == nullptr) continue;
        long raw = 0;
        const int scanned = std::fscanf(temp_file, "%ld", &raw);
        std::fclose(temp_file);
        if (scanned != 1) continue;
        double celsius = 0.0;
        if (!sc2::ParseThermalTempCelsius(raw, celsius)) continue;

        sc2::ThermalZoneSample sample;
        sample.temp_c = celsius;
        sample.valid = true;

        char type_path[96];
        std::snprintf(type_path, sizeof type_path, "/sys/class/thermal/thermal_zone%d/type", i);
        if (std::FILE *type_file = std::fopen(type_path, "r")) {
            char buf[64]{};
            if (std::fgets(buf, sizeof buf, type_file) != nullptr) {
                const size_t len = std::strlen(buf);
                size_t trimmed = len;
                while (trimmed > 0 && (buf[trimmed - 1] == '\n' || buf[trimmed - 1] == '\r' ||
                                       buf[trimmed - 1] == ' ' || buf[trimmed - 1] == '\t')) {
                    --trimmed;
                }
                sample.type.assign(buf, trimmed);
            }
            std::fclose(type_file);
        }
        zones.push_back(sample);
    }
    sc2::ThermalZoneSample best;
    if (!sc2::SelectBestThermalZone(zones, best)) return "n/a";
    char out[96];
    if (best.type.empty()) {
        std::snprintf(out, sizeof out, "%.1fC", best.temp_c);
    } else {
        std::snprintf(out, sizeof out, "%.1fC(%s)", best.temp_c, best.type.c_str());
    }
    return out;
}

// Select active CPU cores dynamically using effective speeds (min(cpuinfo_max_freq, scaling_max_freq)).
// Keeps >= min(4, available) fastest online cores so fibers, audio, video decode, and Vulkan do not
// starve on 2 prime cores or stay trapped when prime cores throttle below the mid cluster.
// SC2_FAST_CORES=1 restores the legacy static top > slowest 2-core filter for A/B checks.
static void applyCoreAffinity(bool log_unchanged)
{
    const int count = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    if (count <= 0) return;

    cpu_set_t desired;
    CPU_ZERO(&desired);
    int chosen_count = 0;

    const char *legacy = std::getenv("SC2_FAST_CORES");
    if (legacy != nullptr && *legacy == '1') {
        std::vector<long> top(static_cast<size_t>(count), 0);
        long slowest = 0;
        for (int cpu = 0; cpu < count; ++cpu) {
            char path[96];
            std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            if (std::FILE *file = std::fopen(path, "r")) {
                if (std::fscanf(file, "%ld", &top[static_cast<size_t>(cpu)]) != 1)
                    top[static_cast<size_t>(cpu)] = 0;
                std::fclose(file);
            }
            const long freq = top[static_cast<size_t>(cpu)];
            if (freq > 0 && (slowest == 0 || freq < slowest)) slowest = freq;
        }
        for (int cpu = 0; cpu < count; ++cpu) {
            if (top[static_cast<size_t>(cpu)] > slowest) {
                CPU_SET(cpu, &desired);
                ++chosen_count;
            }
        }
        if (chosen_count == 0) return;
    } else {
        std::vector<sc2::CpuFreqInfo> cores(static_cast<size_t>(count));
        for (int cpu = 0; cpu < count; ++cpu) {
            char path[96];
            std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/online", cpu);
            if (std::FILE *file = std::fopen(path, "r")) {
                int online = 1;
                if (std::fscanf(file, "%d", &online) == 1)
                    cores[static_cast<size_t>(cpu)].online = (online != 0);
                std::fclose(file);
            }
            std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            if (std::FILE *file = std::fopen(path, "r")) {
                if (std::fscanf(file, "%ld", &cores[static_cast<size_t>(cpu)].info_max_khz) != 1)
                    cores[static_cast<size_t>(cpu)].info_max_khz = 0;
                std::fclose(file);
            }
            std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
            if (std::FILE *file = std::fopen(path, "r")) {
                if (std::fscanf(file, "%ld", &cores[static_cast<size_t>(cpu)].scaling_max_khz) != 1)
                    cores[static_cast<size_t>(cpu)].scaling_max_khz = 0;
                std::fclose(file);
            }
        }
        const std::vector<int> selected = sc2::SelectActiveCores(cores);
        if (selected.empty()) return;
        for (int cpu : selected) CPU_SET(cpu, &desired);
        chosen_count = static_cast<int>(selected.size());
    }

    static cpu_set_t current_mask{};
    static bool has_mask = false;
    if (has_mask && CPU_EQUAL(&desired, &current_mask)) {
        if (log_unchanged) {
            std::fprintf(stderr, "soulcalibur2: active cores = %d/%d (caps %s)\n",
                chosen_count, count, coreCaps().c_str());
        }
        return;
    }
    if (sched_setaffinity(0, sizeof desired, &desired) == 0) {
        current_mask = desired;
        has_mask = true;
        std::fprintf(stderr, "soulcalibur2: active cores = %d/%d (caps %s)\n",
            chosen_count, count, coreCaps().c_str());
    }
}

// (Not the fastest core alone by default: a phone caps that one first when it warms, below the next ones, and the
// game was then held on it. On the active cores the system moves it to whichever is quickest at the time.
// SC2_FASTEST_CORE=1, for measuring: from the first frame the game's thread on the fastest core alone.)
static void fastestCore()
{
    const int count = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    if (count <= 0) return;
    long fastest = 0;
    std::vector<long> top(static_cast<size_t>(count), 0);
    for (int cpu = 0; cpu < count; ++cpu) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        if (std::FILE *file = std::fopen(path, "r")) {
            if (std::fscanf(file, "%ld", &top[static_cast<size_t>(cpu)]) != 1)
                top[static_cast<size_t>(cpu)] = 0;
            std::fclose(file);
        }
        fastest = std::max(fastest, top[static_cast<size_t>(cpu)]);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    int chosen = 0;
    for (int cpu = 0; cpu < count; ++cpu) {
        if (fastest > 0 && top[static_cast<size_t>(cpu)] == fastest) {
            CPU_SET(cpu, &set);
            ++chosen;
        }
    }
    if (chosen > 0 && chosen < count && sched_setaffinity(0, sizeof set, &set) == 0)
        std::fprintf(stderr, "soulcalibur2: the game's thread on the fastest core (SC2_FASTEST_CORE)\n");
}

static void reportCrashes()
{
    static char stack[64 * 1024];
    stack_t alternate{};
    alternate.ss_sp = stack;
    alternate.ss_size = sizeof stack;
    sigaltstack(&alternate, nullptr);
    struct sigaction action{};
    action.sa_sigaction = crashed;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(signal, &action, nullptr);
}

static void sc2_android_on_exit()
{
    std::fflush(stdout);
    std::fflush(stderr);
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    if (!env) return;
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) return;
    jclass activity_class = env->GetObjectClass(activity);
    if (activity_class) {
        jmethodID finish_game = env->GetMethodID(activity_class, "finishGame", "(I)V");
        if (finish_game) {
            env->CallVoidMethod(activity, finish_game, 0);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(activity_class);
    }
    env->DeleteLocalRef(activity);
}

void sc2_android_start()
{
    SDL_SetAssertionHandler(passAssertion, nullptr);
    // Landscape either way up, as the launcher (the manifest's SDL_ENV values don't reach the game's process), and
    // the back button for the game (it opens the menu) instead of closing it
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    // SDL's systems that reach Java start here, on the activity's own thread's stack (host_call.h has the rest),
    // and the touch screen's controls with them
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
        __android_log_print(ANDROID_LOG_WARN, "soulcalibur2", "SDL: %s", SDL_GetError());
    sc2_touch_init();
    if (const char *folder = getenv("RECOMP_FOLDER"); folder && *folder) {
        if (chdir(folder) != 0) __android_log_print(ANDROID_LOG_WARN, "soulcalibur2", "no folder %s", folder);
    }
    // (not freopen: the standard output's descriptor is the system's, and fdsan stops a process that closes it)
    const char *log = getenv("RECOMP_LOG");
    const int file = log && *log ? open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    if (file >= 0) {
        dup2(file, 1);
        dup2(file, 2);
        close(file);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IOLBF, 0);
    } else {
        toSystemLog();
    }
    std::atexit(sc2_android_on_exit);
    reportCrashes();
    // Leave process startup affinity unrestricted so background threads (recomp_host_pump,
    // AAudio, Vulkan driver workers, shader precompile) can be scheduled across all cores
    // while sc2_android_frame pins only the heavy Main Game + Inline Presenter Thread.
    recomp_host_pump_start();
}

// The game's speed in its log every ten seconds: frames begun, longest frame, and per-stage host breakdown
extern "C" void sc2_android_frame(void)
{
    static uint64_t window_start = 0, last = 0, longest = 0, last_affinity_check = 0;
    static unsigned long long gaps_before = 0;
    static double vblank_before = 0.0;
    static bool first_frame = true, pinned_fastest = false;
    static unsigned frames = 0;
    const uint64_t now = SDL_GetTicksNS();
    if (last != 0) longest = std::max(longest, now - last);
    last = now;
    ++frames;
    if (window_start == 0) window_start = now;
    if (first_frame) {
        first_frame = false;
        if (const char *pin = std::getenv("SC2_FASTEST_CORE"); pin != nullptr && *pin == '1') {
            fastestCore();
            pinned_fastest = true;
        } else {
            applyCoreAffinity(true);
        }
        last_affinity_check = now;
    } else if (!pinned_fastest && now - last_affinity_check >= 3000000000ull) {
        last_affinity_check = now;
        applyCoreAffinity(false);
    }
    if (now - window_start >= 10000000000ull) {
        const unsigned long long gaps = recomp_audio_output_underruns();
        RecompD3dPresenterPerf perf{};
        recomp_d3d_presenter_take_perf(&perf);
        const uint64_t prep_ns = sc2_d3d8_take_draw_prep_ns != nullptr ? sc2_d3d8_take_draw_prep_ns() : 0ull;
        const double vblank_now = recomp_d3d_vblank_wait_ms;
        const double vblank_ms = std::max(0.0, vblank_now - vblank_before);
        vblank_before = vblank_now;
        const double denom = frames > 0u ? static_cast<double>(frames) : 1.0;
        const double elapsed_ms = double(now - window_start) / 1e6;
        const double frame_ms = elapsed_ms / denom;
        const double prep_ms = (prep_ns / 1e6) / denom;
        const double draw_ms = (perf.draw_ns / 1e6) / denom;
        const double upload_ms = (perf.upload_ns / 1e6) / denom;
        const double decode_ms = (perf.tex_decode_ns / 1e6) / denom;
        const double flush_ms = (perf.flush_ns / 1e6) / denom;
        const double pres_ms = (perf.present_total_ns / 1e6) / denom;
        const double wait_ms = (perf.present_wait_ns / 1e6) / denom;
        const double guest_ms = std::max(0.0, frame_ms - (prep_ms + draw_ms + upload_ms + flush_ms + pres_ms));
        const std::vector<sc2::ClusterFreqSample> freqs = readClusterFreqs();
        const std::string caps_str = sc2::FormatClusterCaps(freqs);
        const std::string cur_str = sc2::FormatClusterCur(freqs);
        const int active_cpu = sched_getcpu();
        const std::string temp_str = readThermalStatus();
        std::fprintf(stderr,
            "soulcalibur2 speed: %.1f frames a second, longest frame %.1f ms, sound gaps %llu "
            "(caps %s | cur %s | cpu=%d | temp=%s) | guest=%.2f prep=%.2f draw=%.2f upload=%.2f dec=%.2f flush=%.2f pres=%.2f wait=%.2f vblank=%.2f ms/f "
            "max(d=%.1f f=%.1f p=%.1f ms) (d=%u u=%u dec=%u f=%u)\n",
            frames * 1000.0 / elapsed_ms, longest / 1e6, gaps - gaps_before,
            caps_str.c_str(), cur_str.c_str(), active_cpu, temp_str.c_str(),
            guest_ms, prep_ms, draw_ms, upload_ms, decode_ms, flush_ms, pres_ms, wait_ms,
            vblank_ms / denom,
            perf.max_frame_draw_ns / 1e6,
            perf.max_frame_flush_ns / 1e6,
            perf.max_frame_present_ns / 1e6,
            perf.draws, perf.uploads, perf.dxt_decodes, perf.flushes);
        std::fflush(stderr);
        gaps_before = gaps;
        window_start = now;
        frames = 0;
        longest = 0;
    }
}

bool sc2_android_start_again(const std::vector<std::string> &args)
{
    // GameActivity.startAgain(String[]): the game in a new process with these arguments, its result the launcher's
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    jclass activity_class = env->GetObjectClass(activity);
    jmethodID start_again = activity_class ? env->GetMethodID(activity_class, "startAgain", "([Ljava/lang/String;)Z") : nullptr;
    if (!start_again) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (activity_class) env->DeleteLocalRef(activity_class);
        env->DeleteLocalRef(activity);
        return false;
    }
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray arguments = string_class ? env->NewObjectArray(static_cast<jsize>(args.size()), string_class, nullptr) : nullptr;
    if (!arguments) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (string_class) env->DeleteLocalRef(string_class);
        env->DeleteLocalRef(activity_class);
        env->DeleteLocalRef(activity);
        return false;
    }
    for (size_t i = 0; i < args.size(); i++) {
        jstring arg = env->NewStringUTF(args[i].c_str());
        if (!arg) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(arguments);
            env->DeleteLocalRef(string_class);
            env->DeleteLocalRef(activity_class);
            env->DeleteLocalRef(activity);
            return false;
        }
        env->SetObjectArrayElement(arguments, static_cast<jsize>(i), arg);
        env->DeleteLocalRef(arg);
    }
    const bool started = env->CallBooleanMethod(activity, start_again, arguments) != JNI_FALSE;
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(arguments);
    env->DeleteLocalRef(string_class);
    env->DeleteLocalRef(activity_class);
    env->DeleteLocalRef(activity);
    return started;
}

// For the launcher, which has no game program to run with --list-features: it opens the game's library and asks it
// (recomp-common/launcher/src/game_process.cpp). The text is the caller's to free.
extern "C" __attribute__((visibility("default"))) char *RecompListFeatures()
{
    return strdup(recomp::FeatureListText(sc2::Features()).c_str());
}
#endif
