// Assignments: blocking, intra-assignment timing, aggregates, patterns and compound forms.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Emit one leaf store of an assignment. `t` null -- every caller but the
// nonblocking intra-assignment one -- is the ordinary immediate / NBA store;
// non-null defers this leaf through its own record (LRM 9.4.5).
void Lowerer::store_leaf(ir::Inst::Tag        tag,
                         const IntraNbTiming* t,
                         ir::Index            lv,
                         ir::Index            v,
                         ir::Builder&         b,
                         ir::SourceLoc        l) {
    if (t == nullptr) {
        b.binary(tag, lv, v, l);
        return;
    }
    if (!t->is_event) {
        if (!t->zero) {
            b.store_nb_delay(lv, v, t->delay, l);
            return;
        }
        // A delay that is zero at run time lands in the current instant's NBA
        // region, ordered with the other `<=` by execution order (LRM 10.4.2,
        // 4.9.4), so it takes the plain `<=` path exactly as a compile-time `#0`
        // does (intra_nb_delay_is_const_zero). The deferred record's commit order
        // is fixed instead, which put it after a later `<=` to the same signal.
        // Per leaf, like the repeat count below: the delay is evaluated once.
        b.open_region();
        b.binary(ir::Inst::Tag::StoreNb, lv, v, l);
        const ir::ExtraSlice now_r = b.close_region();

        b.open_region();
        b.store_nb_delay(lv, v, t->delay, l);
        const ir::ExtraSlice later_r = b.close_region();

        b.cond(*t->zero, now_r, later_r, l);
        return;
    }
    if (!t->nonpos) {
        b.store_nb_event(lv, v, t->count, t->trigs, l);
        return;
    }
    // LRM 9.4.5: "If the repeat count literal, or signed variable holding the
    // repeat count, is less than or equal to 0 at the time of evaluation, the
    // assignment occurs as if there is no repeat construct" -- and the clause's
    // own example settles what that means: `repeat (-3) @ (event_expression)
    // // will not execute event_expression`. So the store happens with no wait
    // at all, which for the nonblocking form is a plain StoreNb -- the current
    // instant's NBA region. That is what the blocking form already does via its
    // signed `Lt` bound (lower_intra_timing waits zero times), so the two forms
    // agree. Measured against iverilog 13.
    //
    // Per leaf rather than around the whole walk: `count` is evaluated exactly
    // once, before the walk, and re-testing that one SSA value at each leaf is
    // free, whereas wrapping the walk would re-lower every leaf's RHS inside
    // both arms and break 9.4.5's single evaluation.
    b.open_region();
    b.binary(ir::Inst::Tag::StoreNb, lv, v, l);
    const ir::ExtraSlice now_r = b.close_region();

    b.open_region();
    b.store_nb_event(lv, v, t->count, t->trigs, l);
    const ir::ExtraSlice wait_r = b.close_region();

    b.cond(*t->nonpos, now_r, wait_r, l);
}

// Store a whole unpacked struct: one blocking store per member (the aggregate
// has no flat value, so a whole-object copy decomposes field-wise). `lhs_base`
// is the struct's lvalue; `sty` its interned Kind::Struct type. The RHS is
// either a `'{...}` assignment pattern (per-field element expressions, already
// in declaration order and type-converted by slang) or another struct lvalue
// (member-by-member copy). Members are scalar (nested unpacked struct members
// are guarded at lower_type), so each store is a plain scalar store.
void Lowerer::emit_struct_store(ir::Index            lhs_base,
                                ir::Ref              sty,
                                const Expression&    rhs,
                                ir::Inst::Tag        tag,
                                ir::Builder&         b,
                                ir::SourceLoc        l,
                                const IntraNbTiming* nbt) {
    emit_agg_store(lhs_base, sty, rhs, tag, b, l, nbt);
}

// True if a (possibly struct-nested) unpacked struct type has any field
// carrying its own declared initializer (LRM 7.2's struct_union_member
// list_of_variable_decl_assignments) -- applied as that field's default
// value when a variable of the type is declared with no initializer of
// its own, the same way a class property default applies at construction
// (LRM 8.8 / emit_class_property_inits). An unset field still keeps its
// own domain default (X / 0), so this only needs to detect whether ANY
// field needs a store at all.
bool Lowerer::struct_has_field_defaults(const slang::ast::Type& t) {
    const slang::ast::Type& ct = t.getCanonicalType();
    if (!ct.isUnpackedStruct()) {
        return false;
    }
    for (const auto& f : ct.as<slang::ast::UnpackedStructType>().membersOfType<slang::ast::FieldSymbol>()) {
        if (f.getInitializer() != nullptr || struct_has_field_defaults(f.getType())) {
            return true;
        }
    }
    return false;
}

// Apply an unpacked struct type's field-level default initializers (see
// struct_has_field_defaults) to `lhs`, for a variable declared with no
// initializer of its own: one store per field that declares a default, in
// declaration order, recursing into a nested struct field that has one of
// its own. A field with no default (direct or nested) is left alone --
// its storage already carries the plain power-on value (X / 0) the
// backend's state initializer set, same as any other uninitialized
// scalar. `sv_type` is the same struct as `sty`'s interned Kind::Struct
// (needed alongside it: field initializer expressions and their real
// types live on the slang AST, not the IR field table).
void Lowerer::emit_struct_field_defaults(ir::Index               lhs,
                                         ir::Ref                 sty,
                                         const slang::ast::Type& sv_type,
                                         ir::Builder&            b,
                                         ir::SourceLoc           l) {
    const auto& ct  = sv_type.getCanonicalType().as<slang::ast::UnpackedStructType>();
    uint32_t    idx = 0;
    for (const auto& f : ct.membersOfType<slang::ast::FieldSymbol>()) {
        if (const Expression* init = f.getInitializer()) {
            const ir::Index fld_lv = b.member_select(lhs, idx, sty, l);
            const ir::Index rhs    = lower_expr(*init, b);
            if (f.getType().getCanonicalType().isString()) {
                b.binary(ir::Inst::Tag::Store, fld_lv, rhs, l);
            } else {
                b.binary(ir::Inst::Tag::Store, fld_lv, coerce(rhs, *init->type, lower_type(f.getType()), b, l), l);
            }
        } else if (struct_has_field_defaults(f.getType())) {
            emit_struct_field_defaults(
              b.member_select(lhs, idx, sty, l), lower_type(f.getType()), f.getType(), b, l);
        }
        ++idx;
    }
}

// True if a type is, or (through unpacked-array nesting) contains, an
// unpacked struct with a field-level default (struct_has_field_defaults).
bool Lowerer::aggregate_has_field_defaults(const slang::ast::Type& t) {
    const slang::ast::Type& ct = t.getCanonicalType();
    if (ct.isUnpackedStruct()) {
        return struct_has_field_defaults(ct);
    }
    if (const auto* arr = ct.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
        return aggregate_has_field_defaults(arr->elementType);
    }
    return false;
}

// Apply a struct-typed variable's field-level default initializers when
// the variable itself has none (see struct_has_field_defaults /
// emit_struct_field_defaults). An array of such a struct is a documented
// follow-up: loud-guarded, not a silent no-op, since silently skipping it
// would leave every element at its plain power-on value with the field
// defaults dropped -- the exact bug this function exists to fix.
void Lowerer::emit_var_field_defaults(const VariableSymbol& vs, ir::Index lv, ir::Builder& b, ir::SourceLoc l) {
    if (!aggregate_has_field_defaults(vs.getType())) {
        return;
    }
    if (is_unpacked_struct(vs.getType())) {
        emit_struct_field_defaults(lv, lower_type(vs.getType()), vs.getType(), b, l);
        return;
    }
    // TODO: an array of a struct with field-level defaults, and no array
    // initializer of its own -- each element needs the same per-field
    // defaulting emit_struct_field_defaults does for a bare struct.
    fail("an array of a struct with field-level default initializers, and no "
         "array initializer of its own, is not supported");
}

// Store a string literal into a fixed-size unpacked array of byte-shaped
// elements (LRM 5.9: "A string literal can be assigned to an unpacked
// array of bytes. If the size differs, it is left justified."). Character
// 0 lands at the array's declared left bound, one element per character,
// walking toward the right bound; a shorter string zero-pads the
// remaining (rightmost) elements, a longer string's excess tail is
// dropped. Unlike a `string`-typed object, an array element is not
// string-terminated, so an embedded NUL byte is kept, not stripped.
void Lowerer::emit_string_literal_array_store(ir::Index            lhs,
                                              ir::Ref              type,
                                              const ir::Type&      t,
                                              std::string_view     text,
                                              ir::Inst::Tag        tag,
                                              ir::Builder&         b,
                                              ir::SourceLoc        l,
                                              const IntraNbTiming* nbt) {
    const uint32_t array_len = (t.left > t.right ? t.left - t.right : t.right - t.left) + 1;
    for (uint32_t k = 0; k < array_len; ++k) {
        const int32_t coord =
          t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
        const uint8_t   byte_val = k < text.size() ? static_cast<uint8_t>(text[k]) : 0;
        const ir::Index idx      = b.constant(
          module_.pool.put_int(make_int_type(32, false, true), static_cast<uint64_t>(static_cast<int64_t>(coord))),
          l);
        const ir::Index val = b.constant(module_.pool.put_int(t.element, byte_val), l);
        store_leaf(tag, nbt, b.elem_select(lhs, idx, type, l), val, b, l);
    }
}

// Store an aggregate whole-object value into `lhs` (an aggregate lvalue of
// type `type`), decomposing to per-leaf stores. RHS is a `'{...}` pattern
// (per-field elements, recursed for aggregate members), a leaf value, or
// another aggregate lvalue (member-by-member copy). A nonblocking store
// samples each leaf's RHS now and commits per-leaf, which is whole-object NBA
// semantics. Array-typed members are guarded (element-wise unroll is a
// follow-up), so nesting is struct-through-struct here.
void Lowerer::emit_agg_store(ir::Index            lhs,
                             ir::Ref              type,
                             const Expression&    rhs,
                             ir::Inst::Tag        tag,
                             ir::Builder&         b,
                             ir::SourceLoc        l,
                             const IntraNbTiming* nbt) {
    // By value, not by reference: InternPool::get hands back a reference into
    // its items_ vector, and interning anything (a constant index below, or a
    // type while lowering an element expression) push_backs onto that vector
    // and can reallocate it. Holding a reference across such a call is a
    // use-after-free -- ASan caught exactly that here, reading t.left/t.right
    // after the loop's put_int reallocated the pool. Type is a ~20-byte POD,
    // so the copy is free. Same reasoning as the struct_fields span re-fetch
    // below.
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    // A packed plane leaf — a packed vector or packed struct, including one
    // that is the element of an unpacked array recursed into here (`arr <=
    // '{default: 0}` over an array of packed structs). It is a flat plane
    // value, so lower the whole rhs — a `'{...}` pattern, a value, or an
    // lvalue — as that value and store it, rather than decomposing field-by-
    // field (the struct branch below is for UNPACKED structs only). slang
    // types the pattern as the packed target, so lower_expr yields its flat
    // bits. Only ValueCat::scalar: a handle (queue / dynamic array) or string
    // leaf keeps its own copy path below.
    if (value_cat(type) == ValueCat::scalar) {
        store_leaf(tag, nbt, lhs, lower_expr(rhs, b), b, l);
        return;
    }
    // LRM 11.4.11 over an aggregate operand, intercepted ahead of every RHS
    // shape below: the clause's "without evaluating" makes the arms
    // CONDITIONAL, so neither may be lowered into the straight-line sequence
    // those shapes assume. A packed target already returned above through the
    // integral path (lower_lazy_conditional), which implements the same clause
    // for a flat value.
    if (const ConditionalExpression* cond = as_agg_conditional(rhs, type)) {
        if (nbt != nullptr) {
            // TODO: nonblocking intra-assignment timing (LRM 9.4.5) on a whole
            // unpacked aggregate whose RHS is a conditional. 11.4.11 makes the
            // arms conditional, so they lower into Cond regions and the
            // ambiguous arm into per-element Table 7-1 defaults; deferring
            // those leaves means threading the timing through
            // emit_agg_cond_ambiguous / _elementwise / emit_table7_1_default
            // too. Every other whole-aggregate RHS shape defers today.
            fail("intra-assignment timing on a whole unpacked aggregate target "
                 "with a conditional right-hand side is not supported");
        }
        emit_agg_conditional(lhs, type, *cond, tag, b, l);
        return;
    }
    const auto* pat = as_assignment_pattern(rhs);
    // A bare `{...}` concatenation literal targeting a queue / dynamic array /
    // fixed unpacked array is an unpacked-array concatenation (LRM 11.4.14),
    // not a packed bit-concat — slang already resolved the expression's own
    // type to the unpacked target, so it is positional like a `'{...}`
    // pattern and only differs in the element-span accessor.
    const bool is_concat_literal =
      pat == nullptr && rhs.kind == ExpressionKind::Concatenation && rhs.type->isUnpackedArray();
    if (t.kind == ir::Type::Kind::Assoc && pat != nullptr) {
        // An associative-array literal is keyed, never positional, so it has no
        // element list to walk (slang keeps the entries and the `default:` in
        // the structured pattern's own setters).
        if (nbt != nullptr) {
            // Not reachable from legal source today (an associative array is a
            // whole-container target, refused ahead of here), and refused rather
            // than silently dropped if it ever becomes reachable: the deferred
            // record has no owned-handle tail class to stage a container into.
            fail("intra-assignment timing on an associative-array literal target "
                 "is not supported");
        }
        emit_assoc_literal_init(lhs, type, rhs, tag, b, l);
        return;
    }
    if (pat != nullptr || is_concat_literal) {
        const auto     elems = pat != nullptr ? pat->elements() : rhs.as<ConcatenationExpression>().operands();
        const uint32_t reps =
          pat != nullptr && rhs.kind == ExpressionKind::ReplicatedAssignmentPattern
            ? static_cast<uint32_t>(const_uint(rhs.as<ReplicatedAssignmentPatternExpression>().count(),
                                               "an assignment-pattern replication count (LRM 10.9.1)"))
            : 1;
        if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
            if (pat == nullptr) {
                fail("unpacked concatenation literal into a struct is not supported");
            }
            // Re-fetch each row: lowering an element expression can intern a
            // new struct's field table, which invalidates a held
            // struct_fields span (see InternPool::struct_fields).
            const auto     nfields = module_.pool.struct_fields(t.fields_id).size();
            const uint32_t nelems  = static_cast<uint32_t>(elems.size());
            // A replicated pattern (`'{N{...}}`) stores only its per-repeat
            // item list in `elements()` (nelems < nfields) -- cycle through it
            // field-by-field, same as the array case's `elems[k % n]`. A
            // positional/structured pattern already has nelems == nfields, so
            // the modulo is a no-op there.
            for (uint32_t i = 0; i < nfields; ++i) {
                const ir::Field f = module_.pool.struct_fields(t.fields_id)[i];
                emit_agg_store(b.member_select(lhs, i, type, l), f.type, *elems[i % nelems], tag, b, l, nbt);
            }
            return;
        }
        if (t.kind == ir::Type::Kind::UnpackedArray) {
            // Position k of the (expanded) pattern stores into the array
            // coordinate slang assigned it. A keyed (structured) pattern binds
            // element k to the numeric index `min(left,right) + k` (the array
            // index is the key); a positional / replicated pattern (or a bare
            // concatenation literal, always positional) binds it left-to-right,
            // so k is the k-th element from the `left` bound. slang's
            // `elements()` is already fully ordered and default-filled, but a
            // replicated pattern (`'{N{...}}`) stores only one copy in
            // `elements()` — the count multiplies the position span. Verified
            // against Verilator on ascending and descending ranges.
            const bool     keyed = pat != nullptr && rhs.kind == ExpressionKind::StructuredAssignmentPattern;
            const uint32_t n     = static_cast<uint32_t>(elems.size());
            const uint32_t lo    = std::min(t.left, t.right);
            for (uint32_t k = 0; k < n * reps; ++k) {
                const int32_t coord =
                  keyed ? static_cast<int32_t>(lo + k)
                        : (t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k));
                const ir::Index idx =
                  b.constant(module_.pool.put_int(make_int_type(32, false, true),
                                                  static_cast<uint64_t>(static_cast<int64_t>(coord))),
                             l);
                emit_agg_store(b.elem_select(lhs, idx, type, l), t.element, *elems[k % n], tag, b, l, nbt);
            }
            return;
        }
        if (t.kind == ir::Type::Kind::DynArray || t.kind == ir::Type::Kind::Queue) {
            if (nbt != nullptr) {
                // A whole queue / dynamic-array target: LRM 6.21 and the missing
                // owned-handle record tail, the same refusal the top-level form
                // gets. Never silently dropped here.
                fail("intra-assignment timing on a whole queue or dynamic-array "
                     "target is not supported");
            }
            emit_handle_literal_init(lhs, type, t, elems, reps, tag, b, l);
            return;
        }
        fail("'{...} assignment pattern over this aggregate is not supported");
    }
    if (value_cat(type) == ValueCat::handle) {
        // A queue / dynamic-array member: deep-copy from another handle
        // lvalue. A pattern / concat / slice source needs an element-wise
        // build (same follow-up as the top-level queue assignment).
        if (nbt != nullptr) {
            // A queue / dynamic-array member of the aggregate: no deferred
            // analogue of HandleCopyNb exists (the record has no owned-handle
            // tail), so refuse rather than drop the timing.
            fail("intra-assignment timing on an aggregate with a queue or "
                 "dynamic-array member is not supported");
        }
        emit_handle_member_copy(lhs, rhs, type, tag, b, l);
        return;
    }
    if (is_leaf_ref(type)) {
        // A string leaf (a scalar plane leaf already returned at the top).
        if (nbt != nullptr) {
            // TODO: a string leaf under intra-assignment timing. Plain NBA
            // stages an OWNED deep copy per signal, which is sound only because
            // one signal has one pending same-instant value; an intra site can
            // have N in flight, so this needs the deferred record's owned-handle
            // tail class (docs/design/intra-assignment-timing.md, R3).
            fail("intra-assignment timing on a string target is not supported");
        }
        b.binary(tag, lhs, lower_expr(rhs, b), l);
        return;
    }
    // Aggregate RHS: another struct/array lvalue, or an aggregate-returning
    // call (the callee writes into a caller temp — the Call's lvalue — which
    // this copies into the target).
    const Expression& src = unwrap_conversion(rhs);
    if (t.kind == ir::Type::Kind::UnpackedArray && src.kind == ExpressionKind::StringLiteral) {
        if (!is_leaf_ref(t.element) || module_.pool.is_float(t.element) ||
            module_.pool.layout(t.element).width != 8) {
            // LRM 5.9 defines a string literal assignment only for "an
            // unpacked array of bytes", and slang's implicit conversion fires
            // only for Type::isByteArray() -- a `byte` element -- so a
            // non-byte element is rejected upstream and this is defensive.
            fail("a string literal is only assignable to an unpacked array of "
                 "bytes (LRM 5.9)");
        }
        emit_string_literal_array_store(lhs, type, t, src.as<StringLiteral>().getValue(), tag, b, l, nbt);
        return;
    }
    if (src.kind == ExpressionKind::Call) {
        const CallExpression& call = src.as<CallExpression>();
        // A user function returns an aggregate through a caller safe-temp whose
        // lvalue IS the call's value; a SYSTEM call that yields an aggregate --
        // today a queue pop of an aggregate element -- produces the same shape,
        // so both copy identically.
        const std::optional<ir::Index> cv =
          call.subroutine.index() == 0 ? lower_user_call(call, b) : std::optional<ir::Index>{ lower_expr(src, b) };
        if (!cv) {
            fail("aggregate assignment from a void call");
        }
        if (tag == ir::Inst::Tag::Store && is_trivially_copyable(type)) {
            b.agg_copy(lhs, *cv, type, l);
        } else {
            emit_agg_copy(lhs, *cv, type, tag, b, l, nbt);
        }
        return;
    }
    if (!is_struct_lvalue_shape(src)) {
        fail("unsupported unpacked struct rvalue (only '{...} patterns, struct "
             "variables, and function results are supported)");
    }
    const ir::Index src_lv = lower_lvalue(src, b);
    // A blocking copy of a trivially copyable aggregate is a single memcpy
    // of the storage (the byte image is the value). A heap-owning leaf
    // (string / dynamic array / queue) must copy per-leaf (its stores
    // deep-copy); a nonblocking copy always decomposes per-leaf (it defers).
    if (tag == ir::Inst::Tag::Store && is_trivially_copyable(type)) {
        b.agg_copy(lhs, src_lv, type, l);
        return;
    }
    emit_agg_copy(lhs, src_lv, type, tag, b, l, nbt);
}

// -- LRM 11.4.11 conditional operator over an aggregate operand --
//
// The clause's last legality rule is what puts an aggregate `?:` here: "For
// all other cases, the type of the first expression and second expression
// shall be equivalent (see 6.22.2)." slang enforces that upstream, so both
// arms and the result share one layout and the only question left is which
// arm's value reaches the destination.
//
// The `?:` whose result is an unpacked struct or a fixed unpacked array -- the
// aggregate ABI (is_agg_abi_ref) -- as the RHS emit_agg_store must branch on
// rather than evaluate. Deliberately no wider than that:
//
//   * A packed result never arrives; it returned through emit_agg_store's
//     ValueCat::scalar arm into the integral path, which already implements
//     this whole clause for a flat value (lower_lazy_conditional + Table
//     11-20). That covers a packed struct / union too, which is a flat bit
//     vector (LRM 7.2.1).
//   * A `string` leaf keeps its existing single-plane select. It is one plane
//     value with a working lower_expr path, and routing it through the
//     aggregate branch would buy nothing.
//   * A queue / dynamic-array / associative-array result keeps
//     emit_handle_member_copy, which refuses it loudly. The ambiguous half of
//     the clause is what blocks those: it needs both arms staged in temps of
//     the container's own type, and make_canonical_agg_temp builds aggregate
//     storage, not a container handle.
const ConditionalExpression* Lowerer::as_agg_conditional(const Expression& rhs, ir::Ref type) const {
    const Expression& e = unwrap_conversion(rhs);
    if (e.kind != ExpressionKind::ConditionalOp || !is_agg_abi_ref(type)) {
        return nullptr;
    }
    return &e.as<ConditionalExpression>();
}

// Whether a `?:` cond_predicate can evaluate to an ambiguous value at all,
// which is what decides whether LRM 11.4.11's ambiguous-condition rule is
// REACHABLE -- and so whether any of its machinery is emitted.
//
// The answer is fixed by lower_predicate, which is what builds the predicate:
// a string condition reduces StrLen and a real condition compares against
// 0.0, both definite ("a real condition is true iff nonzero -- a definite
// 1-bit result (reals carry no x/z)"); everything else reduces the operand's
// own bits with RedOr, which can be x exactly when those bits have an unknown
// plane to carry one. A 2-state type has none by construction (InternPool
// Type::Kind::Bit is "2-state scalar bit (0, 1)"), so for a `bit` / `int` /
// class-handle condition the entire ambiguous branch below is statically dead
// and none of it is built: no arm staging temps, no AggEq, no Table 7-1 fill.
// That is the case UVM takes (uvm_factory.svh:2318 conditions on a `bit`).
//
// A `matches` condition counts as possibly ambiguous even though lower_pattern
// assembles its result out of definite CaseEq tests. The polarity is
// deliberate and is the one commit "lower: a ?: arm is speculated only when it cannot trap" established for
// is_speculation_safe: an unclassified condition shape must fail CLOSED,
// because being conservative costs one dead compare while being wrong
// silently drops the clause's merge.
bool Lowerer::conditions_may_be_ambiguous(std::span<const ConditionalExpression::Condition> conds) {
    for (const auto& c : conds) {
        if (c.pattern != nullptr) {
            return true;
        }
        if (is_string_expr(*c.expr) || c.expr->type->isFloating()) {
            continue;
        }
        if (c.expr->type->isFourState()) {
            return true;
        }
    }
    return false;
}

// Lower `cond ? a : b` whose result is an aggregate, writing the selected arm
// into `lhs` (LRM 11.4.11).
//
// DESTINATION-DIRECTED: the chosen arm is built straight into `lhs` by a
// recursive emit_agg_store inside a Cond region, so a variable arm is one
// aggregate copy, a `'{...}` arm is built in place, a call arm writes through
// its own out-param, and no staging temp exists on either definite path.
// Nesting (`c1 ? x : c2 ? y : z`) falls out of that recursion, as does every
// arm shape emit_agg_store already accepts.
//
// Arm evaluation is CONDITIONAL, never speculative: "If cond_predicate is
// true, the operator returns the value of the first expression without
// evaluating the second expression; if false, it returns the value of the
// second expression without evaluating the first expression." Unlike the
// integral path there is nothing to trade that requirement against -- an
// aggregate copy is a sequence of stores whichever arm wins, so there is no
// branchless aggregate select to preserve and is_speculation_safe is not
// consulted here at all. A call arm, a trapping arm and a pure arm all take
// the same branch.
void Lowerer::emit_agg_conditional(ir::Index                    lhs,
                                   ir::Ref                      type,
                                   const ConditionalExpression& c,
                                   ir::Inst::Tag                tag,
                                   ir::Builder&                 b,
                                   ir::SourceLoc                l) {
    if (lower_type(*c.type) != type) {
        // TODO: an aggregate ternary whose own result type interns differently
        // from the destination's. 11.4.11 requires the two ARMS be equivalent
        // (6.22.2) to each other, not that either match the target, so slang
        // can hand over a distinct-but-equivalent type; every member / element
        // walk below addresses `type`, so a merely equivalent layout would need
        // a converting copy instead of this one.
        fail("an aggregate conditional whose result type differs from the "
             "assignment target's type is not supported");
    }
    // "If cond_predicate is true" -- evaluated ONCE, before either arm. The
    // pattern-aware form (`x matches pat &&& f ? a : b`) binds its variables
    // here, so both arms below see them.
    const ir::Index pred = lower_conditions(c.conditions, b, l);

    if (!conditions_may_be_ambiguous(c.conditions)) {
        // A cond_predicate that cannot be x or z makes the ambiguous-condition
        // rule unreachable, so it is not emitted: two regions and one Cond.
        b.open_region();
        emit_agg_store(lhs, type, c.left(), tag, b, l);
        const ir::ExtraSlice t_region = b.close_region();

        b.open_region();
        emit_agg_store(lhs, type, c.right(), tag, b, l);
        const ir::ExtraSlice f_region = b.close_region();

        b.cond(pred, t_region, f_region, l);
        return;
    }

    // A 4-state cond_predicate: three ways, in the clause's own order.
    // Case-equality against the definite bits gives the two definite branches,
    // exactly as lower_lazy_conditional does for an integral result; whatever
    // is left over is x or z and takes the ambiguous merge.
    const ir::Ref   bit      = make_int_type(1, true, false);
    const ir::Index one      = b.constant(module_.pool.put_int(bit, 1), l);
    const ir::Index zero     = b.constant(module_.pool.put_int(bit, 0), l);
    const ir::Index is_true  = b.binary(ir::Inst::Tag::CaseEq, pred, one, l);

    b.open_region();
    emit_agg_store(lhs, type, c.left(), tag, b, l);
    const ir::ExtraSlice t_region = b.close_region();

    b.open_region();
    const ir::Index is_false = b.binary(ir::Inst::Tag::CaseEq, pred, zero, l);

    b.open_region();
    emit_agg_store(lhs, type, c.right(), tag, b, l);
    const ir::ExtraSlice f_region = b.close_region();

    b.open_region();
    emit_agg_cond_ambiguous(lhs, type, *c.type, c, tag, b, l);
    const ir::ExtraSlice x_region = b.close_region();

    b.cond(is_false, f_region, x_region, l);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(is_true, t_region, else_region, l);
}

// The ambiguous-condition arm of an aggregate `?:` (LRM 11.4.11): "If
// cond_predicate evaluates to an ambiguous value (x or z), then both the first
// expression and the second expression shall be evaluated, and compared for
// logical equivalence as described in 11.4.5. If that comparison is true (1),
// the operator shall return either the first or second expression. Otherwise
// the operator returns a result based on the data types of the expressions."
//
// Both arms are staged into private temps rather than one of them being built
// straight into `lhs`: the destination may ALIAS an arm (`v = c ? y : v`), and
// writing arm 1 into `lhs` first would clobber the storage arm 2 still has to
// read. emit_agg_store is handed the destination's ir::Index and never the
// destination expression, so there is nothing here to test aliasing against
// the way array_copy_source can. It costs nothing where it matters: this is
// the cold path, reached only when a 4-state condition actually sits at x, and
// neither definite branch allocates a temp.
//
// The staging stores are BLOCKING even under a nonblocking assignment -- the
// temp is a private cell holding the arm as evaluated now, and it is the copy
// OUT of it that defers (LRM 10.4.2). Same rule array_rvalue_lvalue documents.
void Lowerer::emit_agg_cond_ambiguous(ir::Index                    lhs,
                                      ir::Ref                      type,
                                      const slang::ast::Type&      sv_type,
                                      const ConditionalExpression& c,
                                      ir::Inst::Tag                tag,
                                      ir::Builder&                 b,
                                      ir::SourceLoc                l) {
    const ir::Index ta = make_canonical_agg_temp(type, b, l);
    emit_agg_store(ta, type, c.left(), ir::Inst::Tag::Store, b, l);
    const ir::Index tb = make_canonical_agg_temp(type, b, l);
    emit_agg_store(tb, type, c.right(), ir::Inst::Tag::Store, b, l);

    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        emit_agg_cond_elementwise(lhs, type, sv_type, ta, tb, tag, b, l);
        return;
    }
    // Everything that is not an aggregate ARRAY is one unit: "For all other
    // data types, the value specified in Table 7-1 for the resulting type as
    // defined previously shall be returned." So an unpacked STRUCT whose arms
    // differ becomes its Table 7-1 default WHOLE -- not a member-by-member
    // merge, which is the array rule and only the array rule. (Verilator
    // returns the false arm here instead; see docs/TODO.md.)
    guard_agg_eq_members(type);
    const ir::Index eq = b.agg_eq(ta, tb, type, l);

    b.open_region();
    emit_agg_copy(lhs, ta, type, tag, b, l);
    const ir::ExtraSlice same = b.close_region();

    b.open_region();
    emit_table7_1_default(lhs, type, sv_type, tag, b, l);
    const ir::ExtraSlice diff = b.close_region();

    // "If that comparison is TRUE (1)". AggEq AND-reduces the per-leaf
    // compares with Land, which carries 11.4.5's X-propagation, so an x result
    // is not true and the test has to be the definite one.
    b.cond(b.binary(ir::Inst::Tag::CaseEq, eq, b.constant(module_.pool.put_int(make_int_type(1, true, false), 1), l), l),
           same, diff, l);
}

// The array half of the ambiguous-condition rule (LRM 11.4.11): "For
// aggregate array data types, except associative arrays, where both
// expressions contain the same number of elements their results shall be
// combined element by element. If the elements match, the element shall be
// returned. If they do not match, then the value specified in Table 7-1 for
// that element's type shall be returned."
//
// Per element, and NOT recursively. A mismatching element that is itself an
// array or a struct takes "the value specified in Table 7-1 for that
// element's type" -- that element's whole default -- and not a further
// element-wise or member-wise merge, because the clause names Table 7-1 for
// the element type rather than reapplying itself. So `int a[2][3]` whose
// sub-array [0] differs in one position defaults that entire sub-array.
//
// No whole-array compare precedes the walk. AggEq over the array is the
// AND-reduce of precisely these per-element compares (see lower_struct_eq), so
// the clause's "comparison is true (1)" holds iff every element matches, and
// in that case this walk already returns every element unchanged. Emitting the
// whole-array compare as well would be dead weight, not a fast path.
void Lowerer::emit_agg_cond_elementwise(ir::Index               lhs,
                                        ir::Ref                 type,
                                        const slang::ast::Type& sv_type,
                                        ir::Index               ta,
                                        ir::Index               tb,
                                        ir::Inst::Tag           tag,
                                        ir::Builder&            b,
                                        ir::SourceLoc           l) {
    // By value: interning below can reallocate the pool (see emit_agg_store).
    const ir::Type          t   = std::get<ir::Type>(module_.pool.get(type));
    const slang::ast::Type* est = sv_type.getCanonicalType().getArrayElementType();
    if (est == nullptr) {
        fail("an ambiguous aggregate conditional over an array with no element "
             "type is not supported");
    }
    const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
    for (uint32_t k = 0; k < n; ++k) {
        const int32_t coord =
          t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
        const ir::Index idx =
          b.constant(module_.pool.put_int(make_int_type(32, false, true),
                                          static_cast<uint64_t>(static_cast<int64_t>(coord))),
                     l);
        const ir::Index sa      = b.elem_select(ta, idx, type, l);
        const ir::Index sb      = b.elem_select(tb, idx, type, l);
        const ir::Index matched = agg_cond_elem_match(sa, sb, t.element, b, l);

        b.open_region();
        emit_agg_copy(b.elem_select(lhs, idx, type, l), sa, t.element, tag, b, l);
        const ir::ExtraSlice same = b.close_region();

        b.open_region();
        emit_table7_1_default(b.elem_select(lhs, idx, type, l), t.element, *est, tag, b, l);
        const ir::ExtraSlice diff = b.close_region();

        b.cond(matched, same, diff, l);
    }
}

// The definite "these two elements match" predicate for the element-by-element
// merge, under LRM 11.4.5 logical equivalence: an aggregate element compares
// with one AggEq (the same node a whole-aggregate `==` emits), a leaf element
// with the ordinary `==` for its own value kind. 11.4.5 propagates X and the
// clause returns the element only when the comparison is TRUE (1), so a
// compare that can be x is narrowed to a definite bit; a compare of 2-state
// (or real, or string) operands is already definite and needs no narrowing.
ir::Index Lowerer::agg_cond_elem_match(ir::Index a, ir::Index other, ir::Ref ety, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index one = b.constant(module_.pool.put_int(make_int_type(1, true, false), 1), l);
    if (is_agg_abi_ref(ety)) {
        guard_agg_eq_members(ety);
        return b.binary(ir::Inst::Tag::CaseEq, b.agg_eq(a, other, ety, l), one, l);
    }
    if (value_cat(ety) == ValueCat::handle) {
        // TODO: the ambiguous merge over a fixed array whose element is a
        // queue, dynamic array or associative array. The element compare is
        // the ordered element walk guard_agg_eq_members refuses for an
        // associative member, and a mismatching element's Table 7-1 fill would
        // have to clear a container per element.
        fail("an ambiguous aggregate conditional over a fixed unpacked array of "
             "queues, dynamic arrays or associative arrays is not supported "
             "(LRM 11.4.11)");
    }
    const ir::Index a_val     = b.unary(ir::Inst::Tag::Load, a, l);
    const ir::Index other_val = b.unary(ir::Inst::Tag::Load, other, l);
    const ir::Index cmp =
      b.binary(module_.pool.is_float(ety) ? ir::Inst::Tag::FEq : ir::Inst::Tag::Eq, a_val, other_val, l);
    if (module_.pool.layout(ety).domain != ir::Domain::Four) {
        return cmp;
    }
    return b.binary(ir::Inst::Tag::CaseEq, cmp, one, l);
}

// Write the LRM Table 7-1 (7.4.5) value for `ty` into `lv`.
//
// This is Table 7-1, NOT Table 6-7, and the two disagree -- which is why this
// is its own recursion rather than a call into emit_default_init. Table 7-1's
// rows, verbatim: 4-state integral `'x`; 2-state integral `'0`; "Enumeration"
// -> "Value specified in this table for the enumeration's base type";
// real/shortreal `0.0`; string `""`; class, interface class, event, chandle
// and virtual interface all `null`; "Variable-size unpacked array (dynamic,
// queue or associative)" -> "Array of size zero (no elements)"; "Fixed-size
// unpacked array" -> "Array, all of whose elements have the value specified in
// this table for that array's element type"; "Unpacked struct" -> "struct,
// each of whose members has the value specified in this table for that
// member's type, unless the member has an initial assignment as part of its
// declaration (see 7.2.2), in which case the member's value shall be as given
// by its initial assignment"; "Unpacked union" -> "Value specified in this
// table for the type of the first member of the union".
//
// Three rows where emit_default_init gives a different answer, so reusing it
// would have been silently wrong:
//
//   1. EVENT. emit_default_init binds a FRESH event (EventNew) because Table
//      6-7 says "New event" for a declaration, and two concurrent activations
//      of one task must not share a synchronization object. Table 7-1 says
//      `null`, which for clotho's event handle is the all-zero word --
//      InternPool Type::Kind::Event: "`null` is the all-zero handle, which
//      resolves to the reserved null object at slot 0". So this stores zero.
//   2. UNPACKED UNION. Table 7-1 wants the FIRST member's value, while
//      emit_default_init walks every member of an unpacked struct-or-union
//      alike. Refused below rather than guessed: clotho lays an unpacked union
//      out as one flat blob whose members are reached by part-select
//      (lower_member_lvalue), not the field-indexed MemberSelect this
//      recursion addresses members with.
//   3. MEMBER INITIALIZERS. emit_default_init has no notion of them at all;
//      Table 7-1 keeps a member's declared initial assignment in preference to
//      its type default. Applied inline below, one store per member, rather
//      than as a second overlay pass on top of a default that would then be
//      thrown away.
void Lowerer::emit_table7_1_default(ir::Index               lv,
                                    ir::Ref                 ty,
                                    const slang::ast::Type& sv_type,
                                    ir::Inst::Tag           tag,
                                    ir::Builder&            b,
                                    ir::SourceLoc           l) {
    const ir::Type          t  = std::get<ir::Type>(module_.pool.get(ty));
    const slang::ast::Type& ct = sv_type.getCanonicalType();
    if (ct.isUnpackedUnion()) {
        // Divergence 2 above, and it has to be detected on the SLANG type: an
        // unpacked union interns with is_packed SET (lower_type's
        // isUnpackedUnion arm lowers it "identically to a packed union"), so
        // the IR is_union flag never distinguishes one here and this would
        // otherwise fall through to the singular-leaf fill below.
        //
        // That fill is observably wrong for a union, which is why this refuses
        // rather than approximating. The blob's domain is the union of ALL
        // members (`four |= mt.isFourState()`), while Table 7-1 asks for the
        // FIRST member's value: for `union { int i; logic [7:0] w; }` the fill
        // writes all-x, and reading `.w` back yields `xx` where the first
        // member is 2-state and the table therefore requires '0. Writing only
        // the first member's low bits instead would leave the blob's high bits
        // undefined, and LRM 7.3 fixes no bit-level overlay for the unpacked
        // case to say what they should become.
        //
        // TODO: Table 7-1 for an unpacked union. Needs a decision on the high
        // bits above the first member's width, which the LRM does not make.
        fail("a Table 7-1 default for an unpacked union is not supported (LRM "
             "Table 7-1 returns the value of the union's FIRST member, and "
             "clotho's flat unpacked-union blob cannot represent that without "
             "defining the bits above it)");
    }
    switch (t.kind) {
        case ir::Type::Kind::Queue:
        case ir::Type::Kind::DynArray:
        case ir::Type::Kind::Assoc:
            // "Array of size zero (no elements)". None of the three clear ops
            // has a deferred form, so a nonblocking merge cannot use them.
            if (tag != ir::Inst::Tag::Store) {
                // TODO: nonblocking ambiguous merge whose Table 7-1 fill has to
                // empty a container (needs the same handle-aware NBA staging
                // path the sibling queue / dynamic-array NBA guards want).
                fail("nonblocking assignment of an ambiguous aggregate conditional "
                     "over a queue, dynamic array or associative array is not "
                     "supported");
            }
            if (t.kind == ir::Type::Kind::Queue) {
                b.queue_delete(lv, lv, /*has_idx=*/false, ty, l);
            } else if (t.kind == ir::Type::Kind::DynArray) {
                b.dyn_array_delete(lv, ty, l);
            } else {
                // Whole-value replacement, so the old pattern default goes with
                // the entries -- a.delete() would keep it.
                b.assoc_reset(lv, ty, l);
            }
            return;
        case ir::Type::Kind::Event:
            // Divergence 1 above: `null`, not a fresh event.
            b.binary(tag, lv, b.constant(module_.pool.put_int(ty, 0), l), l);
            return;
        case ir::Type::Kind::String:
            b.binary(tag, lv, b.constant(string_value_ref(""), l), l);
            return;
        case ir::Type::Kind::UnpackedArray: {
            const slang::ast::Type* est = ct.getArrayElementType();
            if (est == nullptr) {
                fail("a Table 7-1 default over an array with no element type is "
                     "not supported");
            }
            const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            for (uint32_t k = 0; k < n; ++k) {
                const int32_t coord =
                  t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
                const ir::Index idx =
                  b.constant(module_.pool.put_int(make_int_type(32, false, true),
                                                  static_cast<uint64_t>(static_cast<int64_t>(coord))),
                             l);
                emit_table7_1_default(b.elem_select(lv, idx, ty, l), t.element, *est, tag, b, l);
            }
            return;
        }
        case ir::Type::Kind::Struct:
            if (!t.is_packed) {
                uint32_t i = 0;
                for (const auto& f : ct.as<slang::ast::UnpackedStructType>()
                                       .membersOfType<slang::ast::FieldSymbol>()) {
                    const ir::Index fld = b.member_select(lv, i, ty, l);
                    // Divergence 3 above: "unless the member has an initial
                    // assignment as part of its declaration (see 7.2.2), in
                    // which case the member's value shall be as given by its
                    // initial assignment". Same coerce shape as
                    // emit_struct_field_defaults, which does this for the
                    // declaration path.
                    if (const Expression* init = f.getInitializer()) {
                        const ir::Index rhs = lower_expr(*init, b);
                        if (f.getType().getCanonicalType().isString()) {
                            b.binary(tag, fld, rhs, l);
                        } else {
                            b.binary(tag, fld, coerce(rhs, *init->type, lower_type(f.getType()), b, l), l);
                        }
                    } else {
                        // Re-fetch the row: lowering anything above can intern a
                        // type and invalidate a held struct_fields span (see
                        // InternPool::struct_fields).
                        emit_table7_1_default(
                          fld, module_.pool.struct_fields(t.fields_id)[i].type, f.getType(), tag, b, l);
                    }
                    ++i;
                }
                return;
            }
            break;
        default:
            break;
    }
    if (module_.pool.is_float(ty)) {
        // "real, shortreal" -> 0.0.
        b.binary(tag, lv, b.constant(module_.pool.put_real(ty, 0.0), l), l);
        return;
    }
    // What is left is a single plane value, and one branch covers every
    // remaining row: a 4-state integral is `'x`; a 2-state integral is `'0`; an
    // enum is "the value specified in this table for the enumeration's base
    // type", which is that same integral answer taken at the base type's own
    // domain (the enum interns as its base); a packed struct / union is
    // integral (LRM 7.2.1); and a class handle, interface-class handle, chandle
    // or virtual interface is `null`, which is all-zero -- already what the
    // 2-state side writes.
    const ir::Layout lay = module_.pool.layout(ty);
    const uint32_t   w   = lay.width == 0 ? 1 : lay.width;
    const ir::Ref dv = lay.domain == ir::Domain::Four ? svint_to_ref(slang::SVInt::createFillX(w, t.is_signed), ty)
                                                      : module_.pool.put_int(ty, 0);
    b.binary(tag, lv, b.constant(dv, l), l);
}

// Build a dynamic-array or queue handle from a flat positional element list
// (an assignment-pattern or bare unpacked-concatenation literal, LRM 7.5 /
// 7.10 / 11.4.14). A dynamic array is allocated to the element count and
// stored element-wise; a queue has no bulk-allocate op, so it is cleared
// (whole-handle assignment replaces, not appends) then rebuilt with
// push_back. `reps` is the `'{N{...}}` replicated-pattern multiplier (1 for
// a positional/keyed pattern or a bare concatenation, which has no repeat
// form of its own).
void Lowerer::emit_handle_literal_init(ir::Index                          lhs,
                                       ir::Ref                            type,
                                       const ir::Type&                    t,
                                       std::span<const Expression* const> elems,
                                       uint32_t                           reps,
                                       ir::Inst::Tag                      tag,
                                       ir::Builder&                       b,
                                       ir::SourceLoc                      l) {
    if (tag != ir::Inst::Tag::Store) {
        // TODO: nonblocking '{...}/concatenation literal into a queue or
        // dynamic array (needs a handle-aware NBA staging path — same gap as
        // the sibling top-level nonblocking dynamic-array/queue guards).
        fail("nonblocking assignment of a literal into a queue or dynamic array is not supported");
    }
    const uint32_t n     = static_cast<uint32_t>(elems.size());
    const uint32_t count = n * reps;
    if (t.kind == ir::Type::Kind::DynArray) {
        const ir::Index size = b.constant(module_.pool.put_int(make_int_type(32, false, false), count), l);
        b.dyn_array_new(lhs, size, lhs, /*has_init*/ false, type, l);
        for (uint32_t k = 0; k < count; ++k) {
            const ir::Index idx = b.constant(module_.pool.put_int(make_int_type(32, false, true), k), l);
            if (is_agg_abi_ref(t.element)) {
                // An aggregate element is copied into the slot, not stored as a
                // value: the slot IS its storage image.
                emit_agg_store(b.elem_select(lhs, idx, type, l), t.element, *elems[k % n], tag, b, l);
                continue;
            }
            const ir::Index slot = b.elem_select(lhs, idx, type, l);
            const ir::Index val  = lower_expr(*elems[k % n], b);
            b.binary(ir::Inst::Tag::Store, slot, val, l);
        }
        return;
    }
    b.queue_delete(lhs, lhs, /*has_idx*/ false, type, l);
    for (uint32_t k = 0; k < count; ++k) {
        b.queue_push(lhs, lower_container_element_value(*elems[k % n], type, b, l), /*front*/ false, type, l);
    }
}

// Build an associative array from a `'{...}` literal (LRM 7.9.11): an optional
// `default:` value that every absent key reads back, plus zero or more keyed
// entries. Whole-array assignment replaces, so the old entries AND the old
// default go first; the default is installed before the entries so an explicit
// key is never overwritten by it.
//
// slang keeps neither in `elements()`: the entries are `indexSetters`, already
// bound against the key and value types, and the default is `defaultSetter`,
// which it binds SELF-DETERMINED — so `'{default: "hello"}` over a `string`
// element arrives as a packed byte literal and has to be converted here. It
// also rejects a positional or replicated pattern over an associative array
// outright, so a keyed pattern is the only shape that arrives.
void Lowerer::emit_assoc_literal_init(ir::Index         lhs,
                                      ir::Ref           type,
                                      const Expression& rhs,
                                      ir::Inst::Tag     tag,
                                      ir::Builder&      b,
                                      ir::SourceLoc     l) {
    if (tag != ir::Inst::Tag::Store) {
        // TODO: nonblocking '{...} literal into an associative array (needs the
        // handle-aware NBA staging path, the same gap as the queue / dynamic
        // array literal and the whole-assoc NBA guard).
        fail("nonblocking assignment of a literal into an associative array is not supported");
    }
    if (rhs.kind != ExpressionKind::StructuredAssignmentPattern) {
        // TODO: unreachable through slang today (it rejects a positional or
        // replicated pattern over an associative array); guarded rather than
        // silently dropped if that ever changes.
        fail("only a keyed '{key:value} / '{default:value} literal is supported for an "
             "associative array");
    }
    const auto& sap = rhs.as<slang::ast::StructuredAssignmentPatternExpression>();
    if (!sap.memberSetters.empty() || !sap.typeSetters.empty()) {
        fail("member / type keys in an associative array literal are not supported");
    }
    const slang::ast::Type& elem = *rhs.type->getArrayElementType();
    if (!elem.isIntegral() && !elem.isFloating() && !elem.getCanonicalType().isString()) {
        // TODO: an aggregate value element needs the per-leaf emit_agg_store
        // path for each entry, not one scalar Store.
        fail("associative array literal with an aggregate value element is not supported");
    }
    b.assoc_reset(lhs, type, l);
    if (sap.defaultSetter != nullptr) {
        const ir::Index dv = lower_assoc_value(*sap.defaultSetter, elem, b, l);
        b.binary(ir::Inst::Tag::Store, b.assoc_default(lhs, type, l), dv, l);
    }
    for (const auto& setter : sap.indexSetters) {
        const ir::Index key = lower_assoc_key(*setter.index, type, b, l);
        const ir::Index val = lower_assoc_value(*setter.expr, elem, b, l);
        b.binary(ir::Inst::Tag::Store, b.assoc_select(lhs, key, type, l), val, l);
    }
}

// One value of an associative-array literal, converted to the value element
// type. A keyed value arrives already bound to it; the `default:` value does
// not (slang binds it self-determined), so this applies the assignment-context
// conversions LRM 6.16 / 10.9 would have: packed <-> string both ways, and the
// ordinary integral / real coercion otherwise.
ir::Index Lowerer::lower_assoc_value(const Expression&       e,
                                     const slang::ast::Type& elem,
                                     ir::Builder&            b,
                                     ir::SourceLoc           l) {
    if (elem.getCanonicalType().isString()) {
        if (is_string_expr(e)) {
            return lower_expr(e, b);
        }
        if (e.kind == ExpressionKind::StringLiteral) {
            return b.constant(string_value_ref(e.as<StringLiteral>().getValue()), l);
        }
        if (!e.type->isIntegral()) {
            fail("unsupported associative array literal value for a string element");
        }
        return b.unary(ir::Inst::Tag::StrFromPacked, lower_expr(e, b), l);
    }
    if (is_string_expr(e)) {
        if (!elem.isIntegral()) {
            fail("unsupported string value in an associative array literal");
        }
        return b.str_to_packed(lower_expr(e, b), lower_type(elem), l);
    }
    return coerce(lower_expr(e, b), *e.type, lower_type(elem), b, l);
}

// Per-leaf copy of one aggregate lvalue into another (both of `type`),
// recursing structs and (for a blocking copy) unrolling arrays. Reached only
// for a string-containing aggregate or a nonblocking copy; a nonblocking
// whole-array copy is guarded (element-wise deferral is a follow-up).
void Lowerer::emit_agg_copy(ir::Index            lhs,
                            ir::Index            rhs,
                            ir::Ref              type,
                            ir::Inst::Tag        tag,
                            ir::Builder&         b,
                            ir::SourceLoc        l,
                            const IntraNbTiming* nbt) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        // A nonblocking copy decomposes to per-element StoreNb: each element
        // write into the array rides the backend's deferred array-NBA queue.
        const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        for (uint32_t e = 0; e < n; ++e) {
            const int32_t coord =
              t.left <= t.right ? static_cast<int32_t>(t.left + e) : static_cast<int32_t>(t.left - e);
            const ir::Index idx =
              b.constant(module_.pool.put_int(make_int_type(32, false, true),
                                              static_cast<uint64_t>(static_cast<int64_t>(coord))),
                         l);
            const ir::Index dst = b.elem_select(lhs, idx, type, l);
            const ir::Index src = b.elem_select(rhs, idx, type, l);
            emit_agg_copy(dst, src, t.element, tag, b, l, nbt);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        const auto fields = module_.pool.struct_fields(t.fields_id);
        for (uint32_t i = 0; i < fields.size(); ++i) {
            const ir::Index dst = b.member_select(lhs, i, type, l);
            const ir::Index src = b.member_select(rhs, i, type, l);
            emit_agg_copy(dst, src, fields[i].type, tag, b, l, nbt);
        }
        return;
    }
    if (value_cat(type) == ValueCat::handle) {
        if (tag != ir::Inst::Tag::Store) {
            // TODO: nonblocking copy of a queue / dynamic-array member
            // (needs a handle-aware NBA staging path).
            fail("nonblocking assignment to an aggregate with a queue / "
                 "dynamic array member is not supported");
        }
        if (t.kind == ir::Type::Kind::Queue) {
            b.queue_copy(lhs, rhs, type, l);
        } else if (t.kind == ir::Type::Kind::Assoc) {
            b.assoc_copy(lhs, rhs, type, l);
        } else {
            b.dyn_array_copy(lhs, rhs, type, l);
        }
        return;
    }
    // A string leaf's nonblocking store needs no guard: StoreNb on a string place
    // stages an OWNED deep copy of the handle in the string NBA record range and
    // moves it into the leaf in the NBA region (emit_string_whole_nba), the same
    // machinery a whole-string `s <= t` uses — the record carries the leaf's
    // destination pointer, so a member or a fixed-array element works the same.
    // An INTRA-timed one does need a guard, and for the reason that machinery
    // works: the staged copy is per SIGNAL, sound only because a signal has one
    // pending same-instant value, while an intra site can have N in flight
    // (docs/design/intra-assignment-timing.md, R3).
    if (nbt != nullptr && value_cat(type) == ValueCat::string) {
        // TODO: a string leaf under intra-assignment timing -- needs the deferred
        // record's owned-handle tail class.
        fail("intra-assignment timing on a string target is not supported");
    }
    store_leaf(tag, nbt, lhs, b.unary(ir::Inst::Tag::Load, rhs, l), b, l);
}

// Deep-copy into a queue / dynamic-array member of an aggregate from an
// aggregate-store RHS element: the member's slot in the RHS lvalue. Only a
// handle-shaped source is supported (variable / member / element).
void Lowerer::emit_handle_member_copy(ir::Index         lhs,
                                      const Expression& rhs,
                                      ir::Ref           type,
                                      ir::Inst::Tag     tag,
                                      ir::Builder&      b,
                                      ir::SourceLoc     l) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    if (tag != ir::Inst::Tag::Store) {
        // TODO: nonblocking copy of a queue / dynamic-array member.
        fail("nonblocking assignment to an aggregate with a queue / dynamic "
             "array member is not supported");
    }
    const Expression& src = unwrap_conversion(rhs);
    const bool        src_is_handle_lvalue =
      src.kind == ExpressionKind::NamedValue || src.kind == ExpressionKind::HierarchicalValue ||
      src.kind == ExpressionKind::MemberAccess || src.kind == ExpressionKind::ElementSelect;
    if (src.kind == ExpressionKind::ConditionalOp) {
        // TODO: `?:` over a queue / dynamic-array / associative-array operand
        // (LRM 11.4.11). The two definite branches are reachable -- they are
        // just this same handle copy inside a Cond -- but the ambiguous half is
        // not: it has to stage BOTH arms in temps of the container's own type
        // to compare them (11.4.5), and make_canonical_agg_temp builds
        // aggregate storage, not a container handle. For an associative operand
        // the clause is explicit that the element-wise merge does not apply
        // ("For aggregate array data types, EXCEPT associative arrays"), so the
        // mismatch result is the whole-type Table 7-1 default, an empty array.
        // Refused whole rather than only for a 4-state condition, so the
        // supported set does not depend on the condition's domain.
        fail("a conditional operator over a queue, dynamic array or associative "
             "array operand is not supported (LRM 11.4.11)");
    }
    if (src.kind == ExpressionKind::NewArray) {
        // `T x = new[N]` / `new[N](src)` as a declaration initializer, or any
        // other non-statement store into a handle-typed target (LRM 7.5 /
        // 7.5.1): construct in place, not a copy of an existing handle --
        // new[] IS the constructor, so there is nothing to refuse here.
        // Only a dynamic array admits `new[]` (LRM 7.5); slang rejects it
        // against a queue or associative-array target at the type-check
        // level, so `t.kind` is always DynArray when src is a NewArray.
        // Shares emit_dyn_array_new_expr with the statement form
        // `arr = new[N]` (lower_dyn_array_assign), so sizing / truncate /
        // default-fill semantics live in exactly one place.
        emit_dyn_array_new_expr(lhs, src.as<slang::ast::NewArrayExpression>(), type, b, l);
        return;
    }
    if (!src_is_handle_lvalue) {
        // TODO: queue / dynamic-array member init from a '{...} pattern or
        // concatenation needs an element-wise build (dynamic-arrays doc
        // phase 4), same as the top-level queue assignment.
        fail("queue / dynamic array member assignment from a non-variable "
             "value is not supported");
    }
    if (t.kind == ir::Type::Kind::Assoc) {
        // LRM 7.9 admits no other source kind for an associative array
        // (slang's own type-check rejects anything else at the binding
        // level), so no source-kind dispatch is needed here.
        b.assoc_copy(lhs, lower_lvalue(src, b), type, l);
        return;
    }
    if (t.kind == ir::Type::Kind::Queue) {
        // LRM 7.10.2: a queue can be assigned from another queue, a
        // dynamic array, or a fixed-size unpacked array. Each source
        // shape needs its OWN runtime path -- feeding a mismatched
        // handle's bytes to the wrong rt_queue_* call reads past the
        // struct (an rt_dynarr has no ring fields at all) or reads
        // garbage reinterpreted as one (a fixed array has no handle
        // whatsoever, just flat storage) -- so the source's ACTUAL type,
        // not just its lvalue shape, decides the call.
        if (is_dyn_array(src)) {
            b.queue_copy_dynarr(lhs, lower_lvalue(src, b), type, l);
        } else if (is_unpacked_array(src)) {
            emit_fixed_array_into_queue(lhs, src, type, b, l);
        } else {
            b.queue_copy(lhs, lower_lvalue(src, b), type, l);
        }
        return;
    }
    // t.kind == DynArray (LRM 7.5.1): symmetric to the queue case above --
    // a queue or a fixed-size unpacked array source each need their own
    // runtime path, not dyn_array_copy's rt_dynarr-shaped one.
    if (is_queue(src)) {
        b.dyn_array_copy_queue(lhs, lower_lvalue(src, b), type, l);
    } else if (is_unpacked_array(src)) {
        const auto&   fty = src.type->getCanonicalType().as<slang::ast::FixedSizeUnpackedArrayType>();
        const auto    n   = static_cast<uint32_t>(fty.range.width());
        const ir::Ref i32 = make_int_type(32, false, true);
        b.dyn_array_new(lhs, b.constant(module_.pool.put_int(i32, n), l), lhs, /*has_init=*/false, type, l);
        emit_fixed_array_into_dynarr(lhs, src, type, n, b, l);
    } else {
        b.dyn_array_copy(lhs, lower_lvalue(src, b), type, l);
    }
}

// Whole-object blocking copy of one aggregate lvalue into another, for a port
// connection or net-declaration continuous copy (same fast/slow split as a
// blocking `a = b`). A trivially copyable aggregate is one memcpy (AggCopy);
// a heap-owning leaf forces the per-leaf deep copy.
void Lowerer::emit_port_agg_copy(ir::Index dst, ir::Index src, ir::Ref ty, ir::Builder& b, ir::SourceLoc l) {
    if (is_trivially_copyable(ty)) {
        b.agg_copy(dst, src, ty, l);
    } else {
        emit_agg_copy(dst, src, ty, ir::Inst::Tag::Store, b, l);
    }
}

// Downcast to an assignment pattern (`'{...}`) base if the expression is one
// of the three concrete pattern kinds. The base is abstract (no isKind), so
// it cannot go through `as`/`as_if`; the kind check gates the static_cast.
const slang::ast::AssignmentPatternExpressionBase* Lowerer::as_assignment_pattern(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::SimpleAssignmentPattern:
        case ExpressionKind::StructuredAssignmentPattern:
        case ExpressionKind::ReplicatedAssignmentPattern:
            return static_cast<const slang::ast::AssignmentPatternExpressionBase*>(&e);
        default:
            return nullptr;
    }
}

// Lower a packed (integral) assignment pattern `'{...}` — a packed struct,
// vector, or multi-dim packed array — to its flat value. slang resolves all
// three kinds (positional / keyed-structured / replicated) into an ordered
// `elements()` list; its own const-eval feeds that list to SVInt::concat with
// element 0 at the MSB, and Clotho's `concat` matches that order, so the value
// is the concatenation of the lowered elements.
//
// NOTE: slang orders a *structured* (keyed) packed-array pattern by numeric
// index ascending regardless of the declared range direction, then places the
// lowest index at the MSB — wrong for a descending array (`[3:0]`), where
// index 0 is the LSB. `'{0:x,3:y}` of `logic[3:0][7:0]` is y..x (index 3 at
// MSB), matching Verilator; slang mis-folds it to x..y. Clotho reverses the
// element order for a descending packed array so the concat is LRM-correct.
// Positional/replicated are source-ordered (first = leftmost = MSB) and
// already correct (docs/LRM_NOTES.md, note 11).
ir::Index Lowerer::lower_packed_pattern(const Expression& expr, ir::Builder& b) {
    const ir::SourceLoc    l   = loc(expr);
    const auto*            pat = as_assignment_pattern(expr);
    const auto             es  = pat->elements();
    std::vector<ir::Index> parts;
    parts.reserve(es.size());
    for (const Expression* e : es) {
        parts.push_back(lower_expr(*e, b));
    }
    if (expr.kind == ExpressionKind::StructuredAssignmentPattern && expr.type->isPackedArray()) {
        const slang::ConstantRange r = expr.type->getFixedRange();
        if (r.left > r.right) {
            std::reverse(parts.begin(), parts.end());
        }
    }
    if (expr.kind == ExpressionKind::ReplicatedAssignmentPattern) {
        const uint64_t count = const_uint(expr.as<ReplicatedAssignmentPatternExpression>().count(),
                                          "an assignment-pattern replication count (LRM 10.9.1)");
        return b.repl(b.concat(parts, l), static_cast<uint32_t>(count), l);
    }
    return b.concat(parts, l);
}

// 1-bit 4-state equality of two whole unpacked aggregates (struct or fixed
// unpacked array): one AggEq node the backend expands per leaf, AND-reducing
// the per-leaf `==` results with Land (the reduction carries LRM 11.4.5
// X-propagation). Both operands must be aggregate lvalues (a '{...} pattern
// compare is a follow-up); a heap-owning leaf (string is handled; queue /
// dynamic array is guarded) is rejected.
ir::Index Lowerer::lower_struct_eq(const Expression& lhs, const Expression& rhs, ir::Builder& b, ir::SourceLoc l) {
    const Expression& le = unwrap_conversion(lhs);
    const Expression& re = unwrap_conversion(rhs);
    // An aggregate lvalue: a variable / member / element, or a fixed
    // unpacked-array slice — a constant-bounds one is an ArrSlice lvalue, an
    // indexed one materializes into a temp (lower_array_operand). A `?:`
    // (LRM 11.4.11) is accepted too: lower_lvalue stages it into a canonical
    // aggregate temp, so `(c ? x : y) == z` compares anchored storage exactly
    // as a variable operand does.
    const auto agg_eq_operand = [](const Expression& e) {
        return is_struct_lvalue_shape(e) || e.kind == ExpressionKind::ConditionalOp ||
               (e.kind == ExpressionKind::RangeSelect && is_unpacked_array(e));
    };
    if (!agg_eq_operand(le) || !agg_eq_operand(re)) {
        fail("unpacked aggregate equality is only supported between aggregate variables");
    }
    const ir::Ref sty = lower_type(*lhs.type);
    guard_agg_eq_members(sty);
    const ir::Index lhs_agg = lower_array_operand(le, b);
    const ir::Index rhs_agg = lower_array_operand(re, b);
    return b.agg_eq(lhs_agg, rhs_agg, sty, l);
}

// Reject comparable-aggregate leaf shapes the backend AggEq expansion does
// not cover. An unpacked-array member is walked element-wise (supported);
// only a queue / dynamic-array leaf still needs the length+element compare.
// A REAL leaf works: the backend's leaf compare picks FEq for one (it used to
// hand a real plane to the integer compare and crash).
void Lowerer::guard_agg_eq_members(ir::Ref type) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    if (t.kind == ir::Type::Kind::Queue || t.kind == ir::Type::Kind::DynArray) {
        // A queue / dynamic-array leaf compares element-wise. An aggregate, a
        // nested container or an element wider than one word walks in the backend
        // (emit_container_eq_loop, which recurses through this same compare, so
        // only the element's own shape has to be checked here); anything narrower
        // is a scalar / real / string the flat runtime comparator reads directly.
        if (is_agg_abi_ref(t.element) || value_cat(t.element) == ValueCat::handle ||
            module_.pool.layout(t.element).width > 64) {
            guard_agg_eq_members(t.element);
        }
        return;
    }
    if (value_cat(type) == ValueCat::handle) {
        // TODO: equality of an aggregate with an associative-array / class
        // member (no ordered element walk yet).
        fail("equality of an aggregate with an associative-array member is not supported");
    }
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        guard_agg_eq_members(t.element);
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        for (const auto& f : module_.pool.struct_fields(t.fields_id)) {
            guard_agg_eq_members(f.type);
        }
    }
}

// Whether case equality (===/!==) on `type` is provably identical to
// logical equality (==/!=): true when no leaf can ever carry x/z, the one
// condition under which LRM 11.4.5's two rules (case: x/z compare
// directly, like a procedural case statement, 12.5; logical: x/z
// propagate to an unknown result) can disagree. A string leaf carries no
// x/z at all (LRM 6.16.9); a class handle or chandle is a definite
// pointer or null, never x/z. A 4-state integral leaf is not provably
// safe, and neither is a real/shortreal leaf (=== is outright illegal on
// one, Table 11-1) or an associative-array leaf (no case-compare
// implementation) -- the caller refuses case equality for any of those
// rather than silently reusing the X-propagating AggEq / container-
// compare result.
bool Lowerer::case_eq_matches_logical(ir::Ref type) const {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        for (const auto& f : module_.pool.struct_fields(t.fields_id)) {
            if (!case_eq_matches_logical(f.type)) {
                return false;
            }
        }
        return true;
    }
    if (t.kind == ir::Type::Kind::UnpackedArray || t.kind == ir::Type::Kind::Queue ||
        t.kind == ir::Type::Kind::DynArray) {
        return case_eq_matches_logical(t.element);
    }
    if (t.kind == ir::Type::Kind::String) {
        return true;
    }
    if (module_.pool.is_float(type) || value_cat(type) == ValueCat::handle) {
        return false;
    }
    return module_.pool.layout(type).domain != ir::Domain::Four;
}

// Whether a value of `type` could ever carry an x/z bit, recursing through
// every leaf kind the P3 assoc-key canonical serializer
// (emit_variable_agg_key_bytes, ir/llvm/AssocArrays.cpp) knows how to encode --
// struct / fixed array / queue / dynamic array / associative array nesting,
// down to a plain leaf. False for a leaf with no x/z state at all (string,
// class handle, chandle, event, virtual interface, real/shortreal, a
// 2-state integral); true for a genuinely 4-state-capable integral leaf.
// Used only to gate whole-associative-array logical equality (P4a): a flat
// byte compare on the canonical serialization is exact for a definite
// value, but cannot give the X-propagating answer LRM 11.4.5 wants when a
// value pair is unknown.
bool Lowerer::assoc_value_may_carry_xz(ir::Ref type) const {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        for (const auto& f : module_.pool.struct_fields(t.fields_id)) {
            if (assoc_value_may_carry_xz(f.type)) {
                return true;
            }
        }
        return false;
    }
    if (t.kind == ir::Type::Kind::UnpackedArray || t.kind == ir::Type::Kind::Queue ||
        t.kind == ir::Type::Kind::DynArray || t.kind == ir::Type::Kind::Assoc) {
        return assoc_value_may_carry_xz(t.element);
    }
    if (t.kind == ir::Type::Kind::String || module_.pool.is_float(type)) {
        return false;
    }
    return module_.pool.layout(type).domain == ir::Domain::Four;
}

// Scale a %t argument from its module's time unit to design ticks: the runtime
// converts ticks -> fs -> the $timeformat unit independent of the caller's
// timescale (LRM §20.4.3). A value in module units times this instance's
// time-unit multiplier yields ticks; the result is widened to 64 bits so the
// runtime reads the full tick count.
//
// The count keeps the operand's own sign. LRM 21.2.1.1 and 20.4.3 say nothing
// about a negative %t operand; the argument is a value, so -1 is a time of minus
// one unit (iverilog prints -1), and not 2**32 - 1 or 2**64 - 1. The sign is the
// operand's signedness, not its top bit, so an unsigned all-ones operand stays
// positive. The run-time-format path reaches the same count in
// coerce_dyn_arg (docs/LRM_NOTES.md, note 40).
//
// A count that does not fit 64 bits saturates at +-INT64_MAX ticks instead of
// wrapping, as a real operand's does (saturating_ticks): `longint unsigned u =
// 2**61` in a 1ns module over a 1ps tick is 2**61 * 1000 ticks, whose low 64 bits
// are 0, and a 128-bit operand must not be cut to its low word first.
ir::Index Lowerer::scale_to_ticks(const Expression& arg, ir::Index v, ir::Builder& b) {
    const ir::SourceLoc l = loc(arg);
    if (arg.type->isFloating()) {
        // A real (realtime) argument scales in the real domain, then rounds
        // to the nearest tick (FtoI rounds half away from zero); only
        // sub-tick fraction is lost, which the tick-based %t renderer
        // cannot represent anyway. A time beyond what 64-bit ticks hold
        // prints as the largest one, not as its low 64 bits (saturating_ticks).
        // A shortreal operand widens exactly first (LRM 6.12.2): the multiply and
        // saturating_ticks' range test are against `real` constants.
        ir::Index scaled = coerce(v, *arg.type, module_.pool.real_type, b, l);
        if (time_mul_ > 1) {
            const ir::Index mul =
              b.constant(module_.pool.put_real(module_.pool.real_type, static_cast<double>(time_mul_)), l);
            scaled = b.binary(ir::Inst::Tag::FMul, scaled, mul, l);
        }
        return saturating_ticks(scaled, 1, b, l, /*is_signed=*/true);
    }
    // The test is on the operand, at its own width or at 64 bits if narrower,
    // against the largest one whose product with the multiplier stays within the
    // ceiling, so the multiply cannot wrap either. Each sign saturates at the
    // ceiling of its own sign.
    const bool      is_signed = arg.type->isSigned();
    const ir::Ref   ty        = make_int_type(64, false, is_signed);
    const ir::Ref   wide_ty   = make_int_type(std::max<uint32_t>(64, arg.type->getBitWidth()), false, is_signed);
    const ir::Index wide      = b.resize(v, wide_ty, l);
    const uint64_t  mul       = std::max<uint64_t>(time_mul_, 1);
    const ir::Index limit     = b.constant(module_.pool.put_int(wide_ty, INT64_MAX / mul), l);
    ir::Index       ticks     = b.resize(wide, ty, l);
    if (mul > 1) {
        ticks = b.binary(ir::Inst::Tag::Mul, ticks, b.constant(module_.pool.put_int(ty, mul), l), l);
    }
    const ir::Index over = b.binary(ir::Inst::Tag::Gt, wide, limit, l);
    const ir::Index top  = b.constant(module_.pool.put_int(ty, INT64_MAX), l);
    ir::Index       known;
    if (!is_signed) {
        known = b.select(over, top, ticks, l);
    } else {
        const ir::Index neg_limit =
          b.binary(ir::Inst::Tag::Sub, b.constant(module_.pool.put_int(wide_ty, 0), l), limit, l);
        const ir::Index under = b.binary(ir::Inst::Tag::Lt, wide, neg_limit, l);
        const ir::Index bottom =
          b.constant(module_.pool.put_int(ty, static_cast<uint64_t>(-static_cast<int64_t>(INT64_MAX))), l);
        known = b.select(over, top, b.select(under, bottom, ticks, l), l);
    }
    // $time and $stime read the simulation clock, which is never unknown, though
    // their `time` type is 4-state: the commonest %t operand pays nothing extra.
    const auto* call = arg.as_if<CallExpression>();
    if (!arg.type->isFourState() || (call != nullptr && call->isSystemCall() &&
                                     (call->getSubroutineName() == "$time" || call->getSubroutineName() == "$stime"))) {
        return known;
    }

    // An operand with an x or z bit is not a time, so no tick count stands for it:
    // the argument becomes a 64-bit 4-state word the renderer classifies with the
    // %d rule (LRM 21.2.1.3, docs/LRM_NOTES.md note 41): all bits x is all x, all z
    // is all z, otherwise one x bit when any bit is x, else one z bit. The counts
    // are the operand's own, at its own width, which the 64-bit word could not
    // carry.
    const ir::Ref  ty4   = make_int_type(64, true, is_signed);
    const ir::Ref  cnt_t = make_int_type(32, false, true);
    const uint32_t w     = arg.type->getBitWidth();
    const auto     fill  = [&](bool is_z) {
        slang::SVInt k = is_z ? slang::SVInt::createFillZ(64, is_signed) : slang::SVInt::createFillX(64, is_signed);
        return b.constant(svint_to_ref(k, ty4), l);
    };
    // One x (z) bit and the rest zero: fromDigits takes the digits most significant first.
    const auto one_bit = [&](bool is_z) {
        std::vector<slang::logic_t> digits(64, slang::logic_t(0));
        digits.back() = is_z ? slang::logic_t::z : slang::logic_t::x;
        return b.constant(svint_to_ref(slang::SVInt::fromDigits(64,
                                                                slang::LiteralBase::Binary,
                                                                is_signed,
                                                                /*anyUnknown=*/true,
                                                                digits),
                                       ty4),
                          l);
    };
    const ir::Index nx    = b.count_bits(v, ir::CountX, l);
    const ir::Index nz    = b.count_bits(v, ir::CountZ, l);
    const ir::Index width = b.constant(module_.pool.put_int(cnt_t, w), l);
    const ir::Index zero  = b.constant(module_.pool.put_int(cnt_t, 0), l);
    ir::Index       cls   = b.select(b.binary(ir::Inst::Tag::Ne, nx, zero, l), one_bit(false), one_bit(true), l);
    cls                   = b.select(b.binary(ir::Inst::Tag::Eq, nz, width, l), fill(true), cls, l);
    cls                   = b.select(b.binary(ir::Inst::Tag::Eq, nx, width, l), fill(false), cls, l);
    return b.select(b.unary(ir::Inst::Tag::IsUnknown, v, l), cls, b.resize(known, ty4, l), l);
}

// Whether a nonblocking intra-assignment target's select chain resolves
// through storage the deferred record can address. The record reuses plain
// NBA's lvalue encoding, so the supported chain is exactly what that
// encoding covers: packed bit / part / sub-vector selects, fixed
// unpacked-array element selects, and packed or unpacked struct fields. A
// container element (string, queue, dynamic or associative array) is
// excluded because plain NBA cannot encode one either, so there is nothing
// to reuse.
bool Lowerer::intra_nb_target_path(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            return true;
        case ExpressionKind::ElementSelect: {
            const Expression& base = e.as<ElementSelectExpression>().value();
            if (!base.type->isIntegral() && !is_unpacked_array(base)) {
                return false;
            }
            return intra_nb_target_path(base);
        }
        case ExpressionKind::RangeSelect: {
            // A part-select of a packed value. An unpacked-array slice names
            // several elements, which one record cannot carry.
            const Expression& base = e.as<RangeSelectExpression>().value();
            return base.type->isIntegral() && intra_nb_target_path(base);
        }
        case ExpressionKind::MemberAccess: {
            // A packed or unpacked struct field. A class property has no
            // signal storage and is rejected ahead of the timing block.
            const MemberAccessExpression& ma = e.as<MemberAccessExpression>();
            if (ma.value().type->getCanonicalType().isVirtualInterface()) {
                // `vif.sig <= #d x` (LRM 25.9). The member is an interface
                // SIGNAL and the handle picks which INSTANCE's storage it names,
                // so the record cannot re-derive the destination at commit from
                // the representative instance's static address. It carries the
                // resolved destination pointer and instance context instead
                // (SignalSlot::intra_ptr), captured when the statement executes
                // -- which is not a convenience but the rule, LRM 10.4.2: "If
                // variable_lvalue requires an evaluation, such as an index
                // expression, class handle, or virtual interface reference, it
                // shall be evaluated at the same time as the expression on the
                // right-hand side."
                return true;
            }
            return ma.member.as_if<FieldSymbol>() != nullptr && intra_nb_target_path(ma.value());
        }
        default:
            return false;
    }
}

// Whether `x <= #d y` / `x <= @e y` can lower to this target (LRM 9.4.5).
// One record carries one scalar value, so the target must name a single
// integral (a packed struct or union included) or real object; a whole
// unpacked array or struct would have to spread the sampled RHS across one
// record per leaf.
bool Lowerer::intra_nb_target_supported(const Expression& e) {
    if ((!e.type->isIntegral() && !e.type->isFloating()) || e.type->isUnpackedArray()) {
        return false;
    }
    return intra_nb_target_path(e);
}

// Whether a nonblocking intra-assignment target DECOMPOSES into several leaf
// stores instead of encoding as one deferred record (LRM 9.4.5). These are
// exactly the shapes plain NBA already scatters -- a concatenation, a streaming
// concatenation, an assignment-pattern lvalue, and a whole unpacked struct or
// array -- so the intra form is the same walk emitting one record per leaf
// (docs/design/intra-assignment-timing.md, R1).
//
// A whole container (queue / dynamic array / associative array) is deliberately
// NOT one of them: it is a single owned handle, not N leaves, and is refused
// further down for want of the record's owned-handle tail class.
bool Lowerer::intra_nb_target_decomposes(const Expression& e) {
    if (e.kind == ExpressionKind::Concatenation || e.kind == ExpressionKind::Streaming ||
        as_assignment_pattern(e) != nullptr) {
        return true;
    }
    if (is_dyn_array(e) || is_queue(e) || is_assoc(e)) {
        return false;
    }
    return is_aggregate(*e.type) || is_unpacked_array(e);
}

// Lower the EVENT half of a nonblocking intra-assignment's timing control
// (`<= @e` / `<= repeat(N) @e`) once, into the timing every leaf of the
// statement shares. Empty means "no timing at all": LRM 9.4.5 makes a
// compile-time non-positive repeat count a plain `<=`, so the caller emits an
// ordinary StoreNb and the event expression is never executed.
std::optional<Lowerer::IntraNbTiming> Lowerer::lower_intra_nb_event(const AssignmentExpression& a, ir::Builder& b) {
    const TimingControl& tc = *a.timingControl;
    const ir::SourceLoc  l  = loc(a);
    const TimingControl* evt = &tc;
    IntraNbTiming        t;
    t.is_event = true;
    if (tc.kind == TimingControlKind::RepeatedEvent) {
        const RepeatedEventControl& rec = tc.as<RepeatedEventControl>();
        evt = &rec.event;
        // LRM 9.4.5: "If the repeat count literal, or signed variable holding
        // the repeat count, is less than or equal to 0 at the time of
        // evaluation, the assignment occurs as if there is no repeat
        // construct", and the clause's example fixes what that means --
        // "repeat (-3) @ (event_expression) // will not execute
        // event_expression". So a non-positive count stores with no wait,
        // which for the nonblocking form is the current instant's NBA region.
        // The record's countdown is uint32_t and rt_intra_event_fire tests
        // `--count == 0`, so without this a count of 0 or -1 would wait 2**32
        // edges rather than none.
        //
        // The test is `count < 1` IN THE COUNT'S OWN SIGNEDNESS, which is the
        // clause's rule exactly: for a signed count it is `count <= 0`, and for
        // an unsigned one it degenerates to `count == 0`. That asymmetry is the
        // clause's own, stated in the same example -- "repeat (a) @
        // (event_expression) // if a is assigned -3, it will execute the
        // event_expression if a is declared as an unsigned variable, but not if
        // a is signed" -- because an unsigned `a` holding -3 is 4294967293, a
        // large positive count. Zero is zero in both signednesses and clamps
        // either way. Measured against iverilog 13: a literal 0, an unsigned
        // variable holding 0 and a signed variable holding 0 all store in the
        // arming instant, while an unsigned variable holding -3 waits.
        const ir::Ref cty =
          rec.expr.type->isFloating() ? make_int_type(64, false, true) : lower_type(*rec.expr.type);
        slang::ast::ASTContext     ectx(*scope_, slang::ast::LookupLocation::max);
        slang::ast::EvalContext    ev(ectx);
        const slang::ConstantValue cv = rec.expr.eval(ev);
        std::optional<bool>        folded;
        if (cv.isInteger() && !cv.integer().hasUnknown()) {
            folded = rec.expr.type->isSigned() ? cv.integer().as<int64_t>().value_or(1) <= 0
                                               : cv.integer().as<uint64_t>().value_or(1) == 0;
        } else if (cv.isReal()) {
            folded = cv.real() <= 0.0;
        }
        if (folded && *folded) {
            // Folded away: no repeat construct, so neither the count nor the
            // event expression is lowered at all. Nothing here validates the
            // event, deliberately -- an unexecuted `@(a or b)` or `@(e iff c)`
            // must not be refused when the LRM says it never runs.
            return std::nullopt;
        }
        t.count = lower_expr(rec.expr, b);
        if (rec.expr.type->isFloating()) {
            // A real count rounds half away from zero into the countdown
            // (LRM 6.12.1's real -> integral conversion, which coerce ends
            // in), in the same signed 64-bit type the repeat statement
            // counts a real in (lower_repeat); the deferred record's own
            // 32-bit countdown then narrows it exactly as it narrows an
            // integral longint count.
            t.count = coerce(t.count, *rec.expr.type, cty, b, l);
        }
        if (!folded) {
            ir::Index nonpos =
              b.binary(ir::Inst::Tag::Lt, t.count, b.constant(module_.pool.put_int(cty, 1), l), l);
            if (rec.expr.type->isFourState()) {
                // An x/z count makes the comparison ambiguous; narrow it to the
                // definite bit so the rule is "clamp only when the count is
                // KNOWN non-positive" rather than whatever an x predicate
                // happens to do. 9.4.5 says nothing about an unknown count.
                nonpos = b.binary(ir::Inst::Tag::CaseEq, nonpos,
                                  b.constant(module_.pool.put_int(make_int_type(1, true, false), 1), l), l);
            }
            t.nonpos = nonpos;
        }
    } else {
        // Plain `@e` waits for one occurrence.
        t.count = b.constant(module_.pool.put_int(make_int_type(32, false, false), 1), l);
    }
    if (!is_event_control(*evt)) {
        fail("nonblocking intra-assignment requires a delay or event control");
    }
    std::vector<ir::WaitEvent> events;
    if (collect_wait_events(*evt, events) != nullptr) {
        // TODO: `iff` on a nonblocking intra-assignment event trigger —
        // the deferred-record path (store_nb_event) has no guard hook.
        fail("iff qualifier on a nonblocking intra-assignment event is not supported");
    }
    if (events.empty()) {
        fail("nonblocking intra-assignment requires a delay or event control");
    }
    // Duplicate (signal, edge) pairs are collapsed. The record is registered
    // once per listed trigger, so `@(posedge a or posedge a)` would otherwise
    // sit on one trigger's list twice and count a single edge down twice.
    std::vector<ir::IntraTrigger> trigs;
    trigs.reserve(events.size());
    for (const ir::WaitEvent& ev : events) {
        const ir::IntraTrigger it{ ev.signal, static_cast<uint32_t>(ev.edge) };
        const bool             dup = std::any_of(trigs.begin(), trigs.end(), [&](const ir::IntraTrigger& o) {
            return o.signal == it.signal && o.edge == it.edge;
        });
        if (!dup) {
            trigs.push_back(it);
        }
    }
    t.trigs = b.intra_trigger_run(trigs);
    return t;
}

// Lower a nonblocking intra-assignment's whole timing control once, for a
// target that decomposes into N leaves. Empty means the timing folded away and
// the statement is an ordinary `<=`: a compile-time `#0` (already the current
// instant's NBA region -- keeping it on the block-commit path preserves
// execution-order tie-breaking, see the scalar `#0` note in lower_assign), or a
// compile-time non-positive repeat count.
std::optional<Lowerer::IntraNbTiming> Lowerer::lower_intra_nb_timing(const AssignmentExpression& a, ir::Builder& b) {
    const TimingControl& tc = *a.timingControl;
    if (tc.kind != TimingControlKind::Delay) {
        return lower_intra_nb_event(a, b);
    }
    const Expression& delay_expr = tc.as<DelayControl>().expr;
    if (intra_nb_delay_is_const_zero(delay_expr)) {
        return std::nullopt;
    }
    return lower_intra_nb_delay(delay_expr, b);
}

// Lower an intra-assignment `#d` that is not a compile-time zero into its
// timing. `zero` is set unless `d` is an integer constant known to be nonzero:
// anything else (a variable, a real that may round to zero ticks) can be zero at
// run time, and then the store is a plain `<=` (store_leaf). A known nonzero
// constant, the usual `<= #5`, adds nothing to the IR.
Lowerer::IntraNbTiming Lowerer::lower_intra_nb_delay(const Expression& delay_expr, ir::Builder& b) {
    IntraNbTiming t;
    t.delay = scaled_delay(delay_expr, b);
    slang::ast::ASTContext     ectx(*scope_, slang::ast::LookupLocation::max);
    slang::ast::EvalContext    ev(ectx);
    const slang::ConstantValue cv = delay_expr.eval(ev);
    const bool                 nonzero_const =
      cv.isInteger() && !cv.integer().hasUnknown() && cv.integer().as<uint64_t>().value_or(0) != 0;
    if (!nonzero_const) {
        const ir::SourceLoc l = loc(delay_expr);
        t.zero                = b.binary(
          ir::Inst::Tag::Eq, t.delay, b.constant(module_.pool.put_int(make_int_type(64, false, false), 0), l), l);
    }
    return t;
}

// Whether an intra-assignment `#d` is a COMPILE-TIME zero, a delay that is 0 or
// has an x or z bit (LRM 9.4.1, see scaled_delay). Such a store lands
// in the current instant's NBA region either way, so it is lowered as a plain
// `<=`: that keeps its ordering against the same signal's other NBAs by
// statement-execution order (LRM 10.4.2) through the one block-commit path,
// instead of splitting the signal across block-commit and the deferred record
// path, whose commit order is fixed rather than execution order.
bool Lowerer::intra_nb_delay_is_const_zero(const Expression& delay_expr) {
    slang::ast::ASTContext     ectx(*scope_, slang::ast::LookupLocation::max);
    slang::ast::EvalContext    ev(ectx);
    const slang::ConstantValue cv = delay_expr.eval(ev);
    if (!cv.isInteger()) {
        return false;
    }
    if (cv.integer().hasUnknown()) {
        return true;
    }
    const auto v = cv.integer().as<uint64_t>();
    return v && *v == 0;
}

// Reject the one nonblocking-assignment target a subroutine (task or function)
// cannot stage: a frame local or formal argument (both live in localmap). These
// have no module-signal storage, and nba_target in the backend only walks to a
// Signal root, so there is no shadow to defer the write into. A module signal,
// static local, or unpacked-array element is fine: nba_classify walks subroutine
// bodies, so a signal gets shadow storage and an array element defers through the
// global runtime queue exactly as it does from a process.
void Lowerer::check_subroutine_nba_target(const Expression& lhs) {
    switch (lhs.kind) {
        case ExpressionKind::Concatenation:
            for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                check_subroutine_nba_target(*op);
            }
            return;
        case ExpressionKind::Conversion:
            check_subroutine_nba_target(lhs.as<ConversionExpression>().operand());
            return;
        case ExpressionKind::ElementSelect:
            check_subroutine_nba_target(lhs.as<ElementSelectExpression>().value());
            return;
        case ExpressionKind::RangeSelect:
            check_subroutine_nba_target(lhs.as<RangeSelectExpression>().value());
            return;
        case ExpressionKind::MemberAccess: {
            const MemberAccessExpression& ma = lhs.as<MemberAccessExpression>();
            if (ma.value().type->getCanonicalType().isVirtualInterface()) {
                // `vif.sig <= x` (LRM 25.9 with 10.4.2): the target is the SIGNAL in
                // whichever instance the handle names. That signal has ordinary
                // module storage and stages through the deferred-record path like
                // every other nonblocking write through a handle (collect_vif_nba
                // marks the member, emit_array_nba stages it with the instance
                // resolved now). The handle is only READ here, to choose the
                // instance, so ITS storage class is not the assignment's target --
                // walking past it reported whatever the handle happened to live in
                // (a class property, which is the UVM driver shape `input_vif vif;`,
                // or a frame local) and named a construct the statement never wrote.
                return;
            }
            check_subroutine_nba_target(ma.value());
            return;
        }
        case ExpressionKind::NamedValue: {
            const ValueSymbol& vs = lhs.as<NamedValueExpression>().symbol;
            if (fn_ctx_->localmap.contains(&vs)) {
                // TODO: nonblocking assignment to a subroutine local / formal argument.
                fail("nonblocking assignment to a subroutine-local variable is not supported");
            }
            if (vs.kind == SymbolKind::ClassProperty) {
                // An UNQUALIFIED property write inside a method (`p <= x`, no
                // `this.`): the same gap the explicit `obj.p <= x` form reports in
                // lower_assign, which only sees a MemberAccess. Located here
                // instead of reaching the backend as an unaddressable NBA root
                // (which failed with no source location for every type).
                // TODO: shared with the class-property NBA gap — an object body is
                // a heap block with no per-object shadow.
                fail("nonblocking assignment to a class property is not supported");
            }
            return;
        }
        default:
            // TODO: an unrecognized nonblocking lvalue shape inside a subroutine.
            fail("unsupported nonblocking-assignment target in a subroutine");
    }
}

// Lower an assignment. Returns the raw stored value (the RHS as written,
// before LHS-type truncation) so an assignment used as an rvalue (LRM 11.3.1,
// `y = (x = 5)`) can recover its value; the caller truncates to the LHS type.
// Statement callers discard the return.
ir::Index Lowerer::lower_assign(const AssignmentExpression& a, ir::Builder& b) {
    // Gate ordinary writes to a forced/assigned variable (LRM 10.6.2). The write
    // predicate is "is this lvalue's root a force target?" — the root, not the
    // written shape, carries the site state, so a whole-variable write, a
    // bit/part/element-select of it, a packed struct/union member of it, and a
    // compound update all take the same gate.
    if (const ValueSymbol* root = lvalue_root(a.left())) {
        if (root->kind == SymbolKind::ClockVar) {
            // A synchronous drive (LRM 14.16), which is neither an ordinary store
            // nor an ordinary nonblocking one: the target is written by the
            // clocking block, in the Re-NBA region of its clocking event. slang
            // has already required the `<=` form (ClockVarSyncDrive) and rejected
            // a write to an `input` clockvar.
            lower_clocking_drive(root->as<ClockVarSymbol>(), a, b);
            return ir::Index{};
        }
        if (ForceInfo* fi = force_info_of(*root)) {
            if (a.timingControl != nullptr) {
                // TODO: intra-assignment timing (`x = #5 e` / `x <= @e2 e`) on a
                // write to a force target. LRM 10.6.2 suppresses the update, which
                // for a delayed assignment is decided when it commits, not when the
                // RHS is sampled; the gate here would have to ride the deferred
                // record (store_nb_delay / store_nb_event have no predicate hook)
                // and gated_store would otherwise silently drop the timing control.
                fail("intra-assignment timing on a write to a force-target variable "
                     "is not supported");
            }
            return gated_store(a, *fi, b);
        }
    }
    if (a.left().kind == ExpressionKind::MemberAccess &&
        a.left().as<MemberAccessExpression>().value().type->getCanonicalType().isVirtualInterface() &&
        a.left().as<MemberAccessExpression>().member.kind == SymbolKind::ClockVar) {
        // `vif.cb.d <= x` (LRM 14.16 through 25.9.1): a synchronous drive, not a
        // nonblocking assignment -- it stages nothing in the NBA shadow. Must be
        // tested before the NBA guard below, which would otherwise claim it:
        // lvalue_root of a handle access is the HANDLE variable, so the ordinary
        // ClockVar dispatch above never sees it.
        lower_vif_clocking_drive(a.left().as<MemberAccessExpression>(), a, b);
        return ir::Index{};
    }
    if (a.isNonBlocking() && a.left().kind == ExpressionKind::MemberAccess &&
        a.left().as<MemberAccessExpression>().member.kind == SymbolKind::ClassProperty) {
        // TODO: nonblocking assignment to a class property — legal SV (LRM 6.21
        // bans only continuous / procedural continuous assignments to a
        // non-static property; 10.4.2 bans NBA to automatics and dynamic-array
        // elements, and a property is neither). The aggregate NBA staging path
        // shadows a signal's storage, but an object body is a heap block with
        // no per-object shadow (docs/design/classes.md §8). Distinct from
        // `vif.sig <= x`, whose target is an interface SIGNAL, not a property;
        // that form is supported and never reaches this guard.
        fail("nonblocking assignment to a class property is not supported");
    }
    // Nonblocking intra-assignment timing (LRM 9.4.5) on a target that
    // DECOMPOSES -- a concatenation, a streaming concatenation, an
    // assignment-pattern lvalue, or a whole unpacked struct / array. Plain NBA
    // already scatters each of these into N ordinary StoreNbs over shapes the
    // deferred record encodes, so the intra form is the same walk emitting one
    // record per leaf; the dispatch sites below thread `nbt` into that walk.
    //
    // Lowered ONCE, here: 9.4.5 gives the statement one delay (or one repeat
    // count), not one per leaf, so all N records share this evaluation. The
    // leaves then take consecutive `seq` stamps in walk order, which is what
    // makes a self-overlapping target (`{a, a[0]} <= #5 x`) land exactly as its
    // plain-NBA form does -- one answer to explain, not two.
    //
    // The whole-SCALAR case is deliberately not routed through here: it keeps
    // lowering its RHS before its delay, the order it has always used.
    const bool                   intra_decomposed = a.isNonBlocking() && a.timingControl != nullptr &&
                                   intra_nb_target_decomposes(a.left());
    std::optional<IntraNbTiming> intra_timing;
    const IntraNbTiming*         nbt = nullptr;
    if (intra_decomposed) {
        intra_timing = lower_intra_nb_timing(a, b);
        if (intra_timing) {
            nbt = &*intra_timing;
        }
    }
    if (const auto* pat = as_assignment_pattern(a.left())) {
        // `'{a, b, c} = rhs` (LRM 10.9): a positional assignment-pattern
        // lvalue. Must be intercepted ahead of the is_aggregate /
        // is_unpacked_array dispatch below -- a.left().type mirrors
        // whatever aggregate/packed type slang assigned the pattern, which
        // would otherwise misroute into the ordinary whole-object paths and
        // hit lower_lvalue's default case (a pattern has no single address).
        if (a.left().kind != ExpressionKind::SimpleAssignmentPattern) {
            fail_keyed_pattern_lvalue();
        }
        if (a.isCompound()) {
            // LRM 10.8's assignment-like contexts do not include
            // operator_assignment, so an assignment pattern cannot appear
            // on the left of one.
            fail("compound assignment to an assignment-pattern lvalue is not "
                 "legal SystemVerilog");
        }
        if (a.timingControl != nullptr && !a.isNonBlocking()) {
            // TODO: intra-assignment timing control on a pattern lvalue, BLOCKING
            // form. Unlike the nonblocking form below, this one has to reorder the
            // sampled RHS across a real suspend, and the pattern walk resolves each
            // member's lvalue as it goes -- so the addresses would have to be
            // spilled across the suspend too (the same obstacle as `s = #5 t`).
            fail("intra-assignment timing control on an assignment-pattern "
                 "lvalue is not supported");
        }
        const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
        return lower_pattern_assign(*pat, a.right(), tag, b, loc(a), nbt);
    }
    if (is_aggregate(*a.left().type)) {
        // Whole unpacked-struct assignment: decompose to per-member stores
        // (blocking or nonblocking). Each member commits independently, which
        // is whole-struct NBA semantics.
        const ir::SourceLoc l = loc(a);
        if (a.isCompound()) {
            fail("compound assignment to an unpacked struct is not supported");
        }
        if (a.timingControl != nullptr && !a.isNonBlocking()) {
            // TODO: intra-assignment timing on a whole unpacked aggregate,
            // BLOCKING form -- `s = #5 t`. The aggregate store path resolves the
            // LHS before the RHS, so hoisting the sample across the suspend
            // reorders two evaluations 9.4.5 pins in the other order; the fix is a
            // lowering discipline (spill the LHS address AND the RHS before the
            // suspend), not a record. The nonblocking form decomposes below.
            fail("intra-assignment timing on a whole unpacked struct or array target "
                 "is not supported");
        }
        const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
        emit_struct_store(lower_lvalue(a.left(), b), lower_type(*a.left().type), a.right(), tag, b, l, nbt);
        return true_const(b, l);
    }
    if (a.isCompound()) {
        return lower_compound_assign(a, b);
    }
    // A decomposing nonblocking target (a concatenation, a streaming
    // concatenation, or a whole unpacked array -- a pattern lvalue and a whole
    // unpacked struct already returned above) skips this block entirely: its
    // timing is in `nbt` and rides the ordinary store dispatch below. `nbt` is
    // null there too when the timing FOLDED to a plain `<=` (a compile-time `#0`
    // or a non-positive repeat count), which is why the flag is tested and not
    // the pointer.
    if (a.timingControl != nullptr && !intra_decomposed) {
        // Intra-assignment timing (LRM 9.4.5): sample the RHS now, suspend on
        // the delay/event, then store the sampled value. The blocking form is a
        // pure reorder over the existing Delay/Wait suspends — the sampled RHS
        // is an SSA value live across the suspend, which CoroSplit spills into
        // the process frame. The nonblocking form (`<= #d`) must not block the
        // process and instead schedules a detached future update; that needs
        // scheduler infrastructure not present yet.
        if (a.isNonBlocking()) {
            // Nonblocking form: sample now, do not block, commit after the
            // timing via a detached scheduled store. The supported target set
            // mirrors what plain NBA can encode as an lvalue, since the
            // deferred record reuses that encoding (a positioned value plus a
            // write mask, and for an array element / aggregate leaf the live
            // destination pointer resolved at statement-execution time):
            // a whole integral or real scalar, a bit/part-select of one
            // (constant or variable), an unpacked-array element, and a packed
            // or unpacked struct field. A real rides its IEEE pattern through
            // the same narrow record (intra_pack_mm).
            if (!intra_nb_target_supported(a.left())) {
                fail("nonblocking intra-assignment timing is only supported for a whole "
                     "integral or real scalar, a bit/part-select of one, an "
                     "unpacked-array element, or a struct field");
            }
            const TimingControl& tc = *a.timingControl;
            if (tc.kind == TimingControlKind::Delay) {
                const Expression& delay_expr = tc.as<DelayControl>().expr;
                if (intra_nb_delay_is_const_zero(delay_expr)) {
                    // A compile-time `#0` is identical to a plain `<=`; see
                    // intra_nb_delay_is_const_zero for why it stays on the
                    // block-commit path rather than the deferred record's.
                    const ir::Index val0 = lower_expr(a.right(), b);
                    b.binary(ir::Inst::Tag::StoreNb, lower_lvalue(a.left(), b, /*is_write=*/true), val0, loc(a));
                    return val0;
                }
                // RHS before delay, the order this form has always used. The event
                // form below lowers its count first instead, and both are equally
                // correct: the two expressions evaluate in the same instant with no
                // suspend between them, and 9.4.5's "the right-hand expression
                // shall be evaluated before the delay" is about the delay ELAPSING
                // ("instead of after the delay"), not about the delay expression.
                const ir::Index     val = lower_expr(a.right(), b);
                const IntraNbTiming t   = lower_intra_nb_delay(delay_expr, b);
                store_leaf(ir::Inst::Tag::StoreNb, &t, lower_lvalue(a.left(), b, /*is_write=*/true), val, b, loc(a));
                return val;
            }
            // Event / repeat-event form: `x <= @e y` / `x <= repeat(N) @e y`.
            // Sample the value (and N) now; store after N occurrences of the
            // trigger edge. The count clamp (LRM 9.4.5) and the trigger guards
            // live in lower_intra_nb_event, shared with the decomposing targets so
            // the two cannot drift; an empty result means the count folded to a
            // non-positive constant and this is just a plain `<=`.
            const std::optional<IntraNbTiming> t   = lower_intra_nb_event(a, b);
            const ir::Index                    val = lower_expr(a.right(), b);
            store_leaf(ir::Inst::Tag::StoreNb, t ? &*t : nullptr,
                       lower_lvalue(a.left(), b, /*is_write=*/true), val, b, loc(a));
            return val;
        }
        const bool string_lhs =
          is_string_expr(a.left()) || (a.left().kind == ExpressionKind::ElementSelect &&
                                       is_string_expr(a.left().as<ElementSelectExpression>().value()));
        if (string_lhs || is_unpacked_array(a.left())) {
            // TODO: intra-assignment timing on a string / whole-array target —
            // the string/array store paths evaluate the LHS before the RHS, so
            // reordering them across the suspend needs care.
            fail("intra-assignment timing on a string or array target is not supported");
        }
        const ir::Index rhs = lower_expr(a.right(), b);
        lower_intra_timing(*a.timingControl, b, loc(a));
        if (a.left().kind == ExpressionKind::Concatenation) {
            store_concat(a.left().as<ConcatenationExpression>(), rhs, ir::Inst::Tag::Store, b, loc(a));
            return rhs;
        }
        b.binary(ir::Inst::Tag::Store, lower_lvalue(a.left(), b, /*is_write=*/true), rhs, loc(a));
        return rhs;
    }
    if (a.isNonBlocking() && fn_ctx_ != nullptr) {
        // A subroutine (task or function) runs in its caller's time step; an NBA
        // stages into the target signal's shadow and commits in that instant's
        // NBA region (the backend's nba_classify scans subroutine bodies). A
        // function is legal here per LRM 13.4.4 ("nonblocking assignments ...
        // shall be allowed inside a function") — it schedules the update and
        // returns immediately, no time consumed. slang rejects the one illegal
        // case upstream (a module-signal NBA from a constant function). Only
        // module-signal targets have a shadow, so reject the rest.
        check_subroutine_nba_target(a.left());
    }
    if (is_dyn_array(a.left())) {
        return lower_dyn_array_assign(a, b);
    }
    if (is_queue(a.left())) {
        return lower_queue_assign(a, b);
    }
    if (is_assoc(a.left())) {
        return lower_assoc_assign(a, b);
    }
    if (is_unpacked_array(a.left())) {
        // A whole blocking copy of one trivially copyable plain array into
        // another plain array is a single memcpy of the storage (AggCopy). A
        // nonblocking copy, a `'{...}` pattern, a string-literal assignment,
        // or a slice on either side decomposes per element: each leaf store
        // into an array element rides the backend's deferred array-NBA queue
        // (nba_classify_ir) for the NBA case. An array with heap-owning
        // elements (string / container) also decomposes per element, each leaf
        // a deep copy; only its NONBLOCKING form is still refused (emit_agg_copy).
        const ir::Ref       ty        = lower_type(*a.left().type);
        const Expression&   src       = unwrap_conversion(a.right());
        const ir::Inst::Tag tag       = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
        const bool          whole_lhs = is_whole_array_lvalue(a.left());
        if (is_positional_array_slice(a.left())) {
            // `a[i +: n] = rhs` / `<= rhs`: the target positions are runtime
            // indices, so there is no anchored slice lvalue to store through.
            // Resolve the RHS to an array lvalue of the slice's type and copy it
            // in position-wise; an out-of-range position drops its write (LRM
            // 7.4.5), the same as an ordinary `a[i] = v`.
            if (nbt != nullptr) {
                // TODO: intra-assignment timing on an INDEXED array slice
                // (`a[i +: n] <= #5 rhs`). The target positions are runtime
                // indices resolved inside emit_indexed_slice_copy, with an
                // out-of-range position dropping its own write (LRM 7.4.5); the
                // whole-array and element forms defer today, this shape does not.
                fail("intra-assignment timing on an indexed array-slice target is "
                     "not supported");
            }
            emit_indexed_slice_copy(a.left().as<RangeSelectExpression>(),
                                    array_copy_source(a.left(), src, ty, b, loc(a)),
                                    /*into_slice=*/true, tag, b, loc(a));
            return true_const(b, loc(a));
        }
        if (!a.isNonBlocking() && whole_lhs && is_trivially_copyable(ty) && is_whole_array_lvalue(src) &&
            !is_assoc_element_select(a.left()) && !is_assoc_element_select(src)) {
            const ir::Index dst  = lower_lvalue(a.left(), b);
            const ir::Index from = lower_lvalue(src, b);
            b.agg_copy(dst, from, ty, loc(a));
            return true_const(b, loc(a));
        }
        if (as_assignment_pattern(src)) {
            // `mem = '{...}`: element-wise store of the (slang-ordered) pattern
            // into a scalar-element unpacked array (`is_aggregate` routes an
            // array of aggregates through emit_struct_store instead).
            emit_agg_store(lower_lvalue(a.left(), b), ty, src, tag, b, loc(a), nbt);
            return true_const(b, loc(a));
        }
        if (src.kind == ExpressionKind::StringLiteral) {
            // `mem = "literal"` (LRM 5.9): same element-wise store as the
            // '{...} pattern case above.
            emit_agg_store(lower_lvalue(a.left(), b), ty, src, tag, b, loc(a), nbt);
            return true_const(b, loc(a));
        }
        const bool slice_src = src.kind == ExpressionKind::RangeSelect && is_unpacked_array(src);
        if (is_whole_array_lvalue(src) || slice_src) {
            // Array-to-array copy where a fast memcpy does not apply: a
            // nonblocking copy, or a slice on either side (a constant-bounds
            // ArrSlice lvalue, or an indexed slice materialized into a temp).
            // Per-element, element-order (position k of dst <- position k of
            // the source), reading the source through a staging temp when the
            // two may overlap. A single-memcpy deferred record for the NBA
            // whole-array case is a performance gap listed in docs/TODO.md.
            const ir::Index dst  = lower_lvalue(a.left(), b);
            const ir::Index from = array_copy_source(a.left(), src, ty, b, loc(a));
            emit_agg_copy(dst, from, ty, tag, b, loc(a), nbt);
            return true_const(b, loc(a));
        }
        if (src.kind == ExpressionKind::Call) {
            // `arr = f(...)`: the callee returns its result into a canonical
            // aggregate temp (the call result lvalue); copy it into the array
            // target element-wise (a flat / narrowed target is not memcpy-
            // compatible with the canonical result — the same layout hazard the
            // by-value argument path handles). Blocking and nonblocking both
            // decompose per element.
            const std::optional<ir::Index> cv = lower_user_call(src.as<CallExpression>(), b);
            if (!cv) {
                fail("whole-array assignment from a void call");
            }
            emit_agg_copy(lower_lvalue(a.left(), b), *cv, ty, tag, b, loc(a), nbt);
            return true_const(b, loc(a));
        }
        if (src.kind == ExpressionKind::ConditionalOp) {
            // `arr = c ? p : q` (LRM 11.4.11). A flat (scalar-element) array
            // never reaches the emit_struct_store dispatch, so the conditional
            // is handed to emit_agg_store here for the same destination-directed
            // branch an array of aggregates gets: the selected arm is copied
            // straight into the target and the other arm is not evaluated.
            emit_agg_store(lower_lvalue(a.left(), b), ty, src, tag, b, loc(a), nbt);
            return true_const(b, loc(a));
        }
        // A whole-array assignment whose source is none of the shapes above (a
        // non-lvalue, non-pattern, non-call expression) has no array value to
        // copy from.
        fail("whole unpacked array assignment is not supported");
    }

    // s[i] = c writes one character in place; out-of-range or a zero byte is
    // a no-op (LRM 6.16.2). The LHS element is byte-typed, so detect it via
    // the selected base.
    if (a.left().kind == ExpressionKind::ElementSelect &&
        is_string_expr(a.left().as<ElementSelectExpression>().value())) {
        if (a.isNonBlocking()) {
            // TODO: nonblocking assignment to strings (needs a string-aware
            // NBA staging path).
            fail("nonblocking assignment to a string is not supported");
        }
        const ElementSelectExpression& es  = a.left().as<ElementSelectExpression>();
        const ir::Index                str = lower_lvalue(es.value(), b);
        const ir::Index                idx = lower_index(es.selector(), b);
        const ir::Index                chr = lower_expr(a.right(), b);
        b.str_putc(str, idx, chr, loc(a));
        return chr;
    }
    if (is_string_expr(a.left()) && a.isNonBlocking() && !is_string_nba_place(a.left())) {
        // Every string PLACE — a whole variable, a struct member, a fixed-array
        // element — falls through to StoreNb, which stages an owned deep copy of
        // the handle in the string NBA record range and moves it into the place's
        // own destination in the NBA region. This is the backstop for the rest,
        // each already reported elsewhere: a string character (`s[i] <= c`) above,
        // a class property in lower_assign / check_subroutine_nba_target, and an
        // element of a queue / dynamic / associative array of strings by slang
        // itself (LRM 6.21 forbids a nonblocking assignment to an element of a
        // dynamically sized array variable).
        fail("nonblocking assignment to a string is not supported");
    }
    if (a.isNonBlocking() && a.left().kind == ExpressionKind::ElementSelect &&
        is_queue(a.left().as<ElementSelectExpression>().value())) {
        // Not a gap: LRM 6.21 forbids a nonblocking assignment to an element of a
        // dynamically sized array variable, and 7.10 makes a queue one. slang
        // raises NonblockingDynamicAssign first for every shape tried (bare
        // `q[0] <= v`, a struct field's queue, an array-of-structs field's queue,
        // and a `ref int r[$]` formal), so this is defence in depth rather than a
        // reachable path: slang's test is keyed on the value's
        // getSymbolReference(), and a shape whose reference is null would skip it.
        fail("a nonblocking assignment to a queue element is not legal "
             "SystemVerilog (LRM 6.21)");
    }
    // StoreNb gets correct two-phase NBA semantics in the backend: the RHS is
    // sampled here in the active region and committed in the NBA update region.
    // Intra-assignment timing (`<= #5`) is still rejected above, so the deferred
    // update always lands in the current instant's NBA region.
    const ir::Inst::Tag tag = a.isNonBlocking() ? ir::Inst::Tag::StoreNb : ir::Inst::Tag::Store;
    const ir::Index     rhs = lower_expr(a.right(), b);
    if (a.left().kind == ExpressionKind::Streaming) {
        store_stream_target(a.left().as<StreamingConcatenationExpression>(), a.right(), rhs, tag, b, loc(a),
                            nbt);
        return rhs;
    }
    if (a.left().kind == ExpressionKind::Concatenation) {
        store_concat(a.left().as<ConcatenationExpression>(), rhs, tag, b, loc(a), nbt);
        return rhs;
    }
    // A tagged-union member write needs no special case: LRM 11.9 forbids it
    // from moving the tag (only a whole-value `tagged` expression does that), so
    // it is an ordinary narrowing store over the member's part-select, guarded
    // against the current tag inside lower_member_lvalue.
    //
    // `nbt` is always null here -- every decomposing target returned above, and a
    // whole scalar took the timing block -- but the store still routes through
    // store_leaf so that widening intra_nb_target_decomposes can never silently
    // drop a statement's timing.
    store_leaf(tag, nbt, lower_lvalue(a.left(), b, /*is_write=*/true), rhs, b, loc(a));
    return rhs;
}

// Lower the right-hand side slang synthesized for `lhs op= rhs` (LRM 11.4.1)
// against an already-bound LHS lvalue: `a.right()` is `op(LValueReference,
// rhs)`, and the embedded LValueReference reads back through `lv`, so the
// lvalue is evaluated exactly once. Shared with the force-gated write path,
// which needs the same read-modify-write with only the store-back gated.
ir::Index Lowerer::lower_compound_rhs(const AssignmentExpression& a, ir::Index lv, ir::Builder& b) {
    const std::optional<ir::Index> saved = compound_lv_;
    compound_lv_                         = lv;
    const ir::Index rhs                  = lower_expr(a.right(), b);
    compound_lv_                         = saved;
    return rhs;
}

// Compound assignment `lhs op= rhs` (LRM 11.4.1, always blocking). slang has
// rewritten a.right() into `BinaryExpression(LValueReference, rhs, op)` with
// width/sign/conversion already resolved, so lowering it after binding the
// LHS lvalue yields `op(load(lhs), rhs)`. The LHS lvalue is evaluated exactly
// once and reused for the embedded LValueReference (single-evaluation, so
// `mem[i++] += 1` bumps i once).
ir::Index Lowerer::lower_compound_assign(const AssignmentExpression& a, ir::Builder& b) {
    const ir::SourceLoc l = loc(a);
    if (a.left().kind == ExpressionKind::Concatenation) {
        // TODO: compound assignment to a concatenation lvalue ({a,b} += x).
        // Needs the LValueReference to read the concat and store_concat to
        // scatter the result.
        fail("compound assignment to a concatenation is not supported");
    }
    if (is_string_expr(a.left()) || (a.left().kind == ExpressionKind::ElementSelect &&
                                     is_string_expr(a.left().as<ElementSelectExpression>().value()))) {
        // TODO: compound assignment to a string / string element (the lvalue
        // path below is packed-only; a string char write uses str_putc).
        fail("compound assignment to a string is not supported");
    }
    // A compound assignment reads then writes the same member, so one guard
    // covers both; the write wording is the more specific of the two.
    const ir::Index lv  = lower_lvalue(a.left(), b, /*is_write=*/true);
    const ir::Index rhs = lower_compound_rhs(a, lv, b);
    b.binary(ir::Inst::Tag::Store, lv, rhs, l);
    return rhs;
}

// Distribute a value across a concatenation lvalue, `{a, b, c} = rhs` (LRM
// §11.4.12). The leftmost operand takes the most-significant slice; operand
// widths partition the RHS bits. Lowered to one store per operand: the slice
// is `rhs >> offset` truncated to the operand width via resize (the upper bits
// are discarded by the narrowing store). The RHS is sampled once by the caller,
// so a nonblocking concat assignment keeps single-evaluation semantics.
void Lowerer::store_concat(const ConcatenationExpression& cc,
                           ir::Index                      rhs,
                           ir::Inst::Tag                  tag,
                           ir::Builder&                   b,
                           ir::SourceLoc                  l,
                           const IntraNbTiming*           nbt) {
    store_concat_ops(cc.operands(), rhs, tag, b, l, nbt);
}

// Shared slice-and-scatter loop for a packed concatenation lvalue (LRM
// §11.4.12) and a positional assignment-pattern lvalue over a packed
// right-hand side (LRM 10.9, see store_pattern below): the RHS's bits
// divide MSB-first by each operand's own width — for a packed struct or
// vector, field/element declaration order already equals concatenation
// order, so a pattern lvalue over one needs no separate structural walk.
// A nested concat or (positional-only) pattern operand recurses.
void Lowerer::store_concat_ops(std::span<const Expression* const> ops,
                               ir::Index                          rhs,
                               ir::Inst::Tag                      tag,
                               ir::Builder&                       b,
                               ir::SourceLoc                      l,
                               const IntraNbTiming*               nbt) {
    uint32_t offset = 0;
    for (size_t k = ops.size(); k-- > 0;) {
        const Expression& op = unwrap_lvalue_elem(*ops[k]);
        if (is_string_expr(op)) {
            fail("string operand in an lvalue concatenation is not supported");
        }
        ir::Index chunk = rhs;
        if (offset != 0) {
            const ir::Index sh = b.constant(module_.pool.put_int(make_int_type(32, false, false), offset), l);
            chunk              = b.binary(ir::Inst::Tag::Srl, rhs, sh, l);
        }
        const ir::Index val = b.resize(chunk, lower_type(*op.type), l);
        if (op.kind == ExpressionKind::Concatenation) {
            store_concat(op.as<ConcatenationExpression>(), val, tag, b, l, nbt);
        } else if (const auto* nested = as_assignment_pattern(op)) {
            if (op.kind != ExpressionKind::SimpleAssignmentPattern) {
                fail_keyed_pattern_lvalue();
            }
            store_concat_ops(nested->elements(), val, tag, b, l, nbt);
        } else {
            // Each operand carries its own force/assign state, so the scatter
            // gates per operand: a forced operand keeps its forced value while
            // its siblings still take their slice (LRM 10.6.2, matching
            // iverilog 13). Unforced operands lower to a plain store.
            store_or_gate(pca_info_of_lvalue(op), tag, lower_lvalue(op, b, /*is_write=*/true), val, b, l, nbt);
        }
        offset += op.type->getBitWidth();
    }
}

// LRM 10.9 on an assignment pattern in an lvalue: "the positional notation
// shall be required". A keyed or replicated pattern there is illegal source,
// not a gap. Three positions reach it -- a top-level pattern lvalue, one
// inside an lvalue concatenation, and one nested in another pattern -- and
// they share the sentence.
void Lowerer::fail_keyed_pattern_lvalue() {
    fail("a keyed or replicated assignment pattern is not legal as an lvalue "
         "(LRM 10.9 requires positional notation)");
}

// Entry point for an assignment-pattern lvalue, `'{a, b, c} = rhs` / typed
// `U'{a, b, c} = rhs` (LRM 10.9): a positional, non-compound assignment
// whose right-hand side is a fixed-size array, an unpacked struct, or a
// packed (bit-stream) value. A queue / dynamic-array / associative-array
// right-hand side is rejected outright — the pattern's positional
// correspondence needs a statically-known element count, which those types
// do not have.
ir::Index Lowerer::lower_pattern_assign(const slang::ast::AssignmentPatternExpressionBase& pat,
                                        const Expression&                                  rhs_expr,
                                        ir::Inst::Tag                                      tag,
                                        ir::Builder&                                       b,
                                        ir::SourceLoc                                      l,
                                        const IntraNbTiming*                               nbt) {
    if (is_queue(rhs_expr) || is_dyn_array(rhs_expr) || is_assoc(rhs_expr)) {
        fail("an assignment-pattern lvalue is not legal with a queue, dynamic "
             "array or associative array right-hand value (no statically known "
             "element count)");
    }
    const ir::Ref rty = lower_type(*rhs_expr.type);
    if (!is_agg_abi_ref(rty)) {
        // Packed struct or plain bit vector: field/element order already
        // equals concatenation order (see store_concat_ops above).
        store_concat_ops(pat.elements(), lower_expr(rhs_expr, b), tag, b, l, nbt);
        return true_const(b, l);
    }
    const Expression& src = unwrap_conversion(rhs_expr);
    if (!is_struct_lvalue_shape(src)) {
        // TODO: an aggregate-returning call / rvalue concatenation as the
        // pattern-lvalue source — needs a caller temp the way the by-value
        // call ABI already provides for other whole-aggregate consumers.
        fail("assignment-pattern lvalue from a non-variable right-hand value is "
             "not supported");
    }
    store_pattern(pat, lower_lvalue(src, b), rty, tag, b, l, nbt);
    return true_const(b, l);
}

// Distribute the elements of an unpacked aggregate rvalue (addressed by
// `rhs_addr`) across a positional assignment-pattern lvalue, one struct
// member / array element per pattern member (LRM 10.9). The right-hand
// element is addressed once per member (elem_select/member_select), not
// re-evaluated, so a nonblocking pattern assignment samples once.
void Lowerer::store_pattern(const slang::ast::AssignmentPatternExpressionBase& pat,
                            ir::Index                                          rhs_addr,
                            ir::Ref                                            rhs_ty,
                            ir::Inst::Tag                                      tag,
                            ir::Builder&                                       b,
                            ir::SourceLoc                                      l,
                            const IntraNbTiming*                               nbt) {
    const ir::Type rt    = std::get<ir::Type>(module_.pool.get(rhs_ty));
    const auto     elems = pat.elements();
    if (rt.kind == ir::Type::Kind::Struct) {
        const auto fields = module_.pool.struct_fields(rt.fields_id);
        if (elems.size() != fields.size()) {
            fail("assignment-pattern lvalue element count does not match the "
                 "right-hand struct's member count");
        }
        for (uint32_t i = 0; i < fields.size(); ++i) {
            store_pattern_elem(unwrap_lvalue_elem(*elems[i]), b.member_select(rhs_addr, i, rhs_ty, l),
                               fields[i].type, tag, b, l, nbt);
        }
        return;
    }
    // UnpackedArray — the only other is_agg_abi_ref shape.
    const uint32_t n = (rt.left >= rt.right ? rt.left - rt.right : rt.right - rt.left) + 1;
    if (elems.size() != n) {
        fail("assignment-pattern lvalue element count does not match the "
             "right-hand array's size");
    }
    for (uint32_t k = 0; k < n; ++k) {
        const int32_t coord =
          rt.left <= rt.right ? static_cast<int32_t>(rt.left + k) : static_cast<int32_t>(rt.left - k);
        const ir::Index idx = b.constant(
          module_.pool.put_int(make_int_type(32, false, true), static_cast<uint64_t>(static_cast<int64_t>(coord))),
          l);
        store_pattern_elem(unwrap_lvalue_elem(*elems[k]), b.elem_select(rhs_addr, idx, rhs_ty, l), rt.element,
                           tag, b, l, nbt);
    }
}

// One pattern member: a nested pattern/concat recurses; a plain lvalue
// whose element type is itself an aggregate is a whole-object copy; a
// scalar leaf is an ordinary truncating/extending assignment. LRM 10.9
// requires a member to have "the same number of bits" as its RHS element;
// Clotho does not enforce that here, and lets a mismatch truncate or extend
// exactly like the plain scalar assignment this is sugar for
// (docs/LRM_NOTES.md, note 12).
void Lowerer::store_pattern_elem(const Expression&    member,
                                 ir::Index            rhs_elem_addr,
                                 ir::Ref              elem_ty,
                                 ir::Inst::Tag        tag,
                                 ir::Builder&         b,
                                 ir::SourceLoc        l,
                                 const IntraNbTiming* nbt) {
    if (const auto* nested = as_assignment_pattern(member)) {
        if (member.kind != ExpressionKind::SimpleAssignmentPattern) {
            fail_keyed_pattern_lvalue();
        }
        store_pattern(*nested, rhs_elem_addr, elem_ty, tag, b, l, nbt);
        return;
    }
    if (member.kind == ExpressionKind::Concatenation) {
        store_concat(member.as<ConcatenationExpression>(), b.unary(ir::Inst::Tag::Load, rhs_elem_addr, l), tag,
                     b, l, nbt);
        return;
    }
    if (is_string_expr(member) || value_cat(elem_ty) == ValueCat::string ||
        value_cat(elem_ty) == ValueCat::handle) {
        // TODO: string / queue / dynamic-array / associative-array element in
        // an assignment-pattern lvalue.
        fail("string or queue/dynamic-array/associative-array element in an "
             "assignment-pattern lvalue is not supported");
    }
    if (is_agg_abi_ref(elem_ty)) {
        const ir::Index dst = lower_lvalue(member, b);
        if (tag == ir::Inst::Tag::Store && is_trivially_copyable(elem_ty)) {
            b.agg_copy(dst, rhs_elem_addr, elem_ty, l);
        } else {
            emit_agg_copy(dst, rhs_elem_addr, elem_ty, tag, b, l, nbt);
        }
        return;
    }
    const ir::Index val = b.resize(b.unary(ir::Inst::Tag::Load, rhs_elem_addr, l), lower_type(*member.type), l);
    store_leaf(tag, nbt, lower_lvalue(member, b), val, b, l);
}

} // namespace clotho::lower
