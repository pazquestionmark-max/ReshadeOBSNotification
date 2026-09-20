// SPDX-License-Identifier: MIT
// Newline-delimited framing with a hard size cap enforced *during* accumulation.
//
// The cap matters: a peer that sends 10 MB without a newline must not be able to grow our
// buffer to 10 MB before we notice. LineFramer stops accumulating the moment the pending line
// exceeds the limit and reports an overflow, and the connection owner then drops the peer.
#ifndef OBSN_FRAMING_HPP
#define OBSN_FRAMING_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace obsn {

class LineFramer {
public:
    explicit LineFramer(std::size_t max_line_bytes = 65536) : max_(max_line_bytes) {}

    /// Feeds raw bytes. Complete lines are appended to `out` (without the terminator).
    /// Returns false if the pending line overflowed; the framer then enters a failed state and
    /// discards input until reset(). Bytes already turned into complete lines are still in `out`.
    bool feed(std::string_view bytes, std::vector<std::string>& out);

    void reset() noexcept {
        pending_.clear();
        overflowed_ = false;
    }

    bool overflowed() const noexcept { return overflowed_; }
    std::size_t pending_bytes() const noexcept { return pending_.size(); }
    std::size_t max_line_bytes() const noexcept { return max_; }

private:
    std::size_t max_;
    std::string pending_;
    bool overflowed_ = false;
};

}  // namespace obsn

#endif  // OBSN_FRAMING_HPP
