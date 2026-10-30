// The code of a lowered group in the form two lowerings of it are compared by
// (Backend::verify_shared_group).

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/ModuleSlotTracker.h>

namespace clotho::ir::backend {

// The text of a function as LLVM prints it, line by line, and the same lines
// with what legitimately differs between two lowerings of one group replaced by
// what it stands for, so equal code compares equal:
//   - the function's own name (it carries the instance's);
//   - the private constants the function names (strings, descriptors), whose
//     names and slot numbers count the constants their module holds, by their
//     contents;
//   - named struct types, whose names count the types created before them
//     (agg_storage_ty makes a new `%clotho.agg` at each use), by their bodies;
//   - the numbering of attribute groups, and the comment that restates them.
// Everything else the function names -- a global another module defines, a
// constant operand, the order and the types of the instructions -- stays as it
// is, so a function that names one instance's symbol or bakes one instance's
// number differs from the same function lowered for another. The two forms
// correspond line to line.
class CodeText {
public:
    std::vector<std::string> raw;   // as LLVM prints it
    std::vector<std::string> canon; // as compared

    explicit CodeText(llvm::Function* fn);

    // `canon`, one line after the other: equal text, equal code.
    std::string joined() const;

    // Where `own` first differs from `shared`, for a message: the line in each
    // as LLVM prints it, after the lines before it, and where the compared forms
    // differ (a constant's contents, say, that the printed names do not show).
    static std::string difference(const CodeText& shared, const CodeText& own);

private:
    static constexpr unsigned max_depth = 8;

    llvm::Function*                                    fn_;
    llvm::ModuleSlotTracker                            mst_;
    std::string                                        own_;       // the function's name as the text spells it
    std::map<std::string, const llvm::GlobalVariable*> constants_; // by the name the text spells
    std::map<std::string, std::string>                 content_;   // constants_ by what they hold
    std::map<const llvm::StructType*, std::string>     types_;

    std::string operand_token(const llvm::Value* v);
    void        gather(const llvm::Constant* c, ankerl::unordered_dense::set<const llvm::Constant*>& seen);
    std::string rewrite(std::string_view text, unsigned depth);
    std::string token(std::string_view tok, unsigned depth);
    std::string type_token(const llvm::StructType* st, unsigned depth);
};

} // namespace clotho::ir::backend
