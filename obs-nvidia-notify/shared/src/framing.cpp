// SPDX-License-Identifier: MIT
#include "obsn/framing.hpp"

namespace obsn {

bool LineFramer::feed(std::string_view bytes, std::vector<std::string>& out) {
    if (overflowed_) return false;

    while (!bytes.empty()) {
        const std::size_t nl = bytes.find('\n');
        if (nl == std::string_view::npos) {
            if (pending_.size() + bytes.size() > max_) {
                pending_.clear();
                overflowed_ = true;
                return false;
            }
            pending_.append(bytes);
            return true;
        }

        if (pending_.size() + nl > max_) {
            pending_.clear();
            overflowed_ = true;
            return false;
        }

        pending_.append(bytes.substr(0, nl));
        // Tolerate CRLF: a line written by a text-mode peer must not fail to parse over one byte.
        if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
        // Empty lines are keep-alive padding, not messages; drop them silently.
        if (!pending_.empty()) out.push_back(std::move(pending_));
        pending_.clear();
        bytes.remove_prefix(nl + 1);
    }
    return true;
}

}  // namespace obsn
