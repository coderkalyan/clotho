// Array ordering methods: sort and rsort as an emitted merge sort.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Loud guard for the array ordering methods sort()/rsort() (LRM 7.12.2). The
// clause requires the relational operators (<, >, ==) to be defined for
// whatever is ordered: the ELEMENT type for the bare form, and the `with`
// expression's type when one is given ("If a with clause is specified, the
// relational operators shall be defined for the type of the expression").
// That admits an integral of any width, a real and a string; an enum is
// integral, so it orders by its base value.
//
// A 4-state element or key is ORDERED, not refused. LRM 11.4.4 makes `<`
// yield x when either operand carries an unknown, which is not a strict weak
// ordering and therefore not something a comparison sort can be driven by at
// all; clotho defines a total order instead of refusing (unknown before every
// defined value, all unknowns mutually equal). See sort_right_wins and
// docs/design/array-methods.md.
void Lowerer::guard_ordering_sort(std::string_view name, const CallExpression& call) {
    const auto&             sci  = std::get<CallExpression::SystemCallInfo>(call.subroutine);
    const Expression* const iter = sci.getIteratorInfo().first;
    const slang::ast::Type& kty =
      iter != nullptr ? *iter->type : *call.arguments()[0]->type->getArrayElementType();
    const slang::ast::Type& kc   = kty.getCanonicalType();
    const std::string       what = iter != nullptr ? "`with` clause key" : "element";
    if (kc.isIntegral() || kc.isFloating() || kc.isString()) {
        return;
    }
    fail("'" + std::string(name) + "' needs an " + what +
         " with the relational operators (<, >, ==) defined (LRM 7.12.2), not '" + kty.toString() + "'");
}

// Whether sort()/rsort() has to go through the IR permutation sort instead of
// the runtime's flat value comparator (rt_*_sort). Three cases need it: a
// `with` clause, whose key is arbitrary IR; a string element, which is a heap
// handle rather than a value the comparator can load; and an element wider
// than the one 64-bit word slot_less reads. Everything else -- an integral of
// width <= 64 in either domain, and a real -- keeps the runtime fast path,
// which is a straight std::sort with no call back into generated code. A
// 4-state element of width <= 64 belongs there too: its slot carries the
// must/may planes the runtime comparator needs (sort_unsigned4 / sort_signed4).
bool Lowerer::sort_uses_iter_path(const CallExpression& call) {
    const auto& sci = std::get<CallExpression::SystemCallInfo>(call.subroutine);
    if (sci.getIteratorInfo().first != nullptr) {
        return true;
    }
    const slang::ast::Type& ec = call.arguments()[0]->type->getArrayElementType()->getCanonicalType();
    return ec.isString() || (ec.isIntegral() && ec.getBitWidth() > 64);
}

Lowerer::DeclIndexMap Lowerer::decl_index_map(const Expression& recv) {
    DeclIndexMap m;
    const auto*  arr = recv.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>();
    if (arr == nullptr) {
        return m;
    }
    const slang::ConstantRange r = arr->range;
    m.fixed                      = true;
    m.first                      = r.left;
    m.step                       = r.left <= r.right ? 1 : -1;
    m.fixed_count                = r.width();
    return m;
}

// The declared index of iteration position `pos`, as a 32-bit value. Emits
// nothing at all for a 0-based ascending receiver, so the queue and
// dynamic-array paths keep the code they had.
ir::Index Lowerer::decl_index_at(const DeclIndexMap& m, ir::Index pos, ir::Ref i32, ir::Builder& b, ir::SourceLoc l) {
    using T = ir::Inst::Tag;
    if (m.first == 0 && m.step == 1) {
        return pos;
    }
    const ir::Index first = iconst(i32, static_cast<uint64_t>(static_cast<uint32_t>(m.first)), b, l);
    return m.step == 1 ? b.binary(T::Add, pos, first, l) : b.binary(T::Sub, first, pos, l);
}

// Bind the `with` iterator to the element at logical position `pos`, and aim
// `item.index` (LRM 7.12.4) at that same cell, so a clause reading it sees the
// element's ORIGINAL position -- the permutation moves, the array does not,
// until the whole sort is applied.
void Lowerer::sort_bind(const SortIter& s, const TempCell& pos, ir::Builder& b, ir::SourceLoc l) {
    using T                  = ir::Inst::Tag;
    iter_stack_.back().index = pos;
    if (s.iter_var == nullptr) {
        return;
    }
    const ir::Index src = b.elem_select(s.arr, b.unary(T::Load, pos.lv(b, l), l), s.aty, l);
    const ir::Index dst = value_lvalue(*s.iter_var, b, l);
    if (!s.agg_elem) {
        b.binary(T::Store, dst, b.unary(T::Load, src, l), l);
        return;
    }
    const ir::Ref er = std::get<ir::Type>(module_.pool.get(s.aty)).element;
    if (is_trivially_copyable(er)) {
        b.agg_copy(dst, src, er, l);
        return;
    }
    emit_agg_copy(dst, src, er, T::Store, b, l);
}

// The ordering key of the element at `pos`: the `with` clause evaluated
// against it, or the element itself when there is no clause -- 7.12.2's bare
// form is `with (item)`, which is also what lets a string or wide element
// reuse this path with no runtime comparator.
ir::Index Lowerer::sort_key(const SortIter& s, const TempCell& pos, ir::Builder& b, ir::SourceLoc l) {
    using T = ir::Inst::Tag;
    sort_bind(s, pos, b, l);
    if (s.iter_expr != nullptr) {
        return lower_expr(*s.iter_expr, b);
    }
    return b.unary(T::Load, b.elem_select(s.arr, b.unary(T::Load, pos.lv(b, l), l), s.aty, l), l);
}

// Does the right candidate strictly precede the left one? The merge takes the
// LEFT element unless this is true, which is exactly what makes the sort
// stable: equal keys keep the earlier run's element first.
//
// rsort compares with the operands swapped rather than sorting ascending and
// reversing. Those stop being equivalent the moment a `with` clause makes ties
// observable, because reversing an ascending sort also reverses each tie
// GROUP, so equal keys would come back in reverse original order. The same
// trap is why the 4-state ordering below is mirrored rather than reversed:
// every unknown key is one tie group.
//
// The clause is evaluated HERE, once per element per comparison, and is never
// cached in a key buffer. 7.12.2 does not fix an evaluation count, so a clause
// with a side effect observes one call per comparison; the considered
// alternative -- precompute n keys, sort those -- was rejected to stay
// consistent with the locator family, whose unique() dedup scan likewise
// re-evaluates rather than caching.
ir::Index Lowerer::sort_right_wins(const SortIter& s, ir::Builder& b, ir::SourceLoc l) {
    using T = ir::Inst::Tag;
    // The right key is STORED before the left one is evaluated, and that store
    // is load-bearing rather than a convenience. Binding the iterator writes
    // one variable, so a key that lowers to a place inside it -- a string
    // handle, or a member select of an aggregate element -- would still be
    // aiming at that variable when the second bind overwrote it, and every
    // comparison would then compare the left element against itself and report
    // a tie. Storing forces the value out (a string store deep-copies), which
    // is the same reason min/max and unique hold their keys in cells.
    const ir::Index hold_lv = s.key_hold.lv(b, l);
    const ir::Index key     = sort_key(s, s.pos_b, b, l);
    b.binary(T::Store, hold_lv, key, l);
    const ir::Index left    = sort_key(s, s.pos_a, b, l);
    const ir::Index right   = b.unary(T::Load, s.key_hold.lv(b, l), l);
    const T         ascend  = s.key_real ? T::FLt : (s.key_signed ? T::SLt : T::ULt);
    const T         descend = s.key_real ? T::FGt : (s.key_signed ? T::SGt : T::UGt);
    const ir::Index cmp     = b.binary(s.desc ? descend : ascend, right, left, l);
    if (!s.key_four) {
        return cmp;
    }
    // Clotho's 4-state ordering (docs/design/array-methods.md): a key with ANY
    // x or z bit is "unknown", every unknown is equal to every other one, and
    // an unknown precedes every defined key ascending (so it follows every
    // defined key descending). `cmp` above is LRM 11.4.4's compare, which is x
    // whenever either key is unknown, and x is not an ordering -- so the
    // unknown tests decide those pairs and `cmp` only settles defined pairs:
    //
    //   ascending   right wins iff  !unknown(left)  && (unknown(right) || right < left)
    //   descending  right wins iff  !unknown(right) && (unknown(left)  || right > left)
    //
    // No x reaches the result even though `cmp` may be one: the bitwise ops are
    // monotone (LRM 11.4.7 tables give 0 & x == 0 and 1 | x == 1), and `cmp` is
    // definite in exactly the case where it is the deciding operand. The unknown
    // tests are 2-state, so each is lifted to `cmp`'s domain, and the conversion
    // at the end is the 2-state landing (LRM 6.3.1) for the `take` cell.
    const ir::Ref   four1 = make_int_type(1, true);
    const ir::Index lu    = b.to_four(b.unary(T::IsUnknown, left, l), four1, l);
    const ir::Index ru    = b.to_four(b.unary(T::IsUnknown, right, l), four1, l);
    const ir::Index open = b.unary(T::Bnot, s.desc ? ru : lu, l);
    const ir::Index win  = b.binary(T::Bor, s.desc ? lu : ru, cmp, l);
    return b.convert(b.binary(T::Band, open, win, l), s.bit1, false, l);
}

// One merge step: choose the next index from the left or the right run and
// append it to `buf`. Both exhaustion tests come BEFORE the compare, so the
// clause is never evaluated against a run with nothing left in it.
void Lowerer::emit_merge_step(const SortIter& s, ir::Builder& b, ir::SourceLoc l) {
    using T                 = ir::Inst::Tag;
    const auto      load    = [&](const TempCell& c) { return b.unary(T::Load, c.lv(b, l), l); };
    const auto      ord_at  = [&](ir::Index i) {
        return b.unary(T::Load, b.elem_select(s.ord, b.convert(i, s.i32, true, l), s.oty, l), l);
    };
    const ir::Index take_lv   = s.take.lv(b, l);
    const ir::Index take_left = iconst(s.bit1, 0, b, l);
    b.binary(T::Store, take_lv, take_left, l);

    b.open_region();
    const ir::Index done_lv    = s.take.lv(b, l);
    const ir::Index take_right = iconst(s.bit1, 1, b, l);
    b.binary(T::Store, done_lv, take_right, l);
    const ir::ExtraSlice left_done = b.close_region();

    b.open_region();
    b.open_region();
    const ir::Index pos_a_lv = s.pos_a.lv(b, l);
    const ir::Index pos_a    = ord_at(load(s.li));
    b.binary(T::Store, pos_a_lv, pos_a, l);
    const ir::Index pos_b_lv = s.pos_b.lv(b, l);
    const ir::Index pos_b    = ord_at(load(s.rj));
    b.binary(T::Store, pos_b_lv, pos_b, l);
    const ir::Index wins_lv = s.take.lv(b, l);
    const ir::Index wins    = sort_right_wins(s, b, l);
    b.binary(T::Store, wins_lv, wins, l);
    const ir::ExtraSlice compare = b.close_region();
    b.open_region();
    const ir::ExtraSlice right_done = b.close_region();
    const ir::Index      rj         = load(s.rj);
    const ir::Index      hi         = load(s.hi);
    b.cond(b.binary(T::SLt, rj, hi, l), compare, right_done, l);
    const ir::ExtraSlice left_live = b.close_region();

    const ir::Index li  = load(s.li);
    const ir::Index mid = load(s.mid);
    b.cond(b.binary(T::SGe, li, mid, l), left_done, left_live, l);

    const auto emit_take = [&](const TempCell& cursor) {
        const ir::Index slot =
          b.elem_select(s.buf, b.convert(load(s.out), s.i32, true, l), s.oty, l, /*for_write=*/true);
        const ir::Index val  = ord_at(load(cursor));
        b.binary(T::Store, slot, val, l);
        const ir::Index cursor_lv = cursor.lv(b, l);
        const ir::Index at        = load(cursor);
        const ir::Index inc       = iconst(s.i64, 1, b, l);
        const ir::Index next      = b.binary(T::Add, at, inc, l);
        b.binary(T::Store, cursor_lv, next, l);
    };
    b.open_region();
    emit_take(s.rj);
    const ir::ExtraSlice took_right = b.close_region();
    b.open_region();
    emit_take(s.li);
    const ir::ExtraSlice took_left = b.close_region();
    b.cond(load(s.take), took_right, took_left, l);

    const ir::Index out_lv   = s.out.lv(b, l);
    const ir::Index out_at   = load(s.out);
    const ir::Index out_inc  = iconst(s.i64, 1, b, l);
    const ir::Index out_next = b.binary(T::Add, out_at, out_inc, l);
    b.binary(T::Store, out_lv, out_next, l);
}

// One bottom-up merge pass: walk the array in runs of `w`, merge each adjacent
// pair from `ord` into `buf`, then copy `buf` back. A trailing run shorter than
// `w` (or a missing right run) falls out of the min() clamps, so no pass needs
// a special case for a length that is not a power of two.
void Lowerer::emit_merge_pass(const SortIter& s, ir::Builder& b, ir::SourceLoc l) {
    using T            = ir::Inst::Tag;
    const auto load    = [&](const TempCell& c) { return b.unary(T::Load, c.lv(b, l), l); };
    const auto clamped = [&](ir::Index v) {
        const ir::Index limit = load(s.n);
        const ir::Index below = b.binary(T::SLt, v, limit, l);
        const ir::Index cap   = load(s.n);
        return b.select(below, v, cap, l);
    };

    const ir::Index lo_lv   = s.lo.lv(b, l);
    const ir::Index lo_init = iconst(s.i64, 0, b, l);
    b.binary(T::Store, lo_lv, lo_init, l);

    b.open_region();
    const ir::Index      lo       = load(s.lo);
    const ir::Index      n        = load(s.n);
    const ir::Index      run_pred = b.binary(T::SLt, lo, n, l);
    const ir::ExtraSlice run_cond = b.close_region();

    b.open_region();
    const ir::Index mid_lv  = s.mid.lv(b, l);
    const ir::Index lo_now  = load(s.lo);
    const ir::Index w_now   = load(s.w);
    const ir::Index mid_val = clamped(b.binary(T::Add, lo_now, w_now, l));
    b.binary(T::Store, mid_lv, mid_val, l);
    const ir::Index hi_lv   = s.hi.lv(b, l);
    const ir::Index mid_now = load(s.mid);
    const ir::Index w_next  = load(s.w);
    const ir::Index hi_val  = clamped(b.binary(T::Add, mid_now, w_next, l));
    b.binary(T::Store, hi_lv, hi_val, l);
    const ir::Index li_lv   = s.li.lv(b, l);
    const ir::Index li_init = load(s.lo);
    b.binary(T::Store, li_lv, li_init, l);
    const ir::Index rj_lv   = s.rj.lv(b, l);
    const ir::Index rj_init = load(s.mid);
    b.binary(T::Store, rj_lv, rj_init, l);
    const ir::Index out_lv   = s.out.lv(b, l);
    const ir::Index out_init = load(s.lo);
    b.binary(T::Store, out_lv, out_init, l);

    b.open_region();
    const ir::Index      out_at    = load(s.out);
    const ir::Index      hi_at     = load(s.hi);
    const ir::Index      step_pred = b.binary(T::SLt, out_at, hi_at, l);
    const ir::ExtraSlice step_cond = b.close_region();
    b.open_region();
    emit_merge_step(s, b, l);
    const ir::ExtraSlice step_body = b.close_region();
    b.open_region();
    const ir::ExtraSlice step_inc = b.close_region();
    b.loop(step_cond, step_pred, step_body, step_inc, /*bottom_test*/ false, l);

    // Advance past both runs. `hi` is already the clamped end, so this also
    // terminates on the trailing partial run.
    const ir::Index adv_lv = s.lo.lv(b, l);
    const ir::Index lo_adv = load(s.hi);
    b.binary(T::Store, adv_lv, lo_adv, l);
    const ir::ExtraSlice run_body = b.close_region();

    b.open_region();
    const ir::ExtraSlice run_inc = b.close_region();
    b.loop(run_cond, run_pred, run_body, run_inc, /*bottom_test*/ false, l);

    b.dyn_array_copy(s.ord, s.buf, s.oty, l);
}

// Apply the finished permutation to the receiver: copy it aside, then write
// element ord[k] back at position k. One container copy plus n element stores;
// the O(n log n) work above moved 32-bit indices, not elements.
void Lowerer::emit_sort_permute(const SortIter& s, bool queue, ir::Builder& b, ir::SourceLoc l) {
    using T           = ir::Inst::Tag;
    const auto   load = [&](const TempCell& c) { return b.unary(T::Load, c.lv(b, l), l); };
    const ir::Ref er  = std::get<ir::Type>(module_.pool.get(s.aty)).element;

    const ir::Index src = make_arg_temp(s.aty, b, l);
    if (queue) {
        b.queue_delete(src, src, /*has_idx=*/false, s.aty, l);
        b.queue_copy(src, s.arr, s.aty, l);
    } else {
        b.dyn_array_delete(src, s.aty, l);
        b.dyn_array_copy(src, s.arr, s.aty, l);
    }

    const ir::Index out_lv   = s.out.lv(b, l);
    const ir::Index out_init = iconst(s.i64, 0, b, l);
    b.binary(T::Store, out_lv, out_init, l);
    b.open_region();
    const ir::Index      out_now = load(s.out);
    const ir::Index      n_now   = load(s.n);
    const ir::Index      pred    = b.binary(T::SLt, out_now, n_now, l);
    const ir::ExtraSlice cond    = b.close_region();

    b.open_region();
    const ir::Index k    = b.convert(load(s.out), s.i32, true, l);
    const ir::Index from = b.unary(T::Load, b.elem_select(s.ord, k, s.oty, l), l);
    const ir::Index dst  = b.elem_select(s.arr, k, s.aty, l, /*for_write=*/true);
    const ir::Index elem = b.elem_select(src, from, s.aty, l);
    if (!s.agg_elem) {
        b.binary(T::Store, dst, b.unary(T::Load, elem, l), l);
    } else if (is_trivially_copyable(er)) {
        b.agg_copy(dst, elem, er, l);
    } else {
        emit_agg_copy(dst, elem, er, T::Store, b, l);
    }
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index inc_lv   = s.out.lv(b, l);
    const ir::Index out_cur  = load(s.out);
    const ir::Index one      = iconst(s.i64, 1, b, l);
    const ir::Index out_next = b.binary(T::Add, out_cur, one, l);
    b.binary(T::Store, inc_lv, out_next, l);
    const ir::ExtraSlice inc = b.close_region();
    b.loop(cond, pred, body, inc, /*bottom_test*/ false, l);

    if (queue) {
        b.queue_delete(src, src, /*has_idx=*/false, s.aty, l);
    } else {
        b.dyn_array_delete(src, s.aty, l);
    }
}

// sort() / rsort() whose ordering key the runtime's flat value comparator
// cannot produce (sort_uses_iter_path). Lowered as IR rather than a runtime
// helper, for the reason the locator family already is
// (docs/design/array-methods.md): the key is arbitrary IR, so the ordinary
// expression path makes a string, a wide integral, a real, an enum, a struct
// field and a concatenation all work with no C comparator and no per-type
// descriptor.
//
// It also removes a hazard rather than managing one. A runtime comparator
// calling back into generated code would hold a raw pointer into the element
// buffer across USER CODE that can allocate and can restructure the very
// container being sorted. Here the runtime is never inside the sort: every
// element access goes through elem_select, which re-resolves from the handle,
// so a key expression that pushes or deletes cannot leave a stale pointer
// behind.
//
// `cty` is the interned type of the container actually permuted and `queue`
// its shape; `map` is the RECEIVER's declared-index map. The three are
// separate because a fixed-size receiver is permuted through a mirror
// dynamic array (lower_fixed_array_order): the container is then not the
// receiver, while `item.index` must still report the receiver's own
// declared indices.
ir::Index Lowerer::lower_array_sort_iter(std::string_view      name,
                                         const CallExpression& call,
                                         ir::Index             arr,
                                         ir::Ref               cty,
                                         bool                  queue,
                                         const DeclIndexMap&   map,
                                         ir::Builder&          b,
                                         ir::SourceLoc         l) {
    using T                          = ir::Inst::Tag;
    const Expression& recv           = *call.arguments()[0];
    const auto&       sci            = std::get<CallExpression::SystemCallInfo>(call.subroutine);
    const auto [iter_expr, iter_var] = sci.getIteratorInfo();

    SortIter s;
    s.arr       = arr;
    s.aty       = cty;
    s.i32       = make_int_type(32, false);
    s.i64       = make_int_type(64, false);
    s.bit1      = make_int_type(1, false);
    s.iter_expr = iter_expr;
    s.iter_var  = iter_var;
    s.agg_elem  = is_agg_abi_ref(std::get<ir::Type>(module_.pool.get(s.aty)).element);
    s.desc      = name == "rsort";
    const slang::ast::Type& kty =
      iter_expr != nullptr ? *iter_expr->type : *recv.type->getArrayElementType();
    s.key_real   = kty.isFloating();
    s.key_signed = kty.isSigned();
    s.key_four = kty.getCanonicalType().isIntegral() && kty.getCanonicalType().isFourState();

    ir::Type ot;
    ot.kind    = ir::Type::Kind::DynArray;
    ot.element = s.i32;
    s.oty      = module_.pool.put(ir::Item{ ot });

    s.n     = make_temp_cell(s.i64, "sort_n");
    s.w     = make_temp_cell(s.i64, "sort_w");
    s.lo    = make_temp_cell(s.i64, "sort_lo");
    s.mid   = make_temp_cell(s.i64, "sort_mid");
    s.hi    = make_temp_cell(s.i64, "sort_hi");
    s.li    = make_temp_cell(s.i64, "sort_li");
    s.rj    = make_temp_cell(s.i64, "sort_rj");
    s.out   = make_temp_cell(s.i64, "sort_out");
    s.pos_a = make_temp_cell(s.i32, "sort_pos_a");
    s.pos_b = make_temp_cell(s.i32, "sort_pos_b");
    s.take  = make_temp_cell(s.bit1, "sort_take");
    s.key_hold = make_temp_cell(lower_type(kty), "sort_key");

    const ir::Index size  = queue ? b.queue_size(arr, s.aty, l) : b.dyn_array_size(arr, s.aty, l);
    const ir::Index n_lv  = s.n.lv(b, l);
    const ir::Index n_val = b.convert(size, s.i64, true, l);
    b.binary(T::Store, n_lv, n_val, l);
    if (iter_var != nullptr) {
        declare_storage(iter_var->as<slang::ast::VariableSymbol>(), /*force_frame*/ true);
    }

    s.ord = make_arg_temp(s.oty, b, l);
    s.buf = make_arg_temp(s.oty, b, l);
    iter_stack_.push_back(IterBinding{ .var = iter_var, .index = s.pos_a, .map = map });
    emit_sort_body(s, queue, b, l);
    iter_stack_.pop_back();
    return arr;
}

// The emitted sort itself, split off for length (TigerStyle). Guarded
// by n >= 2: an empty or single-element array is already sorted, and 7.12.2
// gives a clause no reason to run when nothing can be compared.
void Lowerer::emit_sort_body(const SortIter& s, bool queue, ir::Builder& b, ir::SourceLoc l) {
    using T         = ir::Inst::Tag;
    const auto load = [&](const TempCell& c) { return b.unary(T::Load, c.lv(b, l), l); };

    b.open_region();
    const ir::Index n32 = b.convert(load(s.n), s.i32, true, l);
    b.dyn_array_new(s.ord, n32, s.ord, /*has_init*/ false, s.oty, l);
    b.dyn_array_new(s.buf, n32, s.buf, /*has_init*/ false, s.oty, l);

    // ord = identity. The sort permutes THIS, so the comparison always reads
    // the array in its original layout and `item.index` stays meaningful.
    const ir::Index out_lv   = s.out.lv(b, l);
    const ir::Index out_init = iconst(s.i64, 0, b, l);
    b.binary(T::Store, out_lv, out_init, l);
    b.open_region();
    const ir::Index      out_now = load(s.out);
    const ir::Index      n_now   = load(s.n);
    const ir::Index      ipred   = b.binary(T::SLt, out_now, n_now, l);
    const ir::ExtraSlice icond   = b.close_region();
    b.open_region();
    const ir::Index k = b.convert(load(s.out), s.i32, true, l);
    b.binary(T::Store, b.elem_select(s.ord, k, s.oty, l, /*for_write=*/true), k, l);
    const ir::ExtraSlice ibody = b.close_region();
    b.open_region();
    const ir::Index inc_lv   = s.out.lv(b, l);
    const ir::Index out_cur  = load(s.out);
    const ir::Index one      = iconst(s.i64, 1, b, l);
    const ir::Index out_next = b.binary(T::Add, out_cur, one, l);
    b.binary(T::Store, inc_lv, out_next, l);
    const ir::ExtraSlice iinc = b.close_region();
    b.loop(icond, ipred, ibody, iinc, /*bottom_test*/ false, l);

    // Bottom-up merge, run width doubling until it spans the array.
    const ir::Index w_lv   = s.w.lv(b, l);
    const ir::Index w_init = iconst(s.i64, 1, b, l);
    b.binary(T::Store, w_lv, w_init, l);
    b.open_region();
    const ir::Index      w_now = load(s.w);
    const ir::Index      limit = load(s.n);
    const ir::Index      wpred = b.binary(T::SLt, w_now, limit, l);
    const ir::ExtraSlice wcond = b.close_region();
    b.open_region();
    emit_merge_pass(s, b, l);
    const ir::ExtraSlice wbody = b.close_region();
    b.open_region();
    const ir::Index grow_lv = s.w.lv(b, l);
    const ir::Index w_lo    = load(s.w);
    const ir::Index w_hi    = load(s.w);
    const ir::Index w_dbl   = b.binary(T::Add, w_lo, w_hi, l);
    b.binary(T::Store, grow_lv, w_dbl, l);
    const ir::ExtraSlice winc = b.close_region();
    b.loop(wcond, wpred, wbody, winc, /*bottom_test*/ false, l);

    emit_sort_permute(s, queue, b, l);
    b.dyn_array_delete(s.ord, s.oty, l);
    b.dyn_array_delete(s.buf, s.oty, l);
    const ir::ExtraSlice sort_r = b.close_region();

    b.open_region();
    const ir::ExtraSlice skip_r = b.close_region();
    const ir::Index      n_size = load(s.n);
    const ir::Index      two    = iconst(s.i64, 2, b, l);
    b.cond(b.binary(T::SGe, n_size, two, l), sort_r, skip_r, l);
}

} // namespace clotho::lower
