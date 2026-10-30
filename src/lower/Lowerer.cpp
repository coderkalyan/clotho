// Shared Lowerer utilities: diagnostics, type predicates, coercion and constants.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Record a non-fatal finding. Shares the channel `recover` fills, so a
// consumer sees analysis findings and recovered failures in walk order,
// interleaved as encountered rather than sorted into two groups.
//
// Severity is fixed at the call site for now. The three severity policies of
// design §3.4 (simulation / synthesis / lint) are the mechanism that will
// re-rank these per consumer, and `DiagnosticOrigin::Analysis` is the field
// they will key on -- a mismatch risk should be an error in a synthesis run
// and a warning in a linter, without touching Clotho's own hard limits.
void Lowerer::report(Severity sev, std::string code, std::string msg, ir::SourceSpan span) {
    diags_.push_back(Diagnostic{ .severity = sev,
                                 .origin   = DiagnosticOrigin::Analysis,
                                 .code     = std::move(code),
                                 .message  = std::move(msg),
                                 .span     = span });
}

void Lowerer::fail(std::string msg) {
    throw LowerError{ std::move(msg), to_source_span(cur_range_) };
}

ir::SourceLoc Lowerer::loc(const Expression& e) const {
    return to_source_loc(e.sourceRange.start());
}

ir::SourceLoc Lowerer::loc(const Statement& s) const {
    return to_source_loc(s.sourceRange.start());
}

// Lower an index expression, recording its full source extent alongside the
// usual point. An index is the one operand a diagnostic wants to UNDERLINE
// rather than point at -- `--sanitize=bounds` highlights the whole index the
// user wrote -- and the extent is sparse in the IR, so only these get one.
ir::Index Lowerer::lower_index(const Expression& e, ir::Builder& b) {
    const ir::Index i = as_signed_number(lower_expr(e, b), *e.type, b, loc(e));
    b.set_end(i, to_source_loc(e.sourceRange.end()));
    return i;
}

ir::Index Lowerer::as_signed_number(ir::Index v, const slang::ast::Type& t, ir::Builder& b, ir::SourceLoc l) {
    if (t.isSigned()) {
        return v;
    }
    const uint32_t width = std::max(64u, static_cast<uint32_t>(t.getBitWidth()) + 1);

    return b.convert(v, make_int_type(width, t.isFourState()), false, l);
}

// Full source extent of a symbol's declaring syntax, or a degenerate span at
// its name location when it has none (a compiler-synthesized symbol). This is
// what gives a process or a subroutine a real extent rather than a point.
ir::SourceSpan Lowerer::span_of(const Symbol& sym) {
    if (const slang::syntax::SyntaxNode* syn = sym.getSyntax(); syn != nullptr) {
        return to_source_span(syn->sourceRange());
    }
    const ir::SourceLoc l = to_source_loc(sym.location);

    return ir::SourceSpan{ l, l };
}

// The compile-time banner prefix "<file>:<line>: <task>" for a $finish /
// $stop (LRM §20.2). The location is resolved here (the backend is
// slang-free); the runtime appends " called at <time>".
std::string Lowerer::finish_banner(const Expression& call, std::string_view task) const {
    const slang::SourceManager* sm = scope_->getCompilation().getSourceManager();
    const slang::SourceLocation sl = call.sourceRange.start();
    return sources_.shown_file(sl) + ":" + std::to_string(sm->getLineNumber(sl)) + ": " + std::string(task);
}

// Compile-time "<file>:<line>: " banner prefix at a source location. The
// runtime appends the descriptive message and the simulation time (e.g.
// rt_finish, the Observed-region case-violation report). A severity report
// takes severity_banner instead, which also names the scope.
std::string Lowerer::banner_at(slang::SourceLocation sl) const {
    const slang::SourceManager* sm = scope_->getCompilation().getSourceManager();
    return sources_.shown_file(sl) + ":" + std::to_string(sm->getLineNumber(sl)) + ": ";
}

// Compile-time "<file>:<line>: <scope>" banner of a severity report. LRM 20.10
// makes the report name where the call is and the hierarchical name of the
// scope it is made in; the runtime adds " at <time>: <message>" (rt_severity).
// The scope is what `%m` prints at the same place -- the instance, the
// enclosing subroutine, and every named block or assertion label around the
// call -- and it is as static as `%m` is, so no scope is looked up at run
// time.
std::string Lowerer::severity_banner(slang::SourceLocation sl) const {
    const std::string scope  = hier_name();
    std::string       banner = banner_at(sl);
    if (scope.empty()) {
        banner.pop_back(); // a scope-less call keeps no dangling space
    }
    return banner + scope;
}

// Banner for a unique/priority case or if violation, located at the statement.
std::string Lowerer::case_banner(const CaseStatement& c) const {
    return banner_at(c.sourceRange.start());
}

// True if an expression's type is a fixed-size unpacked array (a memory).
bool Lowerer::is_unpacked_array(const slang::ast::Expression& e) {
    return e.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr;
}

bool Lowerer::is_unpacked_array_type(const slang::ast::Type& t) {
    return t.getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr;
}

// True if a type is an unpacked struct (aggregate storage; whole-object ops
// decompose field-wise, distinct from a packed struct's flat bit-vector).
bool Lowerer::is_unpacked_struct(const slang::ast::Type& t) {
    return t.getCanonicalType().isUnpackedStruct();
}

// True if a type uses the by-pointer aggregate function-call ABI: any fixed
// unpacked array or unpacked struct (mirrors the backend is_agg_abi). Distinct
// from is_aggregate (struct-bearing only), which selects the storage model — a
// plain array is by-pointer at the ABI but flat in module storage.
bool Lowerer::is_agg_abi_type(const slang::ast::Type& t) {
    const slang::ast::Type& ct = t.getCanonicalType();
    return ct.isUnpackedStruct() || ct.as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr;
}

// True if a type contains an unpacked struct at any nesting depth (a bare
// struct or an unpacked array whose leaf is one): it is backed by the
// recursive aggregate model, so whole-object assignment / initialization
// routes through the aggregate copy path, not the flat scalar/array path.
bool Lowerer::is_aggregate(const slang::ast::Type& t) {
    const slang::ast::Type& ct = t.getCanonicalType();
    if (ct.isUnpackedStruct()) {
        return true;
    }
    if (const auto* arr = ct.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
        // A fixed array over a queue / dynamic-array element also takes the
        // recursive aggregate model (its cells are heap handles needing
        // per-cell lifecycle), mirroring the backend's needs_agg_storage.
        const slang::ast::Type& et = arr->elementType.getCanonicalType();
        if (et.as_if<slang::ast::QueueType>() != nullptr || et.as_if<slang::ast::DynamicArrayType>() != nullptr) {
            return true;
        }
        return is_aggregate(arr->elementType);
    }
    return false;
}

// True if a type is a fixed-size unpacked array, dynamic array, queue, or
// associative array: the remaining unpacked container shapes (scalar-element,
// so not is_aggregate) whose declaration initializer must route through the
// same element-wise / handle-aware emit_agg_store path as a struct-bearing
// aggregate, rather than the plain scalar Store fallback (which mis-lowers a
// `'{...}` or bare `{...}` literal, or a copy from another handle variable,
// into a raw bit-blast over the container's handle/flat storage).
bool Lowerer::is_unpacked_container(const slang::ast::Type& t) {
    const slang::ast::Type& ct = t.getCanonicalType();
    return ct.as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr ||
           ct.as_if<slang::ast::DynamicArrayType>() != nullptr ||
           ct.as_if<slang::ast::QueueType>() != nullptr ||
           ct.as_if<slang::ast::AssociativeArrayType>() != nullptr;
}

// Whether an expression is a whole unpacked-array lvalue (a named array, or a
// member / element that is itself a whole array) — the source of a whole-array
// memcpy copy, as opposed to a slice / element / pattern. A HierarchicalValue
// names an array reached through an interface modport (`iface.arr`), which
// canonical_value_symbol resolves to the interface's own storage.
bool Lowerer::is_whole_array_lvalue(const Expression& e) {
    if (e.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() == nullptr) {
        return false;
    }
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
        case ExpressionKind::MemberAccess:
        case ExpressionKind::ElementSelect:
            return true;
        default:
            return false;
    }
}

Lowerer::ValueCat Lowerer::value_cat(ir::Ref type) const {
    switch (ir::types::storage_class(module_.pool, type)) {
        case ir::types::StorageClass::Managed:
            // `string` and the three containers differ only in whose deep-copy
            // verb applies, which is exactly the split ValueCat draws.
            return ir::types::is_string(module_.pool, type) ? ValueCat::string
                                                            : ValueCat::handle;
        case ir::types::StorageClass::Aggregate:
            return ValueCat::aggregate;
        case ir::types::StorageClass::Plane:
        case ir::types::StorageClass::Void:
            return ValueCat::scalar;
    }
    return ValueCat::scalar;
}

// Whether an interned IR type is a scalar / string / real / handle leaf.
bool Lowerer::is_leaf_ref(ir::Ref type) const {
    return !ir::types::is_aggregate(module_.pool, type);
}

// Whether an interned IR type contains an unpacked struct at any nesting depth
// (backed by the recursive aggregate model; passed / returned by pointer).
bool Lowerer::contains_struct_ref(ir::Ref type) const {
    return ir::types::contains_struct(module_.pool, type);
}

// Whether an interned IR type is an unpacked aggregate passed / returned by
// pointer through the canonical aggregate ABI: any fixed unpacked array, or an
// unpacked struct (mirrors the backend is_agg_abi). Broader than
// contains_struct_ref, which is false for a plain scalar-element array.
bool Lowerer::is_agg_abi_ref(ir::Ref type) const {
    return ir::types::is_agg_abi(module_.pool, type);
}

// Whether a whole-object copy of the type may be a single memcpy of its
// storage: no leaf owns a heap buffer (string / dynamic array / queue), so
// duplicating the byte image duplicates the value. A class-handle leaf IS
// trivially copyable — the pointer copy is the reference semantics.
//
// NOTE: distinct from trivial relocatability, which every value type here
// has (no self-pointers): the runtime containers memcpy-MOVE elements
// (ring growth, insert/delete shifts) even for heap-owning element types;
// only memcpy DUPLICATION is what this predicate gates.
bool Lowerer::is_trivially_copyable(ir::Ref type) const {
    return ir::types::is_trivially_copyable(module_.pool, type);
}

// A pattern/concatenation-lvalue element that is itself a variable_lvalue
// needing a type inferred from context (observed for a nested assignment
// pattern, e.g. the `'{a,b}` in `'{'{a,b}, c} = r`) arrives wrapped in an
// AssignmentExpression -- the same shape slang uses for a system-task
// output argument (see lower_lvalue's Assignment case) and an associative-
// array first/last/next/prev key argument (lower_assoc_method). Unwrap
// before dispatching on the element's own kind.
const Expression& Lowerer::unwrap_lvalue_elem(const Expression& e) {
    return e.kind == ExpressionKind::Assignment ? e.as<AssignmentExpression>().left() : e;
}

// Strip an outer identity/struct Conversion wrapper slang may insert on a
// struct-typed rvalue, exposing the underlying lvalue-shaped expression.
const Expression& Lowerer::unwrap_conversion(const Expression& e) {
    const Expression* cur = &e;
    while (cur->kind == ExpressionKind::Conversion) {
        cur = &cur->as<ConversionExpression>().operand();
    }
    return *cur;
}

// Whether an expression is an lvalue-shaped struct/array reference (a member-
// by-member or whole-object copy source): a named variable, a select landing
// on one, or an interface-modport member (a HierarchicalValue that
// canonical_value_symbol resolves to the interface's own storage).
bool Lowerer::is_struct_lvalue_shape(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
        case ExpressionKind::MemberAccess:
        case ExpressionKind::ElementSelect:
            return true;
        default:
            return false;
    }
}

// True if an expression's type is a dynamic array (LRM 7.5).
bool Lowerer::is_dyn_array(const slang::ast::Expression& e) {
    return e.type->getCanonicalType().as_if<slang::ast::DynamicArrayType>() != nullptr;
}

// True if an expression's type is a queue (LRM 7.10).
bool Lowerer::is_queue(const slang::ast::Expression& e) {
    return e.type->getCanonicalType().as_if<slang::ast::QueueType>() != nullptr;
}

// True if an expression's type is an associative array (LRM 7.8/7.9).
bool Lowerer::is_assoc(const slang::ast::Expression& e) {
    return e.type->getCanonicalType().as_if<slang::ast::AssociativeArrayType>() != nullptr;
}

// True if an expression is `a[k]` selecting an element out of an associative
// array. Such an element's backend storage is the recursive aggregate image
// in the map slot, which does not share a plain array signal's flat layout --
// so a whole-array memcpy (AggCopy) between the two is invalid; the copy must
// decompose per element (each element resolves the map slot and stores).
bool Lowerer::is_assoc_element_select(const slang::ast::Expression& e) {
    return e.kind == ExpressionKind::ElementSelect && is_assoc(e.as<slang::ast::ElementSelectExpression>().value());
}

// True if an expression is of the dynamic `string` type (LRM 6.16).
bool Lowerer::is_string_expr(const slang::ast::Expression& e) {
    return e.type->getCanonicalType().isString();
}

// Whether a string lvalue is a PLACE whose nonblocking assignment the string NBA
// record can serve: it needs a destination address that exists when the RHS is
// sampled and still exists at the commit. A whole variable qualifies, as does a
// member of an unpacked struct or an element of a FIXED unpacked array (both are
// fixed offsets inside their root signal's storage), composed to any depth. An
// element of a queue / dynamic / associative array does not: its slot can be
// created, moved or freed between the two regions.
bool Lowerer::is_string_nba_place(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            return true;
        case ExpressionKind::MemberAccess: {
            const Expression& base = e.as<MemberAccessExpression>().value();
            // A class property is refused earlier (no per-object shadow); an
            // unpacked-struct member is a fixed offset in its root's storage.
            return !base.type->getCanonicalType().isClass() && is_string_nba_place(base);
        }
        case ExpressionKind::ElementSelect: {
            const Expression& base = e.as<ElementSelectExpression>().value();
            return base.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() != nullptr &&
                   is_string_nba_place(base);
        }
        default:
            return false;
    }
}

// True if an interned IR type is the string type, or an unpacked array whose
// leaf element is.
bool Lowerer::is_string_ref(ir::Ref ty) const {
    return ir::types::is_string_leaf_array(module_.pool, ty);
}

// True if an interned IR type is a dynamic array (LRM 7.5).
bool Lowerer::is_dyn_array_ref(ir::Ref ty) const {
    return ir::types::is_dynarr(module_.pool, ty);
}

// True if an interned IR type is a heap-handle value type — a dynamic array,
// queue, or associative array — which share the by-pointer, deep-copy
// subroutine ABI.
bool Lowerer::is_handle_ref(ir::Ref ty) const {
    return ir::types::is_container(module_.pool, ty);
}

// True if an expression is a dynamic array, queue, or associative array (a
// heap handle).
bool Lowerer::is_handle_expr(const slang::ast::Expression& e) {
    const slang::ast::Type& ct = e.type->getCanonicalType();
    return ct.as_if<slang::ast::DynamicArrayType>() != nullptr || ct.as_if<slang::ast::QueueType>() != nullptr ||
           ct.as_if<slang::ast::AssociativeArrayType>() != nullptr;
}

// Intern a string literal's characters as a string constant. Every \0 byte
// is dropped: a string object never contains one, and literal-to-string
// assignment strips them (LRM 6.16).
ir::Ref Lowerer::string_value_ref(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (const char c : text) {
        if (c != '\0') {
            stripped += c;
        }
    }
    return module_.pool.put_value(module_.pool.string_type,
                                  { reinterpret_cast<const uint8_t*>(stripped.data()), stripped.size() });
}

// Intern a packed integer type of the given width and domain. An integer type
// carries no signedness: the opcode or the instruction data that reads a value
// says how.
ir::Ref Lowerer::make_int_type(uint32_t width, bool is_four) {
    return module_.pool.int_type(width, is_four);
}

// Convert an already-lowered rvalue of slang type `from` to the interned target
// `to`. Integral-to-integral converts; any real endpoint picks the matching float
// conversion (assignment semantics: real -> integral rounds half away from zero,
// LRM 6.12.1). Used where the frontend coerces manually (port copies, call
// arguments, returns) rather than through a slang-inserted Conversion.
//
// An integral widening fills with the sign bit or with zeros, and the language
// decides which: LRM 11.8.3 (assignment) "extend the size of the right-hand
// side, performing sign extension if, and only if, the type of the right-hand
// side is signed" -- the SOURCE decides, and LRM 6.24.1 puts a cast under the same
// rule. LRM 11.8.2, last step, has a context-determined operand "converted to the
// propagated type and size... sign-extended only if the propagated type is
// signed" -- the TARGET decides. The two really do disagree, in both directions:
// with `logic [7:0] u; logic signed [3:0] s = -1`, `u + s` zero-extends s (16),
// while `u = s` sign-extends it (255). Both iverilog 13 and Verilator 5.050 agree
// on both. Every caller is assignment-like (the default, the source's signedness)
// except the propagated conversion, which passes the propagated type's.
ir::Index Lowerer::coerce(ir::Index               v,
                          const slang::ast::Type& from,
                          ir::Ref                 to,
                          ir::Builder&            b,
                          ir::SourceLoc           l,
                          std::optional<bool>     sign_extend) {
    const bool from_f = from.isFloating();
    const bool to_f   = module_.pool.is_float(to);
    if (to_f && from_f) {
        return b.float_cvt(ir::Inst::Tag::FResize, v, to, l);
    }
    if (to_f) {
        return b.float_cvt(from.isSigned() ? ir::Inst::Tag::SItoF : ir::Inst::Tag::UItoF, v, to, l);
    }
    if (from_f) {
        return b.float_cvt(ir::Inst::Tag::FtoI, v, to, l);
    }

    return b.convert(v, to, sign_extend.value_or(from.isSigned()), l);
}

// Narrow/widen an already-loaded rvalue to its expression's own declared
// type (LRM 6.3.1 domain conversion). A no-op for the ordinary
// (domain-uniform) case; skipped for string/real, which have no domain
// to narrow (a packed struct member is always integral, LRM 7.2.1) and
// which an integer conversion rejects outright. Needed because a
// PartSelect/BitSelect's type (ir/Typing.cpp) inherits the base signal's
// domain uniformly -- correct for an ordinary vector/array, wrong when the base
// is a struct field whose own declared domain is narrower than its
// struct's physical storage (LRM 6.11.2 forces the whole struct 4-state
// if any member is). The width is the field's own, so only the domain changes.
ir::Index Lowerer::narrow_to_declared(ir::Index v, const slang::ast::Type& declared, ir::Builder& b, ir::SourceLoc l) {
    if (declared.isString() || declared.isFloating()) {
        return v;
    }
    return b.convert(v, lower_type(declared), /*sign_extend=*/false, l);
}

ir::Ref Lowerer::mask_type(uint32_t width) {
    return make_int_type(width, /*is_four=*/false);
}

// `id` indexes the module's monitor table, so two assertions in one instance
// get distinct names. `ctl` is the assertion's LRM 20.12 control state,
// already allocated before this pass began (see alloc_assert_control), or
// null in a design that controls nothing.
Lowerer::MonitorState Lowerer::alloc_monitor_state(const ir::AssertionMonitor& mon,
                                                   uint32_t                    id,
                                                   const MonitorControl*       ctl) {
    const std::string   base = "sva." + std::to_string(id) + ".";
    const ir::SourceLoc l    = mon.span.start;

    MonitorState st;
    st.ctl = ctl;
    if (mon.has_trigger) {
        st.trigger = new_monitor_state(base + "trig", mask_type(mon.trigger.state_count), l);
        st.fired   = new_monitor_state(base + "fired", mask_type(1), l);
    }
    if (mon.tracking == ir::AssertionTracking::Counted) {
        // One counter per state an attempt can still be sitting in next cycle.
        // The obligation is determinised, so an attempt occupies exactly one
        // state and two attempts in one state have the same future by
        // construction -- which is what lets a count stand in for a state
        // vector per attempt, and what makes the STORAGE fixed while the counts
        // are not.
        //
        // 64 bits and no saturating add: at the measured tick rate a 32-bit
        // counter wraps within minutes of wall clock, while a 64-bit one cannot
        // be reached by any trace that finishes.
        st.counts.assign(mon.obligation.state_count, ir::Signal{ 0 });
        for (uint32_t s = 0; s < mon.obligation.state_count; ++s) {
            if (!counted_carries(mon.obligation, s)) {
                continue;
            }
            st.counts[s] = new_monitor_state(base + "cnt" + std::to_string(s), mask_type(counter_bits), l);
        }
    } else {
        st.pending.reserve(mon.depth);
        for (uint32_t k = 0; k < mon.depth; ++k) {
            st.pending.push_back(
              new_monitor_state(base + "pend" + std::to_string(k), mask_type(mon.obligation.state_count), l));
        }
        st.occupied = new_monitor_state(base + "occ", mask_type(mon.depth), l);
    }

    // The attempt ids, only when the antecedent can match twice in one attempt
    // and its longest match is bounded (attempt_ring_size).
    if (const uint32_t ring = attempt_ring_size(mon); ring != 0) {
        st.tid.reserve(mon.trigger.state_count);
        for (uint32_t s = 0; s < mon.trigger.state_count; ++s) {
            st.tid.push_back(new_monitor_state(base + "tid" + std::to_string(s), mask_type(ring), l));
        }
        // A slot id per slot, and a counted monitor has no slots: its `depth` is
        // 0, so this loop is already empty for one. Labelling slots and deferring
        // a pass are both the GROUPING's, which is the ring discipline's alone --
        // see attempt_ring_size -- so `passing` has to say so explicitly or a
        // counted monitor would carry it as dead state.
        st.oid.reserve(mon.depth);
        for (uint32_t k = 0; k < mon.depth; ++k) {
            st.oid.push_back(new_monitor_state(base + "oid" + std::to_string(k), mask_type(ring), l));
        }
        st.firedid = new_monitor_state(base + "fid", mask_type(ring), l);
        st.idnow   = new_monitor_state(base + "idn", mask_type(ring), l);
        if (mon.tracking == ir::AssertionTracking::Ring) {
            st.passing = new_monitor_state(base + "pass", mask_type(ring), l);
        }
    }
    if (mon.kind == ir::AssertionKind::Cover) {
        // Unsigned: a tally is never negative, and widening the 1-bit match
        // flag into a signed target would sign-extend it to -1 (a Resize takes
        // its signedness from the target type).
        st.covered = new_monitor_state(base + "cov", mask_type(32), l);
    }
    if (mon.kind == ir::AssertionKind::Expect) {
        // Written from the process that executes the statement, in the Active
        // region, and read by this monitor at its next edge in Observed. An
        // ordinary hidden signal is exactly the right carrier for that: the
        // $assertkill request already crosses the same gap the same way.
        st.expect_req  = new_monitor_state(base + "req", mask_type(1), l);
        st.expect_busy = new_monitor_state(base + "flight", mask_type(1), l);
    }
    st.abort_seen.resize(mon.aborts.size());
    for (uint32_t i = 0; i < mon.aborts.size(); ++i) {
        // A `sync_` abort reads the Preponed sample at the edge and has
        // nothing to remember between ticks, so it gets no latch.
        if (!mon.aborts[i].sampled) {
            st.abort_seen[i] = new_monitor_state(base + "abrt" + std::to_string(i), mask_type(1), l);
        }
    }
    st.clkprev = new_monitor_state(base + "clk", mask_type(1), l);

    return st;
}

bool Lowerer::row_is_zero(std::span<const uint64_t> row) {
    for (const uint64_t word : row) {
        if (word != 0) {
            return false;
        }
    }

    return true;
}

// Row `index` of a `words`-per-row bit-set table.
std::span<const uint64_t> Lowerer::table_row(const std::vector<uint64_t>& table, uint32_t index, uint32_t words) {
    return { table.data() + static_cast<size_t>(index) * words, words };
}

// Whether two bit-set rows share a member.
bool Lowerer::rows_intersect(std::span<const uint64_t> a, std::span<const uint64_t> b) {
    for (size_t w = 0; w < a.size() && w < b.size(); ++w) {
        if ((a[w] & b[w]) != 0) {
            return true;
        }
    }

    return false;
}

// Width of the attempt-id ring, or 0 when the monitor needs no per-attempt
// identity at all.
//
// LRM 16.12.1 starts an attempt every tick, and both things clotho needs the ids
// for follow from an attempt being able to have more than one antecedent match,
// or two attempts more than one match ENDING TOGETHER:
//
//  - GROUPING. A ranged antecedent can match more than once within ONE attempt,
//    each match seeding its own obligation, and that attempt gets a SINGLE
//    verdict (docs/LRM_NOTES.md, note 23): it fails at its first failing
//    obligation and passes only once every one of them has passed.
//  - COUNTING. Attempts whose matches end in the SAME cycle raise one shared
//    obligation and are still owed a verdict each (docs/LRM_NOTES.md, note 24).
//
// `mon.trigger_window` is libsva's answer to whether either can happen, and 0
// means neither can -- so the whole mechanism folds away and the monitor keeps
// exactly its pre-existing state and code. That is no trigger, an antecedent
// matching in a single cycle (`req |=> ack`), or one whose matches all span the
// same number of cycles (`(a ##1 b) |-> c`); the last of those is why the window
// is read from libsva rather than derived from the trigger's longest match, which
// cannot see it. An antecedent with no bounded longest match never arrives: libsva
// refuses it as SVA_ERROR_UNBOUNDED_ANTECEDENT.
//
// The window itself only has to keep LIVE ANTECEDENT THREADS apart, which is all
// libsva's own monitor needs. Clotho's ring discipline keeps an id until the
// obligation behind it resolves, because that is what the grouping is: `depth`
// cycles more, plus the one a `|=>` carries the id over. A COUNTED monitor needs
// only that last cycle and its `depth` is 0, so the same formula sizes both.
//
// What a counted monitor does not get is the grouping -- that needs the id to
// survive until the obligation resolves, and a counted obligation has no bounded
// lifetime for a width to be derived from. So it still reports per obligation,
// the residue of the one-verdict rule (docs/LRM_NOTES.md, note 23), and uses
// the ids for the counting alone.
uint32_t Lowerer::attempt_ring_size(const ir::AssertionMonitor& mon) {
    if (mon.trigger_window == 0) {
        return 0;
    }

    return mon.trigger_window + mon.depth + 1;
}

// Whether this root is evaluated from LIVE values rather than from the
// Preponed shadows. Two things read live, and they are the same mechanism at
// two scopes: the `disable iff` condition, which LRM 16.15 deliberately does
// not sample, and the condition of an asynchronous abort, which LRM 16.12.7
// evaluates "at the granularity of the simulation time step like disable
// iff". The `sync_` abort forms sample like everything else.
bool Lowerer::root_reads_live(const ir::AssertionMonitor& mon, uint32_t root) {
    if (root == mon.reset_root) {
        return true;
    }
    for (const ir::AssertionAbort& abort : mon.aborts) {
        if (!abort.sampled && abort.root == root) {
            return true;
        }
    }

    return false;
}

// The obligation states from which the abort's scope is one step away, as a
// compile-time row. libsva's reference arms an abort for an attempt when
// advancing that attempt's live set with every guard satisfied would land
// inside the abort's mask (../libsva/test/backend.c:194-204) -- the states it
// COULD reach, not the ones it ends up in, because an abort covers both the
// cycle its subtree is entered in and the cycle its last thread dies in, and
// the live set has the subtree empty in both.
//
// That test transposes: `advance(cur, ~0)` is just the union of the follow
// rows of `cur`'s members, so it meets the mask exactly when `cur` holds some
// state whose follow row does -- one AND against a constant instead of a
// whole advance per attempt.
std::vector<uint64_t> Lowerer::abort_arming_row(const ir::AssertionAutomaton& au, const ir::AssertionAbort& abort) {
    std::vector<uint64_t> row(au.words, 0);
    for (uint32_t s = 0; s < au.state_count; ++s) {
        if (rows_intersect(table_row(au.follow, s, au.words), abort.mask)) {
            row[s / 64] |= uint64_t{ 1 } << (s % 64);
        }
    }

    return row;
}

// One bit-set row as a single `bit [states-1:0]` constant. The tables arrive
// as 64-bit words because that is libsva's C transport; a clotho value is
// arbitrary-precision, so the row is one constant and never a word array.
ir::Ref Lowerer::mask_const(std::span<const uint64_t> row, uint32_t states) {
    const ir::Ref ty = mask_type(states);
    if (states <= 64) {
        return module_.pool.put_int(ty, row.empty() ? 0 : row[0]);
    }
    // Two-state, so one plane, and both sides are little-endian words.
    std::vector<uint8_t> bytes(ir::plane_bytes(states), 0);
    std::memcpy(bytes.data(), row.data(), std::min<size_t>(bytes.size(), row.size() * sizeof(uint64_t)));

    return module_.pool.put_value(ty, bytes);
}

ir::Index Lowerer::zero_mask(uint32_t width, ir::Builder& b, ir::SourceLoc l) {
    return b.constant(mask_const({}, width), l);
}

ir::Index Lowerer::slot_mask(uint32_t bit, uint32_t width, ir::Builder& b, ir::SourceLoc l) {
    std::vector<uint64_t> row((width + 63) / 64, 0);
    row[bit / 64] = uint64_t{ 1 } << (bit % 64);

    return b.constant(mask_const(row, width), l);
}

ir::Index Lowerer::load_signal(ir::Signal sig, ir::Builder& b, ir::SourceLoc l) {
    return b.unary(ir::Inst::Tag::Load, b.signal(sig, l), l);
}

// Complement of a definite 2-state bit. Not Lnot: these bits are already
// collapsed to 0/1, so there is no unknown left for Lnot to propagate.
ir::Index Lowerer::bit_not(ir::Index v, ir::Builder& b, ir::SourceLoc l) {
    return b.binary(ir::Inst::Tag::Bxor, v, true_const(b, l), l);
}

// `mask != 0`, as a definite bit.
ir::Index Lowerer::mask_any(ir::Index mask, ir::Builder& b, ir::SourceLoc l) {
    return b.unary(ir::Inst::Tag::RedOr, mask, l);
}

// Bit `bit` of a `width`-bit two-state mask, as a definite 1-bit value. Tested
// rather than selected: Inst::Tag::BitSelect addresses storage and needs an
// lvalue base, and every mask here is a computed value.
ir::Index Lowerer::mask_bit(ir::Index mask, uint32_t bit, uint32_t width, ir::Builder& b, ir::SourceLoc l) {
    return mask_any(b.binary(ir::Inst::Tag::Band, mask, slot_mask(bit, width, b, l), l), b, l);
}

// How many bits a `width`-bit two-state mask has set, as a `counter_bits`-wide
// number. Unrolled: `width` is a compile-time constant, the IR has no population
// count, and the step has no loop to put a run-time reduction in.
ir::Index Lowerer::mask_popcount(ir::Index mask, uint32_t width, ir::Builder& b, ir::SourceLoc l) {
    ir::Index total = zero_mask(counter_bits, b, l);
    for (uint32_t i = 0; i < width; ++i) {
        total = b.binary(
          ir::Inst::Tag::Add, total, b.convert(mask_bit(mask, i, width, b, l), mask_type(counter_bits), false, l), l);
    }

    return total;
}

// A `width`-bit unsigned constant. Only ever used for small shift amounts, so
// the low word is the whole value.
ir::Index Lowerer::int_const(uint64_t v, uint32_t width, ir::Builder& b, ir::SourceLoc l) {
    const std::array<uint64_t, 1> row{ v };

    return b.constant(mask_const(row, width), l);
}

// One step of the one-hot attempt-id ring: rotate left by one, so the bit
// walking off the top comes back in at the bottom.
ir::Index Lowerer::rotl1(ir::Index v, uint32_t width, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index up   = b.binary(ir::Inst::Tag::Sll, v, int_const(1, width, b, l), l);
    const ir::Index over = b.binary(ir::Inst::Tag::Srl, v, int_const(width - 1, width, b, l), l);

    return b.binary(ir::Inst::Tag::Bor, up, over, l);
}

// A definite 1-bit `1`, used as the predicate for an omitted for-condition
// or a `forever` loop.
ir::Index Lowerer::true_const(ir::Builder& b, ir::SourceLoc l) {
    return b.constant(module_.pool.put_int(make_int_type(1, false), 1), l);
}

// Allocate one. Inside anything with a frame -- a task, a function, a fork
// child, an action block -- it is a frame Local, which is what makes a
// `repeat` correct in an automatic subroutine: recursive and concurrent
// activations each get their own counter. Module-signal storage gave every
// activation one shared cell, so a recursive `repeat (3)` two levels deep ran
// 5 iterations instead of 39, and two forked calls to the same task clobbered
// each other's count. A process has no frame and is single-activation, so
// there the cell stays a module Signal and the emitted code is unchanged.
Lowerer::TempCell Lowerer::make_temp_cell(ir::Ref type, std::string_view role) {
    if (fn_ctx_ != nullptr) {
        const auto idx = static_cast<uint32_t>(fn_ctx_->local_types.size());
        fn_ctx_->local_types.push_back(type);
        return TempCell{ .type = type, .is_local = true, .local = idx };
    }
    ir::SignalInfo info;
    info.name = prefix_ + "__" + std::string(role) + std::to_string(temp_id_++);
    info.type = type;
    info.dir  = ir::SignalInfo::Direction::Internal;

    return TempCell{ .type = type, .sig = register_signal(std::move(info)) };
}

// Register a hidden temporary that must be real module-signal storage rather
// than a frame cell: a synthesized event net (synth_event_net) is the target
// of its own continuous process and carries a wait queue, neither of which a
// frame Local has. Unlike declare_local there is no source ValueSymbol, so
// the name is synthesized.
ir::Signal Lowerer::declare_temp(ir::Ref type, std::string_view role) {
    ir::SignalInfo info;
    info.name = prefix_ + "__" + std::string(role) + std::to_string(temp_id_++);
    info.type = type;
    info.dir  = ir::SignalInfo::Direction::Internal;

    return register_signal(std::move(info));
}

// An integer constant of the given interned type.
ir::Index Lowerer::iconst(ir::Ref ty, uint64_t v, ir::Builder& b, ir::SourceLoc l) {
    return b.constant(module_.pool.put_int(ty, v), l);
}

// Get (creating if needed) the frame Local index for a function-local symbol.
uint32_t Lowerer::fn_local(const ValueSymbol& vs) {
    const auto it = fn_ctx_->localmap.find(&vs);
    if (it != fn_ctx_->localmap.end()) {
        return it->second;
    }
    const auto idx = static_cast<uint32_t>(fn_ctx_->local_types.size());
    fn_ctx_->local_types.push_back(lower_type(vs.getType()));
    fn_ctx_->localmap[&vs] = idx;

    return idx;
}

} // namespace clotho::lower
