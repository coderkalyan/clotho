// Dynamic arrays, queues, associative arrays, array methods and streaming.

#include "lower/Lowerer.h"

namespace clotho::lower {

// `arr = new[n]` / `arr = new[n](src)` (LRM 7.5 / 7.5.1) construction into an
// already-lvalue-resolved dynamic-array handle `dst`: (re)allocate to `n`
// elements, each the element type's default (rt_dynarr_new), then for the
// copy-constructor form deep-copy the low `min(n, src.size())` from `src`
// and default-fill the rest (rt_dynarr_new_copy, dyn_array_new's `has_init`
// arm). `new[n]('{...}')` is a third shape: the pattern is a literal, not a
// copyable handle, so it allocates bare and fills element-wise instead.
// Shared by the statement-assignment form (lower_dyn_array_assign) and the
// declaration-initializer / handle-member-store form (emit_handle_member_copy
// -- an automatic, static, module-scope or class-property `T x = new[n]`),
// so a change to sizing / truncate / fill semantics cannot land in only one
// of them.
void Lowerer::emit_dyn_array_new_expr(ir::Index                             dst,
                                      const slang::ast::NewArrayExpression& na,
                                      ir::Ref                               aty,
                                      ir::Builder&                          b,
                                      ir::SourceLoc                         l) {
    const ir::Index   size = as_signed_number(lower_expr(na.sizeExpr(), b), *na.sizeExpr().type, b, l);
    const Expression* init = na.initExpr();
    if (init != nullptr && as_assignment_pattern(*init) != nullptr) {
        // `new[N]('{...}')`: allocate N default-filled, then store each
        // pattern element at its position (a shorter pattern leaves the tail
        // at the element default; a longer one is truncated to N). The init
        // is a literal, not a copyable handle.
        b.dyn_array_new(dst, size, dst, /*has_init*/ false, aty, l);
        emit_dyn_pattern_fill(dst, aty, *as_assignment_pattern(*init), b, l);
    } else if (init != nullptr) {
        b.dyn_array_new(dst, size, lower_lvalue(*init, b), /*has_init*/ true, aty, l);
    } else {
        b.dyn_array_new(dst, size, dst, /*has_init*/ false, aty, l);
    }
}

// Copy a FIXED-size unpacked array's `n` elements, in order, position-wise
// into an already-ALLOCATED (size >= n) dynamic-array handle `dst` (LRM
// 7.5.1's resizeable assignment `d = f`, both the statement-assignment
// and the declaration-initializer / handle-member-store form). The
// caller allocates first (dyn_array_new for a whole assign, an
// already-sized `new[N]` frame for a decl-init) -- this only fills.
// `src` may be ANY supported fixed-array rvalue shape (a variable, a
// constant-bounds or indexed slice); lower_array_operand resolves all
// three to an array lvalue.
void Lowerer::emit_fixed_array_into_dynarr(ir::Index         dst,
                                           const Expression& src,
                                           ir::Ref           dst_ty,
                                           uint32_t          n,
                                           ir::Builder&      b,
                                           ir::SourceLoc     l) {
    const ir::Index src_lv = lower_array_operand(src, b);
    const ir::Ref   src_ty = lower_type(*src.type);
    const ir::Type  st     = std::get<ir::Type>(module_.pool.get(src_ty));
    const ir::Type  dt     = std::get<ir::Type>(module_.pool.get(dst_ty));
    const ir::Ref   i32    = make_int_type(32, false);
    for (uint32_t k = 0; k < n; ++k) {
        const ir::Index dst_el =
          b.elem_select(dst, b.constant(module_.pool.put_int(i32, k), l), dst_ty, l, /*for_write=*/true);
        const ir::Index src_el = b.elem_select(src_lv, elem_coord_const(st, k, b, l), src_ty, l);
        emit_agg_copy(dst_el, src_el, dt.element, ir::Inst::Tag::Store, b, l);
    }
}

// Copy a FIXED-size unpacked array's elements, in order, into a queue
// handle `dst` (LRM 7.10.2's whole assignment from a fixed array). No
// direct runtime path exists (rt_queue_append_range only walks an
// rt_queue source); stage through a temp dynamic array of the same
// element type/size -- emit_fixed_array_into_dynarr, the exact fill the
// dynamic-array-target case uses -- and reuse queue_copy_dynarr, the
// already-proven dynarr -> queue path. `dst` is cleared inside
// queue_copy_dynarr's runtime call, matching every other whole-queue
// assignment source.
void Lowerer::emit_fixed_array_into_queue(ir::Index         dst,
                                          const Expression& src,
                                          ir::Ref           queue_ty,
                                          ir::Builder&      b,
                                          ir::SourceLoc     l) {
    const auto&   fty = src.type->getCanonicalType().as<slang::ast::FixedSizeUnpackedArrayType>();
    const auto    n   = static_cast<uint32_t>(fty.range.width());
    ir::Type      dat;
    dat.kind             = ir::Type::Kind::DynArray;
    dat.element          = std::get<ir::Type>(module_.pool.get(queue_ty)).element;
    const ir::Ref   daty = module_.pool.put(ir::Item{ dat });
    const ir::Index tmp  = make_arg_temp(daty, b, l);
    const ir::Ref   i32  = make_int_type(32, false);
    b.dyn_array_new(tmp, b.constant(module_.pool.put_int(i32, n), l), tmp, /*has_init=*/false, daty, l);
    emit_fixed_array_into_dynarr(tmp, src, daty, n, b, l);
    b.queue_copy_dynarr(dst, tmp, queue_ty, l);
}

// Whole dynamic-array assignment (LRM 7.5): `arr = new[n]` / `arr = new[n](src)`
// (allocate + optional element copy) or `arr = other` (deep copy). Element
// writes (`arr[i] = x`) are ordinary Stores over an ElemSelect and do not
// reach here.
ir::Index Lowerer::lower_dyn_array_assign(const AssignmentExpression& a, ir::Builder& b) {
    const ir::SourceLoc l   = loc(a);
    const ir::Index     dst = lower_lvalue(a.left(), b);
    const ir::Ref       aty = lower_type(*a.left().type);
    if (a.isNonBlocking()) {
        // `d <= other` (LRM 7.5, 10.4.2): a container's value is a heap handle,
        // so there is no plane-shaped shadow to stage into. The Active region
        // deep-copies the source into a staging handle and the NBA region moves
        // that into the live one -- HandleCopyNb, shared with queues and
        // associative arrays.
        if (is_dyn_array(a.right()) && is_struct_lvalue_shape(unwrap_conversion(a.right()))) {
            b.handle_copy_nb(dst, lower_lvalue(unwrap_conversion(a.right()), b), aty, l);
            return dst;
        }
        if (as_assignment_pattern(a.right()) != nullptr || a.right().kind == ExpressionKind::NewArray) {
            // `d <= '{e0, e1, ...}` / `d <= new[n]` / `d <= new[n](src)`
            // (confirmed against a real design: uvm_id_register's
            // `id_register_value <= '{'ha0, 'ha1, ...}` in reg_dut.sv,
            // nonblocking-assigning a pattern literal straight into a
            // dynamic array). Neither shape is a copyable handle -- a
            // pattern literal fills element-by-element and `new[n]`
            // allocates -- so, unlike the plain-variable case above,
            // there is no existing handle to stage. Build the value into
            // a FRESH temp handle at Active-region time (the exact
            // blocking-assignment machinery below, redirected at the
            // scratch handle instead of `dst`), then feed that temp
            // through the same HandleCopyNb the plain-variable case
            // uses: the NBA-region commit is a cheap handle move either
            // way, whether the Active-region side built it in place or
            // deep-copied it. Mirrors materialize_queue_rvalue's
            // "materialize into a temp, then move" shape for a queue.
            const ir::Index tmp = make_arg_temp(aty, b, l);
            if (as_assignment_pattern(a.right()) != nullptr) {
                emit_agg_store(tmp, aty, a.right(), ir::Inst::Tag::Store, b, l);
            } else {
                emit_dyn_array_new_expr(tmp, a.right().as<slang::ast::NewArrayExpression>(), aty, b, l);
            }
            b.handle_copy_nb(dst, tmp, aty, l);
            return dst;
        }
        fail("nonblocking assignment to a dynamic array is only supported from another "
             "dynamic-array variable, a `'{...}` literal, or `new[n]`");
    }
    emit_dyn_array_store(dst, aty, a.right(), b, l);
    return dst;
}

// The BLOCKING half of a whole-container store, `dst = rhs`, split out of the
// three lower_*_assign paths so a SYNTHESIZED store reaches exactly the same
// rules with no AssignmentExpression to hand them. Two callers need that: a
// randsequence production returning a container (LRM 18.17.7), whose `return`
// has an expression and a frame slot but no assignment node, and the once-only
// copy-in of a container argument to an interleaved production (18.17.5).
//
// The nonblocking halves stay with their statements: an NBA stages a handle and
// commits it in the NBA region, which no synthesized store wants.
//
// (`emit_handle_member_copy` covers the neighbouring DECLARATION-INITIALIZER
// form. It is deliberately not folded in here: it treats `new[N]` as
// construction rather than assignment and refuses the pattern / conditional
// sources these accept, so the two are not interchangeable.)
void Lowerer::emit_container_store(ir::Index dst, ir::Ref ty, const Expression& rhs, ir::Builder& b, ir::SourceLoc l) {
    switch (std::get<ir::Type>(module_.pool.get(ty)).kind) {
        case ir::Type::Kind::DynArray:
            emit_dyn_array_store(dst, ty, rhs, b, l);
            return;
        case ir::Type::Kind::Queue:
            emit_queue_store(dst, ty, rhs, b, l);
            return;
        case ir::Type::Kind::Assoc:
            emit_assoc_store(dst, ty, rhs, b, l);
            return;
        default:
            fail("internal: whole-container store on a non-container type");
    }
}

void Lowerer::emit_dyn_array_store(ir::Index         dst,
                                   ir::Ref           aty,
                                   const Expression& rhs_expr,
                                   ir::Builder&      b,
                                   ir::SourceLoc     l) {
    // `d = '{e0, e1, ...}` (LRM 10.9): the source is an assignment-pattern
    // literal, not another handle — materialize it element-wise rather than
    // deep-copying a handle (emit_handle_literal_init allocates + stores).
    if (as_assignment_pattern(rhs_expr) != nullptr) {
        emit_agg_store(dst, aty, rhs_expr, ir::Inst::Tag::Store, b, l);
        return;
    }
    if (rhs_expr.kind == ExpressionKind::NewArray) {
        emit_dyn_array_new_expr(dst, rhs_expr.as<slang::ast::NewArrayExpression>(), aty, b, l);
        return;
    }
    if (unwrap_conversion(rhs_expr).kind == ExpressionKind::Streaming) {
        // `d = {>>{...}}` (LRM 11.4.14): sized to the stream's element count,
        // then filled in stream order.
        std::vector<StreamChunk> chunks;
        collect_stream_chunks(unwrap_conversion(rhs_expr).as<StreamingConcatenationExpression>(), chunks, b, l);
        stream_fill_dynarr(dst, chunks, aty, b, l);
        return;
    }
    const Expression& rhs = unwrap_conversion(rhs_expr);
    if (is_queue(rhs)) {
        // `d = q` (LRM 7.5.1's resizeable assignment): allocate to the
        // queue's RUNTIME size() and deep-copy, ring-indexed source into
        // linear destination -- rt_dynarr_assign_queue, the mirror of
        // rt_queue_assign_dynarr (queue = dynamic array). materialize_queue_rvalue
        // resolves ANY queue rvalue shape (a variable, a slice, a
        // concatenation) to a handle lvalue, matching the plain-dynarr
        // fallback below.
        b.dyn_array_copy_queue(dst, materialize_queue_rvalue(rhs, b), aty, l);
        return;
    }
    if (is_unpacked_array(rhs)) {
        // `d = f` (LRM 7.5.1's resizeable assignment): a FIXED-size
        // unpacked array's element count is a COMPILE-TIME constant,
        // unlike a queue's runtime size() -- allocate to that count bare
        // (default-filled, like `new[n]` with no init source), then copy
        // position-wise.
        const auto&   fty = rhs.type->getCanonicalType().as<slang::ast::FixedSizeUnpackedArrayType>();
        const auto    n   = static_cast<uint32_t>(fty.range.width());
        const ir::Ref i32 = make_int_type(32, false);
        b.dyn_array_new(dst, b.constant(module_.pool.put_int(i32, n), l), dst, /*has_init=*/false, aty, l);
        emit_fixed_array_into_dynarr(dst, rhs, aty, n, b, l);
        return;
    }
    // arr = other: deep copy the source handle (value semantics).
    b.dyn_array_copy(dst, lower_lvalue(rhs_expr, b), aty, l);
}

// Store each element of an assignment-pattern literal into an already-allocated
// dynamic array `dst` at its positional coordinate. Used for `new[N]('{...}')`
// where the array is sized to N and the pattern fills the low elements; a
// replicated pattern (`'{M{x}}`) expands its single stored copy M times.
void Lowerer::emit_dyn_pattern_fill(ir::Index                                          dst,
                                    ir::Ref                                            aty,
                                    const slang::ast::AssignmentPatternExpressionBase& pat,
                                    ir::Builder&                                       b,
                                    ir::SourceLoc                                      l) {
    const ir::Type t     = std::get<ir::Type>(module_.pool.get(aty));
    const auto     elems = pat.elements();
    const uint32_t n     = static_cast<uint32_t>(elems.size());
    const uint32_t reps =
      pat.kind == ExpressionKind::ReplicatedAssignmentPattern
        ? static_cast<uint32_t>(const_uint(pat.as<ReplicatedAssignmentPatternExpression>().count(),
                                           "an assignment-pattern replication count (LRM 10.9.1)"))
        : 1;
    for (uint32_t k = 0; k < n * reps; ++k) {
        const ir::Index idx = b.constant(module_.pool.put_int(make_int_type(32, false), k), l);
        emit_agg_store(b.elem_select(dst, idx, aty, l), t.element, *elems[k % n], ir::Inst::Tag::Store, b, l);
    }
}

// A dynamic-array built-in method used as an rvalue or statement (LRM 7.5).
// The receiver arrives as arguments()[0], like a string method.
ir::Index Lowerer::lower_dyn_array_method(std::string_view      name,
                                          const CallExpression& call,
                                          ir::Builder&          b,
                                          ir::SourceLoc         l) {
    // See lower_queue_method for why the receiver carries a direction.
    const ir::Index arr = lower_lvalue(*call.arguments()[0], b, mutates_container(name));
    const ir::Ref   aty = lower_type(*call.arguments()[0]->type);
    if (name == "size") {
        return b.convert(b.dyn_array_size(arr, aty, l), lower_type(*call.type), call.type->isSigned(), l);
    }
    if (name == "delete") {
        return b.dyn_array_delete(arr, aty, l);
    }
    if (name == "reverse") {
        return b.dyn_array_reverse(arr, aty, l);
    }
    if (name == "sort" || name == "rsort") {
        guard_ordering_sort(name, call);
        if (sort_uses_iter_path(call)) {
            return lower_array_sort_iter(name, call, arr, aty, /*queue=*/false, DeclIndexMap{}, b, l);
        }
        return b.dyn_array_sort(arr, name == "rsort", aty, l);
    }
    if (name == "shuffle") {
        return b.dyn_array_shuffle(arr, aty, l);
    }
    if (is_locator_method(name) || is_reduction_method(name)) {
        return lower_array_iter_method(name, call, arr, b, l);
    }
    // TODO: `map` (LRM 7.12.5).
    fail("dynamic array method '" + std::string(name) + "' is not supported");
}

// The LRM 7.12.1 array locator methods. All ten return a queue: of elements,
// or of `int` indices for the `_index` forms. `find*` require a `with` clause;
// min/max/unique take one optionally.
bool Lowerer::is_locator_method(std::string_view name) {
    return name == "find" || name == "find_index" || name == "find_first" || name == "find_first_index" ||
           name == "find_last" || name == "find_last_index" || name == "min" || name == "max" ||
           name == "unique" || name == "unique_index";
}

// The LRM 7.12.3 array reduction methods, which fold the array to one scalar.
bool Lowerer::is_reduction_method(std::string_view name) {
    return name == "sum" || name == "product" || name == "and" || name == "or" || name == "xor";
}

// The LRM 7.12.2 array ordering methods, which mutate the array in place.
bool Lowerer::is_ordering_method(std::string_view name) {
    return name == "reverse" || name == "sort" || name == "rsort" || name == "shuffle";
}

// A built-in method whose receiver is a container: a dynamic array (LRM 7.5), a
// queue (7.10.2), an associative array (7.9), or any of them carrying a 7.12
// manipulation method. The receiver arrives as arguments()[0] and the name
// carries no '$', like a string method. nullopt when the call is something
// else, so the rvalue, statement and lvalue paths can each fall through to
// their own next case.
std::optional<ir::Index> Lowerer::lower_container_method(const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
    const std::string_view name = call.getSubroutineName();
    if (name.empty() || name[0] == '$' || call.arguments().empty()) {
        return std::nullopt;
    }
    const Expression& recv = *call.arguments()[0];
    if (is_dyn_array(recv)) {
        return lower_dyn_array_method(name, call, b, l);
    }
    if (is_queue(recv)) {
        return lower_queue_method(name, call, b, l);
    }
    if (is_assoc(recv)) {
        return lower_assoc_method(name, call, b, l);
    }
    // LRM 7.12 allows the manipulation methods on any unpacked array, and a
    // fixed-size one reaches none of the dispatches above: it has no handle,
    // so it is not a container in clotho's sense. Dispatched by name here, on
    // the path every caller shares, so it does not surface as "system call
    // used where a user subroutine was expected".
    if (is_unpacked_array(recv) &&
        (is_locator_method(name) || is_reduction_method(name) || is_ordering_method(name))) {
        return lower_fixed_array_method(name, call, b, l);
    }
    return std::nullopt;
}

// A locator (LRM 7.12.1), ordering (7.12.2) or reduction (7.12.3) method on a
// fixed-size unpacked array. 7.12 applies to "any unpacked array" and 7.12.2
// says so in as many words ("fixed or dynamically sized"). What a fixed
// receiver adds is that its indices are its DECLARED range rather than
// 0 .. n-1, which DeclIndexMap threads through both shapes below.
//
// The locator and reduction families read the receiver in place: they already
// walk it with elem_select, whose index is a declared-range coordinate for a
// fixed array (ir::ElemSelect), so the map is all they need.
//
// The ordering family borrows the dynamic-array implementations whole rather
// than growing a second copy of any of them: the elements are mirrored into a
// temporary dynamic array in declared-address order, reordered THERE, and
// copied back. That keeps a fixed receiver on the one implementation of each
// ordering method -- the runtime comparators including the 4-state total
// order, Fisher-Yates over the same process RNG stream, and the IR
// permutation sort with its `with` clause and stability.
ir::Index Lowerer::lower_fixed_array_method(std::string_view      name,
                                            const CallExpression& call,
                                            ir::Builder&          b,
                                            ir::SourceLoc         l) {
    const Expression& recv = *call.arguments()[0];
    const ir::Index   arr  = lower_lvalue(recv, b, mutates_container(name));
    if (is_locator_method(name) || is_reduction_method(name)) {
        return lower_array_iter_method(name, call, arr, b, l);
    }
    if (name == "sort" || name == "rsort") {
        guard_ordering_sort(name, call);
    }
    return lower_fixed_array_order(name, call, arr, b, l);
}

// reverse / sort / rsort / shuffle (LRM 7.12.2) on a fixed-size unpacked
// array, run over a mirror dynamic array (see lower_fixed_array_method).
ir::Index Lowerer::lower_fixed_array_order(std::string_view      name,
                                           const CallExpression& call,
                                           ir::Index             arr,
                                           ir::Builder&          b,
                                           ir::SourceLoc         l) {
    const Expression&  recv = *call.arguments()[0];
    const DeclIndexMap map  = decl_index_map(recv);
    const ir::Ref      aty  = lower_type(*recv.type);
    const ir::Ref      er   = std::get<ir::Type>(module_.pool.get(aty)).element;
    // The mirror is a real dynamic array, so its element has to be one a
    // dynamic array admits. Every element a fixed array can hold is, bar the
    // handful guard_container_element names -- a virtual-interface element,
    // say, which 7.12.2 does not exclude but clotho cannot put in a container.
    guard_container_element(recv.type->getArrayElementType()->getCanonicalType(), er, "fixed-size array ordering");

    ir::Type mt;
    mt.kind           = ir::Type::Kind::DynArray;
    mt.element        = er;
    const ir::Ref mty = module_.pool.put(ir::Item{ mt });
    const ir::Ref i32 = make_int_type(32, false);

    const ir::Index mirror = make_arg_temp(mty, b, l);
    b.dyn_array_new(mirror, iconst(i32, map.fixed_count, b, l), mirror, /*has_init*/ false, mty, l);
    emit_fixed_mirror_copy(arr, aty, mirror, mty, map, /*to_mirror=*/true, b, l);

    if (name == "reverse") {
        b.dyn_array_reverse(mirror, mty, l);
    } else if (name == "shuffle") {
        b.dyn_array_shuffle(mirror, mty, l);
    } else if (sort_uses_iter_path(call)) {
        lower_array_sort_iter(name, call, mirror, mty, /*queue=*/false, map, b, l);
    } else {
        b.dyn_array_sort(mirror, name == "rsort", mty, l);
    }

    emit_fixed_mirror_copy(arr, aty, mirror, mty, map, /*to_mirror=*/false, b, l);
    b.dyn_array_delete(mirror, mty, l);
    return arr;
}

// Copy a fixed-size array's elements into the ordering mirror in
// declared-address order (mirror position p <- declared index
// first + step * p), or copy the reordered result back out. 7.12.2 reorders
// "the elements of the array", and for a fixed array that sequence is its
// declared one: leftmost index first, so a descending `int q[7:3]` puts index
// 7 at position 0 and sorts toward index 3.
void Lowerer::emit_fixed_mirror_copy(ir::Index           arr,
                                     ir::Ref             aty,
                                     ir::Index           mirror,
                                     ir::Ref             mty,
                                     const DeclIndexMap& map,
                                     bool                to_mirror,
                                     ir::Builder&        b,
                                     ir::SourceLoc       l) {
    using T                  = ir::Inst::Tag;
    const ir::Ref   er       = std::get<ir::Type>(module_.pool.get(aty)).element;
    const bool      agg      = is_agg_abi_ref(er);
    const ir::Ref   i32      = make_int_type(32, false);
    const TempCell  pos      = make_temp_cell(i32, "fxord_pos");
    const ir::Index pos_lv   = pos.lv(b, l);
    const ir::Index pos_init = iconst(i32, 0, b, l);
    b.binary(T::Store, pos_lv, pos_init, l);

    b.open_region();
    const ir::Index      at    = b.unary(T::Load, pos.lv(b, l), l);
    const ir::Index      count = iconst(i32, map.fixed_count, b, l);
    const ir::Index      pred  = b.binary(T::SLt, at, count, l);
    const ir::ExtraSlice cond  = b.close_region();

    b.open_region();
    const ir::Index p    = b.unary(T::Load, pos.lv(b, l), l);
    const ir::Index in_a = b.elem_select(arr, decl_index_at(map, p, i32, b, l), aty, l, /*for_write=*/!to_mirror);
    const ir::Index in_m = b.elem_select(mirror, p, mty, l, /*for_write=*/to_mirror);
    const ir::Index dst  = to_mirror ? in_m : in_a;
    const ir::Index src  = to_mirror ? in_a : in_m;
    if (!agg) {
        b.binary(T::Store, dst, b.unary(T::Load, src, l), l);
    } else {
        // Per-leaf, never the whole-image agg_copy memcpy the other aggregate
        // copies in this file can take: one side here is a fixed array's
        // storage and the other a container element. A fixed array of
        // scalar-element sub-arrays stores FLAT leaf slots that may be
        // independently z-widened, while the container's element image is the
        // CANONICAL aggregate layout rt_elem_ops::stride was computed from, so
        // the two images can differ in size. Decomposing resolves each side's
        // own layout.
        emit_agg_copy(dst, src, er, T::Store, b, l);
    }
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index slot = pos.lv(b, l);
    const ir::Index cur  = b.unary(T::Load, pos.lv(b, l), l);
    const ir::Index one  = iconst(i32, 1, b, l);
    const ir::Index next = b.binary(T::Add, cur, one, l);
    b.binary(T::Store, slot, next, l);
    const ir::ExtraSlice step = b.close_region();
    b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
}

// An aggregate element on its way from a fixed-size array into a result
// queue. The runtime copies rt_elem_ops::stride bytes from the pointer handed
// over, and that stride comes from the CANONICAL aggregate image; a fixed
// array of scalar-element sub-arrays has flat storage whose leaf slots may be
// independently z-widened, so its element image is a different size and
// handing its pointer over would make the runtime read the wrong byte count.
// Stage those through a canonical temp, exactly as
// lower_container_element_value does for a pushed actual; a struct-bearing
// element is already canonical and needs no copy.
ir::Index Lowerer::canonical_elem_ptr(ir::Index elem, ir::Ref er, ir::Builder& b, ir::SourceLoc l) {
    if (contains_struct_ref(er)) {
        return elem;
    }
    const ir::Index temp = make_canonical_agg_temp(er, b, l);
    emit_agg_copy(temp, elem, er, ir::Inst::Tag::Store, b, l);
    return temp;
}

// `item.index` inside a `with` clause (LRM 7.12.4): the current iteration's
// index. slang resolves the user's chosen index-method name to this builtin
// and hands over the iterator variable as the receiver, so the binding is
// found by symbol identity rather than by name.
std::optional<ir::Index> Lowerer::lower_iterator_index(const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
    const auto& args = call.arguments();
    if (args[0]->kind != ExpressionKind::NamedValue) {
        return std::nullopt;
    }
    const slang::ast::Symbol& sym = args[0]->as<ValueExpressionBase>().symbol;
    if (sym.kind != slang::ast::SymbolKind::Iterator) {
        return std::nullopt;
    }
    if (args.size() > 1 &&
        const_uint(*args[1], "the dimension argument of an iterator index method (LRM 7.12.1)") != 1) {
        // TODO: `item.index(d)` for d > 1 needs multidimensional array
        // manipulation methods, which need multidimensional dynamic arrays.
        fail("the iterator index method is only supported for dimension 1");
    }
    for (size_t k = iter_stack_.size(); k-- > 0;) {
        if (iter_stack_[k].var == &sym) {
            const ir::Index cur  = b.unary(ir::Inst::Tag::Load, iter_stack_[k].index.lv(b, l), l);
            const ir::Ref   i32  = make_int_type(32, false);
            const ir::Index decl = decl_index_at(iter_stack_[k].map, cur, i32, b, l);
            return b.convert(decl, lower_type(*call.type), call.type->isSigned(), l);
        }
    }
    fail("the iterator index method is only supported inside its own `with` clause");
}

// A locator (LRM 7.12.1) or reduction (7.12.3) method on a dynamic array or
// queue, together with the `with` iterator clause both families share.
//
// All fifteen lower to one IR loop over the container rather than to a runtime
// helper the way the ordering methods do. The clause is arbitrary lowered IR,
// so it has to be evaluated in the frontend anyway — and going through the
// ordinary expression path is what makes a 4-state, wide, real or string
// element work with no per-element-type comparator in the runtime. The
// receiver handle and the element count are read once on entry: resizing the
// container from inside the clause is undefined per 7.12, same as mutating the
// array a foreach walks.
//
// A locator fills a fresh queue temp and returns its handle; a reduction folds
// into an accumulator at the result type's width, so a `byte` sum wraps like
// the LRM's worked example (7.12.3) rather than widening.
ir::Index Lowerer::lower_array_iter_method(std::string_view      name,
                                           const CallExpression& call,
                                           ir::Index             arr,
                                           ir::Builder&          b,
                                           ir::SourceLoc         l) {
    using T                          = ir::Inst::Tag;
    const Expression& recv           = *call.arguments()[0];
    const auto&       sci            = std::get<CallExpression::SystemCallInfo>(call.subroutine);
    const auto [iter_expr, iter_var] = sci.getIteratorInfo();

    const bool      queue = is_queue(recv);
    const ir::Ref   aty   = lower_type(*recv.type);
    // A fixed-size receiver differs in exactly two ways: its element count is
    // a compile-time constant rather than a handle query, and the indices it
    // REPORTS are its declared range. The walk itself stays 0-based.
    const DeclIndexMap map      = decl_index_map(recv);
    const ir::Ref   elem_ref = std::get<ir::Type>(module_.pool.get(aty)).element;
    const bool      agg_elem = is_agg_abi_ref(elem_ref);
    // With no `with` clause the key IS the element (7.12.1 defines the bare form
    // as `with (item)`), and an unpacked aggregate has no ordering, no bit-exact
    // dedup compare and no fold -- so those forms need a scalar clause. A
    // find*'s clause is mandatory anyway, and its predicate is already scalar.
    if (agg_elem && iter_expr == nullptr) {
        fail("array method '" + std::string(name) +
             "' on a container with an unpacked struct or array element needs a `with` clause "
             "(the element itself is not a key: LRM 7.12.1 and 7.12.3 both require the operation "
             "to be defined for the element type)");
    }
    const ir::Ref   i32   = make_int_type(32, false);
    const ir::Ref   bit1  = make_int_type(1, false);
    const ir::Index one1  = b.constant(module_.pool.put_int(bit1, 1), l);
    const ir::Index zero1 = b.constant(module_.pool.put_int(bit1, 0), l);

    // 0 .. size-1, with the count snapshotted like a foreach bound.
    const TempCell  idx   = make_temp_cell(i32, "arrm_idx");
    const TempCell  bound = make_temp_cell(i32, "arrm_bound");
    const ir::Index size =
      map.fixed ? iconst(i32, map.fixed_count, b, l)
                : b.convert(queue ? b.queue_size(arr, aty, l) : b.dyn_array_size(arr, aty, l), i32, true, l);
    b.binary(T::Store, bound.lv(b, l), size, l);
    const ir::Index idx_lv   = idx.lv(b, l);
    const ir::Index idx_init = b.constant(module_.pool.put_int(i32, 0), l);
    b.binary(T::Store, idx_lv, idx_init, l);

    if (iter_var != nullptr) {
        declare_storage(iter_var->as<slang::ast::VariableSymbol>(), /*force_frame*/ true);
    }

    // The key is the clause's value, or the element itself when the clause is
    // omitted -- 7.12.1 defines the bare form as "as if a with(item) clause
    // were specified". A find* predicate reduces the same clause to one bit.
    // The element at k: an ordinary value for a scalar element, and the element
    // LVALUE for an aggregate one -- which is what both an aggregate copy and
    // queue_push (whose element crosses by pointer) want.
    //
    // `decl_of` is the index this call REPORTS for position k: the receiver's
    // declared one (7.12.1's "indices of all such items", 7.12.4's index
    // method). It is also what addresses the element, because a fixed array's
    // elem_select takes a declared-range coordinate (ir::ElemSelect).
    const auto decl_of = [&](ir::Index k) { return decl_index_at(map, k, i32, b, l); };
    const auto elem_lv = [&](ir::Index k) { return b.elem_select(arr, decl_of(k), aty, l); };
    const auto elem_at = [&](ir::Index k) { return agg_elem ? elem_lv(k) : b.unary(T::Load, elem_lv(k), l); };
    // The element at k in the form queue_push wants: a value for a scalar
    // element, and for an aggregate one a pointer to a CANONICAL image, which
    // a fixed-size receiver's flat storage may not be (canonical_elem_ptr). A
    // queue's or dynamic array's element already lives in a canonical image,
    // so those emit exactly what they emitted before.
    const auto push_val = [&](ir::Index k) {
        if (!agg_elem || !map.fixed) {
            return elem_at(k);
        }
        return canonical_elem_ptr(elem_lv(k), elem_ref, b, l);
    };
    const auto bind    = [&](ir::Index k) {
        if (iter_var == nullptr) {
            return;
        }
        const ir::Index dst = value_lvalue(*iter_var, b, l);
        if (!agg_elem) {
            b.binary(T::Store, dst, elem_at(k), l);
            return;
        }
        if (is_trivially_copyable(elem_ref)) {
            b.agg_copy(dst, elem_lv(k), elem_ref, l);
            return;
        }
        emit_agg_copy(dst, elem_lv(k), elem_ref, T::Store, b, l);
    };
    const auto key_of = [&](ir::Index k) {
        return iter_expr != nullptr ? lower_expr(*iter_expr, b) : elem_at(k);
    };

    const slang::ast::Type& kty = iter_expr != nullptr ? *iter_expr->type : *recv.type->getArrayElementType();
    const bool              key_real = kty.isFloating();
    const bool              key_str  = kty.getCanonicalType().isString();
    const bool              indexed  = name.ends_with("_index");
    const bool              is_red   = is_reduction_method(name);
    const bool              minmax   = name == "min" || name == "max";
    const bool              uniq     = name == "unique" || name == "unique_index";
    const ir::Ref           rty      = lower_type(*call.type);

    ir::Index res{};
    TempCell  acc;
    if (is_red) {
        // The fold's identity, so a one-element array reduces to that element:
        // 0 for sum / or / xor, 1 for product, all ones for and. An EMPTY array
        // does not yield the identity though -- see the return below.
        acc                      = make_temp_cell(rty, "arrm_acc");
        const ir::Index seed     = b.constant(module_.pool.put_int(rty, name == "product" ? 1 : 0), l);
        const ir::Index acc_lv   = acc.lv(b, l);
        const ir::Index acc_init = name == "and" ? b.unary(T::Bnot, seed, l) : seed;
        b.binary(T::Store, acc_lv, acc_init, l);
    } else {
        res = make_arg_temp(rty, b, l);
        b.queue_delete(res, res, /*has_idx=*/false, rty, l);
    }

    TempCell best, have, key_cell, dup, scan;
    if (minmax) {
        // `best` is only read once `have` is set, so it needs no initializer.
        best = make_temp_cell(lower_type(kty), "arrm_best");
        have = make_temp_cell(bit1, "arrm_have");
        b.binary(T::Store, have.lv(b, l), zero1, l);
    }
    if (uniq) {
        key_cell = make_temp_cell(lower_type(kty), "arrm_key");
        dup      = make_temp_cell(bit1, "arrm_dup");
        scan     = make_temp_cell(i32, "arrm_scan");
    }

    iter_stack_.push_back(IterBinding{ .var = iter_var, .index = idx, .map = map });

    b.open_region();
    const ir::Index      cur   = b.unary(T::Load, idx.lv(b, l), l);
    const ir::Index      lpred = b.binary(T::SLt, cur, b.unary(T::Load, bound.lv(b, l), l), l);
    const ir::ExtraSlice cond  = b.close_region();

    b.open_region();
    const ir::Index k = b.unary(T::Load, idx.lv(b, l), l);
    bind(k);
    if (is_red) {
        const T         op      = name == "sum"       ? T::Add
                                  : name == "product" ? T::Mul
                                  : name == "and"     ? T::Band
                                  : name == "or"      ? T::Bor
                                                      : T::Bxor;
        const ir::Index v       = key_of(k);
        const ir::Index acc_lv  = acc.lv(b, l);
        const ir::Index acc_now = b.unary(T::Load, acc.lv(b, l), l);
        const ir::Index widened = b.convert(v, rty, call.type->isSigned(), l);
        const ir::Index folded  = b.binary(op, acc_now, widened, l);
        b.binary(T::Store, acc_lv, folded, l);
    } else if (minmax) {
        const ir::Index key    = key_of(k);
        const bool      key_signed = kty.isSigned();
        const T         ord        = name == "min" ? (key_real ? T::FLt : (key_signed ? T::SLt : T::ULt))
                                                   : (key_real ? T::FGt : (key_signed ? T::SGt : T::UGt));
        const ir::Index better = b.binary(ord, key, b.unary(T::Load, best.lv(b, l), l), l);
        const ir::Index first  = b.unary(T::Lnot, b.unary(T::Load, have.lv(b, l), l), l);
        // A tie keeps the earlier element: 7.12.1 returns just one of them, and
        // an x-bearing 4-state compare is not a definite 1, so it loses too.
        b.open_region();
        b.binary(T::Store, have.lv(b, l), one1, l);
        b.binary(T::Store, best.lv(b, l), key, l);
        b.queue_delete(res, res, /*has_idx=*/false, rty, l);
        b.queue_push(res, push_val(k), /*front=*/false, rty, l);
        const ir::ExtraSlice take_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice skip_r = b.close_region();
        b.cond(b.binary(T::Lor, first, better, l), take_r, skip_r, l);
    } else if (uniq) {
        // An element is unique when no earlier element yields the same key, so
        // the first of each duplicate group survives (7.12.1 leaves the
        // returned order unrelated to the array's). The scan re-evaluates the
        // clause instead of caching keys in a second heap queue: 7.12 already
        // makes a clause with side effects unpredictable.
        const ir::Index key_lv  = key_cell.lv(b, l);
        const ir::Index key_val = key_of(k);
        b.binary(T::Store, key_lv, key_val, l);
        b.binary(T::Store, dup.lv(b, l), zero1, l);
        const ir::Index scan_lv   = scan.lv(b, l);
        const ir::Index scan_init = b.constant(module_.pool.put_int(i32, 0), l);
        b.binary(T::Store, scan_lv, scan_init, l);

        b.open_region();
        const ir::Index      at    = b.unary(T::Load, scan.lv(b, l), l);
        const ir::Index      spred = b.binary(T::SLt, at, b.unary(T::Load, idx.lv(b, l), l), l);
        const ir::ExtraSlice scond = b.close_region();

        b.open_region();
        const ir::Index j        = b.unary(T::Load, scan.lv(b, l), l);
        iter_stack_.back().index = scan;
        bind(j);
        const ir::Index other    = key_of(j);
        iter_stack_.back().index = idx;
        // Integral keys dedup bit-exactly (===), which is also how slang's own
        // constant evaluation of unique() dedups; a real or string key has no
        // x/z to distinguish.
        const T         eq   = key_real ? T::FEq : (key_str ? T::Eq : T::CaseEq);
        const ir::Index same = b.binary(eq, other, b.unary(T::Load, key_cell.lv(b, l), l), l);
        b.open_region();
        b.binary(T::Store, dup.lv(b, l), one1, l);
        const ir::ExtraSlice mark_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice nomark_r = b.close_region();
        b.cond(same, mark_r, nomark_r, l);
        const ir::ExtraSlice sbody = b.close_region();

        b.open_region();
        const ir::Index scan_at   = b.unary(T::Load, scan.lv(b, l), l);
        const ir::Index scan_one  = b.constant(module_.pool.put_int(i32, 1), l);
        const ir::Index next_scan = b.binary(T::Add, scan_at, scan_one, l);
        b.binary(T::Store, scan.lv(b, l), next_scan, l);
        const ir::ExtraSlice sstep = b.close_region();
        b.loop(scond, spred, sbody, sstep, /*bottom_test*/ false, l);

        b.open_region();
        b.queue_push(res, indexed ? decl_of(k) : push_val(k), /*front=*/false, rty, l);
        const ir::ExtraSlice keep_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice drop_r = b.close_region();
        b.cond(b.unary(T::Lnot, b.unary(T::Load, dup.lv(b, l), l), l), keep_r, drop_r, l);
    } else {
        // find / find_index / find_first* / find_last*: the clause is a
        // predicate. find_last keeps only the newest match by clearing first;
        // find_first stops the walk.
        const ir::Index p = lower_predicate(*iter_expr, b);
        b.open_region();
        if (name.starts_with("find_last")) {
            b.queue_delete(res, res, /*has_idx=*/false, rty, l);
        }
        b.queue_push(res, indexed ? decl_of(k) : push_val(k), /*front=*/false, rty, l);
        if (name.starts_with("find_first")) {
            b.break_(l);
        }
        const ir::ExtraSlice hit_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice miss_r = b.close_region();
        b.cond(p, hit_r, miss_r, l);
    }
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index slot = idx.lv(b, l);
    const ir::Index sc   = b.unary(T::Load, idx.lv(b, l), l);
    const ir::Index one  = b.constant(module_.pool.put_int(i32, 1), l);
    const ir::Index next = b.binary(T::Add, sc, one, l);
    b.binary(T::Store, slot, next, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, lpred, body, step, /*bottom_test*/ false, l);
    iter_stack_.pop_back();

    if (!is_red) {
        return res;
    }
    // An empty array reduces to 0, not to the fold's identity -- LRM 7.12.3 does
    // not say, and Verilator answers 0 for all five, including `product` and
    // `and` (docs/LRM_NOTES.md, note 8).
    const ir::Index n     = b.unary(T::Load, bound.lv(b, l), l);
    const ir::Index empty = b.binary(T::Eq, n, b.constant(module_.pool.put_int(i32, 0), l), l);
    const ir::Index zero  = b.constant(module_.pool.put_int(rty, 0), l);
    const ir::Index total = b.unary(T::Load, acc.lv(b, l), l);
    return b.select(empty, zero, total, l);
}

// Element address for a variable-size array (dynamic array or queue) index
// over the handle lvalue `base`. Handles the queue `$` selector (q[$] is the
// last element, index size-1; size-1 == -1 selects the OOB default on an
// empty queue).
//
// `is_write` marks the direction of the access this select serves. It is only
// consulted when `base` is a DEFERRED associative-array element (`a[k][i]`),
// where the ring arithmetic below forces the entry's slot to be resolved before
// any consumer is in sight: a read resolves with get_ptr and must not create the
// entry (LRM 7.8.6), a write with put_ptr and must (LRM 7.8).
ir::Index Lowerer::var_elem_select(const ElementSelectExpression& es,
                                   ir::Index                      base,
                                   ir::Builder&                   b,
                                   ir::SourceLoc                  l,
                                   bool                           is_write) {
    const ir::Ref arr_ty = lower_type(*es.value().type);
    if (is_queue(es.value()) && es.selector().type->isUnbounded()) {
        const ir::Ref   i32  = make_int_type(32, false);
        const ir::Index size = b.queue_size(base, arr_ty, l);
        const ir::Index idx  = b.binary(ir::Inst::Tag::Sub, size, b.constant(module_.pool.put_int(i32, 1), l), l);
        return b.elem_select(base, idx, arr_ty, l, is_write);
    }
    const ir::Index sel = b.elem_select(base, lower_index(es.selector(), b), arr_ty, l, is_write);
    // The select carries the extent of what was indexed, so a bounds report
    // can underline `grid[a]` under the `[b]` of `grid[a][b]` rather than
    // guess the width from the array's name.
    b.set_end(sel, to_source_loc(es.value().sourceRange.end()));
    return sel;
}

// A key operand of `a[k]` / `exists(k)` / `delete(k)`: an ordinary value for an
// integral, class-handle or string key (the backend normalizes the plane), and an
// LVALUE for an unpacked struct / array key (P0/P1), or a bare queue / dynamic
// array / associative array key itself (P2/P3) — none of these has a plane
// value, so the backend walks its storage / handle directly. A non-lvalue
// aggregate key (a `'{...}` pattern) is staged into a canonical temp of the
// key type first; a non-lvalue queue key (a literal, a slice, a concatenation)
// reuses the same materializing handle temp any other queue-typed rvalue gets.
ir::Index Lowerer::lower_assoc_key(const Expression& key, ir::Ref assoc_ty, ir::Builder& b, ir::SourceLoc l) {
    const ir::Type at = std::get<ir::Type>(module_.pool.get(assoc_ty));
    if (at.right != 0) {
        return lower_expr(key, b); // wildcard [*]: any integral value
    }
    const auto     kr = static_cast<ir::Ref>(at.fields_id);
    const ir::Type kt = std::get<ir::Type>(module_.pool.get(kr));
    if (kt.kind == ir::Type::Kind::Queue) {
        return materialize_queue_rvalue(key, b);
    }
    if (kt.kind == ir::Type::Kind::DynArray || kt.kind == ir::Type::Kind::Assoc) {
        const Expression& src = unwrap_conversion(key);
        if (is_struct_lvalue_shape(src)) {
            return lower_lvalue(src, b);
        }
        // TODO: a non-lvalue dynamic-array / associative-array key expression
        // (a literal, a slice, an in-place call result) — needs the same
        // materializing temp construction materialize_queue_rvalue gives a
        // queue key, written for a dynamic-array / assoc handle instead of a
        // queue's ring.
        fail("a non-variable dynamic-array / associative-array key expression "
             "is not supported");
    }
    if (!is_agg_abi_ref(kr)) {
        return lower_expr(key, b);
    }
    const Expression& src = unwrap_conversion(key);
    if (is_struct_lvalue_shape(src)) {
        return lower_lvalue(src, b);
    }
    const ir::Index temp = make_canonical_agg_temp(kr, b, l);
    emit_agg_store(temp, kr, src, ir::Inst::Tag::Store, b, l);
    return temp;
}

// Element address for an associative array (LRM 7.8): `a[key]` over the
// handle lvalue `base`. The backend normalizes the key into its byte span and
// resolves get_ptr (read) / put_ptr (write) at the consumer — except when THIS
// select's base is itself a deferred entry (`a[k1][k2]`), where the inner select
// must resolve the outer entry eagerly and `is_write` decides how.
ir::Index Lowerer::var_assoc_select(const ElementSelectExpression& es,
                                    ir::Index                      base,
                                    ir::Builder&                   b,
                                    ir::SourceLoc                  l,
                                    bool                           is_write) {
    const ir::Ref aty = lower_type(*es.value().type);
    return b.assoc_select(base, lower_assoc_key(es.selector(), aty, b, l), aty, l, is_write);
}

// Materialize a queue-typed rvalue into a handle lvalue Index. A plain queue
// variable / queue-returning call already has a handle lvalue and is returned
// as-is (no copy). A non-lvalue queue value — an assignment pattern '{...}, a
// concatenation {q, e}, or a slice q[a:b] (LRM 7.10.4) — is built element-wise
// into a fresh temp queue handle (cleared first, so a re-triggered process
// reuses the same slot) and that temp's lvalue is returned. The temp never
// aliases a source, so a self-referential `q = {q, e}` is safe.
ir::Index Lowerer::materialize_queue_rvalue(const Expression& expr, ir::Builder& b) {
    const Expression&   e = unwrap_conversion(expr);
    const ir::SourceLoc l = loc(e);
    if (e.kind == ExpressionKind::NamedValue || e.kind == ExpressionKind::HierarchicalValue ||
        e.kind == ExpressionKind::Call ||
        // A queue that IS an element or a member of something -- a nested queue
        // element, a struct member, a class property -- already has a handle
        // lvalue, so it needs no temp either.
        e.kind == ExpressionKind::ElementSelect || e.kind == ExpressionKind::MemberAccess) {
        return lower_lvalue(e, b);
    }
    const ir::Ref   qty = lower_type(*expr.type);
    const ir::Index tmp = make_arg_temp(qty, b, l);
    b.queue_delete(tmp, tmp, /*has_idx=*/false, qty, l);
    append_queue_pieces(tmp, e, qty, b, l);
    return tmp;
}

// Append the element pieces of a queue-valued rvalue `e` onto the dst handle:
// a slice (append the source's [a:b] range), or the operands of a
// concatenation / elements of an assignment pattern — each either a whole
// queue (spliced in via a full-range append) or a single element (pushed to
// the back). A queue-typed piece is itself materialized first (nested slice /
// concat).
void Lowerer::append_queue_pieces(ir::Index dst, const Expression& e, ir::Ref qty, ir::Builder& b, ir::SourceLoc l) {
    if (e.kind == ExpressionKind::RangeSelect) {
        const auto& rs = e.as<RangeSelectExpression>();
        if (!is_queue(rs.value())) {
            // Structurally unreachable: slang types a RangeSelectExpression as
            // a queue only when its OWN base already is one
            // (SelectExpressions.cpp: `if (isQueue) result->type = Queue...`,
            // decided before any dynamic-array-specific path runs). A
            // dynamic-array slice `da[a:b]` (LRM 7.10.4) instead types as a
            // FixedSizeUnpackedArrayType with constant bounds, so it never
            // reaches a queue-typed consumer at all (see is_unpacked_array /
            // is_dyn_array in lower_range_select for that separate, still
            // unimplemented shape). Kept as defense in depth against that
            // binder guarantee changing.
            fail("internal: slice of a non-queue base flowed into queue lowering");
        }
        const ir::Index src = materialize_queue_rvalue(rs.value(), b);
        // `$` in any bound (LRM 7.10.4, e.g. `q[1:$]`, `q[0:$-1]`) resolves
        // against THIS slice's source queue -- see the UnboundedLiteral case
        // in lower_expr. Scoped around both forms below: the Simple bounds
        // AND an indexed form's base index can equally spell `$`.
        const Restore<std::optional<QueueDollarCtx>> dollar_guard(queue_dollar_, QueueDollarCtx{ src, qty });
        ir::Index lo, hi;
        if (rs.getSelectionKind() == RangeSelectionKind::Simple) {
            lo = lower_expr(rs.left(), b);
            hi = lower_expr(rs.right(), b);
        } else {
            // Indexed part-select `q[i +: n]` / `q[i -: n]` (LRM 11.5.1,
            // extended to queues by 7.10.4). Unlike the fixed/packed-array
            // form, a queue's width `n` need NOT be a compile-time constant
            // (confirmed against slang: it applies no such check when the
            // base is a queue) -- both `i` and `n` are ordinary runtime
            // expressions. Translated to the same inclusive [lo:hi] shape the
            // Simple form already feeds rt_queue_append_range, which clamps
            // both ends against the source's actual length, so no separate
            // bounds handling is needed here.
            // A canonical `int` (LRM's own type for an index/width, and what
            // `q[$]`'s size()-1 resolution already produces below), not
            // `rs.left().type`: the base may itself be bare `$`
            // (UnboundedType), which lower_type rejects outright.
            const ir::Ref   i32   = make_int_type(32, false);
            const ir::Index base  = b.convert(lower_expr(rs.left(), b), i32, true, l);
            const ir::Index count = b.convert(lower_expr(rs.right(), b), i32, true, l);
            const ir::Index one   = b.constant(module_.pool.put_int(i32, 1), l);
            const ir::Index nm1   = b.binary(ir::Inst::Tag::Sub, count, one, l);
            if (rs.getSelectionKind() == RangeSelectionKind::IndexedUp) {
                lo = base;
                hi = b.binary(ir::Inst::Tag::Add, base, nm1, l);
            } else {
                hi = base;
                lo = b.binary(ir::Inst::Tag::Sub, base, nm1, l);
            }
        }
        b.queue_append_range(dst, src, lo, hi, qty, l);
        return;
    }
    if (e.kind == ExpressionKind::Streaming) {
        // `q = {>>{...}}` (LRM 11.4.14): the stream's bits divide into whole
        // elements, pushed back in stream order.
        std::vector<StreamChunk> chunks;
        collect_stream_chunks(e.as<StreamingConcatenationExpression>(), chunks, b, l);
        stream_fill_queue(dst, chunks, qty, b, l);
        return;
    }
    const auto* pat = as_assignment_pattern(e);
    if (pat == nullptr && e.kind != ExpressionKind::Concatenation) {
        // TODO: other queue rvalue sources (an unpacked-array cast, a
        // replication concat, etc.).
        fail("queue assignment from this expression is not supported");
    }
    const std::span<const Expression* const> pieces =
      pat != nullptr ? pat->elements() : e.as<ConcatenationExpression>().operands();
    // A full-queue append: lo = 0, hi = INT64_MAX (runtime-clamped to len-1).
    const ir::Ref   i64   = make_int_type(64, false);
    const ir::Index lo0   = b.constant(module_.pool.put_int(i64, 0), l);
    const ir::Index hiEnd = b.constant(module_.pool.put_int(i64, static_cast<uint64_t>(INT64_MAX)), l);
    // A CONCATENATION operand that is itself a queue splices its elements
    // (LRM 11.4.12): `{q, e}` flattens q's own elements in, one queue-typed
    // operand contributing many. A PATTERN element never does this,
    // regardless of its own type: `'{a, b}` specifies exactly one value per
    // outer element (LRM 5.11 / 7.10.2), so when the outer queue's OWN
    // element type happens to itself be a queue (`int qq[$][$]`), a
    // queue-typed PATTERN piece like `'{1,2}` is one element to push, not a
    // source to splice. Treating it as a splice source read it through the
    // OUTER container's element stride (a queue-handle-sized stride) instead
    // of its own -- silent memory corruption, not a refusal, since both
    // ops tables are well-formed on their own.
    const bool splices_queues = pat == nullptr;
    for (const Expression* op : pieces) {
        if (splices_queues && is_queue(*op)) {
            b.queue_append_range(dst, materialize_queue_rvalue(*op, b), lo0, hiEnd, qty, l);
            continue;
        }
        if (splices_queues &&
            (is_dyn_array(*op) || op->type->getCanonicalType().isUnpackedArray())) {
            // A dynamic-array / unpacked-array operand splices its elements
            // (LRM 11.4.12), but rt_queue_append_range only walks an rt_queue
            // source; the frontend would otherwise push the whole aggregate as
            // one (garbage) scalar element.
            fail("concatenation of a dynamic array / unpacked array into a queue "
                 "is not supported");
        }
        // A single element operand.
        b.queue_push(dst, lower_container_element_value(*op, qty, b, l), /*front=*/false, qty, l);
    }
}

// Whole-queue assignment (LRM 7.10): `q = other` deep-copies the source
// handle; a concatenation ({q, e}), assignment pattern ('{...}), or slice
// (q[a:b]) is materialized element-wise into a temp and then deep-copied in.
// Element writes (`q[i] = x`) are ordinary Stores over an ElemSelect and do
// not reach here.
ir::Index Lowerer::lower_queue_assign(const AssignmentExpression& a, ir::Builder& b) {
    const ir::SourceLoc l   = loc(a);
    const ir::Index     dst = lower_lvalue(a.left(), b);
    const ir::Ref       qty = lower_type(*a.left().type);
    const Expression&   rhs = unwrap_conversion(a.right());
    if (a.isNonBlocking()) {
        // Source-shape refusals come FIRST, as they do on the blocking path, so
        // a nonblocking assignment from an unsupported source still names the
        // source rather than the region.
        if (is_dyn_array(rhs)) {
            if (!is_plain_handle_place(rhs)) {
                fail("queue assignment from this dynamic-array expression is not supported");
            }
            // TODO: nonblocking q <= d whole copy from a dynamic array (needs
            // a dynarr-source staging path in the NBA region).
            fail("nonblocking queue assignment from a dynamic array is not supported");
        }
        if (is_unpacked_array(rhs)) {
            // TODO: nonblocking q <= f whole copy from a fixed array (needs
            // a fixed-array-source staging path in the NBA region, same gap
            // as the dynamic-array source above).
            fail("nonblocking queue assignment from a fixed-size array is not supported");
        }
        // Whole-queue NBA: stage a deep copy now, move it into dst in the NBA
        // region (LRM 10.4.2). The target is a module signal (task/process
        // NBA is module-signal-only; function NBA is rejected above).
        b.handle_copy_nb(dst, materialize_queue_rvalue(a.right(), b), qty, l);
        return dst;
    }
    emit_queue_store(dst, qty, a.right(), b, l);
    return dst;
}

// A source shape a container deep copy can address directly: a named variable,
// a hierarchical reference, or a struct member. Not an element select or a call
// result, neither of which the dynarr-into-queue runtime path can take.
bool Lowerer::is_plain_handle_place(const Expression& e) {
    return e.kind == ExpressionKind::NamedValue || e.kind == ExpressionKind::HierarchicalValue ||
           e.kind == ExpressionKind::MemberAccess;
}

void Lowerer::emit_queue_store(ir::Index         dst,
                               ir::Ref           qty,
                               const Expression& rhs_expr,
                               ir::Builder&      b,
                               ir::SourceLoc     l) {
    // q = d: whole dynamic-array-into-queue copy (LRM 7.10). The source is an
    // rt_dynarr handle, not an rt_queue, so it takes its own runtime path
    // (feeding a dynarr handle to rt_queue_assign reads past the struct and
    // runs away). An empty source clears the queue to size 0.
    const Expression& rhs = unwrap_conversion(rhs_expr);
    if (is_dyn_array(rhs)) {
        if (!is_plain_handle_place(rhs)) {
            // TODO: queue = <dynamic-array rvalue> where the source is not a
            // plain variable (a call result, slice, etc.) — needs the source
            // materialized into a temp dynarray handle first.
            fail("queue assignment from this dynamic-array expression is not supported");
        }
        b.queue_copy_dynarr(dst, lower_lvalue(rhs, b), qty, l);
        return;
    }
    if (is_unpacked_array(rhs)) {
        // q = f: whole fixed-array-into-queue copy (LRM 7.10.2). `f`'s
        // element count is a compile-time constant, unlike a dynamic
        // array or another queue's runtime size().
        emit_fixed_array_into_queue(dst, rhs, qty, b, l);
        return;
    }
    b.queue_copy(dst, materialize_queue_rvalue(rhs_expr, b), qty, l);
}

// Whole-associative-array assignment (LRM 7.9): `a = other` deep-copies the
// source map (the pattern default included — it is part of the value), and
// `a = '{...}` rebuilds it from a literal (7.9.11). Element writes (`a[k] = x`)
// are ordinary Stores over an AssocSelect and do not reach here.
ir::Index Lowerer::lower_assoc_assign(const AssignmentExpression& a, ir::Builder& b) {
    const ir::SourceLoc l   = loc(a);
    const ir::Index     dst = lower_lvalue(a.left(), b);
    const ir::Ref       aty = lower_type(*a.left().type);
    if (a.isNonBlocking()) {
        if (as_assignment_pattern(a.right()) != nullptr) {
            // TODO: a `'{key:value}` / `'{default:v}` literal on the right of a
            // nonblocking assignment. The literal builds its entries in place
            // rather than deep-copying a handle, so staging it needs a temp map
            // first. Must be refused BEFORE the blocking path below, which would
            // otherwise silently apply the literal as a BLOCKING assignment.
            fail("nonblocking assignment of a literal into an associative array is "
                 "not supported");
        }
        if (!is_assoc_source_place(a.right())) {
            fail("associative array assignment from a non-variable value is not supported");
        }
        // `a <= other` (LRM 7.9, 10.4.2): staged deep copy in the Active region,
        // moved into the live map in the NBA region -- the same HandleCopyNb a
        // queue and a dynamic array take.
        b.handle_copy_nb(dst, lower_lvalue(a.right(), b), aty, l);
        return dst;
    }
    emit_assoc_store(dst, aty, a.right(), b, l);
    return dst;
}

// LRM 7.9 admits a variable, a call result or a literal as an associative-array
// source. A "variable" here is any PLACE holding one, not just a named one: an
// assoc that lives in a struct member, an array element, a container element or
// a class property is equally a handle lvalue, and the deep copy reaches it
// through the same resolver.
bool Lowerer::is_assoc_source_place(const Expression& e) {
    return is_assoc(e) && (is_struct_lvalue_shape(e) || e.kind == ExpressionKind::Call);
}

void Lowerer::emit_assoc_store(ir::Index         dst,
                               ir::Ref           aty,
                               const Expression& rhs_expr,
                               ir::Builder&      b,
                               ir::SourceLoc     l) {
    if (as_assignment_pattern(rhs_expr) != nullptr) {
        emit_assoc_literal_init(dst, aty, unwrap_conversion(rhs_expr), ir::Inst::Tag::Store, b, l);
        return;
    }
    if (!is_assoc_source_place(rhs_expr)) {
        // TODO: an associative-array source that is none of those three shapes.
        // LRM 7.9 defines no other whole-array value, so this is a defensive
        // guard rather than a known gap.
        fail("associative array assignment from a non-variable value is not supported");
    }
    b.assoc_copy(dst, lower_lvalue(rhs_expr, b), aty, l);
}

// One element value on its way into a container, in the form the backend wants:
// an ordinary value for a scalar / string / handle element, and an LVALUE Index
// for an aggregate element, which crosses the rt_queue_* ABI as a pointer to its
// storage image. A non-lvalue aggregate actual (a `'{...}` pattern) is staged
// into a canonical aggregate temp first -- the same shape an aggregate
// associative-array key takes (lower_assoc_key).
ir::Index Lowerer::lower_container_element_value(const Expression& val,
                                                 ir::Ref           container_ty,
                                                 ir::Builder&      b,
                                                 ir::SourceLoc     l) {
    const ir::Ref er = std::get<ir::Type>(module_.pool.get(container_ty)).element;
    if (value_cat(er) == ValueCat::handle) {
        // A nested container element crosses as a pointer to ITS handle, which the
        // runtime then deep-copies. A queue value that is not already a handle
        // lvalue (a '{...} pattern, a concatenation, a slice) materializes into a
        // temp handle first, exactly as a whole-queue assignment source does.
        if (std::get<ir::Type>(module_.pool.get(er)).kind == ir::Type::Kind::Queue) {
            return materialize_queue_rvalue(val, b);
        }
        const Expression& src = unwrap_conversion(val);
        if (!is_struct_lvalue_shape(src) && src.kind != ExpressionKind::Call) {
            // TODO: a non-variable dynamic-array element value (`new[n]` / a
            // pattern), which needs the dynamic-array twin of
            // materialize_queue_rvalue.
            fail("this dynamic-array value as a container element is not supported");
        }
        return lower_lvalue(src, b);
    }
    if (!is_agg_abi_ref(er)) {
        return lower_expr(val, b);
    }
    const Expression& src = unwrap_conversion(val);
    // The element crosses the rt_* ABI as a pointer to a CANONICAL aggregate
    // image, because that is the layout `rt_elem_ops::stride` was computed from.
    // A struct-bearing actual already has that layout; a plain (scalar-element)
    // array signal has FLAT storage whose element slot may be independently
    // z-widened, so its image is a different size and handing its pointer over
    // would make the runtime copy the wrong byte count. Stage those through a
    // canonical temp, exactly as a by-value array argument does
    // (build_call_args), and let a non-lvalue actual take the same path.
    if (is_struct_lvalue_shape(src) && contains_struct_ref(er)) {
        return lower_lvalue(src, b);
    }
    const ir::Index temp = make_canonical_agg_temp(er, b, l);
    if (is_struct_lvalue_shape(src)) {
        emit_agg_copy(temp, lower_lvalue(src, b), er, ir::Inst::Tag::Store, b, l);
    } else {
        emit_agg_store(temp, er, src, ir::Inst::Tag::Store, b, l);
    }
    return temp;
}

// Whether a container built-in method WRITES its receiver (LRM 7.9 / 7.10 /
// 7.12). Only used to pick get_ptr vs put_ptr when the receiver's select chain
// contains an associative entry that an inner select has to resolve eagerly; the
// ordinary one-level receiver stays deferred and the backend decides per op.
bool Lowerer::mutates_container(std::string_view name) {
    return name == "push_back" || name == "push_front" || name == "pop_back" || name == "pop_front" ||
           name == "insert" || name == "delete" || name == "reverse" || name == "sort" || name == "rsort" ||
           name == "shuffle" || name == "new";
}

// A queue built-in method used as an rvalue or statement (LRM 7.10.2). The
// receiver arrives as arguments()[0], like a string / dynamic-array method.
ir::Index Lowerer::lower_queue_method(std::string_view      name,
                                      const CallExpression& call,
                                      ir::Builder&          b,
                                      ir::SourceLoc         l) {
    const auto&     args = call.arguments();
    // A MUTATING method operates on the receiver as a variable (LRM 7.10), so a
    // deferred associative entry in its select chain resolves with put_ptr and the
    // entry comes into existence; a query must create nothing (7.8.6). The
    // distinction only bites a NESTED receiver (`a[k1][k2].push_back(v)`), where an
    // inner select resolves the outer entry eagerly -- a one-level `a[k].push_back`
    // stays deferred and the backend picks the direction per op.
    const bool      recv_write = mutates_container(name);
    const ir::Index q          = lower_lvalue(*args[0], b, recv_write);
    const ir::Ref   qty  = lower_type(*args[0]->type);
    if (name == "size") {
        return b.convert(b.queue_size(q, qty, l), lower_type(*call.type), call.type->isSigned(), l);
    }
    if (name == "push_back" || name == "push_front") {
        return b.queue_push(q, lower_container_element_value(*args[1], qty, b, l), name == "push_front", qty, l);
    }
    if (name == "pop_back" || name == "pop_front") {
        return b.queue_pop(q, name == "pop_front", qty, l);
    }
    if (name == "insert") {
        const ir::Index idx  = lower_index(*args[1], b);
        const ir::Index elem = lower_container_element_value(*args[2], qty, b, l);
        return b.queue_insert(q, idx, elem, qty, l);
    }
    if (name == "delete") {
        // q.delete() clears all; q.delete(i) removes one element.
        if (args.size() > 1) {
            return b.queue_delete(q, lower_index(*args[1], b), /*has_idx*/ true, qty, l);
        }
        return b.queue_delete(q, q, /*has_idx*/ false, qty, l);
    }
    if (name == "reverse") {
        return b.queue_reverse(q, qty, l);
    }
    if (name == "sort" || name == "rsort") {
        guard_ordering_sort(name, call);
        if (sort_uses_iter_path(call)) {
            return lower_array_sort_iter(name, call, q, qty, /*queue=*/true, DeclIndexMap{}, b, l);
        }
        return b.queue_sort(q, name == "rsort", qty, l);
    }
    if (name == "shuffle") {
        return b.queue_shuffle(q, qty, l);
    }
    if (is_locator_method(name) || is_reduction_method(name)) {
        return lower_array_iter_method(name, call, q, b, l);
    }
    // TODO: `map` (LRM 7.12.5).
    fail("queue method '" + std::string(name) + "' is not supported");
}

// An associative-array built-in method used as an rvalue or statement (LRM
// 7.9). The receiver arrives as arguments()[0]. Keys are lowered as ordinary
// value expressions (normalized in the backend); first/last/next/prev take
// their key argument by ref (an lvalue output), which slang may wrap in an
// AssignmentExpression.
ir::Index Lowerer::lower_assoc_method(std::string_view      name,
                                      const CallExpression& call,
                                      ir::Builder&          b,
                                      ir::SourceLoc         l) {
    const auto&     args = call.arguments();
    // See lower_queue_method: a mutating method's receiver resolves a deferred
    // outer entry with put_ptr, a query's must not create one.
    const ir::Index a    = lower_lvalue(*args[0], b, mutates_container(name));
    const ir::Ref   aty  = lower_type(*args[0]->type);
    if (name == "num" || name == "size") {
        return b.convert(b.assoc_num(a, aty, l), lower_type(*call.type), call.type->isSigned(), l);
    }
    if (name == "exists") {
        return b.convert(b.assoc_exists(a, lower_assoc_key(*args[1], aty, b, l), aty, l),
                         lower_type(*call.type),
                         call.type->isSigned(),
                         l);
    }
    if (name == "delete") {
        // a.delete() clears all; a.delete(k) removes one entry.
        if (args.size() > 1) {
            return b.assoc_delete(a, lower_assoc_key(*args[1], aty, b, l), /*has_key*/ true, aty, l);
        }
        return b.assoc_delete(a, a, /*has_key*/ false, aty, l);
    }
    if (name == "first" || name == "last" || name == "next" || name == "prev") {
        const slang::ast::Expression* karg = args[1];
        if (karg->kind == slang::ast::ExpressionKind::Assignment) {
            karg = &karg->as<AssignmentExpression>().left();
        }
        const ir::Index key_lv = lower_lvalue(*karg, b);
        using Step             = ir::Builder::AssocStep;
        const Step step        = name == "first"  ? Step::First
                                 : name == "last" ? Step::Last
                                 : name == "next" ? Step::Next
                                                  : Step::Prev;
        return b.convert(b.assoc_traverse(a, key_lv, step, aty, l), lower_type(*call.type), call.type->isSigned(), l);
    }
    // TODO: find/find_index/min/max/unique/sum reduction & locator methods
    // (need iterator-clause lowering, shared with the queue/dynarr gap above),
    // and `map` (LRM 7.12.5).
    fail("associative array method '" + std::string(name) + "' is not supported");
}

// Lower a fixed-width RHS streaming concatenation (LRM §11.4.14).
// `>>` (sliceSize==0): plain left-to-right concat, no reorder.
// `<<` (sliceSize>0): concatenate, then reverse the order of sliceSize-bit
// blocks (rightmost block becomes the MSB of the result).
// Whether an expression is a whole unpacked aggregate (fixed array / unpacked
// struct) used as a bitstream operand — it has no flat load, so it serializes
// to a concatenation of its leaf bits (LRM 11.4.14 / §11.2.2).
bool Lowerer::is_agg_bitstream_operand(const Expression& e) const {
    return is_agg_abi_type(*e.type);
}

// Serialize a whole unpacked aggregate lvalue to a packed bit-value: a
// left-to-right concatenation of its leaf values (declaration / declared-range
// order, first element/field at the MSB, per LRM 11.4.14.2 / 7.2.1). The
// element / field bit widths sum to $bits of the aggregate.
ir::Index Lowerer::serialize_agg_lvalue(ir::Index lv, ir::Ref ty, ir::Builder& b, ir::SourceLoc l) {
    const ir::Type         t = std::get<ir::Type>(module_.pool.get(ty));
    std::vector<ir::Index> parts;
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        for (uint32_t k = 0; k < n; ++k) {
            const int32_t coord =
              t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
            const ir::Index idx = b.constant(
              module_.pool.put_int(make_int_type(32, false), static_cast<uint64_t>(static_cast<int64_t>(coord))), l);
            parts.push_back(serialize_agg_lvalue(b.elem_select(lv, idx, ty, l), t.element, b, l));
        }
    } else if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        const auto fields = module_.pool.struct_fields(t.fields_id);
        for (uint32_t i = 0; i < fields.size(); ++i) {
            parts.push_back(serialize_agg_lvalue(b.member_select(lv, i, ty, l), fields[i].type, b, l));
        }
    } else {
        if (value_cat(ty) != ValueCat::scalar || module_.pool.is_float(ty)) {
            // TODO: a string / real / heap-handle leaf in a bitstream (LRM
            // 11.4.14 restricts streams to bit-stream types anyway).
            fail("streaming a non-integral aggregate leaf is not supported");
        }
        return b.unary(ir::Inst::Tag::Load, lv, l);
    }
    return parts.size() == 1 ? parts[0] : b.concat(parts, l);
}

ir::Index Lowerer::lower_streaming_concat(const StreamingConcatenationExpression& sc,
                                          ir::Ref                                 target_ty,
                                          ir::Builder&                            b,
                                          ir::SourceLoc                           l) {
    if (!sc.isFixedSize()) {
        // The operand width(s) are runtime values (a queue / dynamic array /
        // string operand somewhere in the stream), so the generic stream
        // cannot be built as one compile-time-width concat + resize the way
        // the fixed-size path below does. Share the runtime walk the
        // string/queue/dynamic-array destinations already use below
        // (collect_stream_chunks / walk_stream_granules) and assemble a
        // fixed-width value from it (stream_to_fixed); `with` clauses stay
        // refused inside collect_stream_chunks.
        std::vector<StreamChunk> chunks;
        collect_stream_chunks(sc, chunks, b, l, /*dyn_slice_ok=*/true);
        return stream_to_fixed(chunks, sc, target_ty, b, l);
    }
    for (const auto& stream : sc.streams()) {
        if (stream.withExpr) {
            fail("streaming concatenation `with` clause is not supported");
        }
    }

    // Build the generic stream: concatenate operands left-to-right.
    // A nested streaming concat has void type (slang creates it with getVoidType()),
    // so detect it and supply the concrete target type derived from bitstream width.
    const bool             is_four_outer = module_.pool.layout(target_ty).domain == ir::Domain::Four;
    std::vector<ir::Index> parts;
    for (const auto& stream : sc.streams()) {
        const Expression& operand = *stream.operand;
        if (operand.kind == ExpressionKind::Streaming) {
            const auto&   inner = operand.as<StreamingConcatenationExpression>();
            const ir::Ref inner_ty = make_int_type(static_cast<uint32_t>(inner.getBitstreamWidth()), is_four_outer);
            parts.push_back(lower_streaming_concat(inner, inner_ty, b, l));
        } else if (is_agg_bitstream_operand(operand)) {
            // A whole unpacked array / struct operand serializes to its leaf
            // bit-concatenation (LRM 11.4.14); it has no flat load.
            parts.push_back(
              serialize_agg_lvalue(lower_lvalue(unwrap_conversion(operand), b), lower_type(*operand.type), b, l));
        } else {
            parts.push_back(lower_expr(operand, b));
        }
    }
    ir::Index raw = parts.size() == 1 ? parts[0] : b.concat(parts, l);

    const uint64_t W = sc.getBitstreamWidth();
    const uint64_t S = sc.getSliceSize();

    // sliceSize > 0: '<<' — reverse the order of the stream's S-bit blocks
    // (block i covers bits [min((i+1)*S,W)-1 : i*S]; block 0, the
    // rightmost, lands at the MSB). One StreamReverse node; the backend
    // owns the per-plane block permutation.
    if (S > 0) {
        raw = b.stream_reverse(raw, static_cast<uint32_t>(S), l);
    }

    // Widen if the target is wider than the stream (LRM §11.4.14: zero-fill
    // on the right, i.e. stream occupies the MSBs of the result).
    const uint32_t target_w = module_.pool.layout(target_ty).width;
    if (target_w > static_cast<uint32_t>(W)) {
        const uint32_t         pad      = target_w - static_cast<uint32_t>(W);
        const bool             is_four  = module_.pool.layout(target_ty).domain == ir::Domain::Four;
        const ir::Ref          pad_ty   = make_int_type(pad, is_four);
        const ir::Index        zero_pad = b.constant(module_.pool.put_int(pad_ty, 0), l);
        std::vector<ir::Index> widened  = { raw, zero_pad };
        raw                             = b.concat(widened, l);
    }

    return b.convert(raw, target_ty, false, l);
}

// One element's bit width in a container the stream reads or fills, and the
// guard for an element shape that has no flat width.
uint32_t Lowerer::stream_granule_width(const ir::Type& ct) {
    if (value_cat(ct.element) != ValueCat::scalar || module_.pool.is_float(ct.element)) {
        // TODO: a real / aggregate / nested-container element (LRM 11.4.14
        // allows any bit-stream type). Each needs its own per-element
        // serialization inside the element loop, and a real is not a
        // bit-stream type at all.
        fail("streaming a container whose elements are not integral or string is not supported");
    }
    return module_.pool.layout(ct.element).width;
}

// Flatten `sc` into destination-order chunks.
void Lowerer::collect_stream_chunks(const StreamingConcatenationExpression& sc,
                                    std::vector<StreamChunk>&               out,
                                    ir::Builder&                            b,
                                    ir::SourceLoc                           l,
                                    bool                                    dyn_slice_ok) {
    for (const auto& stream : sc.streams()) {
        if (stream.withExpr) {
            fail("streaming concatenation `with` clause is not supported");
        }
    }
    if (sc.isFixedSize()) {
        bool four = false;
        for (const auto& stream : sc.streams()) {
            four = four || stream.operand->type->isFourState();
        }
        const auto w = static_cast<uint32_t>(sc.getBitstreamWidth());
        out.push_back({ .kind  = StreamChunk::Kind::Fixed,
                        .value = lower_streaming_concat(sc, make_int_type(w, four), b, l),
                        .width = w });
        return;
    }
    if (sc.getSliceSize() > 0 && !dyn_slice_ok) {
        // TODO: `{<<S{...}}` over a dynamically sized stream fed to a
        // string / queue / dynamic-array destination (LRM 11.4.14.2): which
        // block a bit lands in depends on the total width, so the reversal
        // needs the whole stream materialized first, and each of those three
        // destinations below only ever consumes one granule at a time, never
        // holding the whole stream. A fixed-size destination does not have
        // this problem (stream_to_fixed computes each bit's final position
        // directly, with no intermediate buffer) and passes
        // dyn_slice_ok=true, so this guard no longer applies there.
        fail("slice size on a dynamically-sized streaming concatenation is not supported");
    }
    for (const auto& stream : sc.streams()) {
        const Expression& e = *stream.operand;
        if (e.kind == ExpressionKind::Streaming) {
            collect_stream_chunks(e.as<StreamingConcatenationExpression>(), out, b, l);
            continue;
        }
        const Expression& u = unwrap_conversion(e);
        if (is_assoc(u)) {
            // LRM 6.24.1 excludes associative arrays from the bit-stream
            // types, so this is a legality gap, not a size limitation: no
            // sizing/reordering change would ever make this legal.
            fail("streaming an associative-array operand is not supported "
                 "(not a bit-stream type, LRM 6.24.1)");
        }
        if (is_string_expr(u)) {
            out.push_back({ .kind = StreamChunk::Kind::Str, .value = lower_expr(u, b) });
            continue;
        }
        if (is_queue(u) || is_dyn_array(u)) {
            const ir::Ref  cty = lower_type(*u.type);
            const ir::Type ct  = std::get<ir::Type>(module_.pool.get(cty));
            StreamChunk    c{ .kind     = StreamChunk::Kind::Elems,
                              .value    = lower_lvalue(u, b),
                              .ty       = cty,
                              .is_queue = ct.kind == ir::Type::Kind::Queue };
            if (value_cat(ct.element) == ValueCat::string) {
                c.elem_is_str = true;
            } else {
                c.width = stream_granule_width(ct);
            }
            out.push_back(c);
            continue;
        }
        if (is_agg_bitstream_operand(u)) {
            if (u.type->isFixedSize()) {
                // A fixed unpacked aggregate still has one packed serialization.
                out.push_back({ .kind  = StreamChunk::Kind::Fixed,
                                .value = serialize_agg_lvalue(lower_lvalue(u, b), lower_type(*u.type), b, l),
                                .width = static_cast<uint32_t>(u.type->getBitstreamWidth()) });
            } else {
                // A dynamic array / queue / string member somewhere inside:
                // no one packed width, so flatten the aggregate's own leaves
                // into chunks the same way the loop above flattens sc's
                // operands (collect_agg_stream_chunks).
                collect_agg_stream_chunks(lower_lvalue(u, b), lower_type(*u.type), out, b, l);
            }
            continue;
        }
        out.push_back({ .kind  = StreamChunk::Kind::Fixed,
                        .value = lower_expr(u, b),
                        .width = static_cast<uint32_t>(u.type->getBitstreamWidth()) });
    }
}

// The aggregate analogue of collect_stream_chunks's per-operand loop above:
// flatten a dynamically-sized unpacked aggregate lvalue (a struct or fixed
// array with a queue / dynamic array / string member somewhere inside) into
// destination-order chunks, recursing through fixed-size UnpackedArray /
// Struct layers (the only ones that can still contain a dynamic member) and
// pushing one chunk per leaf. Only reached once collect_stream_chunks has
// already established the aggregate is not fixed size, so a leaf's own
// sub-aggregate is never itself dynamic without eventually bottoming out at
// a queue / dynamic array / string leaf.
void Lowerer::collect_agg_stream_chunks(ir::Index                 lv,
                                        ir::Ref                   ty,
                                        std::vector<StreamChunk>& out,
                                        ir::Builder&              b,
                                        ir::SourceLoc             l) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(ty));
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        for (uint32_t k = 0; k < n; ++k) {
            const int32_t coord =
              t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
            const ir::Index idx = b.constant(
              module_.pool.put_int(make_int_type(32, false), static_cast<uint64_t>(static_cast<int64_t>(coord))), l);
            collect_agg_stream_chunks(b.elem_select(lv, idx, ty, l), t.element, out, b, l);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        const auto fields = module_.pool.struct_fields(t.fields_id);
        for (uint32_t i = 0; i < fields.size(); ++i) {
            collect_agg_stream_chunks(b.member_select(lv, i, ty, l), fields[i].type, out, b, l);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Assoc) {
        // TODO: an associative-array member (LRM 6.24.1 excludes associative
        // arrays from the bit-stream types, so this is not a size limitation
        // to lift -- an assoc member is simply never a legal stream leaf).
        fail("streaming an associative-array aggregate member is not supported "
             "(not a bit-stream type, LRM 6.24.1)");
    }
    if (t.kind == ir::Type::Kind::Queue || t.kind == ir::Type::Kind::DynArray) {
        StreamChunk c{ .kind = StreamChunk::Kind::Elems, .value = lv, .ty = ty,
                       .is_queue = t.kind == ir::Type::Kind::Queue };
        if (value_cat(t.element) == ValueCat::string) {
            c.elem_is_str = true;
        } else {
            c.width = stream_granule_width(t);
        }
        out.push_back(c);
        return;
    }
    if (value_cat(ty) == ValueCat::string) {
        out.push_back({ .kind = StreamChunk::Kind::Str, .value = b.unary(ir::Inst::Tag::Load, lv, l) });
        return;
    }
    if (value_cat(ty) != ValueCat::scalar || module_.pool.is_float(ty)) {
        // Mirrors serialize_agg_lvalue's own guard for the fixed-size case.
        fail("streaming a non-integral aggregate leaf is not supported");
    }
    out.push_back({ .kind  = StreamChunk::Kind::Fixed,
                    .value = b.unary(ir::Inst::Tag::Load, lv, l),
                    .width = module_.pool.layout(ty).width });
}

// A string destination takes whole bytes only.
void Lowerer::check_stream_byte_multiple(uint32_t w) {
    if (w % 8 != 0) {
        // TODO: LRM 11.4.14 rounds a string destination up to whole bytes
        // (slang: width = ceil(bits / 8)), so the leftover bits need their pad
        // side pinned down against the LRM text before this can be more than a
        // guess.
        fail("streaming a " + std::to_string(w) +
             "-bit operand into a string is not supported (not a whole number of bytes)");
    }
}

// Build the stream into a string value (LRM 11.4.14): a string destination is
// byte-granular, so each chunk appends a whole number of bytes, most
// significant first. A whole string operand appends in one concat -- which is
// what makes `s = {>>{string_q}}` the join it looks like, at one runtime call
// per element instead of per byte.
ir::Index Lowerer::stream_to_string(std::span<const StreamChunk> chunks, ir::Builder& b, ir::SourceLoc l) {
    using T                = ir::Inst::Tag;
    const TempCell  out    = make_temp_cell(module_.pool.string_type, "stream_str");
    const ir::Index out_lv = out.lv(b, l);
    const ir::Index empty  = b.constant(string_value_ref(""), l);
    b.binary(T::Store, out_lv, empty, l);
    const auto append_str = [&](ir::Index sv) {
        const std::array<ir::Index, 2> parts{ b.unary(T::Load, out.lv(b, l), l), sv };
        const ir::Index                dst    = out.lv(b, l);
        const ir::Index                joined = b.concat(parts, l);
        b.binary(T::Store, dst, joined, l);
    };
    const auto append_packed = [&](ir::Index v, uint32_t w) {
        check_stream_byte_multiple(w);
        b.binary(T::StrAppendPacked, out.lv(b, l), v, l);
    };
    for (const StreamChunk& c : chunks) {
        switch (c.kind) {
            case StreamChunk::Kind::Fixed:
                append_packed(c.value, c.width);
                break;
            case StreamChunk::Kind::Str:
                append_str(c.value);
                break;
            case StreamChunk::Kind::Elems:
                // Checked before the loop is emitted, so the diagnostic names
                // the element width rather than appearing mid-body.
                if (!c.elem_is_str) {
                    check_stream_byte_multiple(c.width);
                }
                emit_stream_elem_loop(c, b, l, [&](ir::Index elem) {
                    const ir::Index v = b.unary(T::Load, elem, l);
                    if (c.elem_is_str) {
                        append_str(v);
                    } else {
                        append_packed(v, c.width);
                    }
                });
                break;
        }
    }
    return b.unary(T::Load, out.lv(b, l), l);
}

// How many `g`-bit granules the stream yields, as a runtime value. Only a
// dynamic array needs it (it is allocated to size, not pushed onto).
ir::Index Lowerer::stream_granule_count(std::span<const StreamChunk> chunks,
                                        uint32_t                     g,
                                        ir::Builder&                 b,
                                        ir::SourceLoc                l) {
    using T           = ir::Inst::Tag;
    const ir::Ref i32 = make_int_type(32, false);
    ir::Index     total{};
    for (const StreamChunk& c : chunks) {
        ir::Index add{};
        if (c.kind == StreamChunk::Kind::Str) {
            add = b.convert(b.unary(T::StrLen, c.value, l), i32, true, l);
        } else if (c.kind == StreamChunk::Kind::Fixed) {
            add = b.constant(module_.pool.put_int(i32, c.width / g), l);
        } else if (c.elem_is_str) {
            // A string per element: only the run can add the lengths up.
            const TempCell  acc      = make_temp_cell(i32, "stream_cnt");
            const ir::Index acc_lv   = acc.lv(b, l);
            const ir::Index acc_init = b.constant(module_.pool.put_int(i32, 0), l);
            b.binary(T::Store, acc_lv, acc_init, l);
            emit_stream_elem_loop(c, b, l, [&](ir::Index elem) {
                const ir::Index len  = b.convert(b.unary(T::StrLen, b.unary(T::Load, elem, l), l), i32, true, l);
                const ir::Index slot = acc.lv(b, l);
                const ir::Index cur  = b.unary(T::Load, acc.lv(b, l), l);
                const ir::Index next = b.binary(T::Add, cur, len, l);
                b.binary(T::Store, slot, next, l);
            });
            add = b.unary(T::Load, acc.lv(b, l), l);
        } else {
            const ir::Index size =
              c.is_queue ? b.queue_size(c.value, c.ty, l) : b.dyn_array_size(c.value, c.ty, l);
            const ir::Index count = b.convert(size, i32, true, l);
            const ir::Index per   = b.constant(module_.pool.put_int(i32, c.width / g), l);
            add                   = b.binary(T::Mul, count, per, l);
        }
        total = total == ir::Index{} ? add : b.binary(T::Add, total, add, l);
    }
    return total == ir::Index{} ? b.constant(module_.pool.put_int(i32, 0), l) : total;
}

// `q = {>>{...}}`: push the stream's granules onto an already-cleared queue
// handle (materialize_queue_rvalue owns the clear, as it does for `{q, e}`).
void Lowerer::stream_fill_queue(ir::Index                    dst,
                                std::span<const StreamChunk> chunks,
                                ir::Ref                      qty,
                                ir::Builder&                 b,
                                ir::SourceLoc                l) {
    const ir::Type qt = std::get<ir::Type>(module_.pool.get(qty));
    const uint32_t g  = stream_granule_width(qt);
    walk_stream_granules(chunks, g, qt.element, b, l, [&](ir::Index v) {
        b.queue_push(dst, v, /*front=*/false, qty, l);
    });
}

// `d = {>>{...}}`: a dynamic array is sized then filled, so the granules are
// counted first and stored by index (no push exists).
void Lowerer::stream_fill_dynarr(ir::Index                    dst,
                                 std::span<const StreamChunk> chunks,
                                 ir::Ref                      aty,
                                 ir::Builder&                 b,
                                 ir::SourceLoc                l) {
    using T           = ir::Inst::Tag;
    const ir::Type at = std::get<ir::Type>(module_.pool.get(aty));
    const uint32_t g  = stream_granule_width(at);
    const ir::Ref  i32 = make_int_type(32, false);
    b.dyn_array_new(dst, stream_granule_count(chunks, g, b, l), dst, /*has_init=*/false, aty, l);
    const TempCell  idx      = make_temp_cell(i32, "stream_o");
    const ir::Index idx_lv   = idx.lv(b, l);
    const ir::Index idx_init = b.constant(module_.pool.put_int(i32, 0), l);
    b.binary(T::Store, idx_lv, idx_init, l);
    walk_stream_granules(chunks, g, at.element, b, l, [&](ir::Index v) {
        const ir::Index k = b.unary(T::Load, idx.lv(b, l), l);
        b.binary(T::Store, b.elem_select(dst, k, aty, l), v, l);
        const ir::Index slot = idx.lv(b, l);
        const ir::Index one  = b.constant(module_.pool.put_int(i32, 1), l);
        const ir::Index next = b.binary(T::Add, k, one, l);
        b.binary(T::Store, slot, next, l);
    });
}

// LRM 11.4.14.2 block reversal, applied to one bit's position in a
// dynamically-sized stream: `p` (the bit's 0-based index in the
// un-reordered `>>`-order generic stream, 0 = MSB) maps to its index after
// reversing `slice`-bit blocks, counted from the stream's right (LSB) end,
// with a possibly-smaller final (leftmost) block (11.4.14.1's "no padding
// or truncation"). Identity when `slice == 0` (`>>`: no reordering).
// `nfull`/`leftover` are `w / slice` / `w % slice`, hoisted by the caller
// since neither depends on `p`.
ir::Index Lowerer::stream_reorder_pos(ir::Index     p,
                                      ir::Index     w,
                                      ir::Index     nfull,
                                      ir::Index     leftover,
                                      uint32_t      slice,
                                      ir::Builder&  b,
                                      ir::SourceLoc l) {
    if (slice == 0) {
        return p;
    }
    using T             = ir::Inst::Tag;
    const ir::Ref   i32 = make_int_type(32, false);
    const ir::Index s   = b.constant(module_.pool.put_int(i32, slice), l);
    const ir::Index one = b.constant(module_.pool.put_int(i32, 1), l);
    // Distance from the stream's right (LSB) end -- blocks are formed
    // starting there (LRM 11.4.14.2) -- then which block `p` falls in and
    // its offset within that block.
    const ir::Index d   = b.binary(T::Sub, b.binary(T::Sub, w, one, l), p, l);
    const ir::Index blk      = b.binary(T::SDiv, d, s, l);
    const ir::Index r        = b.binary(T::SMod, d, s, l);
    const ir::Index blk_size = b.select(b.binary(T::Eq, blk, nfull, l), leftover, s, l);
    const ir::Index offset  = b.binary(T::Sub, b.binary(T::Sub, blk_size, one, l), r, l);
    // Preceding (lower-index) blocks are always full-size `s`, partial or
    // not, since only the highest-index block can be partial.
    return b.binary(T::Add, b.binary(T::Mul, blk, s, l), offset, l);
}

// LRM 11.4.14: "If the target represents a fixed-size variable that is
// narrower [...] than the stream, an error shall be generated." `w` is a
// run-time value here (the stream is dynamically sized), so the check --
// and the report -- happen at run time, the same shape as `mailbox`'s
// negative-bound check elsewhere in this file. Verilator 5.050
// instead truncates silently here (low bits kept, both directions, no
// diagnostic); the LRM's own worked example is unambiguous (`int j =
// {>>{a,b,c}};` with a,b,c a 32-bit `int` each, "error: j is 32 bits < 96
// bits"), so Clotho reports rather than matching Verilator.
void Lowerer::check_stream_fits_fixed(ir::Index                               w,
                                      uint32_t                                tw,
                                      const StreamingConcatenationExpression& sc,
                                      ir::Builder&                            b,
                                      ir::SourceLoc                           l) {
    using T             = ir::Inst::Tag;
    const ir::Ref   i32 = make_int_type(32, false);
    const ir::Index bad = b.binary(T::SGt, w, b.constant(module_.pool.put_int(i32, tw), l), l);
    b.open_region();
    {
        const std::string banner = banner_at(sc.sourceRange.start());
        b.severity(static_cast<uint32_t>(ir::Severity::Fatal), severity_banner(sc.sourceRange.start()),
                   "streaming concatenation source is wider than its fixed-size target (LRM 11.4.14)", {},
                   l);
        b.finish(banner + "stream", 1, l);
    }
    const ir::ExtraSlice then_r = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_r = b.close_region();
    b.cond(bad, then_r, else_r, l, /*unlikely=*/true);
}

// Build a fixed-width value from a dynamically-sized stream (LRM 11.4.14,
// 11.4.14.3): the operand widths are runtime values, so the generic stream
// is walked bit by bit (walk_stream_granules at granule size 1, shared with
// the string/queue/dynamic-array destinations above) rather than
// concatenated at compile time the way the fixed-size lower_streaming_concat
// above does. Each bit's final position folds in both the `<<` block
// reversal (stream_reorder_pos) and the left-align-into-target placement
// (LRM 11.4.14: narrower than a fixed target is padded with zero on the
// right, i.e. the stream occupies the target's MSBs) into one runtime shift
// amount, so no intermediate stream buffer is needed.
ir::Index Lowerer::stream_to_fixed(std::span<const StreamChunk>            chunks,
                                   const StreamingConcatenationExpression& sc,
                                   ir::Ref                                 target_ty,
                                   ir::Builder&                            b,
                                   ir::SourceLoc                           l) {
    using T              = ir::Inst::Tag;
    const ir::Ref  i32   = make_int_type(32, false);
    const uint32_t slice = static_cast<uint32_t>(sc.getSliceSize());
    const uint32_t tw    = module_.pool.layout(target_ty).width;
    const bool     four  = module_.pool.layout(target_ty).domain == ir::Domain::Four;

    const ir::Index w = stream_granule_count(chunks, /*g=*/1, b, l);
    check_stream_fits_fixed(w, tw, sc, b, l);
    ir::Index nfull{};
    ir::Index leftover{};
    if (slice > 0) {
        const ir::Index s = b.constant(module_.pool.put_int(i32, slice), l);
        nfull             = b.binary(T::SDiv, w, s, l);
        leftover          = b.binary(T::SMod, w, s, l);
    }

    // Accumulate in an explicitly UNSIGNED width-`tw` type: `target_ty`
    // itself may be signed (e.g. `int`), and resizing a lone 1 bit
    // straight into a signed destination sign-extends it to all-ones
    // instead of zero-extending it (LRM 11.8.3 extension is by the
    // source's own signedness, and every intermediate value here is
    // conceptually unsigned bits, not a signed number). Match the
    // fixed-size lower_streaming_concat above, which builds its padded
    // `raw` the same way and converts to `target_ty` only in its own
    // final `resize`.
    const ir::Ref   acc_ty   = make_int_type(tw, four);
    const TempCell  acc      = make_temp_cell(acc_ty, "stream_acc");
    const ir::Index acc_lv   = acc.lv(b, l);
    const ir::Index acc_init = b.constant(module_.pool.put_int(acc_ty, 0), l);
    b.binary(T::Store, acc_lv, acc_init, l);
    const TempCell  pos      = make_temp_cell(i32, "stream_p");
    const ir::Index pos_lv   = pos.lv(b, l);
    const ir::Index pos_init = b.constant(module_.pool.put_int(i32, 0), l);
    b.binary(T::Store, pos_lv, pos_init, l);

    const ir::Ref bit_ty = make_int_type(1, four);
    walk_stream_granules(chunks, /*g=*/1, bit_ty, b, l, [&](ir::Index bit_v) {
        const ir::Index p       = b.unary(T::Load, pos.lv(b, l), l);
        const ir::Index new_pos = stream_reorder_pos(p, w, nfull, leftover, slice, b, l);
        const ir::Index shift =
          b.convert(b.binary(T::Sub, b.constant(module_.pool.put_int(i32, tw - 1), l), new_pos, l),
                    make_int_type(32, false),
                    false,
                    l);
        const ir::Index widened = b.convert(bit_v, acc_ty, false, l);
        const ir::Index acc_dst = acc.lv(b, l);
        const ir::Index acc_cur = b.unary(T::Load, acc.lv(b, l), l);
        const ir::Index placed  = b.binary(T::Sll, widened, shift, l);
        const ir::Index merged  = b.binary(T::Bor, acc_cur, placed, l);
        b.binary(T::Store, acc_dst, merged, l);
        const ir::Index pos_dst  = pos.lv(b, l);
        const ir::Index one      = b.constant(module_.pool.put_int(i32, 1), l);
        const ir::Index pos_next = b.binary(T::Add, p, one, l);
        b.binary(T::Store, pos_dst, pos_next, l);
    });
    return b.convert(b.unary(T::Load, acc.lv(b, l), l), target_ty, false, l);
}

ir::Index Lowerer::take_stream_bits(StreamUnpack& u, uint32_t w, ir::Ref ty, ir::Builder& b, ir::SourceLoc l) {
    const uint32_t lsb = u.pos - w;
    ir::Index      v   = u.src;
    if (lsb != 0) {
        const ir::Index sh = b.constant(module_.pool.put_int(make_int_type(32, false), lsb), l);
        v                  = b.binary(ir::Inst::Tag::Srl, u.src, sh, l);
    }
    u.pos = lsb;
    return b.convert(v, ty, false, l);
}

// The granule of the first dynamically sized destination in stream order, if
// there is one: the element width of a queue / dynamic array, 8 for a string.
std::optional<uint32_t> Lowerer::first_dynamic_dest_granule(const StreamingConcatenationExpression& sc) {
    for (const auto& stream : sc.streams()) {
        const Expression& e = *stream.operand;
        if (e.kind == ExpressionKind::Streaming) {
            if (auto g = first_dynamic_dest_granule(e.as<StreamingConcatenationExpression>())) {
                return g;
            }
            continue;
        }
        const Expression& u = unwrap_lvalue_elem(e);
        if (is_string_expr(u)) {
            return 8;
        }
        if (is_queue(u) || is_dyn_array(u)) {
            const ir::Type ct = std::get<ir::Type>(module_.pool.get(lower_type(*u.type)));
            return stream_granule_width(ct);
        }
    }
    return std::nullopt;
}

// Scatter the trimmed source over one destination operand.
void Lowerer::unpack_stream_dest(const Expression&    dest,
                                 StreamUnpack&        u,
                                 ir::Inst::Tag        tag,
                                 ir::Builder&         b,
                                 ir::SourceLoc        l,
                                 const IntraNbTiming* nbt) {
    if (dest.kind == ExpressionKind::Streaming) {
        const auto& inner = dest.as<StreamingConcatenationExpression>();
        if (inner.getSliceSize() > 0) {
            // TODO: a nested `{<<S{...}}` destination reverses only its own
            // slice of the stream, which needs a second trim + reversal.
            fail("nested slice size in a streaming assignment target is not supported");
        }
        for (const auto& stream : inner.streams()) {
            unpack_stream_dest(*stream.operand, u, tag, b, l, nbt);
        }
        return;
    }
    const Expression& d = unwrap_lvalue_elem(dest);
    if (is_string_expr(d) || is_queue(d) || is_dyn_array(d)) {
        if (nbt != nullptr) {
            // TODO: a string / queue / dynamic-array operand of an intra-timed
            // streaming target. The greedy destination is rebuilt in place from
            // the trimmed stream (it resizes a container, writes it, then fills
            // it), which is not one deferrable leaf store; it needs the deferred
            // record's owned-handle tail class
            // (docs/design/intra-assignment-timing.md, R3).
            fail("intra-assignment timing on a streaming target with a string, "
                 "queue or dynamic-array operand is not supported");
        }
        const uint32_t bits = u.dyn_taken ? 0 : u.dyn_bits;
        u.dyn_taken         = true;
        unpack_stream_dynamic_dest(d, bits, u, b, l);
        return;
    }
    if (is_agg_bitstream_operand(d)) {
        unpack_stream_agg(lower_lvalue(d, b, /*is_write=*/true), lower_type(*d.type), u, tag, b, l, nbt);
        return;
    }
    const ir::Ref   ty  = lower_type(*d.type);
    const ir::Index val = take_stream_bits(u, module_.pool.layout(ty).width, ty, b, l);
    // Same per-operand force gating as an lvalue concatenation: a forced
    // destination keeps its value while its siblings still take their slice.
    store_or_gate(pca_info_of_lvalue(d), tag, lower_lvalue(d, b, /*is_write=*/true), val, b, l, nbt);
}

// A fixed unpacked array / struct destination: the exact inverse of
// serialize_agg_lvalue, walking leaves in declared order.
void Lowerer::unpack_stream_agg(ir::Index            lv,
                                ir::Ref              ty,
                                StreamUnpack&        u,
                                ir::Inst::Tag        tag,
                                ir::Builder&         b,
                                ir::SourceLoc        l,
                                const IntraNbTiming* nbt) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(ty));
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        for (uint32_t k = 0; k < n; ++k) {
            const int32_t coord =
              t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
            const ir::Index idx = b.constant(
              module_.pool.put_int(make_int_type(32, false), static_cast<uint64_t>(static_cast<int64_t>(coord))), l);
            unpack_stream_agg(b.elem_select(lv, idx, ty, l), t.element, u, tag, b, l, nbt);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        const auto fields = module_.pool.struct_fields(t.fields_id);
        for (uint32_t i = 0; i < fields.size(); ++i) {
            unpack_stream_agg(b.member_select(lv, i, ty, l), fields[i].type, u, tag, b, l, nbt);
        }
        return;
    }
    if (value_cat(ty) != ValueCat::scalar || module_.pool.is_float(ty)) {
        // TODO: a string / real / handle leaf inside an unpacked destination,
        // the mirror of serialize_agg_lvalue's own guard.
        fail("unpacking a stream into a non-integral aggregate leaf is not supported");
    }
    store_leaf(tag, nbt, lv, take_stream_bits(u, module_.pool.layout(ty).width, ty, b, l), b, l);
}

// The greedy destination: it takes every bit the fixed-size destinations left
// over (LRM 11.4.14.3), and a second one takes none.
void Lowerer::unpack_stream_dynamic_dest(const Expression& dest,
                                         uint32_t          bits,
                                         StreamUnpack&     u,
                                         ir::Builder&      b,
                                         ir::SourceLoc     l) {
    using T = ir::Inst::Tag;
    if (is_string_expr(dest)) {
        const ir::Index lv = lower_lvalue(dest, b, /*is_write=*/true);
        b.binary(T::Store, lv, b.constant(string_value_ref(""), l), l);
        if (bits == 0) {
            return;
        }
        check_stream_byte_multiple(bits);
        // One call: the whole run of bytes is contiguous in the source.
        b.binary(T::StrAppendPacked, lv, take_stream_bits(u, bits, make_int_type(bits, false), b, l), l);
        return;
    }
    const ir::Ref   cty = lower_type(*dest.type);
    const ir::Type  ct  = std::get<ir::Type>(module_.pool.get(cty));
    const uint32_t  g   = stream_granule_width(ct);
    const ir::Index dst = lower_lvalue(dest, b, /*is_write=*/true);
    const uint32_t  n   = bits / g;
    if (ct.kind == ir::Type::Kind::Queue) {
        b.queue_delete(dst, dst, /*has_idx=*/false, cty, l);
        for (uint32_t k = 0; k < n; ++k) {
            b.queue_push(dst, take_stream_bits(u, g, ct.element, b, l), /*front=*/false, cty, l);
        }
        return;
    }
    const ir::Ref i32 = make_int_type(32, false);
    b.dyn_array_new(dst, b.constant(module_.pool.put_int(i32, n), l), dst, /*has_init=*/false, cty, l);
    for (uint32_t k = 0; k < n; ++k) {
        const ir::Index idx   = b.constant(module_.pool.put_int(i32, k), l);
        const ir::Index slot  = b.elem_select(dst, idx, cty, l);
        const ir::Index chunk = take_stream_bits(u, g, ct.element, b, l);
        b.binary(T::Store, slot, chunk, l);
    }
}

// `{>>{...}} = rhs` / `{<<S{...}} = rhs`.
void Lowerer::store_stream_target(const StreamingConcatenationExpression& sc,
                                  const Expression&                       rhs_expr,
                                  ir::Index                               rhs,
                                  ir::Inst::Tag                           tag,
                                  ir::Builder&                            b,
                                  ir::SourceLoc                           l,
                                  const IntraNbTiming*                    nbt) {
    for (const auto& stream : sc.streams()) {
        if (stream.withExpr) {
            fail("streaming concatenation `with` clause is not supported");
        }
    }
    if (rhs_expr.kind == ExpressionKind::Streaming || !rhs_expr.type->isFixedSize() ||
        !rhs_expr.type->isBitstreamType()) {
        // TODO: a dynamically sized source (a queue / string / another stream on
        // the right). The bit count is then a runtime value, so the destination
        // widths cannot be resolved at lower time the way they are below.
        fail("streaming assignment from a dynamically-sized source is not supported");
    }
    const auto src_w = static_cast<uint32_t>(rhs_expr.type->getBitstreamWidth());
    const auto tgt_w = static_cast<uint32_t>(sc.getBitstreamWidth());
    uint32_t   dyn   = 0;
    if (const auto g = first_dynamic_dest_granule(sc)) {
        // Round down: the leftover must divide into whole elements, and the
        // remainder stays unconsumed (slang does the same, `do not exceed
        // srcSize`).
        dyn = src_w > tgt_w ? ((src_w - tgt_w) / *g) * *g : 0;
    }
    const uint32_t used = tgt_w + dyn;
    if (used > src_w) {
        // slang rejects this when both sides are fixed (diag::BadStreamSize);
        // reachable only if a destination width and the source disagree here.
        fail("streaming assignment source is narrower than its target");
    }
    // Take the leftmost `used` bits: the low src_w - used are discarded.
    StreamUnpack u{ .src = rhs, .pos = used, .dyn_bits = dyn };
    if (src_w > used) {
        const ir::Index sh = b.constant(module_.pool.put_int(make_int_type(32, false), src_w - used), l);
        u.src = b.binary(ir::Inst::Tag::Srl, rhs, sh, l);
    }
    u.src = b.convert(u.src, make_int_type(used, rhs_expr.type->isFourState()), false, l);
    if (const uint64_t s = sc.getSliceSize(); s > 0) {
        if (used % s != 0) {
            // TODO: `{<<S{...}}` where S does not divide the stream. slang's
            // unpack reorder puts the partial block FIRST (Bitstream::reOrder
            // with a non-zero unpackWidth), the opposite end from
            // Tag::StreamReverse's pack-direction slicing.
            fail("slice size that does not divide a streaming assignment target is not supported");
        }
        u.src = b.stream_reverse(u.src, static_cast<uint32_t>(s), l);
    }
    for (const auto& stream : sc.streams()) {
        unpack_stream_dest(*stream.operand, u, tag, b, l, nbt);
    }
}

} // namespace clotho::lower
