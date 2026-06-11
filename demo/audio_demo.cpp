// audio_demo.cpp -- the "why lock-free" story made audible.
//
// Simulates a real-time audio engine: a control thread (the "UI") streams
// parameter changes to an audio callback that must render one buffer every
// ~5.3 ms (256 frames @ 48 kHz). That deadline is hard -- if the callback can't
// get its data and render in time, the sound card is handed nothing and you hear
// a click/dropout (an "xrun"). The audio thread must therefore never block.
//
// We run the SAME workload twice: once with a std::mutex + std::queue control
// channel, once with the lock-free SPSCQueue. We count dropouts, measure how
// long the audio thread waited for its data, and write both results to WAV so
// you can literally listen to the difference.
//
// Modeling note (read this -- it's the honest part): on a real system the audio
// glitch is caused by *priority inversion* -- the OS preempts the low-priority
// control thread while it holds the lock (or it page-faults / does a slow op
// under the lock), and the high-priority audio thread is stuck waiting. We model
// that by having the control thread occasionally hold its channel busy for a few
// ms. With a mutex, that busy time is spent holding the lock, so the audio
// thread blocks. With the lock-free queue there is no lock, so the audio thread
// is never blocked by the control thread -- that is the wait-free guarantee,
// demonstrated rather than asserted.
#include "spsc/spsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static constexpr double kPi = 3.14159265358979323846;

// ---- audio / simulation parameters ---------------------------------------
static constexpr unsigned kSampleRate = 48000;
static constexpr int kBufferFrames = 256;  // frames per callback
static constexpr double kBufferMs = 1000.0 * kBufferFrames / kSampleRate;  // ~5.33
static constexpr double kControlIntervalMs = 1.0;                          // UI pushes ~1000/s
static constexpr double kStallEveryMs = 150.0;  // preemption cadence
static constexpr double kStallMinMs = 6.0;      // > buffer budget...
static constexpr double kStallMaxMs = 12.0;     // ...so it causes xruns

static void sleep_ms(double ms) {
    if (ms <= 0) return;
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}
static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ---- the two control channels, same interface ----------------------------
// push_busy(freq, busyMs): hand a new target frequency to the audio thread,
//   spending `busyMs` "preempted". try_pop(out): audio thread drains an event.

struct MutexChannel {
    std::mutex m;
    std::queue<float> q;
    std::size_t cap;
    explicit MutexChannel(std::size_t c) : cap(c) {}
    void push_busy(float v, double busyMs) {
        std::lock_guard<std::mutex> g(m);  // the slow op happens UNDER the lock
        sleep_ms(busyMs);                  // -> audio thread blocks meanwhile
        if (q.size() < cap) q.push(v);
    }
    bool try_pop(float& out) {
        std::lock_guard<std::mutex> g(m);
        if (q.empty()) return false;
        out = q.front();
        q.pop();
        return true;
    }
};

struct LockFreeChannel {
    spsc::SPSCQueue<float> q;
    explicit LockFreeChannel(std::size_t c) : q(c) {}
    void push_busy(float v, double busyMs) {
        sleep_ms(busyMs);  // preempted, but holding NOTHING
        q.try_push(v);     // wait-free hand-off (drop if full)
    }
    bool try_pop(float& out) { return q.try_pop(out); }
};

// ---- minimal 16-bit mono PCM WAV writer ----------------------------------
static void put_le(FILE* f, std::uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) std::fputc((v >> (8 * i)) & 0xff, f);
}
static void write_wav(const std::string& path, const std::vector<std::int16_t>& s) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "could not open %s\n", path.c_str());
        return;
    }
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(s.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    put_le(f, 36 + dataBytes, 4);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    put_le(f, 16, 4);
    put_le(f, 1, 2);  // PCM
    put_le(f, 1, 2);  // mono
    put_le(f, kSampleRate, 4);
    put_le(f, kSampleRate * 2, 4);  // byte rate
    put_le(f, 2, 2);                // block align
    put_le(f, 16, 2);               // bits/sample
    std::fwrite("data", 1, 4, f);
    put_le(f, dataBytes, 4);
    for (std::int16_t v : s) put_le(f, static_cast<std::uint16_t>(v), 2);
    std::fclose(f);
}

// A little C-major scale so the tune is recognizable and dropouts stand out.
static float melody_freq(double elapsedMs) {
    static const float scale[8] = {261.63f, 293.66f, 329.63f, 349.23f,
                                   392.00f, 440.00f, 493.88f, 523.25f};
    return scale[static_cast<int>(elapsedMs / 250.0) % 8];
}

struct Result {
    int buffers = 0;
    int xruns = 0;
    double maxDrainMs = 0;
    double p99DrainMs = 0;
};

template <class Channel> static Result run(const char* wavPath, int numBuffers) {
    Channel ch(1024);
    std::atomic<bool> done{false};

    std::thread control([&] {
        std::mt19937 rng(12345);  // fixed seed = fair A/B
        std::uniform_real_distribution<double> stallDist(kStallMinMs, kStallMaxMs);
        auto t0 = Clock::now();
        double nextStallMs = kStallEveryMs;
        while (!done.load(std::memory_order_relaxed)) {
            double t = ms_since(t0);
            float base = melody_freq(t);
            float vib = 1.0f + 0.02f * std::sin(2.0 * kPi * 5.0 * t / 1000.0);  // 5 Hz vibrato
            double busy = 0.0;
            if (t >= nextStallMs) {
                busy = stallDist(rng);
                nextStallMs += kStallEveryMs;
            }
            ch.push_busy(base * vib, busy);
            sleep_ms(kControlIntervalMs);
        }
    });

    // This thread plays the role of the real-time audio callback.
    std::vector<std::int16_t> pcm;
    pcm.reserve(static_cast<std::size_t>(numBuffers) * kBufferFrames);
    std::vector<double> drain;
    drain.reserve(numBuffers);
    double phase = 0.0, freq = 440.0;
    int xruns = 0;

    auto start = Clock::now();
    for (int b = 0; b < numBuffers; ++b) {
        auto deadline =
            start + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double, std::milli>((b + 1) * kBufferMs));
        // Fetch the latest control state. With the mutex channel this is where
        // the audio thread can block behind the held lock.
        auto d0 = Clock::now();
        float f;
        while (ch.try_pop(f)) freq = f;
        double drainMs = ms_since(d0);
        drain.push_back(drainMs);

        // If fetching the data blew the per-buffer budget, the device starved:
        // emit silence for this buffer == an audible click.
        bool late = drainMs > kBufferMs;
        if (late) ++xruns;
        for (int i = 0; i < kBufferFrames; ++i) {
            float s = late ? 0.0f : 0.25f * static_cast<float>(std::sin(phase));
            phase += 2.0 * kPi * freq / kSampleRate;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
            pcm.push_back(static_cast<std::int16_t>(s * 32767.0f));
        }
        std::this_thread::sleep_until(deadline);  // real-time pacing
    }

    done.store(true, std::memory_order_relaxed);
    control.join();
    write_wav(wavPath, pcm);

    std::sort(drain.begin(), drain.end());
    Result r;
    r.buffers = numBuffers;
    r.xruns = xruns;
    r.maxDrainMs = drain.empty() ? 0 : drain.back();
    r.p99DrainMs =
        drain.empty() ? 0 : drain[static_cast<std::size_t>(0.99 * (drain.size() - 1))];
    return r;
}

int main(int argc, char** argv) {
    double seconds = (argc > 1) ? std::atof(argv[1]) : 4.0;
    int numBuffers = static_cast<int>(seconds * 1000.0 / kBufferMs);

    std::printf("Real-time audio engine simulation\n");
    std::printf("  %u Hz, %d-frame buffers => %.2f ms budget per callback\n", kSampleRate,
                kBufferFrames, kBufferMs);
    std::printf("  control thread: ~%.0f events/s; models OS preemption by staying\n",
                1000.0 / kControlIntervalMs);
    std::printf("  busy %.0f-%.0f ms every ~%.0f ms (priority inversion)\n", kStallMinMs,
                kStallMaxMs, kStallEveryMs);
    std::printf("  duration: %.1f s (%d buffers)\n\n", seconds, numBuffers);

    std::printf("running mutex + std::queue ...\n");
    Result mx = run<MutexChannel>("audio_mutex.wav", numBuffers);
    std::printf("running lock-free SPSCQueue ...\n\n");
    Result lf = run<LockFreeChannel>("audio_lockfree.wav", numBuffers);

    std::printf("%-26s %14s %16s\n", "", "mutex+queue", "lock-free SPSC");
    std::printf("%-26s %14d %16d\n", "buffers rendered", mx.buffers, lf.buffers);
    std::printf("%-26s %14d %16d\n", "audio dropouts (xruns)", mx.xruns, lf.xruns);
    std::printf("%-26s %12.0f ms %14.0f ms\n", "  = silence/glitch time", mx.xruns * kBufferMs,
                lf.xruns * kBufferMs);
    std::printf("%-26s %12.3f ms %14.3f ms\n", "max wait for data", mx.maxDrainMs,
                lf.maxDrainMs);
    std::printf("%-26s %12.3f ms %14.3f ms\n", "p99 wait for data", mx.p99DrainMs,
                lf.p99DrainMs);

    std::printf("\nThe audio thread's per-buffer budget is %.2f ms. The mutex run's\n",
                kBufferMs);
    std::printf("worst-case wait exceeds it (it blocked behind the held lock); the\n");
    std::printf("lock-free run's wait stays in microseconds -- it never waits on the\n");
    std::printf("producer at all.\n\n");
    std::printf("Listen:\n  afplay audio_mutex.wav      # clicks / dropouts\n");
    std::printf("  afplay audio_lockfree.wav   # clean\n");
    return 0;
}
