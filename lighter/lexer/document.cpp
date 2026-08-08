#include "document.h"

#include <algorithm>

namespace lighter::lexer {

namespace {

void append_line_metadata(Document &document, usize begin) {
    for (usize position = begin; position < document.source.size(); ++position) {
        if (document.source[position] == '\n') {
            document.line_starts.push_back(position + 1);
            document.line_states.push_back(0);
        }
    }
}

} // namespace

void assign(Document &document, std::string_view source) {
    document.source.assign(source);
    document.styles.assign(source.size(), 0);
    document.line_starts.assign(1, 0);
    document.line_states.assign(1, 0);
    append_line_metadata(document, 0);
}

LexRange append(Document &document, std::string_view source) {
    if (source.empty()) {
        return {.begin = document.source.size(), .end = document.source.size()};
    }

    const usize old_size = document.source.size();
    const usize dirty_begin = document.line_starts.back();
    document.source.append(source);
    document.styles.resize(document.source.size());
    std::ranges::fill(document.styles.begin() + static_cast<isize>(dirty_begin), document.styles.end(), u8{0});
    append_line_metadata(document, old_size);
    return {.begin = dirty_begin, .end = document.source.size()};
}

usize line_from_position(const Document &document, usize position) {
    const auto next = std::ranges::upper_bound(document.line_starts, position);
    return static_cast<usize>(next - document.line_starts.begin() - 1);
}

LexContext context(Document &document, LexRange range) {
    contract_assert(document.source.size() == document.styles.size());
    contract_assert(document.line_starts.size() == document.line_states.size());
    contract_assert(!document.line_starts.empty() && document.line_starts.front() == 0);
    return {
        .source = document.source,
        .styles = document.styles,
        .line_starts = document.line_starts,
        .line_states = document.line_states,
        .range = range,
    };
}

} // namespace lighter::lexer
