#include "compute_inactive.hpp"
#include <cstdio>
#include <map>
#include <vector>

ComputeInactiveHelper::ComputeInactiveHelper(std::vector<std::string> const& lines, std::map<int, int> const& cond)
    : pos_(0), cond_(cond)
{
    auto is_kw = [&lines](const int i, std::string_view kw) {
        auto const& s = lines[i];
        if (s.size() < kw.size())
            return false;

        const char* p = s.c_str();
        while (p && isspace(*p)) {
            ++p;
        }

        if (!p)
            return false;

        std::string_view sv = p;
        return sv.substr(0, kw.size()) == kw;
    };

// #define PRINT_TOK(tok) fprintf(stderr, "tok %s at %d\n", tok, i)
#define PRINT_TOK(tok)

    std::vector<_Token> toks;
    for (int i = 0; i < lines.size(); ++i) {
        if (is_kw(i, "#if")) {
            toks.push_back({_Token::TokenKind::IF, i});
			PRINT_TOK("#if");
        } else if (is_kw(i, "#elif")) {
            toks.push_back({_Token::TokenKind::ELIF, i});
			PRINT_TOK("#elif");
        } else if (is_kw(i, "#endif")) {
            toks.push_back({_Token::TokenKind::ENDIF, i});
			PRINT_TOK("#endif");
        } else if (is_kw(i, "#else")) {
            toks.push_back({_Token::TokenKind::ELSE, i});
			PRINT_TOK("#else");
        } else {
            continue;
        }
    }

#undef PRINT_TOK

    inputs_.swap(toks);
}

bool ComputeInactiveHelper::match(ComputeInactiveHelper::_Token::TokenKind tok) { return inputs_[pos_].kind == tok; }
void ComputeInactiveHelper::eat() { ++pos_; }
int ComputeInactiveHelper::line() { return inputs_[pos_].line; }

ComputeInactiveHelper::BlockAST* ComputeInactiveHelper::parse_block()
{
    auto* block = new BlockAST;

    if (!match(_Token::TokenKind::IF)) {
        return nullptr;
    }

    block->lines.if_ = line();

    eat();
    while (auto* p = parse_block()) {
        block->then.push_back(p);
    }

    while (match(_Token::TokenKind::ELIF)) {
        block->lines.elif_.push_back(line());
        eat();
        std::vector<BlockAST*> v;
        while (auto* p = parse_block()) {
            v.push_back(p);
        }
        block->elif.push_back(v);
    }

    if (match(_Token::TokenKind::ELSE)) {
        block->lines.el_ = line();
        eat();
        while (auto* p = parse_block()) {
            block->el.push_back(p);
        }
    }

    if (!match(_Token::TokenKind::ENDIF)) {
        return nullptr;
    }

    block->lines.endif_ = line();
    eat();

    return block;
}

static void print_block(ComputeInactiveHelper::BlockAST* block, int depth)
{
    if (!block)
        return;

    std::string header(" ", 4 * depth);
    fprintf(stderr, "%s#start block at line %d\n", header.c_str(), block->lines.if_);
    for (auto* p : block->then) {
        print_block(p, depth + 1);
    }
    for (auto& v : block->elif) {
        for (auto* p : v) {
            print_block(p, depth + 1);
        }
    }

    for (auto* p : block->el) {
        print_block(p, depth + 1);
    }
    fprintf(stderr, "%s#end block at line %d\n", header.c_str(), block->lines.endif_);
}

std::vector<ComputeInactiveHelper::BlockAST*> ComputeInactiveHelper::parse()
{
    std::vector<BlockAST*> blocks;
    while (pos_ < inputs_.size()) {
        auto* b = parse_block();
        if (!b) {
            break;
        }

        blocks.push_back(b);
        // print_block(b, 0);
    }

    return blocks;
}

std::vector<ComputeInactiveHelper::Range> ComputeInactiveHelper::inactive()
{
    auto blocks = parse();
    std::vector<Range> inactive_blocks;
    for (auto const& block : blocks) {
        compute_inactive(block, inactive_blocks);
    }

    for (auto* p : blocks) {
        delete p;
    }

    return inactive_blocks;
}

void ComputeInactiveHelper::compute_inactive(ComputeInactiveHelper::BlockAST* block,
                                             std::vector<ComputeInactiveHelper::Range>& result)
{
#if 1
    if (!block)
        return;

    auto compute_fn = [this, &result](auto& blocks) {
        for (auto* p : blocks) {
            compute_inactive(p, result);
        }
    };

    auto pos = cond_.find(block->lines.if_ + 2);
    bool cond = false;
    if (pos == cond_.end()) {
        fprintf(stderr, "#if at line %d but cond result not found\n", block->lines.if_);
    } else {
        cond = pos->second;
    }

    if (!cond) {
        if (block->lines.elif_.empty() && block->lines.el_ < 0) { //#if .. #endif
            result.push_back(Range{block->lines.if_, block->lines.endif_});
            return;
        } else if (block->lines.elif_.empty() && block->lines.el_ >= 0) { //#if #else #endif
            result.push_back(Range{block->lines.if_, block->lines.endif_});
            compute_fn(block->el);
        } else if (!block->lines.elif_.empty() && block->lines.el_ < 0) { // #if #elif #elif #endif
            result.push_back({block->lines.if_, block->lines.elif_.front()});
            for (int i = 0; i < block->lines.elif_.size(); ++i) {
                pos = cond_.find(block->lines.elif_[i] + 2);
                cond = pos == cond_.end() ? false : pos->second;
                if (pos == cond_.end()) {
                    fprintf(stderr, "#elif at line %d but cond result not found\n", block->lines.elif_[i]);
                }

                if (cond) {
                    compute_fn(block->elif[i]);
                    continue;
                } else {
                    int end = i == block->lines.elif_.size() - 1 ? block->lines.endif_ : block->lines.elif_[i + 1];
                    result.push_back({block->lines.elif_[i], end});
                }
            }
        } else { //#if #elif #else #endif
            result.push_back({block->lines.if_, block->lines.elif_.front()});
            bool goto_el = true;

            for (int i = 0; i < block->lines.elif_.size(); ++i) {
                pos = cond_.find(block->lines.elif_[i] + 2);
                cond = pos == cond_.end() ? false : pos->second;
                if (pos == cond_.end()) {
                    fprintf(stderr, "#elif at line %d but cond result not found\n", block->lines.elif_[i]);
                }

                if (cond) {
                    compute_fn(block->elif[i]);
                    goto_el = false;
                    continue;
                } else {
                    int end = i == block->lines.elif_.size() - 1 ? block->lines.el_ : block->lines.elif_[i + 1];
                    result.push_back({block->lines.elif_[i], end});
                }
            }

            if (goto_el) {
                compute_fn(block->el);
            } else {
                result.push_back({block->lines.el_, block->lines.endif_});
            }
        }
    } else {
        compute_fn(block->then);
        if (block->lines.elif_.empty() && block->lines.el_ < 0) {         //#if .. #endif
        } else if (block->lines.elif_.empty() && block->lines.el_ >= 0) { //#if #else #endif
            result.push_back({block->lines.el_, block->lines.endif_});
        } else if (!block->lines.elif_.empty() && block->lines.el_ < 0) { // #if #elif #elif #endif
            for (int i = 0; i < block->lines.elif_.size(); ++i) {
                int end = i == block->lines.elif_.size() - 1 ? block->lines.endif_ : block->lines.elif_[i + 1];
                result.push_back({block->lines.elif_[i], end});
            }
        } else { //#if #elif #else #endif
            for (int i = 0; i < block->lines.elif_.size(); ++i) {
                int end = i == block->lines.elif_.size() - 1 ? block->lines.endif_ : block->lines.elif_[i + 1];
                result.push_back({block->lines.elif_[i], end});
            }
            result.push_back({block->lines.el_, block->lines.endif_});
        }
    }
#endif
}
