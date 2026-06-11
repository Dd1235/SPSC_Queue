// Head-to-head against the reference SPSC implementations:
//   - rigtorp::SPSCQueue   -- the canonical cached-index implementation
//   - moodycamel::ReaderWriterQueue -- the popular header-only SPSC
// Same generic driver, same workload, same capacity. Thin adapters map each
// library onto the try_push/try_pop interface the driver expects.
//
// Fairness notes printed with the results:
//   - rigtorp has no try_pop; its adapter is front() + move + pop(), which adds
//     one extra move per element versus its native front()/pop() usage.
//   - moodycamel rounds capacity up internally (block-based); we only ever use
//     try_enqueue, which never allocates, so it stays bounded like the others.
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <readerwriterqueue.h>
#include <rigtorp/SPSCQueue.h>

#include <cstdint>
#include <utility>

template <class T> struct RigtorpAdapter {
    rigtorp::SPSCQueue<T> q;
    explicit RigtorpAdapter(std::size_t cap) : q(cap) {}
    bool try_push(const T& v) { return q.try_push(v); }
    bool try_pop(T& out) {
        T* p = q.front();
        if (!p) return false;
        out = std::move(*p);
        q.pop();
        return true;
    }
};

template <class T> struct MoodycamelAdapter {
    moodycamel::ReaderWriterQueue<T> q;
    explicit MoodycamelAdapter(std::size_t cap) : q(cap) {}
    bool try_push(const T& v) { return q.try_enqueue(v); }
    bool try_pop(T& out) { return q.try_dequeue(out); }
};

int main() {
    constexpr std::uint64_t kCount = 20'000'000;
    constexpr std::size_t kCapacity = 1024;

    spsc::SPSCQueue<std::uint64_t> ours(kCapacity);
    RigtorpAdapter<std::uint64_t> rigtorp_q(kCapacity);
    MoodycamelAdapter<std::uint64_t> moody_q(kCapacity);

    double o = bench::throughput_mops(ours, kCount);
    double r = bench::throughput_mops(rigtorp_q, kCount);
    double m = bench::throughput_mops(moody_q, kCount);

    std::printf("capacity=%zu, %llu ops, best of 3\n", kCapacity, (unsigned long long)kCount);
    std::printf("this queue                 : %7.1f Mops/s   %+5.1f%% vs rigtorp\n", o,
                100.0 * (o - r) / r);
    std::printf("rigtorp::SPSCQueue         : %7.1f Mops/s\n", r);
    std::printf("moodycamel::ReaderWriter   : %7.1f Mops/s\n", m);
    std::printf("\nnotes: rigtorp adapter adds a move per pop (no native try_pop);\n");
    std::printf("moodycamel rounds capacity up internally (try_enqueue never allocates).\n");
    return 0;
}
