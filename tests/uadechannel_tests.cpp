// Covers the uadechannel.c semantics the playback smoke cannot reach: EOF on
// close for blocked readers and writers, the no-progress timeout, idempotent
// close, and slot recycling. Built with a short UADE_CHANNEL_TIMEOUT_MS.

#include <uade/uadechannel.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static const int TIMEOUT_MS = UADE_CHANNEL_TIMEOUT_MS;

static long long elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

static void roundtrip_both_directions() {
    int ids[2];
    char buf[8];

    CHECK(uade_ipc_channel_create(ids) == 0);
    CHECK(uade_ipc_write(ids[0], "hello", 5) == 5);
    CHECK(uade_ipc_read(ids[1], buf, 5) == 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK(uade_ipc_write(ids[1], "back", 4) == 4);
    CHECK(uade_ipc_read(ids[0], buf, 4) == 4);
    CHECK(memcmp(buf, "back", 4) == 0);
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);
}

// Far more than the ring capacity, so the writer blocks and the ring wraps.
static void transfer_larger_than_capacity() {
    const size_t total = 1u << 20;
    int ids[2];
    std::vector<unsigned char> out(total), in(total, 0);

    for (size_t i = 0; i < total; i++)
        out[i] = (unsigned char)(i * 31 + (i >> 8));

    CHECK(uade_ipc_channel_create(ids) == 0);
    std::thread writer([&] { CHECK(uade_ipc_write(ids[0], out.data(), total) == (ssize_t)total); });
    CHECK(uade_ipc_read(ids[1], in.data(), total) == (ssize_t)total);
    writer.join();
    CHECK(in == out);
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);
}

// Closing one end is how teardown unwedges the core: the blocked read must
// report EOF rather than sit until the timeout.
static void close_wakes_blocked_reader() {
    int ids[2];
    char buf[1];
    ssize_t ret = 1;

    CHECK(uade_ipc_channel_create(ids) == 0);
    auto start = std::chrono::steady_clock::now();
    std::thread reader([&] { ret = uade_ipc_read(ids[1], buf, 1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uade_ipc_close(ids[0]);
    reader.join();
    CHECK(ret == 0);
    CHECK(elapsed_ms(start) < TIMEOUT_MS);
    uade_ipc_close(ids[1]);
}

// Same for a writer blocked on a full ring with nobody draining it.
static void close_wakes_blocked_writer() {
    const size_t total = 1u << 20;
    int ids[2];
    std::vector<unsigned char> out(total, 0x5a);
    ssize_t ret = 0;

    CHECK(uade_ipc_channel_create(ids) == 0);
    auto start = std::chrono::steady_clock::now();
    std::thread writer([&] { ret = uade_ipc_write(ids[0], out.data(), total); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uade_ipc_close(ids[1]);
    writer.join();
    CHECK(ret == -1);
    CHECK(elapsed_ms(start) < TIMEOUT_MS);
    uade_ipc_close(ids[0]);
}

// Same for a writer nobody is draining: the ring fills, then the deadline hits.
static void write_times_out_on_a_full_ring() {
    const size_t total = 1u << 20; // larger than the ring, so it must block
    int ids[2];
    std::vector<unsigned char> out(total, 0x17);

    CHECK(uade_ipc_channel_create(ids) == 0);
    auto start = std::chrono::steady_clock::now();
    CHECK(uade_ipc_write(ids[0], out.data(), total) == -1);
    long long waited = elapsed_ms(start);
    CHECK(waited >= TIMEOUT_MS);
    CHECK(waited < TIMEOUT_MS * 10);
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);
}

// 777 does not divide the ring capacity, so head and tail sweep past the
// wrap boundary at a different offset every pass, exercising the split copy.
static void ring_wraps_at_uneven_boundaries() {
    const size_t chunk = 777;
    int ids[2];
    std::vector<unsigned char> out(chunk), in(chunk);

    CHECK(uade_ipc_channel_create(ids) == 0);
    for (int round = 0; round < 300; round++) {
        for (size_t i = 0; i < chunk; i++)
            out[i] = (unsigned char)(round * 7 + i);
        if (uade_ipc_write(ids[0], out.data(), chunk) != (ssize_t)chunk ||
            uade_ipc_read(ids[1], in.data(), chunk) != (ssize_t)chunk || in != out) {
            fprintf(stderr, "FAIL: ring wraparound corrupted data in round %d\n", round);
            failures++;
            break;
        }
    }
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);
}

// Teardown can land mid-transfer. Both sides must unblock rather than hang,
// and nothing may touch the channel after it is released.
static void close_racing_an_in_flight_transfer() {
    const size_t total = 4u << 20;
    int ids[2];
    std::vector<unsigned char> out(total, 0x3c), in(total, 0);
    ssize_t wrote = 1, got = 1;

    CHECK(uade_ipc_channel_create(ids) == 0);
    auto start = std::chrono::steady_clock::now();
    std::thread writer([&] { wrote = uade_ipc_write(ids[0], out.data(), total); });
    std::thread reader([&] { got = uade_ipc_read(ids[1], in.data(), total); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uade_ipc_close(ids[1]);
    writer.join();
    reader.join();
    CHECK(wrote == -1 || wrote == (ssize_t)total);
    CHECK(got == -1 || got == 0 || got == (ssize_t)total);
    CHECK(elapsed_ms(start) < TIMEOUT_MS);
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);
}

// Exhausting the table must fail cleanly and recover once a slot is returned.
static void table_exhaustion_fails_then_recovers() {
    std::vector<std::pair<int, int>> open;
    int ids[2];

    while (open.size() < 64 && uade_ipc_channel_create(ids) == 0)
        open.push_back({ids[0], ids[1]});

    CHECK(!open.empty());          // at least one channel was available
    CHECK(open.size() < 64);       // and creation eventually refused

    uade_ipc_close(open.back().first);
    uade_ipc_close(open.back().second);
    open.pop_back();

    CHECK(uade_ipc_channel_create(ids) == 0); // the freed slot is reusable
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);

    for (auto& p : open) {
        uade_ipc_close(p.first);
        uade_ipc_close(p.second);
    }
}

// A wedged peer must surface as an error, not an indefinite block.
static void read_times_out_on_a_wedged_peer() {
    int ids[2];
    char buf[1];

    CHECK(uade_ipc_channel_create(ids) == 0);
    auto start = std::chrono::steady_clock::now();
    CHECK(uade_ipc_read(ids[1], buf, 1) == -1);
    long long waited = elapsed_ms(start);
    CHECK(waited >= TIMEOUT_MS);
    CHECK(waited < TIMEOUT_MS * 10);
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);
}

// Both halves close in_fd and out_fd, which are the same id.
static void close_is_idempotent() {
    int ids[2];
    char buf[1];

    CHECK(uade_ipc_channel_create(ids) == 0);
    CHECK(uade_ipc_close(ids[0]) == 0);
    CHECK(uade_ipc_close(ids[0]) == 0);
    CHECK(uade_ipc_close(ids[1]) == 0);
    CHECK(uade_ipc_close(ids[1]) == 0);
    CHECK(uade_ipc_read(ids[0], buf, 1) == -1);
    CHECK(uade_ipc_write(ids[1], "x", 1) == -1);
}

// A released slot gets reused; ids from the previous occupant must not alias it.
static void recycled_slots_reject_stale_ids() {
    int old_ids[2];
    int ids[2];
    char buf[4];

    CHECK(uade_ipc_channel_create(old_ids) == 0);
    uade_ipc_close(old_ids[0]);
    uade_ipc_close(old_ids[1]);

    CHECK(uade_ipc_channel_create(ids) == 0);
    CHECK(ids[0] != old_ids[0]);
    uade_ipc_close(old_ids[0]);
    uade_ipc_close(old_ids[1]);
    CHECK(uade_ipc_write(ids[0], "ok", 2) == 2);
    CHECK(uade_ipc_read(ids[1], buf, 2) == 2);
    uade_ipc_close(ids[0]);
    uade_ipc_close(ids[1]);
}

// The plugin creates and destroys a state per song; slots must not leak.
static void repeated_create_close_does_not_exhaust_slots() {
    for (int i = 0; i < 1000; i++) {
        int ids[2];
        if (uade_ipc_channel_create(ids) != 0) {
            fprintf(stderr, "FAIL: channel create failed on iteration %d\n", i);
            failures++;
            return;
        }
        uade_ipc_close(ids[0]);
        uade_ipc_close(ids[1]);
    }
}

int main() {
    roundtrip_both_directions();
    transfer_larger_than_capacity();
    close_wakes_blocked_reader();
    close_wakes_blocked_writer();
    write_times_out_on_a_full_ring();
    ring_wraps_at_uneven_boundaries();
    close_racing_an_in_flight_transfer();
    table_exhaustion_fails_then_recovers();
    read_times_out_on_a_wedged_peer();
    close_is_idempotent();
    recycled_slots_reject_stale_ids();
    repeated_create_close_does_not_exhaust_slots();

    if (failures) {
        fprintf(stderr, "uadechannel_tests: %d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "uadechannel_tests: all checks passed\n");
    return 0;
}
