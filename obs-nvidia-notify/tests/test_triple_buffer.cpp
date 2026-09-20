// SPDX-License-Identifier: MIT
#include <atomic>
#include <thread>

#include "obsn/triple_buffer.hpp"
#include "obsn_test.hpp"

using namespace obsn;

TEST(triple_buffer, consumer_sees_the_last_published_value) {
    TripleBuffer<int> buffer;
    buffer.write_slot() = 7;
    buffer.publish();
    CHECK_EQ(buffer.acquire(), 7);
}

TEST(triple_buffer, acquire_without_an_update_keeps_the_previous_value) {
    TripleBuffer<int> buffer;
    buffer.write_slot() = 1;
    buffer.publish();
    CHECK_EQ(buffer.acquire(), 1);
    CHECK(!buffer.has_update());
    CHECK_EQ(buffer.acquire(), 1);
}

TEST(triple_buffer, a_consumer_that_falls_behind_skips_to_the_newest) {
    TripleBuffer<int> buffer;
    for (int i = 1; i <= 5; ++i) {
        buffer.write_slot() = i;
        buffer.publish();
    }
    CHECK_EQ(buffer.acquire(), 5);
}

TEST(triple_buffer, producer_and_consumer_never_share_a_slot) {
    // The safety property that matters: the consumer must never observe a torn value, however
    // the two threads interleave. A two-field struct with an invariant makes tearing visible.
    struct Pair {
        int a = 0;
        int b = 0;
    };
    TripleBuffer<Pair> buffer;
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> observed{0};

    std::thread producer([&] {
        for (int i = 1; i <= 200000 && !stop.load(); ++i) {
            Pair& slot = buffer.write_slot();
            slot.a = i;
            slot.b = i;  // invariant: a == b in every published value
            buffer.publish();
        }
        stop.store(true);
    });

    while (!stop.load()) {
        const Pair& p = buffer.acquire();
        if (p.a != p.b) torn.fetch_add(1);
        observed.fetch_add(1);
    }
    producer.join();

    CHECK_EQ(torn.load(), 0);
    CHECK(observed.load() > 0);
}
