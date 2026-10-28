#include "SourceContext.h"

#include <slang/ast/Compilation.h>
#include <slang/driver/Driver.h>
#include <slang/text/SourceManager.h>

#include "SourceMap.h"

namespace clotho {

SourceContext::SourceContext()                                    = default;
SourceContext::~SourceContext()                                   = default;
SourceContext::SourceContext(SourceContext&&) noexcept            = default;
SourceContext& SourceContext::operator=(SourceContext&&) noexcept = default;

void SourceContext::adopt_source_manager(std::unique_ptr<slang::SourceManager> sm) {
    owned_sm_ = std::move(sm);
    sm_       = owned_sm_.get();
}

void SourceContext::adopt_driver(std::unique_ptr<slang::driver::Driver> driver) {
    driver_ = std::move(driver);
    if (driver_ != nullptr) {
        sm_ = &driver_->sourceManager;
    }
}

void SourceContext::adopt_compilation(std::unique_ptr<slang::ast::Compilation> comp) {
    comp_ = std::move(comp);
}

void SourceContext::borrow_source_manager(const slang::SourceManager* sm) {
    sm_ = sm;
}

bool SourceContext::empty() const {
    return sm_ == nullptr;
}

void SourceContext::set_path_prefix_map(PathPrefixMap map) {
    path_prefix_map_ = std::move(map);
}

std::string SourceContext::shown_file(const slang::SourceLocation& loc) const {
    std::string name(sm_->getFileName(loc));
    // A `line directive's name is the design's own claim; only the buffer's file
    // is mapped.
    if (path_prefix_map_.empty() || name != sm_->getRawFileName(loc.buffer())) {
        return name;
    }
    const std::string                full = sm_->getFullPath(loc.buffer()).string();
    const PathPrefixMap::value_type* best = nullptr;
    for (const auto& entry : path_prefix_map_) {
        const std::string& dir = entry.first;
        const bool         under =
          full.starts_with(dir) && (full.size() == dir.size() || full[dir.size()] == '/' || full[dir.size()] == '\\');
        if (under && (best == nullptr || dir.size() > best->first.size())) {
            best = &entry;
        }
    }
    if (best == nullptr) {
        return name;
    }
    std::string rest = full.substr(best->first.size());
    // An empty replacement leaves the path relative to the mapped directory.
    if (best->second.empty() && !rest.empty()) {
        rest.erase(0, 1);
    }
    return best->second + rest;
}

SourcePosition SourceContext::resolve(ir::SourceLoc loc) const {
    SourcePosition pos;
    if (sm_ == nullptr || loc == ir::SourceLoc{ 0 }) {
        return pos;
    }

    const slang::SourceLocation sl = to_slang_location(loc);
    if (!sl.valid()) {
        return pos;
    }
    // getFileName / getLineNumber / getColumnNumber all assume the location names
    // a buffer this manager knows. A handle minted by a different manager (a cache
    // hit from a previous run, say) would be a silent wrong answer, so check
    // membership first rather than trusting the caller.
    if (!sm_->isFileLoc(sl)) {
        return pos;
    }

    const std::string name = shown_file(sl);
    if (name.empty()) {
        return pos;
    }

    pos.file   = std::string(name);
    pos.line   = static_cast<uint32_t>(sm_->getLineNumber(sl));
    pos.column = static_cast<uint32_t>(sm_->getColumnNumber(sl));
    pos.valid  = true;

    return pos;
}

std::string SourceContext::render(ir::SourceLoc loc) const {
    const SourcePosition pos = resolve(loc);
    if (!pos.valid) {
        return "<unknown>";
    }

    return pos.file + ":" + std::to_string(pos.line) + ":" + std::to_string(pos.column);
}

std::string SourceContext::full_path(ir::SourceLoc loc) const {
    if (sm_ == nullptr || loc == ir::SourceLoc{ 0 }) {
        return {};
    }

    const slang::SourceLocation sl = to_slang_location(loc);
    if (!sl.valid() || !sm_->isFileLoc(sl)) {
        return {};
    }

    return sm_->getFullPath(sl.buffer()).string();
}

std::optional<uint32_t> SourceContext::offset(ir::SourceLoc loc) const {
    if (sm_ == nullptr || loc == ir::SourceLoc{ 0 }) {
        return std::nullopt;
    }

    const slang::SourceLocation sl = to_slang_location(loc);
    if (!sl.valid() || !sm_->isFileLoc(sl)) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(sl.offset());
}

const slang::ast::Compilation* SourceContext::compilation() const {
    return comp_.get();
}

std::string SourceContext::line_text(ir::SourceLoc loc) const {
    if (sm_ == nullptr || loc == ir::SourceLoc{ 0 }) {
        return {};
    }

    const slang::SourceLocation sl = to_slang_location(loc);
    // Same membership guard as resolve: a handle from a different manager would
    // index a buffer this one does not own.
    if (!sl.valid() || !sm_->isFileLoc(sl)) {
        return {};
    }

    // slang already trims the trailing newline. The view points into the
    // manager's buffer, so it is copied out rather than handed back.
    return std::string(sm_->getSourceLine(sl));
}

} // namespace clotho
