// Expressions, selects, patterns and lvalues.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Lower an expression as an rvalue, returning the SSA value index.
ir::Index Lowerer::lower_expr(const Expression& expr, ir::Builder& b) {
    cur_range_            = expr.sourceRange;
    const ir::SourceLoc l = loc(expr);
    switch (expr.kind) {
        case ExpressionKind::IntegerLiteral:
            return b.constant(lower_int_literal(expr.as<IntegerLiteral>()), l);
        case ExpressionKind::NullLiteral:
            // `null` for a chandle (LRM 6.14) is the empty handle, value 0.
            // slang types the literal as NullType (not chandle), so lower a
            // 64-bit 0 in the chandle representation directly; a surrounding
            // conversion to chandle (also i64) is a width-preserving no-op.
            // (Class / event null handles are separate and not lowered here.)
            return b.constant(module_.pool.put_int(make_int_type(64, false, false), 0), l);
        case ExpressionKind::UnbasedUnsizedIntegerLiteral:
            // '0/'1/'x/'z (LRM 5.7.1): the single bit fills the whole
            // context-determined width. slang has already propagated the
            // type and getValue() returns the SVInt sized+filled per the
            // table (setAllOnes / createFillX / createFillZ).
            return b.constant(
              svint_to_ref(expr.as<UnbasedUnsizedIntegerLiteral>().getValue(), lower_type(*expr.type)), l);
        case ExpressionKind::RealLiteral:
            // An IEEE-754 double (LRM 5.7.2); a shortreal literal arrives as
            // a cast around this, handled by the Conversion case.
            return b.constant(module_.pool.put_real(lower_type(*expr.type), expr.as<RealLiteral>().getValue()), l);
        case ExpressionKind::TimeLiteral:
            // A realtime value slang has already scaled to the scope's time
            // unit (LRM 5.8); precision rounding applies only in delay
            // contexts, which reject reals for now.
            return b.constant(module_.pool.put_real(lower_type(*expr.type), expr.as<TimeLiteral>().getValue()), l);
        case ExpressionKind::StringLiteral: {
            // In a string-typed context the literal is a string constant with
            // its \0 bytes dropped (LRM 6.16).
            if (is_string_expr(expr)) {
                return b.constant(string_value_ref(expr.as<StringLiteral>().getValue()), l);
            }
            // Otherwise a string literal is a packed array of 8-bit ASCII bytes
            // (LRM 5.9); as an integral rvalue it is that packed value. slang
            // folds it to an SVInt of the literal's 8*N-bit type; the
            // surrounding conversion resizes it to the assignment context.
            const slang::ConstantValue cv = fold_const(expr, Fold::Required);
            if (!cv.isInteger()) {
                fail("non-integral string literal");
            }
            return b.constant(svint_to_ref(cv.integer(), lower_type(*expr.type)), l);
        }
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue: {
            // A hierarchical reference (`top.u1.sig`, LRM 23.6) resolves to the
            // same ValueSymbol a local name would; slang's whole-design
            // elaboration binds it to the target instance's symbol, so once all
            // signals are collected (the two-pass lowering below) it flows
            // through the ordinary signal_of path.
            const ValueSymbol& sym = expr.as<ValueExpressionBase>().symbol;
            // An event is a plane-typed handle value (LRM 15.5.5), so it needs
            // no special case here: `e2 = e1` merges by copying the handle,
            // `e == null` is an identity compare, and passing one is a scalar
            // copy. See docs/design/event-handles.md.
            if (sym.kind == SymbolKind::Parameter) {
                const ParameterSymbol& p = sym.as<ParameterSymbol>();
                return b.constant(lower_param_constant(p, p.getType()), l);
            }
            if (sym.kind == SymbolKind::EnumValue) {
                // An enum member (LRM 6.19) is a named compile-time constant of
                // the enum's base type; slang computes its value (auto-increment
                // or explicit). The enum type itself lowers as its base integer
                // (lower_type), so the member is just that base-typed constant.
                const EnumValueSymbol& ev = sym.as<EnumValueSymbol>();
                return b.constant(lower_constant(ev.getValue(), ev.getType()), l);
            }
            if (is_unpacked_array(expr)) {
                // TODO: reading a whole unpacked array as a value (array copy,
                // passing a memory by value). Only element reads are supported.
                fail("whole unpacked array read is not supported");
            }
            if (is_aggregate(*expr.type)) {
                // A whole unpacked struct has no flat value: reads are only
                // supported where they decompose field-wise (assignment,
                // equality). A general rvalue context ($display, a by-value
                // argument, a concat operand) would need a by-value struct ABI.
                fail("unpacked struct as a whole-value rvalue is not supported here");
            }
            return b.unary(ir::Inst::Tag::Load, value_lvalue(sym, b, l), l);
        }
        case ExpressionKind::LValueReference: {
            // slang rewrites a compound assignment's RHS into
            // `BinaryExpression(LValueReference, rhs, op)`; the reference
            // stands for the LHS already lowered once by lower_compound_assign
            // (LRM 11.4.1 single-evaluation).
            if (!compound_lv_.has_value()) {
                fail("LValueReference outside a compound assignment");
            }
            const ir::Index v = b.unary(ir::Inst::Tag::Load, *compound_lv_, l);
            // A no-op for every lvalue shape except a struct member
            // narrower than its struct's own physical domain (see the
            // MemberAccess case above) — cheap enough to apply generically
            // rather than special-case the one shape that needs it.
            return narrow_to_declared(v, *expr.type, b, l);
        }
        case ExpressionKind::EmptyArgument:
            // slang's placeholder for an output port's value on the right of its
            // connection assignment (lower_port_output); an empty $display
            // argument is consumed by the format lowering and never reaches here.
            if (!port_value_.has_value()) {
                fail("empty argument outside an output port connection");
            }
            return *port_value_;
        case ExpressionKind::UnaryOp:
            return lower_unary(expr.as<UnaryExpression>(), b);
        case ExpressionKind::BinaryOp:
            return lower_binary(expr.as<BinaryExpression>(), b);
        case ExpressionKind::Inside:
            return lower_inside(expr.as<InsideExpression>(), b);
        case ExpressionKind::TaggedUnion:
            return lower_tagged_union_set(expr.as<slang::ast::TaggedUnionExpression>(), b);
        case ExpressionKind::ConditionalOp: {
            const ConditionalExpression& c = expr.as<ConditionalExpression>();
            // Condition (pattern-aware, incl. `&&&` filters) lowered once up
            // front — LRM 11.4.11 single-evaluation — and shared by both
            // dispatch paths. `x matches pat ? a : b` binds the pattern's
            // variables here, so the arms below see them.
            const ir::Index pred = lower_conditions(c.conditions, b, l);
            // An arm that is not safe to speculate must not run unless chosen
            // (LRM 11.4.11: a definite condition evaluates only its arm; an
            // ambiguous one evaluates both and blends). A speculation-safe arm
            // keeps the branchless packed select — evaluating the unchosen arm
            // is then unobservable, and bit-identical to the ambiguous blend.
            if (!is_speculation_safe(c.left()) || !is_speculation_safe(c.right())) {
                return lower_lazy_conditional(c, pred, b);
            }
            const ir::Index a  = lower_expr(c.left(), b);
            const ir::Index bb = lower_expr(c.right(), b);
            return b.select(pred, a, bb, l);
        }
        case ExpressionKind::Concatenation: {
            std::vector<ir::Index> parts;
            for (const Expression* op : expr.as<ConcatenationExpression>().operands()) {
                parts.push_back(lower_expr(*op, b));
            }
            return b.concat(parts, l);
        }
        case ExpressionKind::Replication: {
            const ReplicationExpression& r = expr.as<ReplicationExpression>();
            // DELIBERATE DEVIATION from LRM 11.4.12.1, which requires the
            // multiplier to be a non-negative, non-x, non-z CONSTANT
            // expression: a runtime multiplier is accepted when — and only
            // when — the result is a string. The clause demands a constant
            // because a concatenation's WIDTH must be known at elaboration;
            // a string result has no elaboration-time width, so the
            // restriction's own reason does not apply there. That boundary —
            // not "UVM needs it" — is what keeps the deviation narrow, though
            // UVM is why it exists (uvm_printer.svh:1501, `dash = {m{"-"}}`).
            // iverilog 13 refuses the construct; Verilator 5.050 accepts it.
            // String-ness is slang's own typing of the expression
            // (ReplicationExpression::fromSyntax types a non-constant
            // multiplier over an implicitly-string operand as `string`), not
            // a syntax pattern. The clause's other two demands bind at RUN
            // TIME instead: a zero multiplier yields the empty string (the
            // clause's own zero rule), a negative or x/z multiplier is an
            // illegal value and a located fatal (rt_string_repl /
            // rt_string_repl_invalid). A packed result stays refused below.
            // TODO: MUST BE REVISITED — tracked as the OPEN M3 item
            // "Non-constant string replication count" in docs/TODO.md.
            if (is_string_expr(expr)) {
                const ir::Index text  = lower_expr(r.concat(), b);
                const ir::Index times = lower_expr(r.count(), b);
                return b.binary(ir::Inst::Tag::StrRepl, text, times, l);
            }
            const uint64_t count = const_uint(
              r.count(), "a replication multiplier with a packed result (LRM 11.4.12.1: the "
                         "result's width is the multiplier times the operand's width, so it "
                         "must be known at elaboration; only a string result may carry a "
                         "runtime multiplier)");
            return b.repl(lower_expr(r.concat(), b), static_cast<uint32_t>(count), l);
        }
        case ExpressionKind::ElementSelect: {
            const ElementSelectExpression& es = expr.as<ElementSelectExpression>();
            if (is_string_expr(es.value())) {
                // s[i] reads the byte at i, 0 when out of range — identical to
                // getc (LRM 6.16.3).
                const ir::Index str = b.unary(ir::Inst::Tag::Load, lower_lvalue(es.value(), b), l);
                return b.binary(ir::Inst::Tag::StrGetc, str, lower_index(es.selector(), b), l);
            }
            if (auto c = try_lower_constant_select(expr, b, l)) {
                return *c;
            }
            const ir::Index base = select_base_lvalue(es.value(), b, l);
            const ir::Index sel =
              is_assoc(es.value()) ? var_assoc_select(es, base, b, l, /*is_write=*/false)
              : is_unpacked_array(es.value()) || is_dyn_array(es.value()) || is_queue(es.value())
                ? var_elem_select(es, base, b, l, /*is_write=*/false)
                : lower_packed_element_lvalue(es, base, b, l);
            const ir::Index v = b.unary(ir::Inst::Tag::Load, sel, l);
            // See narrow_to_declared / the MemberAccess case below: a
            // no-op except when `es` selects into a struct field
            // narrower-domain than its struct's own physical storage
            // (e.g. `x.b[3]` where `b` is 2-state inside an otherwise
            // 4-state struct `x`).
            return narrow_to_declared(v, *expr.type, b, l);
        }
        case ExpressionKind::RangeSelect: {
            const RangeSelectExpression& rs = expr.as<RangeSelectExpression>();
            if (auto c = try_lower_constant_select(expr, b, l)) {
                return *c;
            }
            if (is_unpacked_array(expr)) {
                // A fixed unpacked-array slice as a general by-value rvalue
                // (concat operand, $display %p, by-value argument). Supported
                // slice contexts (copy / equality / lvalue target) intercept
                // before this Load path; a raw value read needs a by-value
                // array ABI (same follow-up as a whole-array read).
                fail("whole unpacked array read is not supported");
            }
            const ir::Index base = select_base_lvalue(rs.value(), b, l);
            const ir::Index v    = b.unary(ir::Inst::Tag::Load, lower_range_select(rs, base, b, l), l);
            // See narrow_to_declared (e.g. `x.b[7:4]`).
            return narrow_to_declared(v, *expr.type, b, l);
        }
        case ExpressionKind::MemberAccess: {
            // The backend's PartSelect/BitSelect type inference
            // (ir/llvm/Inst.cpp compute_type) inherits the BASE signal's
            // domain, since an ordinary vector/array is domain-uniform
            // throughout. A struct is the one aggregate where a field's
            // own declared domain can be narrower than the whole (mixed)
            // struct's forced-4-state storage (LRM 6.11.2) — so a 2-state
            // field (or a further bit/range-select into one, above) read
            // off 4-state storage must be explicitly narrowed back to its
            // declared type here (LRM 6.3.1 conversion), or downstream
            // consumers that trust the declared type (e.g. an arithmetic
            // operand) see a width/domain mismatch. A no-op when the
            // field's own domain already matches the struct's (a packed
            // struct member is always integral, LRM 7.2.1, so
            // narrow_to_declared's string/real skip never triggers here —
            // used anyway for uniformity with its ElementSelect/RangeSelect
            // siblings above).
            if (auto c = try_lower_constant_select(expr, b, l)) {
                return *c;
            }
            const MemberAccessExpression& ma = expr.as<MemberAccessExpression>();
            if (const auto* param = ma.member.as_if<slang::ast::ParameterSymbol>()) {
                // A parameterized class's value parameter accessed via an instance
                // (`obj.param`, LRM 8.5): a compile-time constant of the
                // specialization, independent of the object. slang cannot fold
                // `obj.param` (obj is a runtime handle), so fold it here.
                return b.constant(lower_param_constant(*param, *ma.type), l);
            }
            const ir::Index v = b.unary(ir::Inst::Tag::Load, lower_member_lvalue(ma, b, l), l);
            return narrow_to_declared(v, *ma.type, b, l);
        }
        case ExpressionKind::ArbitrarySymbol: {
            // An interface instance named where a virtual interface is expected
            // (`vif = bus;`, `vif = bus.mp;`, `new(bus)`, LRM 25.9). slang types
            // it as a virtual interface with isRealIface set and hands over the
            // real InstanceSymbol; the value is that instance's identity.
            const Symbol& sym = *expr.as<ArbitrarySymbolExpression>().symbol;
            if (!expr.type->getCanonicalType().isVirtualInterface() ||
                sym.kind != SymbolKind::Instance) {
                fail("unsupported symbol reference in an expression");
            }
            return b.iface_ref(iface_scope_of(sym.as<InstanceSymbol>()), l);
        }
        case ExpressionKind::Conversion: {
            const ConversionExpression& cv       = expr.as<ConversionExpression>();
            const bool                  to_str   = is_string_expr(expr);
            const bool                  from_str = is_string_expr(cv.operand());
            // Tested first: slang wraps a streaming concatenation in a
            // Conversion to the assignment target, and a Streaming operand has
            // void type, so the string tests below would see "not integral" and
            // reject `s = {>>{...}}` before ever reaching the stream path.
            if (cv.conversionKind == ConversionKind::StreamingConcat) {
                const auto& sc = cv.operand().as<StreamingConcatenationExpression>();
                if (to_str) {
                    std::vector<StreamChunk> chunks;
                    collect_stream_chunks(sc, chunks, b, l);
                    return stream_to_string(chunks, b, l);
                }
                return lower_streaming_concat(sc, lower_type(*expr.type), b, l);
            }
            if (to_str && from_str) {
                return lower_expr(cv.operand(), b); // identity
            }
            if (to_str) {
                // A literal folds at compile time (its \0 bytes dropped);
                // anything else is the runtime integral -> string cast
                // (bytes MSB-first, zero bytes removed, LRM 6.16).
                if (cv.operand().kind == ExpressionKind::StringLiteral) {
                    return b.constant(string_value_ref(cv.operand().as<StringLiteral>().getValue()), l);
                }
                if (!cv.operand().type->isIntegral()) {
                    fail("unsupported conversion to string");
                }
                return b.unary(ir::Inst::Tag::StrFromPacked, lower_expr(cv.operand(), b), l);
            }
            if (from_str) {
                // string -> packed integral: characters pack MSB-first,
                // right-justified to the target width (LRM 6.16).
                if (!expr.type->isIntegral()) {
                    fail("unsupported conversion from string");
                }
                return b.str_to_packed(lower_expr(cv.operand(), b), lower_type(*expr.type), l);
            }
            if (is_handle_expr(cv.operand()) && !expr.type->getCanonicalType().isUnpackedArray()) {
                // TODO: bit-stream cast from a dynamic array / queue to a
                // packed type (LRM 6.24.2) -- needs an element-wise pack of
                // the runtime handle's heap contents, not a plain integer
                // coerce/resize (which silently produced the wrong value
                // for a variable operand, and crashed for a call operand's
                // temporary return handle).
                fail("bit-stream cast from a dynamic array or queue to a packed type is not supported");
            }
            // A Propagated conversion is slang's record of LRM 11.8.2 step 3
            // pushing an operator's result type down onto a context-determined
            // operand, so it extends under the propagated type; every other
            // kind (Implicit assignment, Explicit cast, BitstreamCast) is
            // assignment-shaped and extends under the operand's own sign.
            const Extend ext = cv.conversionKind == ConversionKind::Propagated ? Extend::Target : Extend::Source;
            return coerce(lower_expr(cv.operand(), b), *cv.operand().type, lower_type(*expr.type), b, l, ext);
        }
        case ExpressionKind::Call: {
            const CallExpression& call = expr.as<CallExpression>();
            if (call.isSystemCall()) {
                return lower_sysfunc(call, b);
            }
            const std::optional<ir::Index> v = lower_user_call(call, b);
            if (!v) {
                fail("void function call used as an expression");
            }
            return *v;
        }
        case ExpressionKind::NewClass:
            return lower_new_class(expr.as<slang::ast::NewClassExpression>(), b);
        case ExpressionKind::NewCovergroup:
            return lower_new_covergroup(expr.as<slang::ast::NewCovergroupExpression>(), b);
        case ExpressionKind::CopyClass:
            return lower_copy_class(expr.as<slang::ast::CopyClassExpression>(), b);
        case ExpressionKind::Assignment: {
            // Assignment used as an rvalue (`y = (x += 1)`, `y = (x = 5)`). Its
            // value is the LHS after the store, truncated to the LHS type (LRM
            // 11.3.1) — not the raw RHS. A string target yields the assigned
            // handle unchanged.
            const AssignmentExpression& a = expr.as<AssignmentExpression>();
            const ir::Index             v = lower_assign(a, b);
            return is_string_expr(a.left()) ? v : b.resize(v, lower_type(*a.left().type), l);
        }
        case ExpressionKind::SimpleAssignmentPattern:
        case ExpressionKind::StructuredAssignmentPattern:
        case ExpressionKind::ReplicatedAssignmentPattern:
            return lower_packed_pattern(expr, b);
        case ExpressionKind::UnboundedLiteral: {
            // `$` (LRM 7.10.4/11.5.1): legal here only inside a queue
            // range-select's bounds (`q[a:$]`, `q[$:a]`, arithmetic on it
            // like `q[0:$-1]`), where it denotes the queue's last valid
            // index -- size() - 1, a RUNTIME value (unlike a fixed/packed
            // array's `$`, its declared high bound, which slang folds to a
            // constant long before lowering and so never reaches here).
            // append_queue_pieces installs queue_dollar_ around lowering
            // the two bound expressions, against the SAME materialized
            // queue handle the slice reads from, so `$` and the receiver
            // agree on one length.
            if (!queue_dollar_.has_value()) {
                fail("'$' is not supported in this context");
            }
            const ir::Ref   i32  = make_int_type(32, false, true);
            const ir::Index size = b.queue_size(queue_dollar_->src, queue_dollar_->qty, l);
            return b.binary(ir::Inst::Tag::Sub, size, b.constant(module_.pool.put_int(i32, 1), l), l);
        }
        default:
            fail("unsupported expression kind=" + std::string(slang::ast::toString(expr.kind)));
    }
}

ir::Index Lowerer::lower_unary(const UnaryExpression& u, ir::Builder& b) {
    const ir::SourceLoc l = loc(u);
    using U               = slang::ast::UnaryOperator;
    using T               = ir::Inst::Tag;
    // Increment/decrement (LRM 11.4.2): `i = i +/- 1`, LHS evaluated once.
    // Handled before lowering the operand as an rvalue so the lvalue's
    // selectors are not evaluated twice. Pre returns the new value, post the
    // old; slang has already resolved the operand's width/type.
    if (u.op == U::Preincrement || u.op == U::Postincrement || u.op == U::Predecrement ||
        u.op == U::Postdecrement) {
        const bool      inc = u.op == U::Preincrement || u.op == U::Postincrement;
        const bool      pre = u.op == U::Preincrement || u.op == U::Predecrement;
        const ir::Index lv  = lower_lvalue(u.operand(), b);
        const ir::Index cur = b.unary(T::Load, lv, l);
        // `i++` is the read-modify-write `i = i + 1`, so a forced/assigned
        // target drops the update while keeping the expression's value (LRM
        // 10.6.2 suppresses the update alone): the read already saw the forced
        // value, so post yields it and pre yields it plus one, and the variable
        // stays forced. Byte-identical to iverilog 13. Null for the ordinary
        // (never force/assign target) case, which stores unconditionally.
        const ForceInfo* fi = pca_info_of_lvalue(u.operand());
        if (u.operand().type->isFloating()) {
            // ++ / -- on a real add / subtract 1.0 (LRM 11.4.2).
            const ir::Index one = b.constant(module_.pool.put_real(lower_type(*u.operand().type), 1.0), l);
            const ir::Index nv  = b.binary(inc ? T::FAdd : T::FSub, cur, one, l);
            store_or_gate(fi, T::Store, lv, nv, b, l);
            return pre ? nv : cur;
        }
        const ir::Index one = b.constant(module_.pool.put_int(lower_type(*u.operand().type), 1), l);
        const ir::Index nv  = b.binary(inc ? T::Add : T::Sub, cur, one, l);
        store_or_gate(fi, T::Store, lv, nv, b, l);
        return pre ? nv : cur;
    }
    const ir::Index op = lower_expr(u.operand(), b);
    if (u.operand().type->isFloating()) {
        switch (u.op) {
            case U::Plus:
                return op;
            case U::Minus:
                return b.unary(T::FNeg, op, l);
            case U::LogicalNot:
                // !r is r == 0.0: a definite 1-bit result (LRM 11.4.7 with
                // real operands has no ambiguity — reals carry no x/z).
                return b.binary(T::FEq, op, real_zero(u.operand(), b, l), l);
            default:
                // Bitwise / reduction operators are illegal on reals (LRM
                // Table 11-1); slang rejects them upstream.
                fail("this unary operator is not legal on a real operand (LRM "
                     "Table 11-1)");
        }
    }
    switch (u.op) {
        case U::BitwiseNot:
            return b.unary(T::Bnot, op, l);
        case U::LogicalNot:
            if (is_string_expr(u.operand())) {
                // Illegal: LRM Table 11-1 restricts logical operators to
                // integral/real/shortreal, and Table 6-9 defines no logical
                // operators on strings (docs/LRM_NOTES.md, note 17).
                // TODO: fix upstream in slang — Type::isBooleanConvertible
                // whitelists StringType, so UnaryLogicalNot binds with no
                // diagnostic and reaches us.
                fail("logical negation of a string is not legal (LRM Table 11-1)");
            }
            return b.unary(T::Lnot, op, l);
        case U::BitwiseAnd:
            return b.unary(T::RedAnd, op, l);
        case U::BitwiseOr:
            return b.unary(T::RedOr, op, l);
        case U::BitwiseXor:
            return b.unary(T::RedXor, op, l);
        case U::BitwiseNand:
            return b.unary(T::Bnot, b.unary(T::RedAnd, op, l), l);
        case U::BitwiseNor:
            return b.unary(T::Bnot, b.unary(T::RedOr, op, l), l);
        case U::BitwiseXnor:
            return b.unary(T::Bnot, b.unary(T::RedXor, op, l), l);
        case U::Plus:
            return op;
        case U::Minus: {
            const ir::Index zero = b.constant(module_.pool.put_int(lower_type(*u.type), 0), l);
            return b.binary(T::Sub, zero, op, l);
        }
        default:
            // ++ / -- returned above, and every other UnaryOperator has an arm.
            fail("internal: unhandled unary operator");
    }
}

// Short-circuit `&&` / `||` / `->` (LRM 11.3.5, 11.4.7 — the LRM's own
// short-circuit list is exactly these three plus `?:`; `<->` is
// deliberately absent, see lower_binary). The left operand is always
// evaluated; the right operand is evaluated only in the Select's
// then-branch, taken when the left has not already fixed the result. A
// definite left operand short-circuits (`&&` on a false left -> 0, `||`
// on a true left -> 1, `->` on a false left -> 1 — LRM 11.4.7: `a -> b` is
// `!a || b`, so a false `a` makes it true without needing `b`); an x/z
// left is ambiguous, so the right operand is still evaluated and the full
// 3-valued Land/Lor/Limplies computes the merged result.
ir::Index Lowerer::lower_short_circuit(const BinaryExpression& bin, ir::Builder& b) {
    using T                 = ir::Inst::Tag;
    using B                 = slang::ast::BinaryOperator;
    const ir::SourceLoc l   = loc(bin);
    const ir::Ref       bit = make_int_type(1, true, false);

    // A real operand reduces to its own definite truth bit (!= 0.0, reals
    // carry no x/z) before the integer machinery below (LRM 11.4.7).
    ir::Index lhs = lower_expr(bin.left(), b);
    if (bin.left().type->isFloating()) {
        lhs = b.binary(T::FNe, lhs, real_zero(bin.left(), b, l), l);
    }

    // Predicate is a definite 2-state bit: 1 iff the right operand must still
    // be evaluated. `&&` and `->` both skip when the left is logically false
    // (all bits a definite 0) -- `->`'s skip condition is "!left is definite
    // true", which is the same "left is exactly 0" test `&&` already uses.
    // `||` skips when the left is logically true (its reduction is a definite
    // 1). Case-equality (===) yields a definite bit even for an x/z left, so
    // the backend can branch on it directly.
    const ir::Ref lhs_ty        = bin.left().type->isFloating() ? bit : lower_type(*bin.left().type);
    const bool    skip_on_false = bin.op == B::LogicalAnd || bin.op == B::LogicalImplication;
    ir::Index     pred;
    if (skip_on_false) {
        const ir::Index zero = b.constant(module_.pool.put_int(lhs_ty, 0), l);
        pred                 = b.unary(T::Lnot, b.binary(T::CaseEq, lhs, zero, l), l);
    } else {
        const ir::Index ror = b.unary(T::RedOr, lhs, l);
        const ir::Index one = b.constant(module_.pool.put_int(bit, 1), l);
        pred                = b.unary(T::Lnot, b.binary(T::CaseEq, ror, one, l), l);
    }

    // Skipped-branch dominant value and the not-skipped combine op differ per
    // operator: `&&` -> 0 / Land, `||` -> 1 / Lor, `->` -> 1 / Limplies (a
    // false left forces true without needing the right operand at all).
    uint32_t dom_val;
    T        combine_tag;
    switch (bin.op) {
        case B::LogicalAnd:
            dom_val     = 0;
            combine_tag = T::Land;
            break;
        case B::LogicalOr:
            dom_val     = 1;
            combine_tag = T::Lor;
            break;
        case B::LogicalImplication:
            dom_val     = 1;
            combine_tag = T::Limplies;
            break;
        default:
            fail("internal: not a short-circuit operator"); // unreachable: see call site.
    }

    b.open_region();
    ir::Index rhs = lower_expr(bin.right(), b);
    if (bin.right().type->isFloating()) {
        rhs = b.binary(T::FNe, rhs, real_zero(bin.right(), b, l), l);
    }
    const ir::Index      combined    = b.binary(combine_tag, lhs, rhs, l);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    const ir::Index      dom         = b.constant(module_.pool.put_int(bit, dom_val), l);
    const ir::ExtraSlice else_region = b.close_region();

    return b.select_lazy(pred, then_region, combined, else_region, dom, l);
}

// Lazy `?:` for an observable arm (LRM 11.4.11): a definite-true condition
// evaluates only the first expression, definite-false only the second, and
// an ambiguous (x/z) condition evaluates BOTH and blends them bit by bit
// (Table 11-20 — exactly what an eager Select with an unknown condition
// computes). Built as two nested SelectLazy: the outer branches on
// definite-true, its else branches on definite-false, and the innermost
// else region re-lowers both arms eagerly. Case-equality on the 1-bit
// predicate yields the definite branch bits.
// `pred` is the already-lowered 1-bit condition (computed once by the caller,
// pattern-aware) — LRM 11.4.11 requires the condition to be evaluated once.
ir::Index Lowerer::lower_lazy_conditional(const ConditionalExpression& c, ir::Index pred, ir::Builder& b) {
    using T                 = ir::Inst::Tag;
    const ir::SourceLoc l   = loc(c);
    const ir::Ref       bit = make_int_type(1, true, false);

    const ir::Index one     = b.constant(module_.pool.put_int(bit, 1), l);
    const ir::Index zero    = b.constant(module_.pool.put_int(bit, 0), l);
    const ir::Index is_true = b.binary(T::CaseEq, pred, one, l);

    b.open_region();
    const ir::Index      tv       = lower_expr(c.left(), b);
    const ir::ExtraSlice t_region = b.close_region();

    b.open_region();
    const ir::Index is_false = b.binary(T::CaseEq, pred, zero, l);

    b.open_region();
    const ir::Index      fv       = lower_expr(c.right(), b);
    const ir::ExtraSlice f_region = b.close_region();

    b.open_region();
    const ir::Index      xa       = lower_expr(c.left(), b);
    const ir::Index      xb       = lower_expr(c.right(), b);
    const ir::Index      blend    = b.select(pred, xa, xb, l);
    const ir::ExtraSlice x_region = b.close_region();

    const ir::Index      inner       = b.select_lazy(is_false, f_region, fv, x_region, blend, l);
    const ir::ExtraSlice else_region = b.close_region();

    return b.select_lazy(is_true, t_region, tv, else_region, inner, l);
}

// A 0.0 constant of the operand's float type, for predicates.
ir::Index Lowerer::real_zero(const Expression& operand, ir::Builder& b, ir::SourceLoc l) {
    return b.constant(module_.pool.put_real(lower_type(*operand.type), 0.0), l);
}

// Binary operator with real operands. slang has inserted all LRM 11.3.1 /
// 11.8.2 conversions, so both operands share one float type here (except a
// logical operand, which converts to its own truth value first).
ir::Index Lowerer::lower_real_binary(const BinaryExpression& bin, ir::Index lhs, ir::Index rhs, ir::Builder& b) {
    const ir::SourceLoc l = loc(bin);
    using B               = slang::ast::BinaryOperator;
    using T               = ir::Inst::Tag;
    switch (bin.op) {
        case B::Add:
            return b.binary(T::FAdd, lhs, rhs, l);
        case B::Subtract:
            return b.binary(T::FSub, lhs, rhs, l);
        case B::Multiply:
            return b.binary(T::FMul, lhs, rhs, l);
        case B::Divide:
            return b.binary(T::FDiv, lhs, rhs, l);
        case B::Power:
            return b.binary(T::FPow, lhs, rhs, l);
        case B::Equality:
            return b.binary(T::FEq, lhs, rhs, l);
        case B::Inequality:
            return b.binary(T::FNe, lhs, rhs, l);
        case B::GreaterThan:
            return b.binary(T::FGt, lhs, rhs, l);
        case B::GreaterThanEqual:
            return b.binary(T::FGe, lhs, rhs, l);
        case B::LessThan:
            return b.binary(T::FLt, lhs, rhs, l);
        case B::LessThanEqual:
            return b.binary(T::FLe, lhs, rhs, l);
        case B::LogicalAnd:
        case B::LogicalOr: {
            // Each real operand reduces to its own truth value (!= 0.0,
            // definite); an integral operand keeps its planes and the
            // backend's Land/Lor X rules apply to it alone.
            const ir::Index lp =
              bin.left().type->isFloating() ? b.binary(T::FNe, lhs, real_zero(bin.left(), b, l), l) : lhs;
            const ir::Index rp =
              bin.right().type->isFloating() ? b.binary(T::FNe, rhs, real_zero(bin.right(), b, l), l) : rhs;
            return b.binary(bin.op == B::LogicalAnd ? T::Land : T::Lor, lp, rp, l);
        }
        case B::LogicalEquivalence: {
            // Real operand reduction mirrors LogicalAnd/LogicalOr above.
            // <-> does not short-circuit (LRM 11.3.5), so lhs/rhs here are
            // already the eagerly lowered operands lower_binary passed in.
            const ir::Index lp =
              bin.left().type->isFloating() ? b.binary(T::FNe, lhs, real_zero(bin.left(), b, l), l) : lhs;
            const ir::Index rp =
              bin.right().type->isFloating() ? b.binary(T::FNe, rhs, real_zero(bin.right(), b, l), l) : rhs;
            return b.binary(T::Lequiv, lp, rp, l);
        }
        default:
            // %, bitwise, shifts, ===, ==? are illegal on reals (LRM Table
            // 11-1); slang rejects them upstream.
            fail("this binary operator is not legal on real operands (LRM Table "
                 "11-1)");
    }
}

ir::Index Lowerer::lower_binary(const BinaryExpression& bin, ir::Builder& b) {
    using B = slang::ast::BinaryOperator;
    using T = ir::Inst::Tag;

    // Short-circuit logical operators: the right operand must not be
    // evaluated when the left already decides the result, so its side
    // effects (a function call) do not occur (LRM 11.3.5, 11.4.7). `<->`
    // is deliberately excluded -- LRM 11.3.5 lists only &&, ||, ->, and
    // ?: as short-circuiting; <-> "shall be evaluated exactly once" per
    // operand, so it falls through to the eager path below like any other
    // binary op (see the LogicalEquivalence case further down).
    if (bin.op == B::LogicalAnd || bin.op == B::LogicalOr || bin.op == B::LogicalImplication) {
        return lower_short_circuit(bin, b);
    }

    // Unpacked aggregate equality (LRM 7.2, 11.2.2, 11.4.5, Table 11-1): a
    // whole struct / fixed unpacked array / queue / dynamic array / associative
    // array value cannot be loaded, so every equality-family operator reduces to
    // a per-leaf (or, for an associative array, whole-map canonical byte)
    // comparison instead. slang wraps each operand in a bitstream Conversion,
    // so the aggregate type is on the unwrapped operand.
    if (bin.op == B::Equality || bin.op == B::Inequality || bin.op == B::CaseEquality ||
        bin.op == B::CaseInequality) {
        const Expression& ule = unwrap_conversion(bin.left());
        if (is_assoc(ule)) {
            // LRM 11.4.5 / 7.9 (P4a): two associative arrays are equal iff
            // they hold the same set of (key, value) pairs -- no per-leaf
            // walk here (a map has no positional "leaves" to Land-reduce
            // over the way a struct/array/queue does); the backend compares
            // each side's P3 canonical serialization byte for byte instead.
            const Expression& re = unwrap_conversion(bin.right());
            if (!is_struct_lvalue_shape(ule) || !is_struct_lvalue_shape(re)) {
                fail("associative array equality is only supported between "
                     "array variables");
            }
            const ir::Ref  sty     = lower_type(*ule.type);
            const bool     is_case = bin.op == B::CaseEquality || bin.op == B::CaseInequality;
            if (!is_case) {
                const ir::Type t = std::get<ir::Type>(module_.pool.get(sty));
                if (assoc_value_may_carry_xz(t.element)) {
                    // TODO: X-propagating logical equality for a
                    // 4-state-capable value element (LRM 11.4.5) -- the
                    // canonical byte serialization is bit-exact, which is
                    // what case equality (===) wants, but logical equality
                    // needs an unknown VALUE pair to propagate to an
                    // ambiguous (x) result, and a flat byte compare cannot
                    // give that.
                    fail("logical equality of an associative array with a "
                         "4-state-capable value element is not supported "
                         "(LRM 11.4.5); use === instead");
                }
            }
            const ir::SourceLoc l        = loc(bin);
            const ir::Index     left_lv  = lower_lvalue(ule, b);
            const ir::Index     right_lv = lower_lvalue(re, b);
            const ir::Index     acc      = b.agg_eq(left_lv, right_lv, sty, l);
            return (bin.op == B::Equality || bin.op == B::CaseEquality)
                   ? acc
                   : b.unary(ir::Inst::Tag::Lnot, acc, l);
        }
        const bool agg_operand =
          is_unpacked_struct(*ule.type) ||
          ule.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() !=
            nullptr ||
          is_queue(ule) || is_dyn_array(ule);
        if (agg_operand) {
            const bool is_case = bin.op == B::CaseEquality || bin.op == B::CaseInequality;
            if (is_case && !case_eq_matches_logical(lower_type(*ule.type))) {
                // Case equality compares x/z bits directly instead of
                // X-propagating them like logical equality does (11.4.5,
                // "just as ... the procedural case statement", 12.5); the
                // AggEq / runtime container comparator below implements
                // only the X-propagating rule. Reusing it is exact when no
                // leaf can ever carry x/z (case_eq_matches_logical); a leaf
                // that could carry x/z has no dedicated case-equality
                // comparator yet.
                // TODO: a bit-for-bit (x/z-inclusive) case-equality member
                // walk for a struct / queue / dynamic array with a 4-state
                // or real leaf (LRM 11.4.5, 12.5).
                fail("case equality of an aggregate or container with a "
                     "4-state or real leaf is not supported (LRM 11.4.5)");
            }
            // A whole queue / dynamic-array compare (LRM 11.2.2) reduces to
            // a per-element walk with the same X-propagation (Land-reduced
            // Eq leaves) a struct / fixed-array compare uses; the AggEq
            // expansion dispatches the handle leaf to the runtime element
            // compare. Proven leaf-safe above, that walk is exact for a
            // case (in)equality operand too, not merely an approximation.
            const ir::SourceLoc l   = loc(bin);
            const ir::Index     acc = lower_struct_eq(ule, unwrap_conversion(bin.right()), b, l);
            return (bin.op == B::Equality || bin.op == B::CaseEquality)
                   ? acc
                   : b.unary(ir::Inst::Tag::Lnot, acc, l);
        }
    }

    if (bin.left().kind == ExpressionKind::TypeReference || bin.right().kind == ExpressionKind::TypeReference) {
        // type(a) == type(b) (LRM 6.23): a type reference is a compile-time
        // marker, not a value -- slang restricts this expression kind to
        // exactly this position (an equality/case-equality comparison with
        // another type reference) and to a case-statement selector/item
        // (handled separately in lower_case), so both operands are
        // guaranteed TypeReference here; mirrors slang's own constant fold
        // in BinaryExpression::evalImpl.
        if (bin.left().kind != ExpressionKind::TypeReference || bin.right().kind != ExpressionKind::TypeReference ||
            (bin.op != B::Equality && bin.op != B::Inequality && bin.op != B::CaseEquality &&
             bin.op != B::CaseInequality)) {
            fail("type() is only supported in an equality comparison with another type()");
        }
        const bool matches = bin.left().as<TypeReferenceExpression>().targetType.isMatching(
          bin.right().as<TypeReferenceExpression>().targetType);
        const bool result = (bin.op == B::Equality || bin.op == B::CaseEquality) ? matches : !matches;
        return b.constant(module_.pool.put_int(make_int_type(1, false, false), result ? 1 : 0), loc(bin));
    }

    const ir::SourceLoc l   = loc(bin);
    const ir::Index     lhs = lower_expr(bin.left(), b);
    const ir::Index     rhs = lower_expr(bin.right(), b);

    if (is_string_expr(bin.left()) || is_string_expr(bin.right())) {
        // slang's implicit conversions make both operands string here. Only
        // comparisons apply to strings; ===/!== degenerate to ==/!= since a
        // string carries no X/Z (LRM 6.16.9).
        switch (bin.op) {
            case B::Equality:
            case B::CaseEquality:
                return b.binary(T::Eq, lhs, rhs, l);
            case B::Inequality:
            case B::CaseInequality:
                return b.binary(T::Ne, lhs, rhs, l);
            case B::LessThan:
                return b.binary(T::Lt, lhs, rhs, l);
            case B::LessThanEqual:
                return b.binary(T::Le, lhs, rhs, l);
            case B::GreaterThan:
                return b.binary(T::Gt, lhs, rhs, l);
            case B::GreaterThanEqual:
                return b.binary(T::Ge, lhs, rhs, l);
            default:
                // Only comparison, concatenation and replication apply to a
                // string (LRM 6.16); slang rejects the rest upstream.
                fail("this binary operator is not legal on string operands (LRM "
                     "6.16)");
        }
    }
    if (bin.left().type->isFloating() || bin.right().type->isFloating()) {
        return lower_real_binary(bin, lhs, rhs, b);
    }

    switch (bin.op) {
        case B::Add:
            return b.binary(T::Add, lhs, rhs, l);
        case B::Subtract:
            return b.binary(T::Sub, lhs, rhs, l);
        case B::Multiply:
            return b.binary(T::Mul, lhs, rhs, l);
        case B::Divide:
            return b.binary(T::Div, lhs, rhs, l);
        case B::Mod:
            return b.binary(T::Mod, lhs, rhs, l);
        case B::Power:
            return b.binary(T::Pow, lhs, rhs, l);
        case B::BinaryAnd:
            return b.binary(T::Band, lhs, rhs, l);
        case B::BinaryOr:
            return b.binary(T::Bor, lhs, rhs, l);
        case B::BinaryXor:
            return b.binary(T::Bxor, lhs, rhs, l);
        case B::BinaryXnor:
            return b.binary(T::Bxnor, lhs, rhs, l);
        case B::Equality:
            return b.binary(T::Eq, lhs, rhs, l);
        case B::Inequality:
            return b.binary(T::Ne, lhs, rhs, l);
        case B::CaseEquality:
            return b.binary(T::CaseEq, lhs, rhs, l);
        case B::CaseInequality:
            // !== is the strict inequality: logical negation of the always-known
            // === result.
            return b.unary(T::Lnot, b.binary(T::CaseEq, lhs, rhs, l), l);
        case B::WildcardEquality:
            return b.binary(T::WildEq, lhs, rhs, l);
        case B::WildcardInequality:
            // !=? is the logical negation of ==? (X-propagating, so Lnot keeps
            // x as x).
            return b.unary(T::Lnot, b.binary(T::WildEq, lhs, rhs, l), l);
        case B::LogicalEquivalence:
            // <-> does not short-circuit (LRM 11.3.5); both integral
            // operands are already the eagerly lowered lhs/rhs.
            return b.binary(T::Lequiv, lhs, rhs, l);
        case B::GreaterThanEqual:
            return b.binary(T::Ge, lhs, rhs, l);
        case B::GreaterThan:
            return b.binary(T::Gt, lhs, rhs, l);
        case B::LessThanEqual:
            return b.binary(T::Le, lhs, rhs, l);
        case B::LessThan:
            return b.binary(T::Lt, lhs, rhs, l);
        case B::LogicalShiftLeft:
        case B::ArithmeticShiftLeft:
            return b.binary(T::Sll, lhs, rhs, l);
        case B::LogicalShiftRight:
            return b.binary(T::Srl, lhs, rhs, l);
        case B::ArithmeticShiftRight:
            return b.binary(T::Sra, lhs, rhs, l);
        default:
            // The three short-circuit operators returned at the top of this
            // function, and the other 25 BinaryOperator enumerators each have an
            // arm above, so this is an invariant rather than a gap.
            fail("internal: unhandled binary operator");
    }
}

// One comparison of the `inside` left operand against a singular member value
// (LRM 11.4.13): "the inside operator uses the equality (==) operator on
// nonintegral expressions ... Integral expressions use the wildcard equality
// (==?) operator so that an x or z bit in a VALUE IN THE SET is treated as a
// do-not-care". WildEq's wildcards are its right operand's, which is exactly
// that asymmetry; a real has no x/z, so it compares with plain FEq.
ir::Index Lowerer::inside_leaf_eq(ir::Index               sel,
                                  ir::Index               val,
                                  const slang::ast::Type& vt,
                                  ir::Ref                 ty,
                                  bool                    real,
                                  ir::Builder&            b,
                                  ir::SourceLoc           l) {
    if (real) {
        return b.binary(ir::Inst::Tag::FEq, sel, coerce(val, vt, ty, b, l), l);
    }
    return b.binary(ir::Inst::Tag::WildEq, sel, b.resize(val, ty, l), l);
}

// A FIXED-SIZE unpacked-array member of an `inside` set, as an OR over its
// elements. 11.4.13 traverses "by descending into the array until reaching a
// singular value", so an element that is itself an array dimension recurses.
// The element count is static here and the elements are ordinary addressable
// storage, so this unrolls rather than looping.
ir::Index Lowerer::inside_fixed_array_pred(ir::Index               base,
                                           const slang::ast::Type& arr,
                                           ir::Index               sel,
                                           ir::Ref                 ty,
                                           bool                    real,
                                           ir::Builder&            b,
                                           ir::SourceLoc           l) {
    using T                      = ir::Inst::Tag;
    const ir::Ref           aty  = lower_type(arr);
    const ir::Type          at   = std::get<ir::Type>(module_.pool.get(aty));
    const slang::ast::Type& et   = *arr.getArrayElementType();
    const bool              nest = et.getCanonicalType().isUnpackedArray();
    const uint32_t          n    = (at.left >= at.right ? at.left - at.right : at.right - at.left) + 1;
    const ir::Ref           i32  = make_int_type(32, false, true);
    ir::Index               acc{};
    for (uint32_t k = 0; k < n; ++k) {
        // Element k in DECLARED index coordinates, as every other fixed-array
        // element walk builds them (store_pattern).
        const int32_t coord =
          at.left <= at.right ? static_cast<int32_t>(at.left + k) : static_cast<int32_t>(at.left - k);
        const ir::Index idx =
          b.constant(module_.pool.put_int(i32, static_cast<uint64_t>(static_cast<int64_t>(coord))), l);
        const ir::Index elv = b.elem_select(base, idx, aty, l);
        const ir::Index one = nest ? inside_fixed_array_pred(elv, et, sel, ty, real, b, l)
                                   : inside_leaf_eq(sel, b.unary(T::Load, elv, l), et, ty, real, b, l);
        acc                 = k == 0 ? one : b.binary(T::Lor, acc, one, l);
    }
    return acc;
}

// A QUEUE or DYNAMIC-ARRAY member of an `inside` set: the same OR, but its
// element count is a runtime value, so it walks (`q.size()` snapshotted like a
// foreach bound) and folds into a cell. LRM 11.4.13's own example is a queue
// member: `ex inside {1, 2, array}` with `int array[$] = '{3,4,5}`.
ir::Index Lowerer::inside_container_pred(const Expression& m,
                                         ir::Index         sel,
                                         ir::Ref           ty,
                                         bool              real,
                                         ir::Builder&      b,
                                         ir::SourceLoc     l) {
    using T                    = ir::Inst::Tag;
    const slang::ast::Type& et = *m.type->getArrayElementType();
    if (et.getCanonicalType().isUnpackedArray() || is_agg_abi_type(et.getCanonicalType())) {
        // TODO: descend into a non-singular element of a variable-size member
        // (11.4.13 traverses until it reaches a singular value). The fixed-size
        // member above recurses; doing it here needs the walk nested inside
        // itself, one loop per dimension.
        fail("inside operator with a queue or dynamic-array member whose elements are "
             "not singular is not supported");
    }
    const ir::Ref   aty      = lower_type(*m.type);
    const ir::Index arr      = lower_lvalue(m, b);
    const ir::Ref   i32      = make_int_type(32, false, true);
    const ir::Ref   log1     = make_int_type(1, true, false);
    const TempCell  hit      = make_temp_cell(log1, "inside_hit");
    const TempCell  idx      = make_temp_cell(i32, "inside_idx");
    const TempCell  bound    = make_temp_cell(i32, "inside_n");
    const ir::Index size     = is_queue(m) ? b.queue_size(arr, aty, l) : b.dyn_array_size(arr, aty, l);
    const ir::Index hit_lv   = hit.lv(b, l);
    const ir::Index hit_init = b.constant(module_.pool.put_int(log1, 0), l);
    b.binary(T::Store, hit_lv, hit_init, l);
    const ir::Index bound_lv  = bound.lv(b, l);
    const ir::Index bound_val = b.resize(size, i32, l);
    b.binary(T::Store, bound_lv, bound_val, l);
    const ir::Index idx_lv   = idx.lv(b, l);
    const ir::Index idx_init = b.constant(module_.pool.put_int(i32, 0), l);
    b.binary(T::Store, idx_lv, idx_init, l);

    b.open_region();
    const ir::Index      cur  = b.unary(T::Load, idx.lv(b, l), l);
    const ir::Index      lim  = b.unary(T::Load, bound.lv(b, l), l);
    const ir::Index      pred = b.binary(T::Lt, cur, lim, l);
    const ir::ExtraSlice cond = b.close_region();

    // No early exit on a match: the comparisons are side-effect-free, and
    // 11.4.13 makes the set's order of evaluation nondeterministic anyway, so
    // the full OR reduction is the cheaper shape (no branch per element) and
    // gives the same answer, x propagation included.
    b.open_region();
    const ir::Index elem     = b.unary(T::Load, b.elem_select(arr, b.unary(T::Load, idx.lv(b, l), l), aty, l), l);
    const ir::Index hit_slot = hit.lv(b, l);
    const ir::Index seen     = b.unary(T::Load, hit.lv(b, l), l);
    const ir::Index match    = inside_leaf_eq(sel, elem, et, ty, real, b, l);
    const ir::Index hit_any  = b.binary(T::Lor, seen, match, l);
    b.binary(T::Store, hit_slot, hit_any, l);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index idx_slot = idx.lv(b, l);
    const ir::Index sc       = b.unary(T::Load, idx.lv(b, l), l);
    const ir::Index one      = b.constant(module_.pool.put_int(i32, 1), l);
    const ir::Index next     = b.binary(T::Add, sc, one, l);
    b.binary(T::Store, idx_slot, next, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test=*/false, l);
    return b.unary(T::Load, hit.lv(b, l), l);
}

// The `inside` set-membership operator (LRM 11.4.13): `sel inside { m0, m1, … }`
// is 1'b1 if any member matches, 1'bx if some comparison is x with no definite
// match, else 1'b0 — exactly a logical-OR reduction of the per-member
// predicates. slang has already widened the left operand and every member (and
// range bound) to a common comparison type (bindMembershipExpressions ->
// contextDetermined), so the compares are width-matched.
//
// Integral members and ranges ride one InsideSet instruction. A member that
// needs its own control flow -- an unpacked array, whose elements are
// traversed -- and every member of a REAL set, whose compares are the float
// ops rather than InsideSet's integral ones, build their own predicate and
// fold in with Lor. That reorders those members ahead of the instruction's,
// which 11.4.13 allows outright ("the order of evaluation of the expressions
// and ranges is nondeterministic").
ir::Index Lowerer::lower_inside(const InsideExpression& e, ir::Builder& b) {
    using T               = ir::Inst::Tag;
    const ir::SourceLoc l = loc(e);
    if (is_string_expr(e.left())) {
        // A string set (LRM 11.4.13; uvm_reg_field::needs_update is
        // `get_access() inside {"RO","RC","RS"}`): per-member equality and, for
        // a range, the two lexicographic relationals, folded with Lor. The
        // backend's Eq / Ge / Le over string operands are already the content
        // compares `==` and `<=` lower to (LRM 6.16.9), so no new instruction.
        const ir::Index sel = lower_expr(e.left(), b);
        ir::Index       acc{};
        bool            have = false;
        const auto      fold = [&](ir::Index p) {
            acc  = have ? b.binary(T::Lor, acc, p, l) : p;
            have = true;
        };
        for (const Expression* m : e.rangeList()) {
            if (m->kind == ExpressionKind::ValueRange) {
                const auto& vr = m->as<ValueRangeExpression>();
                if (vr.rangeKind != ValueRangeKind::Simple) {
                    // A tolerance range needs arithmetic, which a string has none of.
                    fail("inside operator tolerance ranges are not supported");
                }
                const ir::Index above = b.binary(T::Ge, sel, lower_expr(vr.left(), b), l);
                const ir::Index below = b.binary(T::Le, sel, lower_expr(vr.right(), b), l);
                fold(b.binary(T::Land, above, below, l));
                continue;
            }
            if (m->type->getCanonicalType().isUnpackedArray()) {
                // TODO: a container of strings as a set member — the element
                // walk above compares through resize, which rejects strings.
                fail("inside operator with a container of strings is not supported");
            }
            fold(b.binary(T::Eq, sel, lower_expr(*m, b), l));
        }
        if (!have) {
            fail("internal: an inside set with no members");
        }
        return acc;
    }
    // A real set compares at `real`: a shortreal widens exactly (LRM 6.12.2),
    // and 11.4.13's relational range test is defined for reals (Table 11-1).
    const bool      real = e.left().type->isFloating();
    const ir::Ref   ty   = real ? module_.pool.real_type : lower_type(*e.left().type);
    const ir::Index sel =
      real ? coerce(lower_expr(e.left(), b), *e.left().type, ty, b, l) : lower_expr(e.left(), b);

    std::vector<ir::Index> values;
    std::vector<ir::Index> ranges;
    ir::Index              acc{};
    bool                   have_acc = false;
    const auto             fold     = [&](ir::Index p) {
        acc      = have_acc ? b.binary(T::Lor, acc, p, l) : p;
        have_acc = true;
    };

    for (const Expression* m : e.rangeList()) {
        if (m->kind == ExpressionKind::ValueRange) {
            const auto& vr = m->as<ValueRangeExpression>();
            if (vr.rangeKind != ValueRangeKind::Simple) {
                // TODO: tolerance ranges (`[c +/- d]`, `[c +%- d]`, LRM 11.4.13).
                fail("inside operator tolerance ranges are not supported");
            }
            if (vr.left().type->isUnbounded() || vr.right().type->isUnbounded()) {
                // TODO: unbounded `$` range bounds (`[lo:$]` / `[$:hi]`).
                fail("inside operator unbounded ($) range bounds are not supported");
            }
            if (real) {
                // `sel >= lo && sel <= hi`, inclusive both ends; an inverted
                // range is empty and matches nothing, which falls out of the
                // conjunction (11.4.13).
                const ir::Index lo    = coerce(lower_expr(vr.left(), b), *vr.left().type, ty, b, loc(vr.left()));
                const ir::Index hi    = coerce(lower_expr(vr.right(), b), *vr.right().type, ty, b, loc(vr.right()));
                const ir::Index above = b.binary(T::FGe, sel, lo, l);
                const ir::Index below = b.binary(T::FLe, sel, hi, l);
                fold(b.binary(T::Land, above, below, l));
                continue;
            }
            ranges.push_back(b.resize(lower_expr(vr.left(), b), ty, loc(vr.left())));
            ranges.push_back(b.resize(lower_expr(vr.right(), b), ty, loc(vr.right())));
            continue;
        }
        if (m->type->getCanonicalType().isUnpackedArray()) {
            if (is_assoc(*m)) {
                // TODO: an associative-array set member (LRM 11.4.13). Its
                // traversal is the first/next key walk (7.9.4), not the index
                // loop the ordered containers share.
                fail("inside operator with an associative-array set member is not supported");
            }
            fold(is_unpacked_array(*m) ? inside_fixed_array_pred(lower_lvalue(*m, b), *m->type, sel, ty, real, b, l)
                                       : inside_container_pred(*m, sel, ty, real, b, l));
            continue;
        }
        if (real) {
            fold(inside_leaf_eq(sel, lower_expr(*m, b), *m->type, ty, real, b, l));
            continue;
        }
        values.push_back(b.resize(lower_expr(*m, b), ty, loc(*m)));
    }
    if (!values.empty() || !ranges.empty()) {
        fold(b.inside_set(sel, values, ranges, l));
    }
    if (!have_acc) {
        // A range_list has at least one value_range (A.8.3), and every shape of
        // one either lands in the instruction or folds its own predicate.
        fail("internal: an inside set with no members");
    }
    return acc;
}

// Build the lvalue for a part-select over an already-lowered base lvalue: a
// constant `[msb:lsb]` slice, or a runtime-base indexed slice `[off +: w]` /
// `[off -: w]` (width constant, base a runtime value). Shared by the rvalue and
// lvalue paths so a read and a write of the same shape lower identically.
ir::Index Lowerer::lower_range_select(const RangeSelectExpression& rs,
                                      ir::Index                    base,
                                      ir::Builder&                 b,
                                      ir::SourceLoc                l) {
    if (is_queue(rs.value())) {
        // A queue slice `q[a:b]` (LRM 7.10.4) yields a new queue: materialize
        // the source range into a fresh temp handle (usable in any rvalue
        // position — assignment RHS, method receiver, argument).
        return materialize_queue_rvalue(rs, b);
    }
    if (is_dyn_array(rs.value())) {
        // A dynamic-array slice (LRM 7.4.5/7.5.1 -- `da[a:b]`, `da[i +: n]`,
        // `da[i -: n]`) types as a FixedSizeUnpackedArrayType, never a queue
        // (slang forces QueueType only when the base being sliced already is
        // one); `$` and a runtime BASE in the `a:b` form are queue-only (slang
        // requires both bounds literal-constant there), but the indexed forms'
        // base index may be runtime, matching a fixed array. Its storage is
        // handle-indirect (rt_dynarr{data,len}, heap, relocatable) rather than
        // flat/address-indexed, so even the constant-bounds form has no
        // storage-anchored sub-array to point at -- it can never become an
        // ArrSlice, and is materialized position-wise like the fixed array's
        // runtime-indexed form (emit_indexed_slice_copy handles both shapes).
        return materialize_indexed_slice(rs, b, l);
    }
    if (is_unpacked_array(rs.value())) {
        // Fixed-size unpacked array slice (LRM 7.4.5 `busB = busA[7:6]`, "a
        // slice name of an unpacked array is an unpacked array"). Element
        // storage is address-indexed, so this is an ArrSlice lvalue (a
        // position-0-anchored sub-array), not a packed part-select. The
        // per-element copy / eq / store paths consume it; a whole-slice
        // by-value read is still guarded upstream.
        if (rs.getSelectionKind() != RangeSelectionKind::Simple) {
            // An indexed part-select slice has no storage-anchored sub-array to
            // point at (its first element's position is a runtime value), so it
            // never becomes an ArrSlice: the supported contexts expand it
            // position-wise instead (materialize_indexed_slice for a read,
            // emit_indexed_slice_copy for a write). Reaching here means some
            // other consumer asked for it as a plain lvalue.
            fail("indexed part-select slice of an unpacked array is only supported as a "
                 "whole-slice copy source or target, an equality operand, or a "
                 "subroutine argument");
        }
        if (is_aggregate(*rs.value().type)) {
            // TODO: slice of an array whose element is an unpacked struct /
            // heap handle (recursive aggregate storage) — the ArrSlice
            // pre-advance is a flat-storage elem_idx step.
            fail("slice of an unpacked array with aggregate elements is not supported");
        }
        const std::optional<int32_t> lo = try_const_int(rs.left(), Fold::Required);
        if (!lo) {
            fail("expected constant unpacked-array slice bounds");
        }
        const slang::ConstantRange base_range =
          rs.value().type->getCanonicalType().as<slang::ast::FixedSizeUnpackedArrayType>().range;
        // Storage position of the slice's first element (its left bound) in
        // the base array: coordinate `left` maps to position 0 at the base's
        // own left bound (the backend's element normalization), for either
        // endianness.
        const uint32_t first_pos = base_range.left <= base_range.right
                                     ? static_cast<uint32_t>(*lo - base_range.left)
                                     : static_cast<uint32_t>(base_range.left - *lo);
        return b.arr_slice(base, lower_type(*rs.type), first_pos, l);
    }
    // A range select on a multi-dim packed array picks whole W-wide elements
    // (LRM 7.4.6): both bounds and the width scale by the element width W.
    // W is 1 for an ordinary vector, leaving the bit-select math unchanged.
    const auto*                pa    = rs.value().type->getCanonicalType().as_if<slang::ast::PackedArrayType>();
    const uint32_t             w     = pa != nullptr ? static_cast<uint32_t>(pa->elementType.getBitWidth()) : 1;
    const slang::ConstantRange range = packed_range_of(rs.value());
    if (rs.getSelectionKind() == RangeSelectionKind::Simple) {
        const std::optional<int32_t> left  = try_const_int(rs.left(), Fold::Required);
        const std::optional<int32_t> right = try_const_int(rs.right(), Fold::Required);
        if (!left || !right) {
            fail("expected constant part-select bounds");
        }
        // translateIndex puts the left bound at the higher LSB offset for
        // either endianness (slang has already checked the select's
        // direction matches the declared range). Scaled by W: the top
        // element's high bit is `tl*W + W-1`, the bottom element's low bit
        // is `tr*W`.
        const auto msb = static_cast<uint32_t>(range.translateIndex(*left)) * w + (w - 1);
        const auto lsb = static_cast<uint32_t>(range.translateIndex(*right)) * w;
        return b.part_select(base, msb, lsb, l);
    }
    // Indexed part-select: the element count is a constant (LRM 11.5.1) and
    // the base is a runtime value in declared-range coordinates. The IR
    // carries the low-bit LSB offset: the translated base element offset
    // (minus count-1 when the selection extends away from the LSB end — `-:`
    // on a little-endian range, `+:` on a big-endian one) scaled by W. An
    // out-of-range or X/Z base is bounds-checked in the backend per LRM
    // 11.5.1 (out-of-range read -> X, write -> no-op; a fully-out-of-range
    // W-wide window fills X / no-ops as a whole).
    const auto      count = static_cast<uint32_t>(
      const_uint(rs.right(), "an indexed part-select width (LRM 11.5.1)"));
    const bool      down  = rs.getSelectionKind() == RangeSelectionKind::IndexedDown;
    const int32_t   bias  = down == range.isDescending() ? static_cast<int32_t>(count) - 1 : 0;
    // LRM 11.5.1: an indexed part-select whose base is constant too is a constant
    // part-select. Inside the vector it lowers as one, so the driver analysis can
    // pin the bits it drives; a window that reaches outside keeps the bounds-
    // checked dynamic form.
    if (const std::optional<int32_t> c = try_const_int(rs.left())) {
        const int64_t lsb = (int64_t{ range.translateIndex(*c) } - bias) * w;
        const int64_t msb = lsb + int64_t{ count } * w - 1;
        if (lsb >= 0 && msb < static_cast<int64_t>(rs.value().type->getBitWidth())) {
            return b.part_select(base, static_cast<uint32_t>(msb), static_cast<uint32_t>(lsb), l);
        }
    }
    const ir::Index off = lower_packed_offset(rs.left(), range, bias, b, l, w);
    return b.dyn_part_select(base, off, count * w, l);
}

// Whether an expression is an indexed part-select slice `a[i +: n]` / `a[i -: n]`
// of a fixed unpacked array (LRM 7.4.5): the element count is constant but the
// base coordinate is a runtime value, so — unlike a constant-bounds slice — the
// selected elements have no compile-time storage positions.
bool Lowerer::is_indexed_array_slice(const Expression& e) {
    return e.kind == ExpressionKind::RangeSelect && is_unpacked_array(e) &&
           e.as<RangeSelectExpression>().getSelectionKind() != RangeSelectionKind::Simple;
}

// Whether a range-select's BASE is a dynamic array (LRM 7.4.5/7.5.1): unlike a
// fixed unpacked array, a dynamic array's storage is handle-indirect
// (rt_dynarr{data,len}, heap, relocatable), so even its CONSTANT-bounds
// (`Simple`) form has no storage-anchored sub-array to point at -- every
// selection kind expands position-wise (emit_indexed_slice_copy), never
// becomes an ArrSlice.
bool Lowerer::is_dynarr_slice(const Expression& e) {
    return e.kind == ExpressionKind::RangeSelect && is_dyn_array(e.as<RangeSelectExpression>().value());
}

// Whether a range-select has no anchored sub-array lvalue and must instead be
// read/written position-wise: an indexed part-select of a fixed array (runtime
// base), or ANY slice of a dynamic array (constant-bounds included).
bool Lowerer::is_positional_array_slice(const Expression& e) {
    return is_indexed_array_slice(e) || is_dynarr_slice(e);
}

// The parity guards the constant-bounds slice applies, for the indexed form: an
// unpacked-struct / heap-handle element needs the recursive aggregate element
// copy, and a string element needs a deep copy per element.
void Lowerer::guard_indexed_array_slice(const RangeSelectExpression& rs) {
    if (is_aggregate(*rs.value().type)) {
        // TODO: indexed slice of an array of unpacked structs / handles (the
        // same gap the constant-bounds form has).
        fail("slice of an unpacked array with aggregate elements is not supported");
    }
}

// The declared-range coordinate of 0-based position `pos` of an unpacked array
// type, as a constant index: position 0 is the array's own left bound, walking
// toward its right bound for either endianness.
ir::Index Lowerer::elem_coord_const(const ir::Type& t, uint32_t pos, ir::Builder& b, ir::SourceLoc l) {
    const int32_t coord = t.left <= t.right ? static_cast<int32_t>(t.left) + static_cast<int32_t>(pos)
                                            : static_cast<int32_t>(t.left) - static_cast<int32_t>(pos);
    return b.constant(
      module_.pool.put_int(make_int_type(32, false, true), static_cast<uint64_t>(static_cast<int64_t>(coord))), l);
}

// Copy between a positional array slice `rs` (see is_positional_array_slice)
// and an array lvalue `other` of the slice's own type, one position at a time;
// `into_slice` picks the direction (a write to the slice, or a read out of it).
// Two shapes reach here:
//  - An indexed part-select `a[i +: n]` / `a[i -: n]` of EITHER a fixed or a
//    dynamic array (LRM 11.5.1, extended to a dynamic array by 7.5.1): `i` may
//    be a runtime value, `n` a compile-time constant.
//  - A dynamic array's CONSTANT-bounds `a[a:b]` (LRM 7.4.5) -- only reachable
//    when the base is a dynamic array; a fixed array's own constant-bounds
//    form is a live, zero-copy ArrSlice view instead and never reaches here.
//    Both bounds are already constant (slang requires it off a non-queue
//    base -- `$` and a runtime bound there are queue-only), so no runtime
//    index arithmetic is needed at all: position `pos` is simply `lo + pos`.
//
// For the indexed form, positions run in the base array's declaration order
// (its left bound first), so position `pos` selects the base coordinate plus a
// compile-time delta: an ascending range walks up and a descending one down,
// and the `-:` form starts n-1 positions before the base. A dynamic array has
// no declared range to be descending IN (LRM 7.5.1: always 0-based ascending),
// so `asc` is forced true there. The base expression is lowered ONCE (LRM
// 11.5.1 evaluates it in a self-determined context, so a call in the index runs
// once), widened to 32 bits in its own signedness and 4-state-ness the way
// lower_packed_offset does: an x/z base then poisons every position's index,
// which is LRM 7.4.5's "any bit x or z -> the index shall be invalid", and the
// two's-complement wrap of a negative coordinate lands out of range under the
// unsigned bound compare (or back in range for a negative-bound array, which is
// also correct).
//
// Each position is an ordinary ElemSelect, so the LRM 7.4.5/7.5 out-of-range
// rule applies per position: a read past the bound yields the element default
// (x for 4-state, 0 for 2-state, Table 7-1) and a write there is dropped,
// leaving a partially out-of-range slice positionally correct rather than
// shifted. `for_write` on the base-container side threads only for a dynamic
// array: `d[i] = v` (LRM 7.5) notifies a waited-on dynamic array's mutation on
// a for-write element select (ElemSelect::for_write, src/ir/llvm/); a fixed
// array has no handle to notify through, so the flag stays off there exactly
// as it always has.
void Lowerer::emit_indexed_slice_copy(const RangeSelectExpression& rs,
                                      ir::Index                    other,
                                      bool                         into_slice,
                                      ir::Inst::Tag                tag,
                                      ir::Builder&                 b,
                                      ir::SourceLoc                l) {
    guard_indexed_array_slice(rs);
    const bool     simple   = rs.getSelectionKind() == RangeSelectionKind::Simple;
    const bool     dynarr   = is_dyn_array(rs.value());
    if (simple && !dynarr) {
        fail("internal: a fixed array's constant-bounds slice has a live ArrSlice "
             "view and should never reach position-wise copy");
    }
    const ir::Ref  arr_ty   = lower_type(*rs.value().type);
    const ir::Ref  slice_ty = lower_type(*rs.type);
    const ir::Type at       = std::get<ir::Type>(module_.pool.get(arr_ty));
    const ir::Type st       = std::get<ir::Type>(module_.pool.get(slice_ty));
    const ir::Index arr_lv  = lower_lvalue(rs.value(), b);
    const ir::SourceLoc il  = loc(rs.left());

    uint32_t  n;
    ir::Ref   idx_ty;
    ir::Index base;
    if (simple) {
        const std::optional<int32_t> lo = try_const_int(rs.left(), Fold::Required);
        const std::optional<int32_t> hi = try_const_int(rs.right(), Fold::Required);
        if (!lo || !hi) {
            fail("expected constant dynamic-array slice bounds (LRM 7.5.1: `$` and a "
                 "runtime bound there are queue-only)");
        }
        n      = static_cast<uint32_t>(*hi - *lo + 1);
        idx_ty = make_int_type(32, false, true);
        base   = b.constant(module_.pool.put_int(idx_ty, static_cast<uint64_t>(static_cast<int64_t>(*lo))), il);
    } else {
        n = static_cast<uint32_t>(const_uint(rs.right(), "an indexed part-select width (LRM 11.5.1)"));
        // LRM 7.4.5 compares the base index as the VALUE it holds, so it keeps
        // its own width: it extends by its own signedness, then is retyped
        // signed two bits wider, where adding a position's delta cannot wrap
        // (a 64-bit base of 2**32 + 1 is not 1, nor 2**32 - 1 plus one 0).
        const auto    w        = std::max(static_cast<uint32_t>(rs.left().type->getBitWidth()), 32u) + 2;
        const bool    four     = rs.left().type->isFourState();
        const ir::Ref extended = make_int_type(w, four, rs.left().type->isSigned());
        idx_ty                 = make_int_type(w, four, true);
        base                   = b.resize(b.resize(lower_expr(rs.left(), b), extended, il), idx_ty, il);
    }
    const bool down = rs.getSelectionKind() == RangeSelectionKind::IndexedDown;
    const bool asc  = dynarr || at.left <= at.right;
    for (uint32_t pos = 0; pos < n; ++pos) {
        ir::Index     idx   = base;
        int32_t       delta = static_cast<int32_t>(pos);
        if (!simple) {
            const auto last = static_cast<int32_t>(n - 1);
            delta            = asc ? (down ? delta - last : delta) : (down ? -delta : last - delta);
        }
        if (delta != 0) {
            // put_int zero-extends past 64 bits, so a negative delta is subtracted.
            const auto step = static_cast<uint64_t>(delta > 0 ? int64_t{ delta } : -int64_t{ delta });
            idx             = b.binary(delta > 0 ? ir::Inst::Tag::Add : ir::Inst::Tag::Sub, base,
                                       b.constant(module_.pool.put_int(idx_ty, step), il), il);
        }
        b.set_end(idx, to_source_loc(rs.left().sourceRange.end()));
        const ir::Index sel = b.elem_select(arr_lv, idx, arr_ty, l, /*for_write=*/dynarr && into_slice);
        b.set_end(sel, to_source_loc(rs.value().sourceRange.end()));
        const ir::Index oth = b.elem_select(other, elem_coord_const(st, pos, b, l), slice_ty, l);
        if (into_slice) {
            emit_agg_copy(sel, oth, at.element, tag, b, l);
        } else {
            emit_agg_copy(oth, sel, at.element, tag, b, l);
        }
    }
}

// A positional array slice (see is_positional_array_slice) read as a whole:
// copy it position-wise into a fresh temp array of the slice's own type and
// hand back the temp's lvalue, so every whole-slice consumer (copy source,
// equality operand, by-value argument) sees an ordinary array lvalue. The
// temp is a per-call frame Local inside a subroutine and a module signal in
// a process (make_canonical_agg_temp), which also makes it memcpy-compatible
// with a canonical aggregate formal.
ir::Index Lowerer::materialize_indexed_slice(const RangeSelectExpression& rs, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index temp = make_canonical_agg_temp(lower_type(*rs.type), b, l);
    emit_indexed_slice_copy(rs, temp, /*into_slice=*/false, ir::Inst::Tag::Store, b, l);
    return temp;
}

// An unpacked-array operand as an array lvalue: a variable / member / element
// or a fixed array's own constant-bounds slice already is one; a positional
// slice (a fixed array's indexed part-select, or any slice of a dynamic
// array) is materialized into a temp first. Read contexts only — a write
// target must go through emit_indexed_slice_copy, or it would land in the temp.
ir::Index Lowerer::lower_array_operand(const Expression& e, ir::Builder& b) {
    if (is_positional_array_slice(e)) {
        return materialize_indexed_slice(e.as<RangeSelectExpression>(), b, loc(e));
    }
    return lower_lvalue(e, b);
}

// Any supported unpacked-array rvalue as an array lvalue of type `ty`, for a
// consumer that can only read through an anchored array (the indexed-slice
// write path). An lvalue shape passes straight through; a `'{...}` pattern, a
// string literal, a call result, or an indexed slice is staged in a temp of
// `ty`. The staging stores are always BLOCKING: the temp is a private cell
// holding the value as evaluated now, and it is the copy OUT of it that the
// caller may defer (LRM 10.4.2 evaluates a nonblocking RHS at execution time).
ir::Index Lowerer::array_rvalue_lvalue(const Expression& src, ir::Ref ty, ir::Builder& b, ir::SourceLoc l) {
    if (is_positional_array_slice(src)) {
        return materialize_indexed_slice(src.as<RangeSelectExpression>(), b, l);
    }
    if (is_whole_array_lvalue(src) || (src.kind == ExpressionKind::RangeSelect && is_unpacked_array(src))) {
        return lower_lvalue(src, b);
    }
    if (as_assignment_pattern(src) != nullptr || src.kind == ExpressionKind::StringLiteral) {
        const ir::Index temp = make_canonical_agg_temp(ty, b, l);
        emit_agg_store(temp, ty, src, ir::Inst::Tag::Store, b, l);
        return temp;
    }
    if (src.kind == ExpressionKind::Call) {
        const std::optional<ir::Index> cv = lower_user_call(src.as<CallExpression>(), b);
        if (!cv) {
            fail("unpacked-array slice assignment from a void call");
        }
        return *cv;
    }
    // TODO: a concatenation / streaming rvalue as an array slice source (the
    // same by-value array read gap the general rvalue path has).
    fail("unsupported source for an unpacked-array slice assignment");
}

// Whether an expression is an unpacked-array lvalue shape a copy can read
// directly: a whole array (variable / member / element) or any slice of one.
bool Lowerer::is_array_lvalue_shape(const Expression& e) {
    return is_whole_array_lvalue(e) || (e.kind == ExpressionKind::RangeSelect && is_unpacked_array(e));
}

// Whether a whole-array / slice copy from `src` into `dst` may overlap in
// storage: both are selects rooted at the same variable, so an element-wise copy
// could read a position it has already written. LRM 10.4.1 evaluates the
// right-hand side and then assigns, so `a[1:3] = a[0:2]` must see the source's
// pre-assignment values; the caller stages it in a temp when this is true.
// Conservative by design: a same-root pair that cannot actually overlap (say
// `a[0:1] = a[2:3]`) only pays one extra copy.
bool Lowerer::array_copy_may_overlap(const Expression& dst, const Expression& src) {
    const ValueSymbol* dr = lvalue_root(dst);
    const ValueSymbol* sr = lvalue_root(src);
    if (dr == nullptr || sr == nullptr) {
        return false;
    }
    return &canonical_value_symbol(*dr) == &canonical_value_symbol(*sr);
}

// The source of a whole-array / slice copy into `dst`, as an array lvalue of
// `ty`: an overlapping lvalue source is staged in a temp first, everything else
// (a `'{...}` pattern, a call result, an indexed slice) is already a private
// temp built by array_rvalue_lvalue.
ir::Index Lowerer::array_copy_source(const Expression& dst,
                                     const Expression& src,
                                     ir::Ref           ty,
                                     ir::Builder&      b,
                                     ir::SourceLoc     l) {
    const ir::Index lv = array_rvalue_lvalue(src, ty, b, l);
    if (is_positional_array_slice(src) || !is_array_lvalue_shape(src) || !array_copy_may_overlap(dst, src)) {
        return lv;
    }
    const ir::Index temp = make_canonical_agg_temp(ty, b, l);
    emit_agg_copy(temp, lv, ty, ir::Inst::Tag::Store, b, l);
    return temp;
}

// Lower an element select on a packed value to its lvalue index. A 1-bit
// element (ordinary vector `v[i]`) is a BitSelect. A wide element (a
// multi-dim packed array `a[i]`, LRM 7.4.5) selects the whole W-wide element
// k at flat bits [k*W .. k*W+W-1], so it is a W-wide part-select whose low
// bit is the normalized element offset scaled by W. Nested selects compose
// for free: the result type slang assigns to `a[i]` carries the inner range,
// which a further select indexes the same way (part-select-of-part-select).
ir::Index Lowerer::lower_packed_element_lvalue(const ElementSelectExpression& es,
                                               ir::Index                      base,
                                               ir::Builder&                   b,
                                               ir::SourceLoc                  l) {
    const auto w = static_cast<uint32_t>(es.type->getBitWidth());
    if (w == 1) {
        return b.binary(ir::Inst::Tag::BitSelect, base, packed_bit_index(es, b, l), l);
    }
    const slang::ConstantRange range = packed_range_of(es.value());
    if (const std::optional<int32_t> c = try_const_int(es.selector())) {
        const auto lsb = static_cast<uint32_t>(range.translateIndex(*c)) * w;
        return b.part_select(base, lsb + w - 1, lsb, l);
    }
    const ir::Index off = lower_packed_offset(es.selector(), range, 0, b, l, w);
    return b.dyn_part_select(base, off, w, l);
}

// Number of high tag bits a tagged union (LRM 7.3.2) reserves. For a packed
// union slang computes it (`tagBits`); for an unpacked union Clotho
// synthesizes the same `ceil(log2 n)` layout. Zero for a non-tagged union or
// a single-member tagged union (no discriminant needed).
uint32_t Lowerer::union_tag_bits(const slang::ast::Type& ct) {
    if (ct.isPackedUnion()) {
        return ct.as<slang::ast::PackedUnionType>().tagBits;
    }
    const auto& ut = ct.as<slang::ast::UnpackedUnionType>();
    if (!ut.isTagged) {
        return 0;
    }
    uint32_t n = 0;
    for (const auto& f : static_cast<const Scope&>(ut).membersOfType<slang::ast::FieldSymbol>()) {
        (void)f;
        ++n;
    }
    return n > 1 ? static_cast<uint32_t>(std::bit_width(n - 1)) : 0;
}

// Lower a `tagged Member (value)` union setter (LRM 7.3.2), the only
// expression that assigns a tagged union as a whole value. Builds the flat
// bit-vector `{tag, gap, payload}` MSB-to-LSB by concatenation: the member's
// `fieldIndex` in the high `tagBits` bits, the payload in the low bits, and
// the bits between (present only for a member narrower than the widest, or a
// `void` member with no payload) taking the union's power-on default — x for
// a 4-state union, 0 for 2-state. This mirrors slang's own const-eval
// (`type->getDefaultValue()` then overlay tag/payload), so a runtime setter
// and a folded parameter of the same expression produce identical bits
// (docs/design/tagged-unions.md).
ir::Index Lowerer::lower_tagged_union_set(const slang::ast::TaggedUnionExpression& tu, ir::Builder& b) {
    const ir::SourceLoc     l        = loc(tu);
    const slang::ast::Type& ct       = tu.type->getCanonicalType();
    const auto              lay      = module_.pool.layout(lower_type(*tu.type));
    const bool              four     = lay.domain == ir::Domain::Four;
    const uint32_t          tag_bits = union_tag_bits(ct);
    const auto&             field    = tu.member.as<FieldSymbol>();
    const uint32_t          member_w = static_cast<uint32_t>(field.getType().getBitWidth());

    std::vector<ir::Index> parts;
    if (tag_bits != 0) {
        parts.push_back(
          b.constant(module_.pool.put_int(make_int_type(tag_bits, four, false), field.fieldIndex), l));
    }
    if (const uint32_t gap_w = lay.width - tag_bits - member_w; gap_w != 0) {
        parts.push_back(
          b.constant(four ? svint_to_ref(slang::SVInt::createFillX(gap_w, false), make_int_type(gap_w, true, false))
                          : module_.pool.put_int(make_int_type(gap_w, false, false), 0),
                     l));
    }
    if (member_w != 0) {
        parts.push_back(b.resize(lower_expr(*tu.valueExpr, b), make_int_type(member_w, four, false), l));
    }
    return b.concat(parts, l);
}

// Guard one tagged-union member access against the union's current tag (LRM
// 11.9): "Members of tagged unions can be read or assigned using the usual dot
// notation. Such accesses are completely type-checked, i.e., the value read or
// assigned shall be consistent with the current tag. In general, this can
// require a run-time check. An attempt to read or assign a value whose type is
// inconsistent with the tag results in a run-time error."
//
// So a member write does NOT retag: LRM 7.3.2 says "the tag and value can only
// be updated together consistently using a statically type-checked tagged union
// expression", and 11.9's own example calls `i1.Add = '{19, 4, 3};` legal "only
// if the instruction variable i1 currently has tag Add". The whole-value
// `tagged` setter (lower_tagged_union_set) remains the only way the tag moves.
//
// `base` is the union's already-computed lvalue, so nothing is re-evaluated.
// Emits nothing when no tag value can disagree: a plain (untagged) union, or a
// single-member tagged union, whose one member is always the active one.
void Lowerer::check_union_tag(const MemberAccessExpression& ma,
                              const FieldSymbol&            field,
                              ir::Index                     base,
                              bool                          is_write,
                              ir::Builder&                  b,
                              ir::SourceLoc                 l) {
    const slang::ast::Type& ct = ma.value().type->getCanonicalType();
    if (!ct.isTaggedUnion()) {
        return;
    }
    const uint32_t tag_bits = union_tag_bits(ct);
    if (tag_bits == 0) {
        return;
    }
    const auto lay = module_.pool.layout(lower_type(*ma.value().type));
    // The tag is the high `tag_bits` of the flat value; read it off the same
    // lvalue the member select is about to use.
    const ir::Index tagv =
      b.unary(ir::Inst::Tag::Load, b.part_select(base, lay.width - 1, lay.width - tag_bits, l), l);

    // Member names in tag order (a field's tag value IS its fieldIndex), so the
    // report can name whichever tag the union turns out to hold. Both union types
    // derive Scope, but Type itself does not, so the cast goes through the
    // concrete type.
    const Scope& sc = ct.isPackedUnion()
                        ? static_cast<const Scope&>(ct.as<slang::ast::PackedUnionType>())
                        : static_cast<const Scope&>(ct.as<slang::ast::UnpackedUnionType>());
    std::vector<std::string_view> names;
    for (const auto& f : sc.membersOfType<FieldSymbol>()) {
        names.push_back(f.name);
    }
    b.tagged_check(tagv, field.fieldIndex, is_write, field.name, names, l);
}

// Lower a pattern (LRM 12.6) against a subject rvalue `subject` of type `sty`,
// returning a 1-bit match predicate and, as a side effect, binding any
// `PatternVarSymbol`s the pattern introduces (storing the matched sub-value
// into the variable's slot). Bindings are emitted eagerly into the current
// region — a pattern variable is read only in the arm that the predicate
// guards, so an unconditional store of a pure sub-value is harmless. The
// subject is a flat rvalue: sub-values are extracted by shift/resize (low
// bits = resize, a field/tag at offset = Srl then resize), so this works for
// packed structs and tagged unions but not an unpacked (aggregate) subject,
// which is rejected loud. See docs/design/tagged-unions.md.
// `cmp` is the 2-bit comparison op the enclosing case keyword selects (LRM
// 12.6.1): CaseEq (`case` / `if` / `?:`, exact 4-state), CasezEq (`casez`,
// z wildcard), or CasexEq (`casex`, x/z wildcard). It governs every bit
// comparison the pattern generates — the tag compare and each Constant leaf.
ir::Index Lowerer::lower_pattern(const slang::ast::Pattern& pat,
                                 ir::Index                  subject,
                                 const slang::ast::Type&    sty,
                                 ir::Inst::Tag              cmp,
                                 ir::Builder&               b,
                                 ir::SourceLoc              l) {
    using PK = slang::ast::PatternKind;
    switch (pat.kind) {
        case PK::Wildcard:
            return true_const(b, l);
        case PK::Variable: {
            // `.name` — bind the pattern variable to the whole subject.
            const auto& var = pat.as<slang::ast::VariablePattern>().variable;
            declare_storage(var, /*force_frame*/ true);
            const ir::Index slot = value_lvalue(var, b, l);
            b.binary(ir::Inst::Tag::Store, slot, b.resize(subject, lower_type(var.getType()), l), l);
            return true_const(b, l);
        }
        case PK::Constant: {
            // A constant expression: the subject matches when equal under the
            // case keyword's comparison (exact `===` for `case`; z/x wildcarded
            // for casez/casex). An x subject under `case` then fails to match.
            const auto&     cp = pat.as<slang::ast::ConstantPattern>();
            const ir::Ref   ty = lower_type(sty);
            const ir::Index cv = b.resize(lower_expr(cp.expr, b), ty, l);
            return b.binary(cmp, subject, cv, l);
        }
        case PK::Tagged: {
            // `tagged Member [pat]` — the tag must equal the member's index,
            // then (if present) the nested pattern must match the payload.
            const auto&    tp       = pat.as<slang::ast::TaggedPattern>();
            const uint32_t tag_bits = union_tag_bits(sty);
            ir::Index      tag_pred = true_const(b, l);
            if (tag_bits != 0) {
                const auto      lay  = module_.pool.layout(lower_type(sty));
                const bool      four = lay.domain == ir::Domain::Four;
                const ir::Ref   tt   = make_int_type(tag_bits, four, false);
                const ir::Index sh =
                  b.constant(module_.pool.put_int(make_int_type(32, false, false), lay.width - tag_bits), l);
                const ir::Index tagv = b.resize(b.binary(ir::Inst::Tag::Srl, subject, sh, l), tt, l);
                const ir::Index idx  = b.constant(module_.pool.put_int(tt, tp.member.fieldIndex), l);
                tag_pred             = b.binary(cmp, tagv, idx, l);
            }
            if (tp.valuePattern == nullptr) {
                return tag_pred; // void member: tag match is the whole test.
            }
            const slang::ast::Type& mt      = tp.member.getType();
            const ir::Index         payload = b.resize(subject, lower_type(mt), l);
            const ir::Index sub_pred = lower_pattern(*tp.valuePattern, payload, mt.getCanonicalType(), cmp, b, l);
            return b.binary(ir::Inst::Tag::Land, tag_pred, sub_pred, l);
        }
        case PK::Structure: {
            // `'{field: pat, ...}` — every field's nested pattern must match.
            if (sty.isUnpackedStruct()) {
                // TODO: structure pattern over an unpacked struct (aggregate
                // subject has no flat value to shift/extract from). Phase 2.1.
                fail("structure pattern over an unpacked struct is not supported");
            }
            const auto& sp  = pat.as<slang::ast::StructurePattern>();
            ir::Index   acc = true_const(b, l);
            for (const auto& fp : sp.patterns) {
                const slang::ast::Type& ft = fp.field->getType();
                const ir::Index         sh = b.constant(
                  module_.pool.put_int(make_int_type(32, false, false), static_cast<uint32_t>(fp.field->bitOffset)),
                  l);
                const ir::Index fv = b.resize(b.binary(ir::Inst::Tag::Srl, subject, sh, l), lower_type(ft), l);
                acc                = b.binary(
                  ir::Inst::Tag::Land, acc, lower_pattern(*fp.pattern, fv, ft.getCanonicalType(), cmp, b, l), l);
            }
            return acc;
        }
        default:
            fail("unsupported pattern");
    }
}

// The bit-comparison op for a pattern-case keyword (LRM 12.6.1). `if` / `?:`
// matching is always exact (CaseEq) — no casez/casex form exists there.
ir::Inst::Tag Lowerer::pattern_cmp(CaseStatementCondition cond) {
    switch (cond) {
        case CaseStatementCondition::WildcardJustZ:
            return ir::Inst::Tag::CasezEq;
        case CaseStatementCondition::WildcardXOrZ:
            return ir::Inst::Tag::CasexEq;
        default:
            return ir::Inst::Tag::CaseEq;
    }
}

// `case (expr) matches` (LRM 12.6): a first-match chain of pattern items,
// each an `item.pattern [ &&& filter ] : stmt`. Lowered as a nested if/else
// over the shared subject value (evaluated once), mirroring the plain-case
// priority chain. unique/priority overlap/no-match reporting for pattern
// case is a follow-up (guarded in the dispatcher).
void Lowerer::lower_pattern_case(const slang::ast::PatternCaseStatement& c, ir::Builder& b) {
    using Check = slang::ast::UniquePriorityCheck;
    if (c.condition == CaseStatementCondition::Inside) {
        // `case (x) matches inside` is not valid SV (matches and inside are
        // mutually exclusive case forms); defend anyway.
        fail("case ... matches inside is not supported");
    }
    if (is_aggregate(*c.expr.type)) {
        // TODO: pattern case over an unpacked (aggregate) subject — no flat
        // value to extract sub-values from (Phase 2.1).
        fail("pattern case over an unpacked aggregate subject is not supported");
    }
    const ir::Index subject = lower_expr(c.expr, b);
    if (c.check == Check::None) {
        // Plain first-match: the lazy chain binds each item's pattern vars
        // only when its item is reached.
        lower_pattern_case_item(c, 0, subject, b);
        return;
    }
    lower_pattern_case_checked(c, subject, b);
}

// Compute a pattern item's match predicate (tag/field tests) AND-ed with its
// optional `&&& filter`, binding the pattern's variables as a side effect.
ir::Index Lowerer::lower_pattern_item_pred(const slang::ast::PatternCaseStatement&            c,
                                           const slang::ast::PatternCaseStatement::ItemGroup& item,
                                           ir::Index                                          subject,
                                           ir::Builder&                                       b,
                                           ir::SourceLoc                                      l) {
    ir::Index pred =
      lower_pattern(*item.pattern, subject, c.expr.type->getCanonicalType(), pattern_cmp(c.condition), b, l);
    if (item.filter != nullptr) {
        // `&&& filter` — evaluated after the pattern's bindings are in scope.
        pred = b.binary(ir::Inst::Tag::Land, pred, lower_predicate(*item.filter, b), l);
    }
    return pred;
}

void Lowerer::lower_pattern_case_item(const slang::ast::PatternCaseStatement& c,
                                      size_t                                  k,
                                      ir::Index                               subject,
                                      ir::Builder&                            b) {
    const ir::SourceLoc l = loc(c);
    if (k == c.items.size()) {
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        }
        return;
    }
    const ir::Index pred = lower_pattern_item_pred(c, c.items[k], subject, b, l);

    b.open_region();
    lower_stmt(*c.items[k].stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    lower_pattern_case_item(c, k + 1, subject, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, l);
}

// unique / unique0 / priority pattern case (LRM 12.6 → 12.5.3): every item's
// predicate is evaluated once up front (the LRM permits any-order eager
// evaluation and requires continuing past the first match to detect overlap),
// then the temps drive the overlap check and the first-match dispatch —
// mirroring lower_case_unique over pattern items. Binding all items' pattern
// vars up front is harmless (pure sub-value extraction, each read only in its
// own arm).
void Lowerer::lower_pattern_case_checked(const slang::ast::PatternCaseStatement& c, ir::Index subject, ir::Builder& b) {
    using Check                = slang::ast::UniquePriorityCheck;
    const ir::SourceLoc l      = loc(c);
    const std::string   banner = banner_at(c.sourceRange.start());
    const bool          no_match_report =
      (c.check == Check::Priority || c.check == Check::Unique) && c.defaultCase == nullptr;
    const bool needs_overlap = c.check == Check::Unique || c.check == Check::Unique0;

    std::vector<ir::Index> preds;
    preds.reserve(c.items.size());
    for (const auto& item : c.items) {
        preds.push_back(lower_pattern_item_pred(c, item, subject, b, l));
    }
    if (needs_overlap) {
        emit_overlap_check(preds, banner, l, b);
    }
    dispatch_pattern_preds(c, 0, preds, no_match_report, banner, l, b);
}

// First-match dispatch for a checked pattern case, reusing the precomputed
// predicates: item k's body runs when preds[k] is the first true one. The
// terminal runs the default, else emits the no-match report when requested
// (priority / unique with no default).
void Lowerer::dispatch_pattern_preds(const slang::ast::PatternCaseStatement& c,
                                     size_t                                  k,
                                     const std::vector<ir::Index>&           preds,
                                     bool                                    no_match_report,
                                     const std::string&                      banner,
                                     ir::SourceLoc                           rloc,
                                     ir::Builder&                            b) {
    if (k == c.items.size()) {
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        } else if (no_match_report) {
            b.case_violation(banner, 0, rloc);
        }
        return;
    }
    b.open_region();
    lower_stmt(*c.items[k].stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    dispatch_pattern_preds(c, k + 1, preds, no_match_report, banner, rloc, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(preds[k], then_region, else_region, rloc);
}

// Struct/union member select (LRM 7.2.1 / 7.3): a packed struct or union is
// a flat bit-vector, so `x.field` is exactly a constant part-select at the
// field's declared bit offset (0 for every union member — all members
// overlay the LSB) — the same IR shape (PartSelect over the base lvalue)
// as a plain `vec[msb:lsb]`, so it reuses the backend's existing
// partial-write / NBA-shadow machinery with no backend changes. Nested
// struct member access (`x.outer.inner`) composes for free: the base
// lvalue is itself a part-select, and part-select-of-a-part-select
// already works (the same shape a multi-dimensional packed select uses).
// `is_write` only selects the wording of the LRM 11.9 diagnostic (see
// check_union_tag); the guard itself is identical for a read and a write.
ir::Index Lowerer::lower_member_lvalue(const MemberAccessExpression& ma,
                                       ir::Builder&                  b,
                                       ir::SourceLoc                 l,
                                       bool                          is_write) {
    if (ma.value().type->getCanonicalType().isVirtualInterface()) {
        // `vif.sig` (LRM 25.9): the member of whichever instance the handle
        // names. slang resolved `ma.member` against a cached placeholder body,
        // so it names WHICH member; the representative names where in a
        // definition's state that member sits.
        const ir::Index                 handle = lower_expr(ma.value(), b);
        const std::optional<ir::Signal> rep =
          vif_member_signal(ma.member, vif_named_body(ma.value()),
                            "'" + std::string(ma.member.name) + "'");
        if (!rep) {
            return vif_null_place(*ma.type, b, l);
        }
        return b.iface_signal(handle, *rep, l);
    }
    if (const auto* prop = ma.member.as_if<slang::ast::ClassPropertySymbol>()) {
        if (prop->lifetime == slang::ast::VariableLifetime::Static) {
            // `obj.static_prop` (LRM 8.9): the shared slot, not the object body.
            // Evaluate the object expression for any side effects (it may be a
            // call), then discard it and address the shared signal.
            (void)lower_expr(ma.value(), b);
            return b.signal(declare_static_prop(*prop), l);
        }
        // `obj.prop` (LRM 8.4): load the object handle (a value), then a
        // handle-deref + field select rooted at the object body. The base
        // handle is an rvalue (the pointer), not the slot's lvalue.
        const ir::Index handle = lower_expr(ma.value(), b);
        return class_member_lvalue(
          handle, *prop, lower_type(*ma.value().type), b, l, to_source_span(ma.value().sourceRange));
    }
    const auto* field = ma.member.as_if<FieldSymbol>();
    if (field == nullptr) {
        // TODO: non-field member access (interface modports, ...) — not lowered.
        fail("unsupported member access");
    }
    // A packed or unpacked union member sits at bit_offset 0 (all members
    // overlay the LSB), so it flows to the flat part-select path below — an
    // unpacked union lowers to the same flat plane value as a packed union
    // (max member width / domain), just with possibly-different member widths.
    const ir::Index base = lower_lvalue(ma.value(), b);
    // Every tagged-union member access — read, write, ref argument, force,
    // $fread target — funnels through here, so this is the one place the LRM
    // 11.9 tag check has to be planted.
    check_union_tag(ma, *field, base, is_write, b, l);
    // An unpacked struct is an aggregate: the member is addressed by field
    // index (MemberSelect -> struct GEP), not a bit range. A packed struct/union
    // (and an unpacked union) is a flat bit-vector, so the member is a constant
    // part-select.
    if (ma.value().type->getCanonicalType().isUnpackedStruct()) {
        return b.member_select(base, field->fieldIndex, lower_type(*ma.value().type), l);
    }
    const auto lsb = static_cast<uint32_t>(field->bitOffset);
    const auto msb = lsb + static_cast<uint32_t>(ma.type->getBitWidth()) - 1;
    return b.part_select(base, msb, lsb, l);
}

// Lower an expression as an lvalue, returning the lvalue index (no Load).
ir::Index Lowerer::lower_lvalue(const Expression& expr, ir::Builder& b, bool is_write) {
    const ir::SourceLoc l = loc(expr);
    switch (expr.kind) {
        case ExpressionKind::Assignment:
            // slang (post-8.0) binds a system-task output argument ($fgets,
            // $sformat, $*scanf, ... destinations) as an AssignmentExpression
            // whose left side is the target lvalue, the same shape a user
            // task's output actual has. Unwrap to the target.
            return lower_lvalue(expr.as<AssignmentExpression>().left(), b);
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            // A hierarchical lvalue (`top.u1.sig = …`, LRM 23.6) writes the
            // target instance's signal; the resolved symbol goes through the
            // ordinary signal path (selects below recurse to this base).
            return value_lvalue(expr.as<ValueExpressionBase>().symbol, b, l);
        case ExpressionKind::ElementSelect: {
            const ElementSelectExpression& es = expr.as<ElementSelectExpression>();
            // The direction travels down the chain: `a[k][i] = v` writes the outer
            // entry too, since the entry must exist for its element to be
            // written (LRM 7.8), while a read must leave the map alone (7.8.6).
            const ir::Index base = lower_lvalue(es.value(), b, is_write);
            if (is_assoc(es.value())) {
                return var_assoc_select(es, base, b, l, is_write);
            }
            if (is_unpacked_array(es.value()) || is_dyn_array(es.value()) || is_queue(es.value())) {
                return var_elem_select(es, base, b, l, is_write);
            }
            return lower_packed_element_lvalue(es, base, b, l);
        }
        case ExpressionKind::RangeSelect: {
            const RangeSelectExpression& rs   = expr.as<RangeSelectExpression>();
            const ir::Index              base = lower_lvalue(rs.value(), b);
            return lower_range_select(rs, base, b, l);
        }
        case ExpressionKind::MemberAccess:
            return lower_member_lvalue(expr.as<MemberAccessExpression>(), b, l, is_write);
        case ExpressionKind::Call:
            // A dynamic-array / queue-returning call is an lvalue: its result
            // handle (allocated by the backend) is read by an assignment /
            // .size() / element access / foreach. Others are not lvalues.
            if (is_handle_expr(expr)) {
                const CallExpression& call = expr.as<CallExpression>();
                // A locator method (LRM 7.12.1) returns a queue, and its
                // handle is reached the same way -- but it is a system call,
                // which lower_user_call refuses.
                if (const std::optional<ir::Index> v = lower_container_method(call, b, l)) {
                    return *v;
                }
                const std::optional<ir::Index> v = lower_user_call(call, b);
                if (!v) {
                    fail("handle-returning call used as an lvalue produced no value");
                }
                return *v;
            }
            // An aggregate-returning call is an lvalue for the same reason: the
            // callee writes its result through the caller-side out-param temp the
            // call allocates, and the call's value IS that temp's lvalue. Every
            // structural READ of an aggregate result needs exactly that -- `%p` of
            // it (LRM 21.2.1.6), a member select `f().m`, an element select
            // `f()[i]`, and LRM 13.4's `return f();`, which all route here. Lowered
            // once, so a call operand is evaluated exactly once. A WRITE never
            // arrives: slang rejects `f().m = v` as "expression is not assignable",
            // so no store can be silently discarded into the temp.
            if (is_agg_abi_type(*expr.type)) {
                return lower_expr(expr, b);
            }
            fail("unsupported assignment target kind=Call");
        case ExpressionKind::ConditionalOp: {
            // An aggregate `?:` in a position that structurally requires an
            // lvalue (LRM 11.4.11), for the same reason the aggregate call above
            // is one: the value has no plane representation, so a structural read
            // has to address storage. Staged into a canonical aggregate temp and
            // handed over as that temp's lvalue -- `%p` of it (LRM 21.2.1.6), a
            // member select `(c ? x : y).m`, an element select `(c ? x : y)[i]`,
            // an `==` operand. emit_agg_store owns the branch itself, so the arms
            // stay conditional even here, and the staging costs one temp in an
            // expression position that by definition has no destination of its
            // own to be written into.
            //
            // A WRITE never arrives: slang rejects a conditional as assignable
            // ("expression is not assignable"), exactly as it does `f().m = v`,
            // so no store can be silently discarded into the temp. Guarded
            // anyway, because silently writing a temp is the one failure mode
            // here that would produce no diagnostic at all.
            if (!is_agg_abi_type(*expr.type)) {
                fail("unsupported assignment target kind=ConditionalOp");
            }
            if (is_write) {
                fail("a conditional operator is not a legal assignment target "
                     "(LRM 11.4.11)");
            }
            const ir::Ref   cty  = lower_type(*expr.type);
            const ir::Index temp = make_canonical_agg_temp(cty, b, l);
            emit_agg_store(temp, cty, expr, ir::Inst::Tag::Store, b, l);
            return temp;
        }
        default:
            fail("unsupported assignment target kind=" + std::string(slang::ast::toString(expr.kind)));
    }
}

// Evaluate a constant integer expression (a literal, a parameter reference,
// or any constant-foldable expression over them, e.g. `W-1` or `W*2`).
// Folded with slang against the current instance scope so parameterized
// widths, replication counts and part-select bounds resolve to a value.
// `what` names the construct whose grammar demands the constant, so the
// refusal reads as a language rule, not a parse error.
uint64_t Lowerer::const_uint(const Expression& e, std::string_view what) {
    const slang::ConstantValue cv = fold_const(e, Fold::Required);
    if (!cv.isInteger()) {
        fail(std::string(what) + " must be a constant integer expression");
    }
    return cv.integer().as<uint64_t>().value_or(0);
}

// Fold an expression with slang. See Fold for what each mode allows.
slang::ConstantValue Lowerer::fold_const(const Expression& e, Fold mode, const slang::ast::ASTContext& astCtx) {
    if (mode == Fold::Runtime && has_user_call(e)) {
        return nullptr;
    }
    slang::ast::EvalContext evalCtx(astCtx);
    return e.eval(evalCtx);
}

slang::ConstantValue Lowerer::fold_const(const Expression& e, Fold mode) {
    const slang::ast::ASTContext astCtx(*scope_, slang::ast::LookupLocation::max);
    return fold_const(e, mode, astCtx);
}

// Constant-fold an integer expression against the current scope; nullopt
// when it is non-constant (a runtime index) or holds X/Z bits.
std::optional<int32_t> Lowerer::try_const_int(const Expression& e, Fold mode) {
    const slang::ConstantValue cv = fold_const(e, mode);
    if (!cv.isInteger()) {
        return std::nullopt;
    }
    return cv.integer().as<int32_t>();
}

// Declared range of the packed value a select indexes into. Taken from
// slang's type so nested selects use whatever coordinates slang assigned
// to the inner select's result.
slang::ConstantRange Lowerer::packed_range_of(const Expression& value) {
    return value.type->getCanonicalType().getFixedRange();
}

// Whether the offset arithmetic of lower_packed_offset, `(v - adj) * stride` for a
// descending range and `(adj - v) * stride` for an ascending one, is exact in 32
// bits for every value `v` of a `width`-bit index of the given signedness. Where
// nothing is computed (stride 1, adj 0, descending) the index only has to be
// reinterpreted as an i32: a value that does not fit, a 32-bit unsigned one
// included, turns negative, which no container contains.
static bool packed_offset_fits_i32(uint32_t width, bool is_signed, bool descending, int64_t adj, uint32_t stride) {
    if (width > 32) {
        return false;
    }
    if (descending && adj == 0 && stride == 1) {
        return true;
    }
    if (stride > (uint32_t{ 1 } << 24)) {
        return false; // keeps the products below inside an i64
    }
    const int64_t vmin = is_signed ? -(int64_t{ 1 } << (width - 1)) : 0;
    const int64_t vmax = is_signed ? (int64_t{ 1 } << (width - 1)) - 1 : (int64_t{ 1 } << width) - 1;
    const int64_t lo   = (descending ? vmin - adj : adj - vmax) * stride;
    const int64_t hi   = (descending ? vmax - adj : adj - vmin) * stride;
    return lo >= INT32_MIN && hi <= INT32_MAX;
}

// Lower a packed-select index to its 32-bit zero-based LSB offset, minus
// `bias` (0 for a bit select; width-1 when an indexed part-select's low
// bit sits below the translated base), then scaled by `stride` (the element
// bit width for a multi-dim packed array element/range select, 1 otherwise).
// A constant index folds to an immediate. A runtime index is widened by its
// own signedness (so a signed index into a negative-bound range resolves
// correctly) and keeps the source's 4-state-ness so an X/Z index still
// poisons the offset arithmetic instead of being silently coerced to a known
// value. The stride multiply is built in `off_ty` so it shares the offset's
// domain (a 4-state offset is physically wider than a 2-state constant, so a
// mismatched-domain Mul operand would be an invalid instruction).
//
// LRM 11.5.1 compares the index VALUE against the vector, so a 32-bit offset is
// only used where it cannot wrap (packed_offset_fits_i32): an `int` index into
// `[3:0][7:0]` or an unsigned one into a negative range can, and a 64-bit one
// never fits, and those take lower_wide_packed_offset.
ir::Index Lowerer::lower_packed_offset(const Expression&    idx,
                                       slang::ConstantRange range,
                                       int32_t              bias,
                                       ir::Builder&         b,
                                       ir::SourceLoc        l,
                                       uint32_t             stride) {
    if (const std::optional<int32_t> c = try_const_int(idx)) {
        const auto off = static_cast<uint32_t>((range.translateIndex(*c) - bias) * static_cast<int32_t>(stride));
        return b.constant(module_.pool.put_int(make_int_type(32, false, false), off), l);
    }
    const bool    descending = range.isDescending();
    const int64_t adj        = descending ? int64_t{ range.right } + bias : int64_t{ range.right } - bias;
    if (!packed_offset_fits_i32(static_cast<uint32_t>(idx.type->getBitWidth()), idx.type->isSigned(), descending,
                                adj, stride)) {
        return lower_wide_packed_offset(idx, descending, adj, b, stride);
    }
    // The normalization arithmetic is attributed to the index expression, not
    // to the select `l` sits on: this offset IS the index as far as everything
    // downstream can see, so a diagnostic that points at it (--sanitize=bounds
    // puts its caret here) must point at what the user wrote inside the
    // brackets rather than at the vector being indexed.
    const ir::SourceLoc il     = loc(idx);
    const ir::Ref       off_ty = make_int_type(32, idx.type->isFourState(), idx.type->isSigned());
    const ir::Index     v      = b.resize(lower_expr(idx, b), off_ty, il);
    ir::Index           off;
    if (range.isDescending()) {
        const int32_t sub = range.right + bias; // off = idx - right - bias
        off = sub == 0 ? v
                       : b.binary(ir::Inst::Tag::Sub,
                                  v,
                                  b.constant(module_.pool.put_int(off_ty, static_cast<uint32_t>(sub)), il),
                                  il);
    } else {
        // Big-endian: off = (right - bias) - idx.
        off = b.binary(ir::Inst::Tag::Sub,
                       b.constant(module_.pool.put_int(off_ty, static_cast<uint32_t>(range.right - bias)), il),
                       v,
                       il);
    }
    // The extent goes on whatever instruction the backend will report, which
    // is the offset's final value.
    const ir::Index result =
      stride == 1 ? off
                  : b.binary(ir::Inst::Tag::Mul, off, b.constant(module_.pool.put_int(off_ty, stride), il), il);
    b.set_end(result, to_source_loc(idx.sourceRange.end()));
    return result;
}

// lower_packed_offset for an index whose 32-bit arithmetic could wrap. The
// offset is computed in a signed type wide enough that nothing does, so the
// backend sees the true value and (select_index) decides it is out of range,
// or keeps it exact when it only straddles the vector.
ir::Index Lowerer::lower_wide_packed_offset(const Expression& idx,
                                            bool              descending,
                                            int64_t           adj,
                                            ir::Builder&      b,
                                            uint32_t          stride) {
    using T = ir::Inst::Tag;
    const ir::SourceLoc il   = loc(idx);
    const auto          w    = static_cast<uint32_t>(idx.type->getBitWidth());
    const bool          four = idx.type->isFourState();
    // |index - adj| < 2^(max(w, 32) + 1) and the stride scales it by less than
    // 2^bit_width(stride); one more bit is the sign.
    const uint32_t ew = std::max(64u, std::max(w, 32u) + 2 + static_cast<uint32_t>(std::bit_width(stride)));
    // The index extends by its own signedness, then is retyped signed (same
    // width, so no code): the offset is a signed quantity whatever the index is.
    const ir::Ref   off_ty = make_int_type(ew, four, true);
    const ir::Index v =
      b.resize(b.resize(lower_expr(idx, b), make_int_type(ew, four, idx.type->isSigned()), il), off_ty, il);
    // put_int zero-extends past 64 bits, so only non-negative constants are built.
    const auto constant = [&](uint64_t n) { return b.constant(module_.pool.put_int(off_ty, n), il); };
    ir::Index  off;
    if (descending) {
        // off = v - adj
        off = adj == 0 ? v
              : adj > 0 ? b.binary(T::Sub, v, constant(static_cast<uint64_t>(adj)), il)
                        : b.binary(T::Add, v, constant(static_cast<uint64_t>(-adj)), il);
    } else {
        // off = adj - v
        off = adj >= 0 ? b.binary(T::Sub, constant(static_cast<uint64_t>(adj)), v, il)
                       : b.binary(T::Sub, constant(0), b.binary(T::Add, v, constant(static_cast<uint64_t>(-adj)), il), il);
    }
    const ir::Index result = stride == 1 ? off : b.binary(T::Mul, off, constant(stride), il);
    b.set_end(result, to_source_loc(idx.sourceRange.end()));
    return result;
}

// Normalized bit offset for a 1-bit element select on a packed value.
// Wide (multi-dim packed array) elements go through
// lower_packed_element_lvalue, which part-selects instead of bit-selecting;
// this is only ever reached for a 1-bit result.
ir::Index Lowerer::packed_bit_index(const ElementSelectExpression& es, ir::Builder& b, ir::SourceLoc l) {
    return lower_packed_offset(es.selector(), packed_range_of(es.value()), 0, b, l);
}

// The lvalue leaf for a value-symbol reference: a function-frame Local if the
// symbol is one of the current function's locals, else a module Signal (which
// a function may also reference directly). signal_of fails loudly if unknown.
ir::Index Lowerer::value_lvalue(const ValueSymbol& vs, ir::Builder& b, ir::SourceLoc l) {
    if (sva_sampled_) {
        // Inside a sampled assertion atom (LRM 16.5): an assertion expression
        // reads the value each variable held before this time step's updates,
        // so every operand resolves to its Preponed shadow rather than the live
        // slot. signal_of is the only path taken here -- an assertion is never
        // lowered inside a function, so a frame local cannot reach this, and a
        // `this`-relative property fails loudly there rather than silently
        // reading live state.
        return b.signal(sva_shadow_of(signal_of(vs)), l);
    }
    // A static class property is shared storage (never a frame local or `this`
    // member), whether referenced bare in a method, as `C::p`, or `p` in a
    // static method. Resolve it to its shared signal before any frame/this path.
    if (const auto* prop = vs.as_if<slang::ast::ClassPropertySymbol>()) {
        if (prop->lifetime == slang::ast::VariableLifetime::Static) {
            return b.signal(declare_static_prop(*prop), l);
        }
    }
    if (default_arg_this_) {
        if (const auto* prop = vs.as_if<slang::ast::ClassPropertySymbol>()) {
            // A bare property reference inside a DEFAULT argument expression
            // (LRM 13.5.3): the default was bound in the callee's declaration
            // scope, so the property belongs to the CALL's receiver. Ahead of
            // both the ctor and the frame paths, which name different objects
            // whenever the receiver was spelled out. Same precedence, and the
            // same reason, as resolve_method_this.
            return class_member_lvalue(default_arg_this_->handle, *prop, default_arg_this_->cls, b, l);
        }
    }
    if (ctor_this_) {
        if (const auto* prop = vs.as_if<slang::ast::ClassPropertySymbol>()) {
            // A property initializer runs as part of construction (LRM 8.7), so a
            // bare property reference in it is `this`-relative to the object being
            // BUILT. Resolved ahead of the frame path below, which would hand back
            // the enclosing method's `this` when the `new` sits inside another
            // class's method -- silently reading a different object's property of
            // the same name (and, outside a method, failing as an unknown signal).
            // Same precedence, and the same reason, as resolve_method_this.
            return class_member_lvalue(ctor_this_->handle, *prop, ctor_this_->cls, b, l);
        }
    }
    if (fn_ctx_ != nullptr) {
        // A captured automatic living in a GC box: every access in this activation
        // goes through the box, so the fork child that shares it sees the same
        // variable (LRM 9.3.2). Ahead of the frame path, which for a boxed FORMAL
        // still holds the ABI's copy-in slot.
        if (const auto bx = fn_ctx_->boxed.find(&vs); bx != fn_ctx_->boxed.end()) {
            return boxed_lvalue(bx->second, b, l);
        }
        const auto it = fn_ctx_->localmap.find(&vs);
        if (it != fn_ctx_->localmap.end()) {
            return b.local(it->second, fn_ctx_->local_types[it->second], l);
        }
        if (fn_ctx_->has_this) {
            if (const auto* prop = vs.as_if<slang::ast::ClassPropertySymbol>()) {
                // A bare property reference inside a method (implicit `this`):
                // load the `this` handle and select the property.
                return class_member_lvalue(this_handle(b, l), *prop, fn_ctx_->this_class, b, l);
            }
        }
    }

    // A fork-scope declaration of a fork in a PROCESS body: its per-entry box lives
    // in a design-state signal because the parent has no frame. AFTER the frame path
    // above, so inside a fork CHILD -- which received the handle as a formal and has
    // a `boxed` entry of its own -- the child's own box wins.
    if (const auto fb = proc_fork_boxes_.find(&vs); fb != proc_fork_boxes_.end()) {
        return box_field_lvalue(b.unary(ir::Inst::Tag::Load, b.signal(fb->second.sig, l), l),
                                fb->second.cls,
                                b,
                                l);
    }

    if (is_const_table_symbol(vs)) {
        // An elaboration constant reached as an lvalue root: every fold-only
        // route (a constant select, a whole reference) resolved earlier, so
        // this reference genuinely needs an address. Materialize the value.
        return b.signal(const_table_signal(vs), l);
    }

    return b.signal(signal_of(vs), l);
}

// The Ret operand for an aggregate result. LRM 13.4 defines `return expr` as
// assigning `expr` to the function's implicit return variable and then
// returning that variable, and this follows the clause literally -- which is
// also what keeps `return` accepting exactly the rvalues an assignment
// accepts, no more and no less.
//
// An operand that already has anchored storage is returned IN PLACE, so the
// backend's copy-out through the return out-param is the only copy: a
// variable or a select (is_struct_lvalue_shape), and a call, whose result the
// callee already wrote into the caller-side out-param temp that is the call's
// lvalue. Staging either in the return variable first would copy twice.
//
// Everything else -- a `'{...}` pattern, a byte-array string literal -- has no
// storage of its own, so it is built by the ordinary aggregate assignment path
// directly INTO the return variable, whose frame storage already exists for
// the bare-`return;` path. That costs no temp and no extra copy, and it leaves
// emit_agg_store owning the diagnostic for any form assignment cannot lower
// either (an aggregate ternary, LRM 11.4.11).
ir::Index Lowerer::agg_return_operand(const Expression& src, ir::Builder& b, ir::SourceLoc l) {
    if (is_struct_lvalue_shape(src)) {
        return lower_lvalue(src, b);
    }
    if (src.kind == ExpressionKind::Call) {
        return lower_lvalue(src, b);
    }
    if (src.kind == ExpressionKind::RangeSelect) {
        // TODO: return an unpacked-array slice. Assignment already accepts one
        // as a whole-array source, so 13.4 equivalence wants this too, but the
        // gap is below the frontend: inside a subroutine the slice source and
        // the return variable are both canonical aggregate frame storage, and
        // the backend has no aggregate-element slice ("aggregate-element
        // unpacked-array slice not supported"). A plain `o = x[0:1]` between two
        // frame-local arrays hits the identical wall, so lowering a slice here
        // would only move the failure later. Refused with its own message so it
        // does not read as a struct diagnostic.
        fail("return of an unpacked-array slice is not supported");
    }
    const ir::Index rv = ret_var_ref(*fn_ctx_, l, b);
    emit_agg_store(rv, fn_ctx_->ret_type, src, ir::Inst::Tag::Store, b, l);
    return rv;
}

} // namespace clotho::lower
