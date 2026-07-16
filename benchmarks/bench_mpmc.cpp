// bench_mpmc.cpp -- the study's single scriptable measurement binary.
//
//   ./bench_mpmc --queue vyukov --producers 4 --consumers 4 --mode throughput \
//                --seconds 3 --capacity 1024 --qos none --oversubscribe 1 \
//                --trial 0 --csv paper/data/raw.csv
//
// One process = one trial = one CSV row (header auto-written when the file is
// new). The experiment matrix lives in scripts/run_matrix.py, NOT here -- this
// binary measures exactly one configuration so trials are isolated processes.
//
// Modes
//   throughput: producers push a counter payload as fast as the queue accepts;
//     after --seconds the stop flag rises, producers exit, one poison pill per
//     consumer is enqueued, consumers drain and exit. Reported ops = real pops.
//   latency: producers are PACED to a fixed schedule (--rate total msgs/s split
//     across producers). The payload is the *scheduled* send time, and latency
//     is measured consumer-side as (now - scheduled): late producers are
//     charged for their lateness, which is the coordinated-omission-aware
//     measurement (Tene). p50/p99/p99.9/max + mean reported.
//
// Uniform shutdown protocol (works for every queue, including the FAA queue
// whose pop() blocks): payload ~0ull is reserved as the poison pill; producers
// never emit it; after producers join, main enqueues exactly one pill per
// consumer; a consumer exits on its first pill.
//
// macOS-specific axes for the paper:
//   --qos {none,all-int,all-bg,prod-bg,cons-bg}: requests macOS scheduling
//     policy classes. It neither controls nor observes P/E-core residency, so
//     rows record the requested policy only.
//   --oversubscribe F: multiplies both thread counts (ratio preserved) to test
//     how each queue's reservation/completion structure responds to preemption.
//   A spin-calibration loop (fixed work, timed) runs before the trial as a
//   cheap frequency/thermal proxy, logged per row (fanless M2 methodology).
#include "bench_common.hpp"
#include "mpmc/faa_queue.hpp"
#include "mpmc/ms_queue.hpp"
#include "mpmc/vyukov_queue.hpp"
#include "spsc/spsc_queue.hpp"

#ifdef SPSC_HAVE_MOODYCAMEL
#include <concurrentqueue.h>
#endif

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kPoison = ~0ull;

// ---------------- queue adapters: uniform try_push/try_pop ----------------
// FAA's operations block instead of failing; its adapter reports true always.
// The poison protocol makes that safe (consumers never wait for data that
// cannot arrive).

struct MSAdapter {
    mpmc::MSQueue<std::uint64_t> q;
    explicit MSAdapter(std::size_t cap) : q(cap) {}
    bool try_push(std::uint64_t v) { return q.try_push(v); }
    bool try_pop(std::uint64_t& v) { return q.try_pop(v); }
    std::uint64_t stat_retries() const { return q.stat_retries(); }
    std::uint64_t stat_secondary() const { return q.stat_secondary(); }
};

struct VyukovAdapter {
    mpmc::VyukovQueue<std::uint64_t, false> q;
    explicit VyukovAdapter(std::size_t cap) : q(cap) {}
    bool try_push(std::uint64_t v) { return q.try_push(v); }
    bool try_pop(std::uint64_t& v) { return q.try_pop(v); }
    std::uint64_t stat_retries() const { return q.stat_retries(); }
    std::uint64_t stat_secondary() const { return q.stat_secondary(); }
};

struct VyukovBackoffAdapter {  // control: same algorithm + FAA's spin-then-yield
    mpmc::VyukovQueue<std::uint64_t, true> q;
    explicit VyukovBackoffAdapter(std::size_t cap) : q(cap) {}
    bool try_push(std::uint64_t v) { return q.try_push(v); }
    bool try_pop(std::uint64_t& v) { return q.try_pop(v); }
    std::uint64_t stat_retries() const { return q.stat_retries(); }
    std::uint64_t stat_secondary() const { return q.stat_secondary(); }
};

template <bool CasClaim> struct TicketAdapter {
    mpmc::FAAQueue<std::uint64_t, CasClaim> q;
    explicit TicketAdapter(std::size_t cap) : q(cap) {}
    bool try_push(std::uint64_t v) {
        q.push(v);
        return true;
    }
    bool try_pop(std::uint64_t& v) {
        q.pop(v);
        return true;
    }
    std::uint64_t stat_retries() const { return q.stat_retries(); }
    std::uint64_t stat_secondary() const { return q.stat_secondary(); }
};
using FAAAdapter = TicketAdapter<false>;
using CasTicketAdapter = TicketAdapter<true>;  // control: CAS claim, same completion

struct MutexAdapter {
    std::uint64_t stat_retries() const { return 0; }
    std::uint64_t stat_secondary() const { return 0; }
    bench::MutexQueue<std::uint64_t> q;
    explicit MutexAdapter(std::size_t cap) : q(cap) {}
    bool try_push(std::uint64_t v) { return q.try_push(v); }
    bool try_pop(std::uint64_t& v) { return q.try_pop(v); }
};

struct SPSCAdapter {
    std::uint64_t stat_retries() const { return 0; }
    std::uint64_t stat_secondary() const {
        return 0;
    }  // valid only for exactly 1 producer + 1 consumer
    spsc::SPSCQueue<std::uint64_t> q;
    explicit SPSCAdapter(std::size_t cap) : q(cap) {}
    bool try_push(std::uint64_t v) { return q.try_push(v); }
    bool try_pop(std::uint64_t& v) { return q.try_pop(v); }
};

#ifdef SPSC_HAVE_MOODYCAMEL
struct MoodyAdapter {
    std::uint64_t stat_retries() const { return 0; }
    std::uint64_t stat_secondary() const { return 0; }
    moodycamel::ConcurrentQueue<std::uint64_t> q;
    explicit MoodyAdapter(std::size_t cap) : q(cap) {}
    bool try_push(std::uint64_t v) { return q.try_enqueue(v); }
    bool try_pop(std::uint64_t& v) { return q.try_dequeue(v); }
};
#endif

// ---------------- config / CSV ----------------

struct Config {
    std::string queue = "vyukov";
    int producers = 1, consumers = 1, oversubscribe = 1, trial = 0;
    std::size_t capacity = 1024;
    std::string mode = "throughput";
    double seconds = 3.0;
    double rate = 1e6;  // latency mode: total offered msgs/sec
    std::string qos = "none";
    std::string csv;  // empty = print human-readable only
};

struct Result {
    std::uint64_t ops = 0;
    double elapsed = 0;
    double mean_ns = 0;
    std::uint64_t p50 = 0, p99 = 0, p999 = 0, mx = 0;
    // Historical CSV names retained for compatibility. These CoVs describe
    // finite-window work balance, not starvation freedom or scheduler fairness.
    double fair_cov = 0;  // per-producer completed-push count CoV
    std::uint64_t calib_ns = 0;
    double peak_rss_mb = 0;       // process peak RSS (H3: unbounded MS memory growth)
    double cons_cov = 0;          // per-consumer completed-pop count CoV
    long vcsw = 0, ivcsw = 0;     // voluntary / involuntary context switches
    double retries_per_op = 0;    // stats build: queue-specific primary counter
    double secondary_per_op = 0;  // stats build: queue-specific secondary counter
};

[[noreturn]] void invalid_number(const char* flag, const char* value, const char* expected) {
    std::fprintf(stderr, "invalid value for %s: '%s' (expected %s)\n", flag, value, expected);
    std::exit(2);
}

int parse_int(const char* value, const char* flag, bool allowZero = false) {
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed > std::numeric_limits<int>::max() ||
        (allowZero ? parsed < 0 : parsed <= 0))
        invalid_number(flag, value,
                       allowZero ? "a non-negative integer" : "a positive integer");
    return static_cast<int>(parsed);
}

std::size_t parse_size(const char* value, const char* flag) {
    const char* first = value;
    while (std::isspace(static_cast<unsigned char>(*first))) ++first;
    if (*first == '-') invalid_number(flag, value, "a positive integer");

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::size_t>::max())
        invalid_number(flag, value, "a positive integer");
    return static_cast<std::size_t>(parsed);
}

double parse_double(const char* value, const char* flag) {
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (errno == ERANGE || end == value || *end != '\0' || !std::isfinite(parsed) ||
        parsed <= 0)
        invalid_number(flag, value, "a positive finite number");
    return parsed;
}

int checked_thread_total(int base, int multiplier, const char* role) {
    if (base > std::numeric_limits<int>::max() / multiplier) {
        std::fprintf(stderr, "%s thread count overflows int (%d x %d)\n", role, base,
                     multiplier);
        std::exit(2);
    }
    return base * multiplier;
}

bool supported_queue(const std::string& queue) {
    if (queue == "ms" || queue == "ms-fix" || queue == "ms-retry" ||
        queue == "vyukov" || queue == "vyukov-b" || queue == "faa" ||
        queue == "casticket" || queue == "mutex" || queue == "spsc")
        return true;
#ifdef SPSC_HAVE_MOODYCAMEL
    if (queue == "moody") return true;
#endif
    return false;
}

bool supported_qos(const std::string& qos) {
    return qos == "none" || qos == "all-int" || qos == "all-bg" || qos == "prod-bg" ||
           qos == "cons-bg";
}

#ifdef __APPLE__
void set_qos_checked(qos_class_t cls, const char* context) {
    const int rc = pthread_set_qos_class_self_np(cls, 0);
    if (rc != 0) {
        std::fprintf(stderr, "failed to apply %s QoS: %s\n", context, std::strerror(rc));
        std::abort();  // never emit a row whose requested policy was not applied
    }
}
#endif

double cov_of(const std::vector<std::uint64_t>& xs) {
    if (xs.empty()) return 0.0;
    double sum = 0, sq = 0;
    for (auto v : xs) sum += static_cast<double>(v);
    const double mean = sum / static_cast<double>(xs.size());
    if (mean <= 0) return 0.0;
    for (auto v : xs) sq += (static_cast<double>(v) - mean) * (static_cast<double>(v) - mean);
    return std::sqrt(sq / static_cast<double>(xs.size())) / mean;
}

void apply_qos(const std::string& policy, bool isProducer) {
#ifdef __APPLE__
    qos_class_t cls;
    if (policy == "all-int")
        cls = QOS_CLASS_USER_INTERACTIVE;
    else if (policy == "all-bg")
        cls = QOS_CLASS_BACKGROUND;
    else if (policy == "prod-bg")
        cls = isProducer ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE;
    else if (policy == "cons-bg")
        cls = isProducer ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_BACKGROUND;
    else
        return;  // "none"
    set_qos_checked(cls, policy.c_str());
#else
    (void)policy;
    (void)isProducer;
#endif
}

// Fixed dependent-work loop, timed: a cheap frequency proxy so the matrix
// script can detect thermal drift between trials (fanless M2). A fixed
// USER_INTERACTIVE request reduces scheduler-policy variation but neither
// guarantees nor observes core residency or frequency. Matrix v1's 97-172 ms
// spread was policy/placement-confounded; see paper/notes/claims.md caveat C1.
std::uint64_t spin_calibration() {
#ifdef __APPLE__
    set_qos_checked(QOS_CLASS_USER_INTERACTIVE, "calibration interactive");
#endif
    volatile std::uint64_t acc = 1;
    const auto t0 = bench::Clock::now();
    for (int i = 0; i < 50'000'000; ++i)
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(bench::Clock::now() - t0)
            .count());
#ifdef __APPLE__
    // Workers inherit the spawning thread's QoS: restore DEFAULT so the probe's
    // QoS bias does not silently turn the "none" policy into all-interactive.
    set_qos_checked(QOS_CLASS_DEFAULT, "post-calibration default");
#endif
    return ns;
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          bench::Clock::now().time_since_epoch())
                                          .count());
}

// Peak resident set size in MB (macOS reports ru_maxrss in bytes, Linux in KB).
double peak_rss_mb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

// ---------------- the two measurement modes ----------------

template <class Q> Result run_throughput(const Config& cfg) {
    const int P = cfg.producers * cfg.oversubscribe;
    const int C = cfg.consumers * cfg.oversubscribe;
    Q q(cfg.capacity);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> pops{0};
    std::vector<std::uint64_t> pushed(static_cast<std::size_t>(P), 0);
    std::vector<std::uint64_t> popped(static_cast<std::size_t>(C), 0);

    std::vector<std::thread> prods, cons;
    for (int c = 0; c < C; ++c) {
        cons.emplace_back([&, c] {
            apply_qos(cfg.qos, false);
            std::uint64_t v = 0, local = 0;
            for (;;) {
                if (!q.try_pop(v)) {
                    std::this_thread::yield();
                    continue;
                }
                if (v == kPoison) break;
                ++local;
            }
            popped[static_cast<std::size_t>(c)] = local;
            pops.fetch_add(local, std::memory_order_relaxed);
        });
    }
    const auto t0 = bench::Clock::now();
    for (int p = 0; p < P; ++p) {
        prods.emplace_back([&, p] {
            apply_qos(cfg.qos, true);
            std::uint64_t i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (q.try_push(i)) ++i;
                // saturated mode: spin on full (no yield) -- documented choice
            }
            pushed[static_cast<std::size_t>(p)] = i;
        });
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.seconds));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : prods) t.join();
    for (int c = 0; c < C; ++c)
        while (!q.try_push(kPoison)) std::this_thread::yield();
    for (auto& t : cons) t.join();
    const double elapsed = bench::seconds_since(t0);

    Result r;
    r.ops = pops.load();
    r.elapsed = elapsed;
    r.fair_cov = cov_of(pushed);  // finite-window producer work balance
    r.cons_cov = cov_of(popped);  // finite-window consumer work balance
    if (r.ops > 0) {
        r.retries_per_op = static_cast<double>(q.stat_retries()) / static_cast<double>(r.ops);
        r.secondary_per_op =
            static_cast<double>(q.stat_secondary()) / static_cast<double>(r.ops);
    }
    return r;
}

template <class Q> Result run_latency(const Config& cfg) {
    const int P = cfg.producers * cfg.oversubscribe;
    const int C = cfg.consumers * cfg.oversubscribe;
    Q q(cfg.capacity);
    const double perProducerRate = cfg.rate / P;
    const std::uint64_t periodNs = static_cast<std::uint64_t>(1e9 / perProducerRate);
    const std::uint64_t sends =
        static_cast<std::uint64_t>(cfg.seconds * perProducerRate);  // per producer

    std::vector<std::uint64_t> popped(static_cast<std::size_t>(C), 0);
    std::vector<std::vector<std::uint64_t>> samples(static_cast<std::size_t>(C));
    // Reserve 1.5x an even consumer share as a hot-path allocation hint. A
    // vector may still grow when dequeue work is imbalanced: every observation
    // must be retained for unbiased tail statistics and exact accounting.
    for (auto& s : samples)
        s.reserve(
            static_cast<std::size_t>(std::min<double>(cfg.rate * cfg.seconds / C * 1.5, 4e6)));

    std::vector<std::thread> prods, cons;
    for (int c = 0; c < C; ++c) {
        cons.emplace_back([&, c] {
            apply_qos(cfg.qos, false);
            std::uint64_t v = 0;
            auto& mine = samples[static_cast<std::size_t>(c)];
            for (;;) {
                if (!q.try_pop(v)) {
                    std::this_thread::yield();
                    continue;
                }
                if (v == kPoison) break;
                ++popped[static_cast<std::size_t>(c)];
                const std::uint64_t nowv = now_ns();
                mine.push_back(nowv - v);
            }
        });
    }
    const std::uint64_t start = now_ns() + 10'000'000;  // 10 ms in the future
    for (int p = 0; p < P; ++p) {
        prods.emplace_back([&, p] {
            apply_qos(cfg.qos, true);
            // stagger producers within one period to avoid a thundering herd
            const std::uint64_t offset =
                periodNs * static_cast<std::uint64_t>(p) / static_cast<std::uint64_t>(P);
            for (std::uint64_t k = 0; k < sends; ++k) {
                const std::uint64_t scheduled = start + offset + k * periodNs;
                while (now_ns() < scheduled) { /* spin: sub-us pacing */
                }
                // payload = SCHEDULED time: lateness is charged (no CO omission)
                while (!q.try_push(scheduled)) std::this_thread::yield();
            }
        });
    }
    for (auto& t : prods) t.join();
    for (int c = 0; c < C; ++c)
        while (!q.try_push(kPoison)) std::this_thread::yield();
    for (auto& t : cons) t.join();

    std::vector<std::uint64_t> all;
    std::size_t total = 0;
    for (auto& s : samples) total += s.size();
    all.reserve(total);
    for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());

    Result r;
    r.ops = all.size();
    r.elapsed = cfg.seconds;
    r.cons_cov = cov_of(popped);
    std::uint64_t total_pops = 0;
    for (auto v : popped) total_pops += v;
    const std::uint64_t producerCount = static_cast<std::uint64_t>(P);
    if (sends > std::numeric_limits<std::uint64_t>::max() / producerCount) {
        std::fprintf(stderr, "latency expected-count overflow\n");
        std::exit(3);
    }
    const std::uint64_t expected = sends * producerCount;
    if (total_pops != expected || all.size() != total_pops) {
        std::fprintf(stderr,
                     "latency accounting mismatch: expected=%" PRIu64 ", popped=%" PRIu64
                     ", retained=%zu\n",
                     expected, total_pops, all.size());
        std::exit(3);
    }
    if (total_pops > 0) {
        r.retries_per_op =
            static_cast<double>(q.stat_retries()) / static_cast<double>(total_pops);
        r.secondary_per_op =
            static_cast<double>(q.stat_secondary()) / static_cast<double>(total_pops);
    }
    if (!all.empty()) {
        double sum = 0;
        for (auto v : all) sum += static_cast<double>(v);
        r.mean_ns = sum / static_cast<double>(all.size());
        r.p50 = bench::percentile(all, 0.50);
        r.p99 = bench::percentile(all, 0.99);
        r.p999 = bench::percentile(all, 0.999);
        r.mx = all.back();  // percentile() sorted the vector
    }
    return r;
}

template <class Q> Result dispatch_mode(const Config& cfg) {
    if (cfg.mode == "latency") return run_latency<Q>(cfg);
    return run_throughput<Q>(cfg);
}

void write_csv(const Config& cfg, const Result& r) {
    if (cfg.csv.empty()) return;
    FILE* f = std::fopen(cfg.csv.c_str(), "a");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", cfg.csv.c_str());
        std::exit(2);
    }
    std::fseek(f, 0, SEEK_END);
    if (std::ftell(f) == 0)
        std::fprintf(f, "queue,mode,producers,consumers,oversubscribe,capacity,qos,seconds,"
                        "rate,trial,ops,elapsed_s,throughput_mops,mean_ns,p50_ns,p99_ns,"
                        "p999_ns,max_ns,fair_cov,cons_cov,vcsw,ivcsw,retries_per_op,"
                        "secondary_per_op,calib_ns,peak_rss_mb,unix_time\n");
    std::fprintf(
        f,
        "%s,%s,%d,%d,%d,%zu,%s,%.17g,%.17g,%d,%" PRIu64 ",%.4f,%.3f,%.1f,%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%.4f,%.4f,%ld,%ld,%.4f,%.4f,%" PRIu64 ",%.1f,%ld\n",
        cfg.queue.c_str(), cfg.mode.c_str(), cfg.producers, cfg.consumers, cfg.oversubscribe,
        cfg.capacity, cfg.qos.c_str(), cfg.seconds, cfg.rate, cfg.trial, r.ops, r.elapsed,
        r.ops / r.elapsed / 1e6, r.mean_ns, r.p50, r.p99, r.p999, r.mx, r.fair_cov, r.cons_cov,
        r.vcsw, r.ivcsw, r.retries_per_op, r.secondary_per_op, r.calib_ns, r.peak_rss_mb,
        static_cast<long>(std::time(nullptr)));
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--queue"))
            cfg.queue = next("--queue");
        else if (!std::strcmp(argv[i], "--producers"))
            cfg.producers = parse_int(next("--producers"), "--producers");
        else if (!std::strcmp(argv[i], "--consumers"))
            cfg.consumers = parse_int(next("--consumers"), "--consumers");
        else if (!std::strcmp(argv[i], "--capacity"))
            cfg.capacity = parse_size(next("--capacity"), "--capacity");
        else if (!std::strcmp(argv[i], "--mode"))
            cfg.mode = next("--mode");
        else if (!std::strcmp(argv[i], "--seconds"))
            cfg.seconds = parse_double(next("--seconds"), "--seconds");
        else if (!std::strcmp(argv[i], "--rate"))
            cfg.rate = parse_double(next("--rate"), "--rate");
        else if (!std::strcmp(argv[i], "--qos"))
            cfg.qos = next("--qos");
        else if (!std::strcmp(argv[i], "--oversubscribe"))
            cfg.oversubscribe = parse_int(next("--oversubscribe"), "--oversubscribe");
        else if (!std::strcmp(argv[i], "--trial"))
            cfg.trial = parse_int(next("--trial"), "--trial", true);
        else if (!std::strcmp(argv[i], "--csv"))
            cfg.csv = next("--csv");
        else {
            std::fprintf(stderr, "unknown flag %s\n", argv[i]);
            return 2;
        }
    }

    if (!supported_queue(cfg.queue)) {
        std::fprintf(stderr, "unknown queue '%s'\n", cfg.queue.c_str());
        return 2;
    }
    if (cfg.mode != "throughput" && cfg.mode != "latency") {
        std::fprintf(stderr, "unknown mode '%s' (expected throughput or latency)\n",
                     cfg.mode.c_str());
        return 2;
    }
    if (!supported_qos(cfg.qos)) {
        std::fprintf(stderr, "unknown qos '%s'\n", cfg.qos.c_str());
        return 2;
    }

    const int actualProducers =
        checked_thread_total(cfg.producers, cfg.oversubscribe, "producer");
    const int actualConsumers =
        checked_thread_total(cfg.consumers, cfg.oversubscribe, "consumer");
    if (cfg.queue == "spsc" && (actualProducers != 1 || actualConsumers != 1)) {
        std::fprintf(stderr, "spsc requires exactly 1 producer and 1 consumer\n");
        return 2;
    }
    const bool isMs = cfg.queue == "ms" || cfg.queue == "ms-fix" || cfg.queue == "ms-retry";
    const std::size_t actualWorkers = static_cast<std::size_t>(actualProducers) +
                                      static_cast<std::size_t>(actualConsumers);
    if (isMs && actualWorkers > mpmc::ebr::kMaxThreads) {
        std::fprintf(stderr, "MS queues support at most %zu worker threads (requested %zu)\n",
                     mpmc::ebr::kMaxThreads, actualWorkers);
        return 2;
    }
    if (cfg.mode == "latency") {
        const double perProducerRate = cfg.rate / static_cast<double>(actualProducers);
        const double periodNs = 1e9 / perProducerRate;
        const double sends = cfg.seconds * perProducerRate;
        const double totalSends = sends * static_cast<double>(actualProducers);
        const double u64Limit =
            std::ldexp(1.0, std::numeric_limits<std::uint64_t>::digits);
        if (!std::isfinite(periodNs) || periodNs >= u64Limit || periodNs < 1.0 ||
            !std::isfinite(sends) || sends >= u64Limit || sends < 1.0 ||
            !std::isfinite(totalSends) || totalSends >= u64Limit) {
            std::fprintf(stderr,
                         "latency rate/seconds produce an unsupported schedule "
                         "(period %.3g ns, sends/producer %.3g)\n",
                         periodNs, sends);
            return 2;
        }
    }

    const std::uint64_t calib = spin_calibration();
    rusage ru0{};
    getrusage(RUSAGE_SELF, &ru0);
    Result r;
    if (cfg.queue == "ms") {
        r = dispatch_mode<MSAdapter>(cfg);
    } else if (cfg.queue == "ms-fix") {  // F8: prefix-reclaim fix
        mpmc::ebr::set_reclaim_mode(1);
        r = dispatch_mode<MSAdapter>(cfg);
    } else if (cfg.queue == "ms-retry") {  // F8: heavy variant (negative result)
        mpmc::ebr::set_reclaim_mode(2);
        r = dispatch_mode<MSAdapter>(cfg);
    } else if (cfg.queue == "vyukov")
        r = dispatch_mode<VyukovAdapter>(cfg);  // NOLINT
    else if (cfg.queue == "vyukov-b")
        r = dispatch_mode<VyukovBackoffAdapter>(cfg);
    else if (cfg.queue == "faa")
        r = dispatch_mode<FAAAdapter>(cfg);
    else if (cfg.queue == "casticket")
        r = dispatch_mode<CasTicketAdapter>(cfg);
    else if (cfg.queue == "mutex")
        r = dispatch_mode<MutexAdapter>(cfg);
    else if (cfg.queue == "spsc")
        r = dispatch_mode<SPSCAdapter>(cfg);
#ifdef SPSC_HAVE_MOODYCAMEL
    else if (cfg.queue == "moody")
        r = dispatch_mode<MoodyAdapter>(cfg);
#endif
    else
        std::abort();  // validated above; keeps dispatch exhaustive
    r.calib_ns = calib;
    r.peak_rss_mb = peak_rss_mb();
    rusage ru1{};
    getrusage(RUSAGE_SELF, &ru1);
    r.vcsw = ru1.ru_nvcsw - ru0.ru_nvcsw;     // voluntary (yields that slept)
    r.ivcsw = ru1.ru_nivcsw - ru0.ru_nivcsw;  // involuntary (preemptions)

    if (cfg.queue.rfind("ms", 0) == 0) mpmc::ebr::flush_all_unsafe();

    std::printf("%s %s P=%d C=%d x%d cap=%zu qos=%s: %.2f Mops/s (%" PRIu64 " ops / %.2fs)",
                cfg.queue.c_str(), cfg.mode.c_str(), cfg.producers, cfg.consumers,
                cfg.oversubscribe, cfg.capacity, cfg.qos.c_str(), r.ops / r.elapsed / 1e6,
                r.ops, r.elapsed);
    if (cfg.mode == "latency")
        std::printf("  mean=%.0fns p50=%" PRIu64 " p99=%" PRIu64 " p99.9=%" PRIu64
                    " max=%" PRIu64,
                    r.mean_ns, r.p50, r.p99, r.p999, r.mx);
    std::printf("  calib=%.1fms\n", r.calib_ns / 1e6);
    write_csv(cfg, r);
    return 0;
}
