#include <algorithm>
#include <array>
#include <ranges>

#include <lighter/lexer/document.h>
#include <lighter/lexer/lexer.h>

namespace {

using namespace lighter;
using namespace lighter::lexer;

struct TestLexer {
    enum struct Style : u8 {
        DEFAULT[[= token_role(TokenRole::DEFAULT)]] = 0,
        WORD[[= token_role(TokenRole::IDENTIFIER)]] = 4,
    };

    static constexpr LanguageInfo language{.id = "test", .name = "Test"};

    void lex(LexContext &lex_context) const {
        std::ranges::fill(lex_context.styles.begin() + static_cast<isize>(lex_context.range.begin),
                          lex_context.styles.begin() + static_cast<isize>(lex_context.range.end), static_cast<u8>(Style::WORD));
    }
};

static_assert(role_for_style(TestLexer::Style::DEFAULT) == TokenRole::DEFAULT);
static_assert(role_for_style(TestLexer::Style::WORD) == TokenRole::IDENTIFIER);
static_assert(role_for_style<TestLexer::Style>(3) == TokenRole::ERROR);

bool test_document() {
    Document document;
    assign(document, "first\nsecond");
    if (document.styles.size() != document.source.size()) return false;
    if (document.line_starts != std::vector<usize>{0, 6}) return false;
    if (line_from_position(document, 0) != 0 || line_from_position(document, 6) != 1) return false;

    std::ranges::fill(document.styles, u8{7});
    const LexRange dirty = append(document, " line\nthird");
    if (dirty.begin != 6 || dirty.end != document.source.size()) return false;
    if (document.line_starts != std::vector<usize>{0, 6, 18}) return false;
    if (!std::ranges::all_of(document.styles | std::views::drop(6), [](u8 style) { return style == 0; })) return false;
    return document.styles[5] == 7 && document.line_states.size() == document.line_starts.size();
}

bool test_facade() {
    Document document;
    assign(document, "word");
    auto lex_context = context(document, {.begin = 0, .end = document.source.size()});

    auto statically_dispatched = reflect_lexer(TestLexer{});
    statically_dispatched.lex(lex_context);
    if (!std::ranges::all_of(document.styles, [](u8 style) { return style == 4; })) return false;
    if (statically_dispatched.role_for_style(4) != TokenRole::IDENTIFIER) return false;

    std::ranges::fill(document.styles, u8{0});
    Lexer dynamically_dispatched = make_lexer(TestLexer{});
    dynamically_dispatched->lex(lex_context);
    return dynamically_dispatched->language_info().id == "test" && dynamically_dispatched->role_for_style(4) == TokenRole::IDENTIFIER &&
           std::ranges::all_of(document.styles, [](u8 style) { return style == 4; });
}

} // namespace

int main() { return test_document() && test_facade() ? 0 : 1; }
