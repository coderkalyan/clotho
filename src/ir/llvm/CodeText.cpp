// The code of a lowered group in the form two lowerings of it are compared by.

#include "ir/llvm/CodeText.h"

#include <algorithm>
#include <charconv>
#include <format>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace clotho::ir::backend {

namespace {

// Whether `c` may appear in a name LLVM prints without quotes.
bool bare_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '$' || c == '-';
}

// One past the end of the name after the sigil at `s[at]` (`@` or `%`): a bare
// name, or a quoted one whose escapes are a backslash and two hex digits.
size_t name_end(std::string_view s, size_t at) {
    size_t j = at + 1;
    if (j < s.size() && s[j] == '"') {
        for (++j; j < s.size() && s[j] != '"'; ++j) {
            j += s[j] == '\\' ? 2 : 0;
        }
        return std::min(j + 1, s.size());
    }
    while (j < s.size() && bare_name_char(s[j])) {
        ++j;
    }
    return j;
}

// The name a token spells, unquoted and unescaped.
std::string token_name(std::string_view tok) {
    tok.remove_prefix(1);
    if (tok.size() < 2 || tok.front() != '"') {
        return std::string(tok);
    }
    tok = tok.substr(1, tok.size() - 2);
    std::string name;
    for (size_t i = 0; i < tok.size(); ++i) {
        unsigned code = 0;
        if (tok[i] == '\\' && i + 2 < tok.size() &&
            std::from_chars(tok.data() + i + 1, tok.data() + i + 3, code, 16).ec == std::errc{}) {
            name += static_cast<char>(code);
            i += 2;
        } else {
            name += tok[i];
        }
    }
    return name;
}

// A line clipped for a message.
std::string clip(const std::string& line) {
    return line.size() <= 240 ? line : line.substr(0, 240) + " ...";
}

// The part of `s` around its byte `at`.
std::string window(const std::string& s, size_t at) {
    const size_t lo = at > 48 ? at - 48 : 0;
    const size_t hi = std::min(s.size(), at + 72);
    return (lo > 0 ? "..." : "") + s.substr(lo, hi - lo) + (hi < s.size() ? "..." : "");
}

} // namespace

CodeText::CodeText(llvm::Function* fn)
  : fn_(fn)
  , mst_(fn->getParent(), /*ShouldInitializeAllMetadata=*/false) {
    std::string text;
    {
        llvm::raw_string_ostream os(text);
        // Function::print(OS, AAW*) hides Value's slot-tracker overload.
        fn->Value::print(os, mst_);
    }
    own_ = operand_token(fn);
    ankerl::unordered_dense::set<const llvm::Constant*> seen;
    for (const llvm::BasicBlock& bb : *fn) {
        for (const llvm::Instruction& inst : bb) {
            for (const llvm::Use& u : inst.operands()) {
                if (const auto* c = llvm::dyn_cast<llvm::Constant>(u.get())) {
                    gather(c, seen);
                }
            }
        }
    }
    for (size_t at = 0; at < text.size();) {
        const size_t           end = std::min(text.find('\n', at), text.size());
        const std::string_view line(text.data() + at, end - at);
        at = end + 1;
        if (!line.starts_with("; Function Attrs")) {
            raw.emplace_back(line);
            canon.push_back(rewrite(line, 0));
        }
    }
}

std::string CodeText::joined() const {
    std::string out;
    for (const std::string& line : canon) {
        out += line;
        out += '\n';
    }
    return out;
}

std::string CodeText::operand_token(const llvm::Value* v) {
    std::string          tok;
    llvm::raw_string_ostream os(tok);
    v->printAsOperand(os, /*PrintType=*/false, mst_);
    return os.str();
}

// The write-only sinks of a module (a place an out-of-range access is redirected to): internal
// variables, not constants, one per module and type. Which of the module's own sinks a body names
// is no fact of an instance, so they compare by what they hold like the constants do.
static bool is_sink(const llvm::GlobalVariable& gv) {
    const llvm::StringRef name = gv.getName();
    return name.starts_with("clotho.handle.sink") || name.starts_with("clotho.agg.sink") ||
           name.starts_with("clotho.str.sink");
}

// The private constants of the function's own module that its instructions name,
// directly or through other constants.
void CodeText::gather(const llvm::Constant* c, ankerl::unordered_dense::set<const llvm::Constant*>& seen) {
    if (!seen.insert(c).second) {
        return;
    }
    if (const auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(c)) {
        if (gv->getParent() == fn_->getParent() && gv->hasLocalLinkage() && gv->hasInitializer() && (gv->isConstant() || is_sink(*gv))) {
            constants_.emplace(operand_token(gv), gv);
            gather(gv->getInitializer(), seen);
        }
        return;
    }
    if (llvm::isa<llvm::GlobalValue>(c) || llvm::isa<llvm::ConstantData>(c)) {
        return;
    }
    for (const llvm::Use& u : c->operands()) {
        if (const auto* sub = llvm::dyn_cast<llvm::Constant>(u.get())) {
            gather(sub, seen);
        }
    }
}

std::string CodeText::rewrite(std::string_view text, unsigned depth) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '@' || c == '%') {
            const size_t end = name_end(text, i);
            out += token(text.substr(i, end - i), depth);
            i = end;
        } else if (c == '#' && i > 0 && text[i - 1] == ' ' && i + 1 < text.size() && text[i + 1] >= '0' &&
                   text[i + 1] <= '9') {
            out += "#A";
            for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            }
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

std::string CodeText::token(std::string_view tok, unsigned depth) {
    if (tok.size() < 2) {
        return std::string(tok);
    }
    if (tok.front() == '@') {
        if (depth == 0 && tok == own_) {
            return "@FN";
        }
        const auto it = constants_.find(std::string(tok));
        if (it == constants_.end() || depth >= max_depth) {
            // The element kernels a descriptor names are generated per module from the layout the
            // descriptor is keyed by, and LLVM numbers them as they come: the number is no fact of
            // an instance.
            if (tok.starts_with("@clotho.elem.ops.copy.") || tok.starts_with("@clotho.elem.ops.destroy.")) {
                return std::string(tok.substr(0, tok.rfind('.')));
            }
            return std::string(tok);
        }
        const auto [held, fresh] = content_.try_emplace(it->first);
        if (fresh) {
            std::string init;
            {
                llvm::raw_string_ostream os(init);
                it->second->getInitializer()->print(os, mst_);
            }
            held->second = rewrite(init, depth + 1);
        }
        return "@{" + held->second + "}";
    }
    const auto* st = llvm::StructType::getTypeByName(fn_->getContext(), token_name(tok));
    return st != nullptr && depth < max_depth ? type_token(st, depth) : std::string(tok);
}

// `%<name without its numeric components>#<hash of the body>`: LLVM uniques a
// type's name with a counter (`t.state.5`, and `t.state.5.ctx` after it), and a
// width or a count can be one too (`logic.8`), which the body says anyway.
std::string CodeText::type_token(const llvm::StructType* st, unsigned depth) {
    const auto [held, fresh] = types_.try_emplace(st);
    if (fresh) {
        std::string body;
        {
            llvm::raw_string_ostream os(body);
            st->print(os, /*IsForDebug=*/false, /*NoDetails=*/false);
        }
        if (const size_t eq = body.find(" = type "); eq != std::string::npos) {
            body.erase(0, eq + 8);
        }
        const std::string inner = rewrite(body, depth + 1);
        const std::string full = st->getName().str();
        std::string       name;
        for (size_t at = 0; at <= full.size();) {
            const size_t           end = std::min(full.find('.', at), full.size());
            const std::string_view part(full.data() + at, end - at);
            if (part.empty() || part.find_first_not_of("0123456789") != std::string_view::npos) {
                name += name.empty() ? "" : ".";
                name += part;
            }
            at = end + 1;
        }
        held->second = std::format("%{}#{:x}", name, ankerl::unordered_dense::detail::wyhash::hash(inner.data(), inner.size()));
    }
    return held->second;
}

std::string CodeText::difference(const CodeText& shared, const CodeText& own) {
    const size_t n = std::min(shared.canon.size(), own.canon.size());
    size_t       i = 0;
    while (i < n && shared.canon[i] == own.canon[i]) {
        ++i;
    }
    const auto line = [](const std::vector<std::string>& v, size_t k) {
        return k < v.size() ? v[k] : std::string("<end of the function>");
    };
    std::string out = std::format("first difference at line {} of the group's code:\n", i + 1);
    for (size_t k = i >= 2 ? i - 2 : 0; k < i; ++k) {
        out += std::format("      {}\n", clip(shared.raw[k]));
    }
    out += std::format("    shared: {}\n    own:    {}\n", clip(line(shared.raw, i)), clip(line(own.raw, i)));
    const std::string s = line(shared.canon, i);
    const std::string o = line(own.canon, i);
    if (s != line(shared.raw, i) || o != line(own.raw, i)) {
        size_t at = 0;
        while (at < s.size() && at < o.size() && s[at] == o[at]) {
            ++at;
        }
        out += std::format("  where they differ, once the function's name, its private constants and its types "
                           "stand for what they are:\n    shared: {}\n    own:    {}\n",
                           window(s, at), window(o, at));
    }
    return out;
}

} // namespace clotho::ir::backend
