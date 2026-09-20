// SPDX-License-Identifier: MIT
#include "obsn/framing.hpp"
#include "obsn_test.hpp"

using namespace obsn;

TEST(framing, complete_lines_come_out_whole) {
    LineFramer framer;
    std::vector<std::string> lines;
    CHECK(framer.feed("one\ntwo\nthree\n", lines));
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK_EQ(lines[0], std::string("one"));
    CHECK_EQ(lines[2], std::string("three"));
}

TEST(framing, a_partial_line_is_held_until_it_completes) {
    LineFramer framer;
    std::vector<std::string> lines;
    CHECK(framer.feed("par", lines));
    CHECK(lines.empty());
    CHECK(framer.feed("tial", lines));
    CHECK(lines.empty());
    CHECK(framer.feed("\n", lines));
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], std::string("partial"));
}

TEST(framing, an_over_long_line_is_refused_during_accumulation) {
    // The point of the cap is that a peer sending 10 MB with no newline cannot grow the buffer
    // to 10 MB before we notice.
    LineFramer framer(64);
    std::vector<std::string> lines;
    CHECK(!framer.feed(std::string(200, 'x'), lines));
    CHECK(framer.overflowed());
    CHECK(framer.pending_bytes() <= 64);

    // It stays failed until reset, rather than resynchronising on the next newline.
    CHECK(!framer.feed("short\n", lines));
    framer.reset();
    CHECK(framer.feed("short\n", lines));
    CHECK_EQ(lines.size(), std::size_t{1});
}

TEST(framing, lines_already_completed_survive_a_later_overflow) {
    LineFramer framer(32);
    std::vector<std::string> lines;
    CHECK(!framer.feed("good\n" + std::string(100, 'x'), lines));
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], std::string("good"));
}
