// N-producer x M-consumer stress for the MPMC queues.
//
// Each producer p pushes payloads (p << 48) | seq for seq = 0..perProducer-1.
// Invariants checked (together they catch loss, duplication, and reordering):
//   1. exact count: total pops == total pushes
//   2. no duplicates / no loss: a mark array counts each (p, seq) exactly once
//   3. per-producer FIFO: any single consumer observes each producer's seqs in
//      strictly increasing order (linearizable FIFO implies this projection)
// Run under TSan for the memory-ordering verdict (wired into CI's TSAN job).
#include "mpmc/ms_queue.hpp"
#include "test_util.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

template <class Q>
static bool run_stress(int producers, int consumers, std::uint64_t perProducer,
                       std::size_t capacity) {
    Q q(capacity);
    const std::uint64_t total = static_cast<std::uint64_t>(producers) * perProducer;

    // marks[p * perProducer + seq] counts deliveries of that payload.
    std::unique_ptr<std::atomic<std::uint32_t>[]> marks(
        new std::atomic<std::uint32_t>[static_cast<std::size_t>(total)]());
    std::atomic<std::uint64_t> popped{0};
    std::atomic<bool> dup{false}, disorder{false};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers + consumers));

    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&, producers] {
            std::vector<std::int64_t> lastSeq(static_cast<std::size_t>(producers), -1);
            std::uint64_t v = 0;
            while (popped.load(std::memory_order_relaxed) < total) {
                if (!q.try_pop(v)) {
                    std::this_thread::yield();
                    continue;
                }
                const std::uint64_t p = v >> 48;
                const std::uint64_t seq = v & 0xffffffffffffull;
                if (marks[static_cast<std::size_t>(p * perProducer + seq)].fetch_add(
                        1, std::memory_order_relaxed) != 0)
                    dup.store(true, std::memory_order_relaxed);
                if (static_cast<std::int64_t>(seq) <= lastSeq[static_cast<std::size_t>(p)])
                    disorder.store(true, std::memory_order_relaxed);
                lastSeq[static_cast<std::size_t>(p)] = static_cast<std::int64_t>(seq);
                popped.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            for (std::uint64_t seq = 0; seq < perProducer; ++seq) {
                const std::uint64_t v = (static_cast<std::uint64_t>(p) << 48) | seq;
                while (!q.try_push(v)) std::this_thread::yield();
            }
        });
    }
    for (auto& t : threads) t.join();

    bool allExactlyOnce = true;
    for (std::uint64_t i = 0; i < total; ++i)
        if (marks[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) != 1)
            allExactlyOnce = false;

    return popped.load() == total && allExactlyOnce && !dup.load() && !disorder.load();
}

int main() {
    using MS = mpmc::MSQueue<std::uint64_t>;

    // Balanced, producer-heavy, and consumer-heavy mixes on 8 logical cores.
    CHECK((run_stress<MS>(2, 2, 60'000, 1024)));
    CHECK((run_stress<MS>(4, 4, 30'000, 1024)));
    CHECK((run_stress<MS>(7, 1, 20'000, 1024)));
    CHECK((run_stress<MS>(1, 7, 120'000, 1024)));

    mpmc::ebr::flush_all_unsafe();
    return TEST_SUMMARY();
}
