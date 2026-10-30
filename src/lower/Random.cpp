// Randomization: randomize(), rand_mode, randcase and randsequence.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Admit one `rand` / `randc` property (LRM 18.3) and describe it for the
// generated body. `field` is its body field index, `ordinal` its bit in the mask.
// slang has already rejected the types that are illegal for its mode
// (Type::isValidForRand); what is refused here is the legal-but-unimplemented
// remainder, each loud per PRINCIPLES.md.
Lowerer::RandProp Lowerer::admit_rand_prop(const slang::ast::ClassPropertySymbol& prop,
                                           uint32_t                               field,
                                           uint32_t                               ordinal) {
    RandProp                p{ .sym = &prop, .field = field, .ordinal = ordinal };
    const slang::ast::Type& ty = prop.getType().getCanonicalType();
    if (ordinal >= 64) {
        // TODO: more than 64 rand properties in one chain — the rand_mode mask is
        // one word (LRM 18.8 puts no bound on the count).
        fail("more than 64 rand / randc properties in one class is not supported");
    }
    rand_ordinal_[&prop] = ordinal;
    if (prop.randMode == slang::ast::RandMode::RandC) {
        if (!ty.isIntegral()) {
            fail("randc is supported on an integral property only");
        }
        const uint32_t w = ty.getBitWidth();
        if (w > 8) {
            // LRM 18.6 lets an implementation cap randc and requires only 8 bits;
            // the cycle history is a 256-bit bitmap in the object.
            fail("randc wider than 8 bits is not supported");
        }
        p.cyclic     = true;
        p.randc_vals = 1u << w;
        return p;
    }
    return p;
}

// Why step 1 cannot draw a value for a `rand` property of this type, or null
// when it can. Every named shape is legal SystemVerilog and a TODO, not a
// limitation: `rand real` (LRM 1800-2023 18.3) has no uniform distribution over
// its range to draw from; a rand dynamic array / queue / associative array /
// unpacked struct (LRM 18.4) needs an element walk the fixed-array case does
// statically. A class-property caller records the reason in
// ClassRand::unsupported, which turns the class's generated $randomize body
// into a run-time fatal; std::randomize (a direct call site) fails at compile
// time.
const char* Lowerer::rand_type_unsupported(const slang::ast::Type& ty) {
    const slang::ast::Type* ct = &ty.getCanonicalType();
    while (const auto* arr = ct->as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
        ct = &arr->elementType.getCanonicalType();
    }
    if (ct->isIntegral() || ct->isClass()) {
        return nullptr;
    }
    if (ct->isFloating()) {
        return "rand real properties are not supported";
    }
    return "rand properties of this type are not supported";
}

// The type of one 256-bit RNG / cyclic-history slot: four 64-bit words, laid out
// as an unpacked array so the storage is addressable as a `uint64_t[4]` — the
// shape rt_urandom_seed and the inlined xoshiro256++ step already take.
ir::Ref Lowerer::rng_state_type() {
    ir::Type t;
    t.kind    = ir::Type::Kind::UnpackedArray;
    t.element = make_int_type(64, false);
    t.left    = 3;
    t.right   = 0;
    return module_.pool.put(ir::Item{ t });
}

// True if `clt`'s own member list carries a `constraint` block (LRM 18.5).
// A constraint is declarative and step 1 solves nothing, so a chain carrying one
// must never be silently randomized as if unconstrained -- the caller records it
// in ClassRand::unsupported and the generated $randomize body refuses at run
// time. Enumerating them needs slang's own walk: an inherited member is wrapped
// in a TransparentMemberSymbol and membersOfType does not unwrap, so the chain
// is walked by the caller and only this class's own members are examined here.
//
// TODO: constraint blocks (LRM 18.5) — step 2, the solver.
bool Lowerer::has_constraint_blocks(const slang::ast::ClassType& clt) {
    for (const slang::ast::Symbol& member : clt.members()) {
        const slang::ast::Symbol* sym = &member;
        while (sym->kind == SymbolKind::TransparentMember) {
            sym = &sym->as<slang::ast::TransparentMemberSymbol>().wrapped;
        }
        if (sym->kind == SymbolKind::ConstraintBlock) {
            return true;
        }
    }
    return false;
}

// randcase (LRM 18.16): take one branch at random, weighted. Every weight is
// evaluated exactly once and the sum is formed with unsigned addition at the
// widest weight's precision -- 18.16 makes each summand unsigned, so a signed
// weight is reinterpreted rather than sign-extended, and a sum that does not
// fit that precision wraps. One $urandom_range draw in [0, sum-1] then picks
// the first branch whose cumulative weight passes it, so a zero-weight branch
// is never taken and an all-zero sum takes no branch at all (18.16 leaves the
// warning for that case optional; Clotho does not emit one).
void Lowerer::lower_randcase(const slang::ast::RandCaseStatement& rc, ir::Builder& b) {
    const ir::SourceLoc l = loc(rc);
    if (rc.items.empty()) {
        return;
    }

    uint32_t width = 1;
    for (const slang::ast::RandCaseStatement::Item& item : rc.items) {
        width = std::max(width, item.expr->type->getBitWidth());
    }
    if (width > 32) {
        // TODO: a wider sum needs the LRM's several-draw scheme, stitching
        // multiple $urandom_range results into one number (LRM 18.16).
        fail("randcase with a weight wider than 32 bits is not supported");
    }

    // The weight domain is 2-state: an x/z weight bit reads as 0, which is how
    // $urandom_range already treats its own operands (18.16 does not define a
    // 4-state weight).
    const ir::Ref          wty = make_int_type(width, false);
    std::vector<ir::Index> weights;
    weights.reserve(rc.items.size());
    for (const slang::ast::RandCaseStatement::Item& item : rc.items) {
        weights.push_back(b.convert(lower_expr(*item.expr, b), wty, false, loc(*item.expr)));
    }

    ir::Index sum = weights[0];
    for (size_t k = 1; k < weights.size(); k++) {
        sum = b.binary(ir::Inst::Tag::Add, sum, weights[k], l);
    }

    // The draw is guarded, not clamped: with a zero sum there is no branch to
    // take, and $urandom_range(-1, 0) would otherwise draw over the full range
    // (and consume a value from the process stream).
    const ir::Ref   u32  = make_int_type(32, false);
    const ir::Index zero = b.constant(module_.pool.put_int(wty, 0), l);
    b.open_region();
    const ir::Index wide = b.convert(sum, u32, false, l);
    const ir::Index one  = b.constant(module_.pool.put_int(u32, 1), l);
    const ir::Index top  = b.binary(ir::Inst::Tag::Sub, wide, one, l);
    const ir::Index low  = b.constant(module_.pool.put_int(u32, 0), l);
    const ir::Index draw = b.urandom_range(top, low, l);
    lower_randcase_arm(rc, 0, zero, draw, weights, u32, b);
    const ir::ExtraSlice pick_region = b.close_region();
    b.open_region();
    const ir::ExtraSlice none_region = b.close_region();
    b.cond(b.binary(ir::Inst::Tag::Ne, sum, zero, l), pick_region, none_region, l);
}

// One arm of the randcase chain: item k runs when the draw is below its
// cumulative weight and no earlier arm claimed it. The cumulative sums carry
// the same wrapping unsigned precision as the total and are widened to 32 bits
// only to meet the draw, which is below the total -- so the terminal else is
// unreachable for a nonzero total.
void Lowerer::lower_randcase_arm(const slang::ast::RandCaseStatement& rc,
                                 size_t                               k,
                                 ir::Index                            acc,
                                 ir::Index                            draw,
                                 const std::vector<ir::Index>&        weights,
                                 ir::Ref                              u32,
                                 ir::Builder&                         b) {
    if (k == weights.size()) {
        return;
    }
    const ir::SourceLoc l    = loc(*rc.items[k].stmt);
    const ir::Index     next = b.binary(ir::Inst::Tag::Add, acc, weights[k], l);
    const ir::Index     pred = b.binary(ir::Inst::Tag::ULt, draw, b.convert(next, u32, false, l), l);

    b.open_region();
    lower_stmt(*rc.items[k].stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    lower_randcase_arm(rc, k + 1, next, draw, weights, u32, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, l);
}

void Lowerer::lower_randsequence(const RandSequenceStatement& rs, ir::Builder& b) {
    if (rs.firstProduction == nullptr) {
        // An empty randsequence, or a top-production lookup slang already
        // diagnosed. Nothing to generate either way.
        return;
    }
    const RandSeqCtx&   ctx = randseq_block(rs);
    const ir::SourceLoc l   = loc(rs);
    const auto          it  = ctx.ids.find(rs.firstProduction);
    if (it == ctx.ids.end()) {
        fail("internal: randsequence top production has no lowered body");
    }
    // The entry call. slang has already required the top production to take no
    // arguments (RandSequenceStatement::fromSyntax), and its result -- if it
    // declares one -- is discarded, so both output slots here are write-only
    // temps: there is no frame above to unwind to, and a `break` reaching this
    // point has already ended the block.
    std::vector<ir::Index> args;
    if (ctx.this_var != nullptr) {
        args.push_back(this_handle(b, l));
    }
    args.push_back(abort_lvalue(b, l));
    if (!rs.firstProduction->getReturnType().isVoid()) {
        args.push_back(make_arg_temp(lower_type(rs.firstProduction->getReturnType()), b, l));
    }
    push_capture_args(ctx, args, b, l);
    if (ctx.is_task) {
        b.task_call(it->second, args, l);
    } else {
        b.call(it->second, args, l);
    }
}

// Declare the trailing capture formals of one production entry point, in the
// single order every entry point of the block agrees on.
void Lowerer::declare_capture_formals(const RandSeqCtx&        rs,
                                      FnCtx&                   ctx,
                                      std::vector<ir::Ref>&    param_types,
                                      std::vector<ir::ArgDir>& arg_dirs) {
    for (const RandSeqCtx::Capture& cap : rs.captures) {
        if (cap.boxed) {
            // box_class_for, not box_class_type: the enclosing frame may have
            // minted the NOTIFIED variant for a symbol some `wait` reads, and a
            // store resolving the other (class, field) pair would fire nothing.
            const ir::Ref cls = box_class_for(*cap.sym, lower_type(cap.sym->getType()));
            param_types.push_back(cls);
            arg_dirs.push_back(ir::ArgDir::In);
            const uint32_t k   = fn_local(*cap.sym);
            ctx.local_types[k] = cls;
            ctx.boxed[cap.sym] = { k, cls };
            continue;
        }
        param_types.push_back(lower_type(cap.sym->getType()));
        arg_dirs.push_back(ir::ArgDir::Ref);
        fn_local(*cap.sym);
    }
}

// The matching actuals, resolved in whichever body is making the call: the
// enclosing procedural body at the randsequence statement, or a production's own
// frame when one production calls another. value_lvalue and capture_handle both
// route through the callee-side registrations above, so a nested call re-passes
// exactly what it was given.
void Lowerer::push_capture_args(const RandSeqCtx& rs, std::vector<ir::Index>& args, ir::Builder& b, ir::SourceLoc l) {
    for (const RandSeqCtx::Capture& cap : rs.captures) {
        args.push_back(cap.boxed ? capture_handle(*cap.sym, b, l) : value_lvalue(*cap.sym, b, l));
    }
}

// Build, once per randsequence statement, the function set it lowers to. Ids
// are reserved for the whole set before any body is built, so a production may
// call itself or one declared later (slang resolves production names with
// LookupFlags::AllowDeclaredAfter).
//
// Keyed on the statement alone: the production functions take everything they
// read as formals, so they do not depend on which enclosing body is lowering
// the block -- only the call-site actuals do.
const Lowerer::RandSeqCtx& Lowerer::randseq_block(const RandSequenceStatement& rs) {
    if (const auto it = randseq_.find(&rs); it != randseq_.end()) {
        return it->second;
    }
    RandSeqCtx ctx;
    // A production body is a coroutine exactly when the randsequence sits in
    // one, because a code block may consume time only there (LRM 13.4 forbids
    // it in a function, and the function body's own timing guards report it).
    ctx.is_task = fn_ctx_ == nullptr || fn_ctx_->is_task;

    if (fn_ctx_ != nullptr) {
        // Everything the block reads out of the ENCLOSING frame has to travel:
        // a production is its own function with its own frame. Same detector,
        // and the same first-reference formal order, as a fork child's captures.
        ForkCaptureDetector d;
        d.locals = &fn_ctx_->localmap;
        ankerl::unordered_dense::set<const ValueSymbol*> boxed_syms;
        for (const auto& [sym, bx] : fn_ctx_->boxed) {
            boxed_syms.insert(sym);
        }
        d.boxed    = &boxed_syms;
        d.this_var = fn_ctx_->this_var;
        visit_randseq(rs, d);
        for (const ValueSymbol* sym : d.captured) {
            if (sym == fn_ctx_->this_var) {
                // A handle carries its own reference semantics, so `this` goes by
                // value as formal 0, exactly as splice_this passes it to a method.
                ctx.this_var   = sym;
                ctx.this_class = fn_ctx_->this_class;
                continue;
            }
            // A variable a DETACHING fork also captured lives in a GC box rather
            // than in the frame (docs/design/fork-join.md V4), so there is no
            // frame slot to alias. Its box HANDLE travels by value instead --
            // reference semantics come from the box, and a first-class handle is
            // a GC root, where a `ref` into the box's body would be a bare
            // interior pointer held across a suspend (a production is a task) and
            // would break the moment evacuation is switched on
            // (docs/design/garbage-collection.md §8).
            ctx.captures.push_back({ sym, fn_ctx_->boxed.contains(sym) });
        }
    }

    for (const RsProd* p : rs.productions) {
        ctx.ids[p] = module_.reserve_function(module_.pool.void_type);
    }
    // A `rand join` operand needs its own plan / step pair. Collected across
    // the whole block first, because the operand may be declared after the rule
    // that names it.
    for (const RsProd* p : rs.productions) {
        for (const RsProd::Rule& rule : p->getRules()) {
            if (!rule.isRandJoin) {
                continue;
            }
            for (const RsProdBase* prod : rule.prods) {
                // The parser guarantees every rand join operand is a plain
                // production reference (RandJoinProdItem).
                const RsProd* target = prod->kind == RsProdKind::Item ? prod->as<RsProdItem>().target : nullptr;
                if (target == nullptr || ctx.steps.contains(target)) {
                    continue;
                }
                check_rand_join_operand(*target);
                ctx.steps[target] = { module_.reserve_function(module_.pool.void_type),
                                      module_.reserve_function(module_.pool.void_type) };
            }
        }
    }
    const RandSeqCtx& stored = randseq_.emplace(&rs, std::move(ctx)).first->second;

    // Every reserved slot must be filled even when a body fails, or the
    // backend's unconditional declare_function pass reads an unset one
    // (build_fork_child documents the same hazard). Each builder completes its
    // own slot before rethrowing; this loop keeps going so the SIBLINGS' slots
    // are completed too, and reports the first failure.
    std::optional<LowerError> failure;
    const auto                build = [&](auto&& fn) {
        try {
            fn();
        } catch (const LowerError& e) {
            if (!failure) {
                failure = e;
            }
        }
    };
    for (const RsProd* p : rs.productions) {
        build([&] { build_production(*p, stored, stored.ids.at(p)); });
        if (const auto it = stored.steps.find(p); it != stored.steps.end()) {
            build([&] { build_production_plan(*p, stored, it->second.plan); });
            build([&] { build_production_step(*p, stored, it->second.step); });
        }
    }
    if (failure) {
        throw *failure;
    }
    return stored;
}

// The one thing an interleaved production may not be (LRM 18.17.5).
//
// Everything else a production holds across its units -- its declared formals
// and its rules' implicit result variables -- is hoisted into the driver's frame
// and passed back in by reference (bind_rand_join_formal, rand_join_rule_vars),
// because `step` is a function of its own and is entered once per unit. An inner
// `rand join` cannot be hoisted the same way: its state is not a fixed set of
// variables but an interleaving cursor per inner operand, chosen at ITS plan
// time, so the outer `step` would have to be a resumable driver rather than an
// indexed one.
//
// Deliberately deferred rather than unnoticed: 18.17.5 says the generator
// "interleaves nonterminal symbols to depth of 1", which does not describe
// nesting at all; verilator ignores the bias expression entirely and cannot
// compile a recursive production.
void Lowerer::check_rand_join_operand(const RsProd& p) {
    for (const RsProd::Rule& rule : p.getRules()) {
        if (rule.isRandJoin) {
            // TODO: an operand of a `rand join` that is itself a `rand join`.
            fail("a randsequence rand join operand that is itself a rand join is "
                 "not supported");
        }
    }
}

// How many depth-1 units one rule contributes to an interleaving: its prods,
// plus the weight clause's trailing code block when it has one.
uint32_t Lowerer::rand_join_units(const RsProd::Rule& rule) {
    return static_cast<uint32_t>(rule.prods.size()) + (rule.codeBlock ? 1u : 0u);
}

void Lowerer::build_production(const RsProd& p, const RandSeqCtx& rs, ir::FunctionId id) {
    const ir::SourceLoc l    = to_source_loc(p.location);
    const ir::Ref       bit1 = make_int_type(1, false);
    const Type&         rty  = p.getReturnType();

    FnCtx ctx;
    ctx.name_prefix = "randseq." + std::string(p.name) + ".";
    ctx.is_task     = rs.is_task;
    ctx.in_randseq  = true;
    // As build_function / build_task: an enclosing construction's or default
    // argument's `this` is another builder's Index and stops at this boundary.
    const Restore<std::optional<CtorThis>> no_ctor_this(ctor_this_, std::nullopt);
    const Restore<std::optional<CtorThis>> no_dflt_this(default_arg_this_, std::nullopt);
    const FnScope                          fn_scope(*this, &ctx);
    ir::Builder                            b(module_);
    std::vector<ir::Ref>                   param_types;
    std::vector<ir::ArgDir>                arg_dirs;

    // Formal order -- and therefore frame Local order, which the backend's
    // prologue relies on: `this`, the abort flag, the result slot, the declared
    // formals, the captures. Every production of one block agrees on it.
    if (rs.this_var != nullptr) {
        param_types.push_back(rs.this_class);
        arg_dirs.push_back(ir::ArgDir::In);
        ctx.this_local = fn_local(*rs.this_var);
        ctx.has_this   = true;
        ctx.this_class = rs.this_class;
        ctx.this_var   = rs.this_var;
    }
    param_types.push_back(bit1);
    arg_dirs.push_back(ir::ArgDir::Out);
    ctx.randseq_abort = anon_local(bit1);
    if (!rty.isVoid()) {
        const ir::Ref rref = lower_type(rty);
        param_types.push_back(rref);
        arg_dirs.push_back(ir::ArgDir::Out);
        ctx.randseq_result     = anon_local(rref);
        ctx.randseq_result_ty  = rref;
        ctx.randseq_has_result = true;
    }
    for (const FormalArgumentSymbol* fa : p.arguments) {
        param_types.push_back(lower_type(fa->getType()));
        arg_dirs.push_back(to_arg_dir(fa->direction));
        fn_local(*fa);
    }
    declare_capture_formals(rs, ctx, param_types, arg_dirs);

    const ir::Builder::Mark   body_mark = b.mark();
    std::optional<LowerError> failure;
    try {
        // An output formal is not seeded from the caller, so the flag starts at
        // the frame's X fill. Clear it before any call can read it back.
        const ir::Index dst  = b.local(ctx.randseq_abort, bit1, l);
        const ir::Index zero = b.constant(module_.pool.put_int(bit1, 0), l);
        b.binary(ir::Inst::Tag::Store, dst, zero, l);
        gen_production(p, rs, b);
    } catch (const LowerError& e) {
        b.rollback(body_mark);
        failure = e;
    }
    b.ret_void(l);
    module_.set_function(id,
                         ir::Function{ .name        = "randseq." + std::string(p.name),
                                       .return_type = module_.pool.void_type,
                                       .param_types = std::move(param_types),
                                       .arg_dirs    = std::move(arg_dirs),
                                       .local_types = std::move(ctx.local_types),
                                       .is_task     = rs.is_task,
                                       .ir          = b.finish() });
    if (failure) {
        throw *failure;
    }
}

// The `plan` entry point of a `rand join` operand (LRM 18.17.5): perform the
// production's rule draw and report which rule was chosen and how many depth-1
// units it contributes. Everything the weights may read -- `this`, the declared
// formals, the enclosing frame captures -- is passed exactly as to the
// production itself, so a weight tracks design state here as it does there.
//
//   randseq.<name>.plan( <this>, output int $rule, output int $len,
//                        <declared formals>, <captures...> )
//
// `$rule` is -1 when the weights summed to zero, which generates nothing.
void Lowerer::build_production_plan(const RsProd& p, const RandSeqCtx& rs, ir::FunctionId id) {
    const ir::SourceLoc l   = to_source_loc(p.location);
    const ir::Ref       i32 = make_int_type(32, false);

    FnCtx ctx;
    ctx.name_prefix = "randseq." + std::string(p.name) + ".plan.";
    ctx.is_task     = rs.is_task;
    ctx.in_randseq  = true;
    const Restore<std::optional<CtorThis>> no_ctor_this(ctor_this_, std::nullopt);
    const Restore<std::optional<CtorThis>> no_dflt_this(default_arg_this_, std::nullopt);
    const FnScope                          fn_scope(*this, &ctx);
    ir::Builder                            b(module_);
    std::vector<ir::Ref>                   param_types;
    std::vector<ir::ArgDir>                arg_dirs;

    if (rs.this_var != nullptr) {
        param_types.push_back(rs.this_class);
        arg_dirs.push_back(ir::ArgDir::In);
        ctx.this_local = fn_local(*rs.this_var);
        ctx.has_this   = true;
        ctx.this_class = rs.this_class;
        ctx.this_var   = rs.this_var;
    }
    param_types.push_back(i32);
    arg_dirs.push_back(ir::ArgDir::Out);
    const uint32_t rule_out = anon_local(i32);
    param_types.push_back(i32);
    arg_dirs.push_back(ir::ArgDir::Out);
    const uint32_t len_out = anon_local(i32);
    // A plan body raises no abort of its own -- a weight expression is an
    // ordinary expression -- but in_randseq is set for the shared helpers, so
    // give the flag a slot nothing writes.
    ctx.randseq_abort = anon_local(make_int_type(1, false));
    // As `step`: a reference to the driver's activation cell, so `plan`'s weight
    // expressions read exactly what the units will.
    for (const FormalArgumentSymbol* fa : p.arguments) {
        param_types.push_back(lower_type(fa->getType()));
        arg_dirs.push_back(ir::ArgDir::Ref);
        fn_local(*fa);
    }
    declare_capture_formals(rs, ctx, param_types, arg_dirs);

    const ir::Builder::Mark   body_mark = b.mark();
    std::optional<LowerError> failure;
    try {
        const auto store_plan = [&](int64_t rule_idx, uint32_t units) {
            const ir::Index rule_lv  = b.local(rule_out, i32, l);
            const ir::Index rule_val = b.constant(module_.pool.put_int(i32, rule_idx), l);
            b.binary(ir::Inst::Tag::Store, rule_lv, rule_val, l);
            const ir::Index len_lv  = b.local(len_out, i32, l);
            const ir::Index len_val = b.constant(module_.pool.put_int(i32, units), l);
            b.binary(ir::Inst::Tag::Store, len_lv, len_val, l);
        };
        store_plan(-1, 0);
        const std::span<const RsProd::Rule> rules = p.getRules();
        if (rules.size() == 1) {
            store_plan(0, rand_join_units(rules[0]));
        } else if (rules.size() > 1) {
            gen_weighted_rules(p, rules, b, [&](size_t k, ir::Builder& bb) {
                (void)bb;
                store_plan(static_cast<int64_t>(k), rand_join_units(rules[k]));
            });
        }
    } catch (const LowerError& e) {
        b.rollback(body_mark);
        failure = e;
    }
    b.ret_void(l);
    module_.set_function(id,
                         ir::Function{ .name        = "randseq." + std::string(p.name) + ".plan",
                                       .return_type = module_.pool.void_type,
                                       .param_types = std::move(param_types),
                                       .arg_dirs    = std::move(arg_dirs),
                                       .local_types = std::move(ctx.local_types),
                                       .is_task     = rs.is_task,
                                       .ir          = b.finish() });
    if (failure) {
        throw *failure;
    }
}

// The `step` entry point of a `rand join` operand: generate unit `$pos` of rule
// `$rule`, one depth-1 item of the sequence the driver is interleaving.
//
//   randseq.<name>.step( <this>, output bit $abort, <output T $result>,
//                        input int $rule, input int $pos, output bit $stop,
//                        <declared formals>, <captures...> )
void Lowerer::build_production_step(const RsProd& p, const RandSeqCtx& rs, ir::FunctionId id) {
    const ir::SourceLoc l    = to_source_loc(p.location);
    const ir::Ref       bit1 = make_int_type(1, false);
    const ir::Ref       i32  = make_int_type(32, false);
    const Type&         rty  = p.getReturnType();

    FnCtx ctx;
    ctx.name_prefix = "randseq." + std::string(p.name) + ".step.";
    ctx.is_task     = rs.is_task;
    ctx.in_randseq  = true;
    const Restore<std::optional<CtorThis>> no_ctor_this(ctor_this_, std::nullopt);
    const Restore<std::optional<CtorThis>> no_dflt_this(default_arg_this_, std::nullopt);
    const FnScope                          fn_scope(*this, &ctx);
    ir::Builder                            b(module_);
    std::vector<ir::Ref>                   param_types;
    std::vector<ir::ArgDir>                arg_dirs;

    if (rs.this_var != nullptr) {
        param_types.push_back(rs.this_class);
        arg_dirs.push_back(ir::ArgDir::In);
        ctx.this_local = fn_local(*rs.this_var);
        ctx.has_this   = true;
        ctx.this_class = rs.this_class;
        ctx.this_var   = rs.this_var;
    }
    param_types.push_back(bit1);
    arg_dirs.push_back(ir::ArgDir::Out);
    ctx.randseq_abort = anon_local(bit1);
    if (!rty.isVoid()) {
        const ir::Ref rref = lower_type(rty);
        // A container result rides the same output formal as any other: the
        // backend passes the handle pointer and deep-copies the callee's frame
        // handle back out.
        param_types.push_back(rref);
        arg_dirs.push_back(ir::ArgDir::Out);
        ctx.randseq_result     = anon_local(rref);
        ctx.randseq_result_ty  = rref;
        ctx.randseq_has_result = true;
    }
    param_types.push_back(i32);
    arg_dirs.push_back(ir::ArgDir::In);
    const uint32_t rule_in = anon_local(i32);
    param_types.push_back(i32);
    arg_dirs.push_back(ir::ArgDir::In);
    const uint32_t pos_in = anon_local(i32);
    param_types.push_back(bit1);
    arg_dirs.push_back(ir::ArgDir::Out);
    ctx.randseq_stop     = anon_local(bit1);
    ctx.randseq_has_stop = true;
    // Every declared formal is a REFERENCE to the driver's activation cell, not
    // a per-call copy: `step` is entered once per unit, so a copy would lose an
    // `input` the body mutated and would repeat an `output` copy-out at every
    // unit. bind_rand_join_formal set the cells up.
    for (const FormalArgumentSymbol* fa : p.arguments) {
        param_types.push_back(lower_type(fa->getType()));
        arg_dirs.push_back(ir::ArgDir::Ref);
        fn_local(*fa);
    }
    // Then the rule variables, in rand_join_rule_vars' order. Registering each
    // symbol against its ref formal is all it takes: value_lvalue resolves a
    // reference to one straight to the formal's slot, so gen_rule's ordinary
    // machinery below reads and writes the driver's cell with no special case.
    std::vector<const VariableSymbol*> rule_var_syms;
    rand_join_rule_vars(p, rule_var_syms);
    for (const VariableSymbol* vs : rule_var_syms) {
        param_types.push_back(lower_type(vs->getType()));
        arg_dirs.push_back(ir::ArgDir::Ref);
        fn_local(*vs);
    }
    declare_capture_formals(rs, ctx, param_types, arg_dirs);

    const ir::Builder::Mark   body_mark = b.mark();
    std::optional<LowerError> failure;
    try {
        const ir::Index zero = b.constant(module_.pool.put_int(bit1, 0), l);
        b.binary(ir::Inst::Tag::Store, b.local(ctx.randseq_abort, bit1, l), zero, l);
        b.binary(ir::Inst::Tag::Store, b.local(ctx.randseq_stop, bit1, l), zero, l);
        const ir::Index rule_val = b.unary(ir::Inst::Tag::Load, b.local(rule_in, i32, l), l);
        const ir::Index pos_lv   = b.local(pos_in, i32, l);
        gen_step_rules(rs, p.getRules(), 0, rule_val, pos_lv, i32, b);
    } catch (const LowerError& e) {
        b.rollback(body_mark);
        failure = e;
    }
    b.ret_void(l);
    module_.set_function(id,
                         ir::Function{ .name        = "randseq." + std::string(p.name) + ".step",
                                       .return_type = module_.pool.void_type,
                                       .param_types = std::move(param_types),
                                       .arg_dirs    = std::move(arg_dirs),
                                       .local_types = std::move(ctx.local_types),
                                       .is_task     = rs.is_task,
                                       .ir          = b.finish() });
    if (failure) {
        throw *failure;
    }
}

// `switch ($rule)` as a Cond chain: both indices are small syntactic counts, so
// a chain costs less than a jump table's key packing.
void Lowerer::gen_step_rules(const RandSeqCtx&             rs,
                             std::span<const RsProd::Rule> rules,
                             size_t                        r,
                             ir::Index                     rule_val,
                             ir::Index                     pos_lv,
                             ir::Ref                       i32,
                             ir::Builder&                  b) {
    if (r == rules.size()) {
        return;
    }
    const ir::SourceLoc l = to_source_loc(rules[r].ruleBlock->location);
    const ir::Index     pred =
      b.binary(ir::Inst::Tag::Eq, rule_val, b.constant(module_.pool.put_int(i32, static_cast<int64_t>(r)), l), l);
    b.open_region();
    gen_step_units(rs, rules[r], 0, pos_lv, i32, b);
    const ir::ExtraSlice then_region = b.close_region();
    b.open_region();
    gen_step_rules(rs, rules, r + 1, rule_val, pos_lv, i32, b);
    const ir::ExtraSlice else_region = b.close_region();
    b.cond(pred, then_region, else_region, l);
}

void Lowerer::gen_step_units(const RandSeqCtx&   rs,
                             const RsProd::Rule& rule,
                             uint32_t            u,
                             ir::Index           pos_lv,
                             ir::Ref             i32,
                             ir::Builder&        b) {
    if (u == rand_join_units(rule)) {
        return;
    }
    const ir::SourceLoc l    = to_source_loc(rule.ruleBlock->location);
    const ir::Index     pos  = b.unary(ir::Inst::Tag::Load, pos_lv, l);
    const ir::Index     unit = b.constant(module_.pool.put_int(i32, static_cast<int64_t>(u)), l);
    const ir::Index     pred = b.binary(ir::Inst::Tag::Eq, pos, unit, l);
    b.open_region();
    // The rule's result variables are ref formals over the driver's cells, so
    // they are already declared and already initialized: map the names without
    // giving them storage of their own.
    const RandSeqRuleVars vars = rule_vars(rule, /*declare=*/false, b);
    if (u < rule.prods.size()) {
        gen_prod(*rule.prods[u], rs, vars, b);
    } else {
        gen_code_block(*rule.codeBlock, b);
    }
    const ir::ExtraSlice then_region = b.close_region();
    b.open_region();
    gen_step_units(rs, rule, u + 1, pos_lv, i32, b);
    const ir::ExtraSlice else_region = b.close_region();
    b.cond(pred, then_region, else_region, l);
}

// One activation of a production: pick a rule, then generate it.
void Lowerer::gen_production(const RsProd& p, const RandSeqCtx& rs, ir::Builder& b) {
    const std::span<const RsProd::Rule> rules = p.getRules();
    if (rules.empty()) {
        return;
    }
    if (rules.size() == 1) {
        // No alternative to weigh, so no weight expression is evaluated and no
        // random value is drawn: LRM 18.17.1 makes a weight meaningful only
        // between alternatives, and draw counts are observable (they shift every
        // later value in the process's stream).
        gen_rule(rules[0], rs, b);
        return;
    }
    gen_weighted_rules(p, rules, b, [&](size_t k, ir::Builder& bb) { gen_rule(rules[k], rs, bb); });
}

// Everything one rule generates, in order: its prods left to right, then the
// weight clause's trailing code block. That order is forced by LRM 18.17.7's
// GenQueue example, whose weight-clause block reads the result of a production
// in the same rule's list.
void Lowerer::gen_rule(const RsProd::Rule& rule, const RandSeqCtx& rs, ir::Builder& b) {
    const RandSeqRuleVars vars = rule_vars(rule, /*declare=*/true, b);
    if (rule.isRandJoin) {
        gen_rand_join(rule, rs, vars, b);
    } else {
        for (const RsProdBase* prod : rule.prods) {
            gen_prod(*prod, rs, vars, b);
        }
    }
    if (rule.codeBlock) {
        gen_code_block(*rule.codeBlock, b);
    }
}

// A rule's implicit result variables (LRM 18.17.7) and the occurrence index of
// each production reference within it.
//
// slang synthesizes one const variable per non-void production the rule names
// and puts it in the rule block's scope. They arrive as scope members, never as
// VariableDeclStatements, so nothing else would ever declare them --
// lower_var_decl, not bare declare_storage, because the type default it writes
// is what a slot for a production the rule did NOT generate reads back. 18.17.7
// calls that value "undefined"; Table 6-7's default is a defined choice, and the
// alternative here is the frame's previous contents.
//
// `declare` is false inside a `rand join` operand's `step` body, where the
// variables are ref formals over the driver's cells and were initialized once,
// before the first unit.
Lowerer::RandSeqRuleVars Lowerer::rule_vars(const RsProd::Rule& rule, bool declare, ir::Builder& b) {
    RandSeqRuleVars vars;
    for (const Symbol& m : rule.ruleBlock->members()) {
        const auto* vs = m.as_if<VariableSymbol>();
        if (vs == nullptr) {
            continue;
        }
        if (declare) {
            lower_var_decl(*vs, b, /*force_frame=*/true);
        }
        vars.byname[vs->name] = vs;
    }
    count_rule_items(rule, vars);
    return vars;
}

// `rand join [ (expr) ] S1 S2 ...` (LRM 18.17.5): interleave the operands'
// sequences at random while preserving each one's own order.
//
// Each operand is planned once -- one call that draws its rule and reports that
// rule's depth-1 unit count -- and then advanced one unit at a time by its
// `step` entry point. The parser guarantees every operand is a plain production
// reference and that there are at least two.
//
// The optional expression is "a real number in the range of 0.0 to 1.0" fixing
// "the degree to which the length of the sequences to be interleaved affects
// the probability of selecting a sequence": 0.0 favours the shortest remaining
// sequence, 1.0 the longest, and 0.5 "does not prioritize any sequence length".
// The clause pins those three points and nothing between them, so the weight is
//
//     w_i = (K - X) * (Lmax + 1 - L_i) + X * L_i
//
// with X the expression quantized to K = 1024ths and L_i the operand's
// remaining unit count. At X = 0 that is proportional to Lmax + 1 - L_i
// (shortest first), at X = K to L_i (longest first), and at X = K/2 it collapses
// to the constant K/2 * (Lmax + 1), i.e. uniform over the eligible operands --
// exactly the three anchors, monotone in X between them, and integer-only, so
// the pick reuses the same cumulative ladder every other weighted choice here
// uses. w_i is at least 1 for an eligible operand at every X, so the total can
// only be zero when none is eligible, which the loop condition already tests.
void Lowerer::gen_rand_join(const RsProd::Rule&    rule,
                            const RandSeqCtx&      rs,
                            const RandSeqRuleVars& vars,
                            ir::Builder&           b) {
    const ir::SourceLoc l      = to_source_loc(rule.ruleBlock->location);
    const ir::Ref       i32    = make_int_type(32, false);
    const ir::Ref       u32    = make_int_type(32, false);
    const ir::Ref       bit1   = make_int_type(1, false);
    const auto          kconst = [&](int64_t v) { return b.constant(module_.pool.put_int(i32, v), l); };

    // One operand's live interleaving state. Everything the production holds
    // ACROSS its units lives here, in the driver's frame, because `step` is
    // entered once per unit and its own frame does not survive between them:
    // the declared formals (one activation's copies, LRM 18.17.7's "similar to
    // a task call") and the rule's implicit result variables.
    struct Operand {
        const RsProdItem*       item = nullptr;
        const RsProd*           prod = nullptr;
        TempCell                rule_idx; // the rule `plan` drew
        TempCell                nleft;    // units not yet generated
        TempCell                pos;      // next unit index
        TempCell                stop;     // the production returned; drop the rest
        std::optional<TempCell> result;
        std::vector<ir::Index>  formals;  // one lvalue per declared formal
        std::vector<ir::Index>  rulevars; // one lvalue per (rule, result variable)
        std::vector<CopyOutArg> outs;     // output / inout copy-backs, run once at the end
    };
    std::vector<Operand> ops;
    ops.reserve(rule.prods.size());
    for (const RsProdBase* prod : rule.prods) {
        if (prod->kind != RsProdKind::Item) {
            fail("internal: a randsequence rand join operand is not a production reference");
        }
        const RsProdItem& item = prod->as<RsProdItem>();
        if (item.target == nullptr) {
            return; // an unresolved operand; slang already diagnosed it
        }
        Operand op;
        op.item     = &item;
        op.prod     = item.target;
        op.rule_idx = make_temp_cell(i32, "rj_rule");
        op.nleft    = make_temp_cell(i32, "rj_left");
        op.pos      = make_temp_cell(i32, "rj_pos");
        op.stop     = make_temp_cell(bit1, "rj_stop");
        if (!op.prod->getReturnType().isVoid()) {
            op.result = make_temp_cell(lower_type(op.prod->getReturnType()), "rj_res");
        }
        ops.push_back(std::move(op));
    }

    // The activation state, set up once, before any unit runs.
    for (Operand& op : ops) {
        if (op.item->args.size() != op.prod->arguments.size()) {
            fail("randsequence production argument count mismatch");
        }
        for (size_t k = 0; k < op.prod->arguments.size(); k++) {
            op.formals.push_back(bind_rand_join_formal(*op.prod->arguments[k], *op.item->args[k], op.outs, b, l));
        }
        // LRM 18.17.7's implicit result variables. Default-initialized here and
        // not per unit: a slot written by one unit is read by a later one, and a
        // slot for a production the rule did not generate keeps the type default
        // (18.17.7's "undefined value").
        std::vector<const VariableSymbol*> rvs;
        rand_join_rule_vars(*op.prod, rvs);
        for (const VariableSymbol* vs : rvs) {
            const ir::Ref   vty = lower_type(vs->getType());
            const ir::Index cell =
              is_agg_abi_ref(vty) ? make_canonical_agg_temp(vty, b, l) : make_arg_temp(vty, b, l);
            emit_default_init(cell, vty, b, l);
            op.rulevars.push_back(cell);
        }
    }

    // Plan every operand up front, left to right.
    for (const Operand& op : ops) {
        const RandSeqCtx::Steps& st = rs.steps.at(op.prod);
        std::vector<ir::Index>   args;
        if (rs.this_var != nullptr) {
            args.push_back(this_handle(b, l));
        }
        args.push_back(op.rule_idx.lv(b, l));
        args.push_back(op.nleft.lv(b, l));
        args.insert(args.end(), op.formals.begin(), op.formals.end());
        push_capture_args(rs, args, b, l);
        if (rs.is_task) {
            b.task_call(st.plan, args, l);
        } else {
            b.call(st.plan, args, l);
        }
        const ir::Index pos_lv   = op.pos.lv(b, l);
        const ir::Index pos_init = kconst(0);
        b.binary(ir::Inst::Tag::Store, pos_lv, pos_init, l);
        const ir::Index stop_lv   = op.stop.lv(b, l);
        const ir::Index stop_init = b.constant(module_.pool.put_int(bit1, 0), l);
        b.binary(ir::Inst::Tag::Store, stop_lv, stop_init, l);
    }

    const ir::Index bias = rand_join_bias(rule, i32, b, l);

    // while (any operand has units left)
    b.open_region();
    const ir::Index first_left = b.unary(ir::Inst::Tag::Load, ops[0].nleft.lv(b, l), l);
    const ir::Index first_zero = kconst(0);
    ir::Index       any        = b.binary(ir::Inst::Tag::SGt, first_left, first_zero, l);
    for (size_t i = 1; i < ops.size(); i++) {
        const ir::Index left = b.unary(ir::Inst::Tag::Load, ops[i].nleft.lv(b, l), l);
        const ir::Index zero = kconst(0);
        const ir::Index busy = b.binary(ir::Inst::Tag::SGt, left, zero, l);
        any                  = b.binary(ir::Inst::Tag::Bor, any, busy, l);
    }
    const ir::ExtraSlice cond_region = b.close_region();

    b.open_region();
    // Lmax over the remaining lengths, then the per-operand weight above.
    ir::Index lmax = b.unary(ir::Inst::Tag::Load, ops[0].nleft.lv(b, l), l);
    for (size_t i = 1; i < ops.size(); i++) {
        const ir::Index cur = b.unary(ir::Inst::Tag::Load, ops[i].nleft.lv(b, l), l);
        lmax                = b.select(b.binary(ir::Inst::Tag::SGt, cur, lmax, l), cur, lmax, l);
    }
    const ir::Index        span = b.binary(ir::Inst::Tag::Add, lmax, kconst(1), l);
    const ir::Index        kmx  = kconst(kRandJoinScale);
    std::vector<ir::Index> weights;
    weights.reserve(ops.size());
    for (const Operand& op : ops) {
        const ir::Index left     = b.unary(ir::Inst::Tag::Load, op.nleft.lv(b, l), l);
        const ir::Index inv_bias = b.binary(ir::Inst::Tag::Sub, kmx, bias, l);
        const ir::Index gap      = b.binary(ir::Inst::Tag::Sub, span, left, l);
        const ir::Index shortw   = b.binary(ir::Inst::Tag::Mul, inv_bias, gap, l);
        const ir::Index longw    = b.binary(ir::Inst::Tag::Mul, bias, left, l);
        const ir::Index w        = b.binary(ir::Inst::Tag::Add, shortw, longw, l);
        const ir::Index has_left = b.binary(ir::Inst::Tag::SGt, left, kconst(0), l);
        const ir::Index none     = kconst(0);
        const ir::Index weight   = b.select(has_left, w, none, l);
        weights.push_back(b.convert(weight, u32, false, l));
    }
    ir::Index sum = weights[0];
    for (size_t i = 1; i < weights.size(); i++) {
        sum = b.binary(ir::Inst::Tag::Add, sum, weights[i], l);
    }
    const ir::Index top  = b.binary(ir::Inst::Tag::Sub, sum, b.constant(module_.pool.put_int(u32, 1), l), l);
    const ir::Index low  = b.constant(module_.pool.put_int(u32, 0), l);
    const ir::Index draw = b.urandom_range(top, low, l);
    gen_rand_join_arm(ops, 0, b.constant(module_.pool.put_int(u32, 0), l), draw, weights, u32, rs, vars, b, l);
    const ir::ExtraSlice body_region = b.close_region();

    b.open_region();
    const ir::ExtraSlice step_region = b.close_region();
    b.loop(cond_region, any, body_region, step_region, /*bottom_test*/ false, l);

    // One copy-out per output / inout formal, after the last unit: the
    // production's activation spans the whole interleaving, so LRM 18.17.7's
    // "similar to a task call" puts its completion here and not at each unit.
    for (const Operand& op : ops) {
        emit_call_copyouts(op.outs, b, l);
    }
}

// One declared formal of an interleaved production, bound to storage that
// outlives a single unit (LRM 18.17.5 with 18.17.7). Returns the lvalue the
// `plan` and `step` calls pass, which is why every declared formal of those two
// entry points is an ArgDir::Ref: the callee reads and writes the one copy the
// whole activation shares, exactly as a plain production's frame slot does.
//
// A `ref` formal aliases the actual itself, materialized once here so an index
// expression inside it is evaluated once (13.5.2 with 18.17.7's once-only rule).
// Everything else takes a driver-frame cell: copied in for `input` / `inout`,
// default-initialized for `output`, and recorded in `outs` for the single
// copy-back the caller runs after the interleaving finishes.
ir::Index Lowerer::bind_rand_join_formal(const FormalArgumentSymbol& fa,
                                         const Expression&           actual_expr,
                                         std::vector<CopyOutArg>&    outs,
                                         ir::Builder&                b,
                                         ir::SourceLoc               l) {
    const auto dir = fa.direction;
    if (dir == slang::ast::ArgumentDirection::Ref) {
        return lower_ref_actual(actual_expr, b);
    }
    const ir::Ref pty = lower_type(fa.getType());
    const bool    agg = is_agg_abi_ref(pty);
    // slang models an output / inout actual as an AssignmentExpression whose
    // left side is the caller lvalue; unwrap it to the bound expression.
    const Expression* actual = &actual_expr;
    if (actual->kind == ExpressionKind::Assignment) {
        actual = &actual->as<AssignmentExpression>().left();
    }
    const ir::Index cell = agg ? make_canonical_agg_temp(pty, b, l) : make_arg_temp(pty, b, l);
    if (dir == slang::ast::ArgumentDirection::Out) {
        // Not seeded from the caller (LRM 13.5.1); the type default is what an
        // ordinary callee's frame slot would hold at entry.
        emit_default_init(cell, pty, b, l);
    } else if (agg) {
        const Expression& src = unwrap_conversion(*actual);
        if (is_struct_lvalue_shape(src)) {
            emit_agg_copy(cell, lower_lvalue(src, b), pty, ir::Inst::Tag::Store, b, l);
        } else {
            emit_agg_store(cell, pty, src, ir::Inst::Tag::Store, b, l);
        }
    } else if (is_handle_ref(pty)) {
        emit_container_store(cell, pty, *actual, b, l);
    } else if (is_string_ref(pty)) {
        b.binary(ir::Inst::Tag::Store, cell, lower_expr(*actual, b), l);
    } else {
        b.binary(ir::Inst::Tag::Store, cell, coerce(lower_expr(*actual, b), *actual->type, pty, b, l), l);
    }
    if (dir != slang::ast::ArgumentDirection::In) {
        outs.push_back(CopyOutArg{ cell, actual, lower_type(*actual->type), &fa.getType(), agg, pty });
    }
    return cell;
}

// Every implicit result variable of every rule of `p`, in rule order then
// declaration order (LRM 18.17.7). For an interleaved production these cannot be
// frame locals of `step`: one is written by the unit that generates the
// production and read by a later unit of the same rule, with other sequences'
// units in between. The driver owns them and passes each by reference, in this
// order, which build_production_step's formal list mirrors.
void Lowerer::rand_join_rule_vars(const RsProd& p, std::vector<const VariableSymbol*>& out) {
    for (const RsProd::Rule& rule : p.getRules()) {
        for (const Symbol& m : rule.ruleBlock->members()) {
            if (const auto* vs = m.as_if<VariableSymbol>()) {
                out.push_back(vs);
            }
        }
    }
}

ir::Index Lowerer::rand_join_bias(const RsProd::Rule& rule, ir::Ref i32, ir::Builder& b, ir::SourceLoc l) {
    if (rule.randJoinExpr == nullptr) {
        return b.constant(module_.pool.put_int(i32, kRandJoinScale / 2), l);
    }
    const ir::SourceLoc el = loc(*rule.randJoinExpr);
    const ir::Index     rv =
      coerce(lower_expr(*rule.randJoinExpr, b), *rule.randJoinExpr->type, module_.pool.real_type, b, el);
    const ir::Index scaled =
      b.binary(ir::Inst::Tag::FMul,
               rv,
               b.constant(module_.pool.put_real(module_.pool.real_type, static_cast<double>(kRandJoinScale)), el),
               el);
    const ir::Index q  = b.float_cvt(ir::Inst::Tag::FtoITrunc, scaled, i32, el);
    const ir::Index lo = b.constant(module_.pool.put_int(i32, 0), el);
    const ir::Index hi = b.constant(module_.pool.put_int(i32, kRandJoinScale), el);
    // An out-of-range expression is not diagnosed by slang (it only requires a
    // numeric type), and 18.17.5 gives no behaviour for one; clamping keeps the
    // weights non-negative, which the ladder needs.
    const ir::Index c1 = b.select(b.binary(ir::Inst::Tag::SLt, q, lo, el), lo, q, el);
    return b.select(b.binary(ir::Inst::Tag::SGt, c1, hi, el), hi, c1, el);
}

// Replays createRuleVariables' counting walk (slang's MemberSymbols.cpp) to
// number each syntactic production reference within its rule: the k-th
// reference to a non-void production P writes P[k], 1-based, or the scalar P
// when the rule names P once (LRM 18.17.7). slang records no such mapping.
//
// Walks the SYNTAX for a case production, because a `default` item's position
// among the standard ones is not recoverable from CaseProd -- it is held apart,
// in `defaultItem`, while createRuleVariables counts it where it was written.
void Lowerer::count_rule_items(const RsProd::Rule& rule, RandSeqRuleVars& vars) {
    const auto bump = [&](const RsProdItem& it) {
        if (it.target == nullptr || it.target->getReturnType().isVoid()) {
            return;
        }
        vars.occ[&it] = ++vars.total[it.target];
    };
    const slang::syntax::SyntaxNode* syn  = rule.ruleBlock->getSyntax();
    const auto*                      rsyn = syn != nullptr ? syn->as_if<slang::syntax::RsRuleSyntax>() : nullptr;
    size_t                           k    = 0;
    for (const RsProdBase* prod : rule.prods) {
        const slang::syntax::SyntaxNode* psyn =
          (rsyn != nullptr && k < rsyn->prods.size()) ? rsyn->prods[k] : nullptr;
        k++;
        switch (prod->kind) {
            case RsProdKind::Item:
                bump(prod->as<RsProdItem>());
                break;
            case RsProdKind::CodeBlock:
                break;
            case RsProdKind::IfElse: {
                const auto& ie = prod->as<RsProd::IfElseProd>();
                bump(ie.ifItem);
                if (ie.elseItem) {
                    bump(*ie.elseItem);
                }
                break;
            }
            case RsProdKind::Repeat:
                // Counted once however many times it runs: LRM 18.17.7 Example 2
                // declares `int C;` scalar for `repeat(5) C`, so every iteration
                // overwrites one slot and the readable value is the last.
                bump(prod->as<RsProd::RepeatProd>().item);
                break;
            case RsProdKind::Case: {
                const auto& cp = prod->as<RsProd::CaseProd>();
                const auto* cs = psyn != nullptr ? psyn->as_if<slang::syntax::RsCaseSyntax>() : nullptr;
                if (cs == nullptr) {
                    for (const RsProd::CaseItem& ci : cp.items) {
                        bump(ci.item);
                    }
                    if (cp.defaultItem) {
                        bump(*cp.defaultItem);
                    }
                    break;
                }
                size_t si          = 0;
                bool   did_default = false;
                for (const slang::syntax::RsCaseItemSyntax* ci : cs->items) {
                    if (ci->kind == slang::syntax::SyntaxKind::DefaultRsCaseItem) {
                        // slang keeps only the first default; the parser already
                        // reported the rest (MultipleDefaultCases).
                        if (!did_default && cp.defaultItem) {
                            bump(*cp.defaultItem);
                            did_default = true;
                        }
                        continue;
                    }
                    if (si < cp.items.size()) {
                        bump(cp.items[si++].item);
                    }
                }
                break;
            }
        }
    }
}

void Lowerer::gen_prod(const RsProdBase& prod, const RandSeqCtx& rs, const RandSeqRuleVars& vars, ir::Builder& b) {
    switch (prod.kind) {
        case RsProdKind::Item:
            gen_prod_item(prod.as<RsProdItem>(), rs, vars, b);
            return;
        case RsProdKind::CodeBlock:
            gen_code_block(prod.as<RsProd::CodeBlockProd>(), b);
            return;
        case RsProdKind::IfElse: {
            // LRM 18.17.2. The condition is an ordinary boolean, evaluated once.
            const auto&         ie = prod.as<RsProd::IfElseProd>();
            const ir::SourceLoc el = loc(*ie.expr);
            const ir::Index     c  = lower_predicate(*ie.expr, b);
            b.open_region();
            gen_prod_item(ie.ifItem, rs, vars, b);
            const ir::ExtraSlice then_region = b.close_region();
            b.open_region();
            if (ie.elseItem) {
                gen_prod_item(*ie.elseItem, rs, vars, b);
            }
            const ir::ExtraSlice else_region = b.close_region();
            b.cond(c, then_region, else_region, el);
            return;
        }
        case RsProdKind::Repeat: {
            // LRM 18.17.4: the count is evaluated once, then the production is
            // generated that many times. The loop is synthesized here, so
            // loop_depth_ is deliberately NOT raised -- "the repeat production
            // statement itself cannot be terminated prematurely. A break statement
            // will terminate the entire randsequence block." A negative count runs
            // zero times; 18.17.4 requires a non-negative one.
            const auto&         rp        = prod.as<RsProd::RepeatProd>();
            const ir::SourceLoc el        = loc(*rp.expr);
            const ir::Ref       ty        = lower_type(*rp.expr->type);
            const TempCell      bound     = make_temp_cell(ty, "rs_repeat_bound");
            const TempCell      counter   = make_temp_cell(ty, "rs_repeat_count");
            const ir::Index     bound_lv  = bound.lv(b, el);
            const ir::Index     bound_val = lower_expr(*rp.expr, b);
            b.binary(ir::Inst::Tag::Store, bound_lv, bound_val, el);
            const ir::Index counter_lv = counter.lv(b, el);
            const ir::Index zero       = b.constant(module_.pool.put_int(ty, 0), el);
            b.binary(ir::Inst::Tag::Store, counter_lv, zero, el);

            b.open_region();
            const ir::Index      cur  = b.unary(ir::Inst::Tag::Load, counter.lv(b, el), el);
            const ir::Index      lim  = b.unary(ir::Inst::Tag::Load, bound.lv(b, el), el);
            const ir::Index      pred =
              b.binary(rp.expr->type->isSigned() ? ir::Inst::Tag::SLt : ir::Inst::Tag::ULt, cur, lim, el);
            const ir::ExtraSlice cond = b.close_region();

            b.open_region();
            gen_prod_item(rp.item, rs, vars, b);
            const ir::ExtraSlice body = b.close_region();

            b.open_region();
            const ir::Index slot = counter.lv(b, el);
            const ir::Index sc   = b.unary(ir::Inst::Tag::Load, counter.lv(b, el), el);
            const ir::Index one  = b.constant(module_.pool.put_int(ty, 1), el);
            const ir::Index next = b.binary(ir::Inst::Tag::Add, sc, one, el);
            b.binary(ir::Inst::Tag::Store, slot, next, el);
            const ir::ExtraSlice step = b.close_region();

            b.loop(cond, pred, body, step, /*bottom_test*/ false, el);
            return;
        }
        case RsProdKind::Case:
            gen_case_prod(prod.as<RsProd::CaseProd>(), rs, vars, b);
            return;
    }
}

// A production code block: LRM 18.17 makes each one "an anonymous automatic
// scope", which is what its statements' ordinary declare_storage path already
// gives them inside the production's frame. slang bound the body during
// createRule, so tryGetStatement is enough.
void Lowerer::gen_code_block(const RsProd::CodeBlockProd& cb, ir::Builder& b) {
    if (const Statement* s = cb.block->tryGetStatement()) {
        lower_stmt(*s, b);
    }
}

// LRM 18.17.3: the selector is evaluated once and compared, in order, against
// each item's labels; the first match generates that item's production, and no
// match generates the default item's, or nothing. The same first-match rule as
// a procedural `case` -- lower_case itself cannot be reused because its arm
// body is a Statement, not a production.
void Lowerer::gen_case_prod(const RsProd::CaseProd& cp,
                            const RandSeqCtx&       rs,
                            const RandSeqRuleVars&  vars,
                            ir::Builder&            b) {
    const ir::SourceLoc l = loc(*cp.expr);
    // One common compare type for the selector and every label, as
    // common_case_type computes for a procedural case (LRM 12.5).
    uint32_t width = cp.expr->type->getBitWidth();
    bool     four  = cp.expr->type->isFourState();
    bool     signd = cp.expr->type->isSigned();
    for (const RsProd::CaseItem& ci : cp.items) {
        for (const Expression* e : ci.expressions) {
            width = std::max(width, e->type->getBitWidth());
            four  = four || e->type->isFourState();
            signd = signd && e->type->isSigned();
        }
    }
    const ir::Ref   ty  = make_int_type(width, four);
    const ir::Index sel = b.convert(lower_expr(*cp.expr, b), ty, signd, l);
    gen_case_arm(cp, 0, sel, ty, signd, rs, vars, b);
}

void Lowerer::gen_case_arm(const RsProd::CaseProd& cp,
                           size_t                  k,
                           ir::Index               sel,
                           ir::Ref                 ty,
                           bool                    signd,
                           const RandSeqCtx&       rs,
                           const RandSeqRuleVars&  vars,
                           ir::Builder&            b) {
    if (k == cp.items.size()) {
        if (cp.defaultItem) {
            gen_prod_item(*cp.defaultItem, rs, vars, b);
        }
        return;
    }
    const RsProd::CaseItem& ci = cp.items[k];
    const ir::SourceLoc     l  = loc(*ci.expressions[0]);
    ir::Index               pred =
      b.binary(ir::Inst::Tag::CaseEq, sel, b.convert(lower_expr(*ci.expressions[0], b), ty, signd, l), l);
    for (size_t j = 1; j < ci.expressions.size(); j++) {
        // Labels sharing one production (`1, 2 : DISK`) match as a set.
        const ir::SourceLoc el = loc(*ci.expressions[j]);
        pred                   = b.binary(
          ir::Inst::Tag::Bor,
          pred,
          b.binary(ir::Inst::Tag::CaseEq, sel, b.convert(lower_expr(*ci.expressions[j], b), ty, signd, el), el),
          el);
    }

    b.open_region();
    gen_prod_item(ci.item, rs, vars, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    gen_case_arm(cp, k + 1, sel, ty, signd, rs, vars, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, l);
}

// Generate one production reference: evaluate its arguments, call it, then test
// the abort flag it may have raised.
void Lowerer::gen_prod_item(const RsProdItem& item, const RandSeqCtx& rs, const RandSeqRuleVars& vars, ir::Builder& b) {
    if (item.target == nullptr) {
        return; // an unresolved production name; slang already diagnosed it
    }
    const RsProd&       target = *item.target;
    const ir::SourceLoc l      = to_source_loc(target.location);
    const auto          idit   = rs.ids.find(&target);
    if (idit == rs.ids.end()) {
        fail("internal: randsequence production '" + std::string(target.name) + "' has no lowered body");
    }
    if (item.args.size() != target.arguments.size()) {
        fail("randsequence production argument count mismatch");
    }

    std::vector<ir::Index> args;
    if (rs.this_var != nullptr) {
        args.push_back(this_handle(b, l));
    }
    args.push_back(abort_lvalue(b, l));
    // The result rides a caller temp, not the rule variable's place directly:
    // an `output` actual is a raw slot the callee writes through, and the rule
    // variable may be an element of a `[1:N]` array, which is a composed lvalue
    // rather than a slot. Same temp-then-copy-back shape build_call_args uses
    // for every ordinary output argument.
    std::optional<ir::Index> res_temp;
    ir::Ref                  res_ty = ir::Ref::Invalid;
    if (!target.getReturnType().isVoid()) {
        res_ty   = lower_type(target.getReturnType());
        res_temp = make_arg_temp(res_ty, b, l);
        args.push_back(*res_temp);
    }
    std::vector<CopyOutArg>      outs;
    const std::vector<ir::Index> declared = build_call_args(target.arguments, item.args, outs, b, l);
    args.insert(args.end(), declared.begin(), declared.end());
    push_capture_args(rs, args, b, l);

    if (rs.is_task) {
        b.task_call(idit->second, args, l);
    } else {
        b.call(idit->second, args, l);
    }
    emit_call_copyouts(outs, b, l);
    if (res_temp) {
        // The value is the production's own return type on both sides, so
        // nothing is coerced -- a string Store deep-copies the handle.
        const ir::Index place = result_slot(item, target, vars, b, l);
        emit_result_copy(place, *res_temp, res_ty, b, l);
    }
    emit_abort_check(b, l);
}

// Move a production's returned value out of the caller temp the output formal
// wrote and into the rule variable that names it (LRM 18.17.7). Place-to-place,
// so there is no expression to dispatch on: an aggregate copies element-wise, a
// container deep-copies its handle, everything else is a load and a store.
void Lowerer::emit_result_copy(ir::Index place, ir::Index temp, ir::Ref ty, ir::Builder& b, ir::SourceLoc l) {
    if (is_agg_abi_ref(ty)) {
        emit_agg_copy(place, temp, ty, ir::Inst::Tag::Store, b, l);
        return;
    }
    switch (std::get<ir::Type>(module_.pool.get(ty)).kind) {
        case ir::Type::Kind::DynArray:
            b.dyn_array_copy(place, temp, ty, l);
            return;
        case ir::Type::Kind::Queue:
            b.queue_copy(place, temp, ty, l);
            return;
        case ir::Type::Kind::Assoc:
            b.assoc_copy(place, temp, ty, l);
            return;
        default:
            b.binary(ir::Inst::Tag::Store, place, b.unary(ir::Inst::Tag::Load, temp, l), l);
    }
}

// The lvalue this body passes as a production call's `$abort` output. Inside a
// production it is that production's own flag, which its own caller receives at
// return; at the randsequence statement there is nothing above to unwind, so it
// is a write-only temp.
ir::Index Lowerer::abort_lvalue(ir::Builder& b, ir::SourceLoc l) {
    const ir::Ref bit1 = make_int_type(1, false);
    if (fn_ctx_ != nullptr && fn_ctx_->in_randseq) {
        return b.local(fn_ctx_->randseq_abort, bit1, l);
    }
    return make_arg_temp(bit1, b, l);
}

// After every production call: a `break` executed anywhere below raised the
// flag, and LRM 18.17.6 ends the whole randsequence, so unwind one frame. At
// the randsequence statement itself there is nothing left to unwind and
// execution simply continues after the block.
void Lowerer::emit_abort_check(ir::Builder& b, ir::SourceLoc l) {
    if (fn_ctx_ == nullptr || !fn_ctx_->in_randseq) {
        return;
    }
    const ir::Ref bit1 = make_int_type(1, false);
    b.open_region();
    b.ret_void(l);
    const ir::ExtraSlice abort_region = b.close_region();
    b.open_region();
    const ir::ExtraSlice cont_region = b.close_region();
    b.cond(b.unary(ir::Inst::Tag::Load, b.local(fn_ctx_->randseq_abort, bit1, l), l),
           abort_region,
           cont_region,
           l,
           /*unlikely=*/true);
}

// Where a non-void production's returned value lands: the rule variable slang
// synthesized for it, indexed from 1 in syntactic order of appearance when the
// rule names the production more than once (LRM 18.17.7).
ir::Index Lowerer::result_slot(const RsProdItem&      item,
                               const RsProd&          target,
                               const RandSeqRuleVars& vars,
                               ir::Builder&           b,
                               ir::SourceLoc          l) {
    const auto vit = vars.byname.find(target.name);
    const auto oit = vars.occ.find(&item);
    const auto tit = vars.total.find(&target);
    if (vit == vars.byname.end() || oit == vars.occ.end() || tit == vars.total.end()) {
        // createRuleVariables makes one variable for every non-void production a
        // rule names, so a miss is a lowering bug, not an unimplemented construct.
        fail("internal: randsequence production '" + std::string(target.name) +
             "' has no result variable in its rule");
    }
    const ir::Index base = value_lvalue(*vit->second, b, l);
    if (tit->second == 1) {
        return base;
    }
    // The array is `[1:N]`, and ElemSelect indexes in declared-range coordinates.
    const ir::Ref idx_ty = make_int_type(32, false);
    return b.elem_select(base,
                         b.constant(module_.pool.put_int(idx_ty, oit->second), l),
                         lower_type(vit->second->getType()),
                         l,
                         /*for_write=*/true);
}

// Reserve the FunctionId of one of class `clt`'s two synthesized randomization
// bodies, recording the class for drain_rand_bodies. The id has to exist now
// (compute_vtable is filling the slot with it) and the body cannot be built now
// (see rand_pending_).
ir::FunctionId Lowerer::rand_body_id(const slang::ast::ClassType& clt,
                                     ir::Ref                      class_ref,
                                     ir::Ref                      return_type,
                                     ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::FunctionId>& map) {
    const auto it = map.find(&clt);
    if (it != map.end()) {
        return it->second;
    }
    const ir::FunctionId id = module_.reserve_function(return_type);
    map[&clt]               = id;
    if (randomize_fn_.contains(&clt) && randops_fn_.contains(&clt)) {
        rand_pending_.emplace_back(&clt, class_ref);
    }
    return id;
}

// Build every deferred randomization body. Runs from the outermost
// lower_class_type frame, where every class this lowering reached has its vtable
// numbering; a body built here can reach a further class, whose own bodies join
// the list and are drained by the same loop.
void Lowerer::drain_rand_bodies() {
    while (!rand_pending_.empty()) {
        const auto [clt, class_ref] = rand_pending_.back();
        rand_pending_.pop_back();
        build_randomize_body(*clt, class_ref, randomize_fn_.at(clt));
        build_randops_body(*clt, class_ref, randops_fn_.at(clt));
    }
}

// The most-derived USER declaration of `pre_randomize` / `post_randomize` in
// `clt`'s chain, or null when no class in it declares one (LRM 18.6.2). slang
// synthesizes an empty built-in on every class, which is what BuiltIn filters
// out. Only this one is called: an override is responsible for reaching its
// base's with an explicit `super.pre_randomize()`, and where a class declares no
// override the walk finds the nearest base's declaration, which is exactly what
// the LRM's default (chaining) implementation would have run.
const SubroutineSymbol* Lowerer::find_rand_hook(const slang::ast::ClassType& clt, std::string_view name) {
    for (const slang::ast::ClassType* k = &clt; k != nullptr; k = base_class_of(*k)) {
        for (const auto& m : k->membersOfType<SubroutineSymbol>()) {
            if (m.name == name && !m.flags.has(slang::ast::MethodFlags::BuiltIn)) {
                return &m;
            }
        }
    }
    return nullptr;
}

void Lowerer::emit_rand_hook(const slang::ast::ClassType& clt,
                             std::string_view             name,
                             ir::Index                    self,
                             ir::Builder&                 b,
                             ir::SourceLoc                l) {
    if (const SubroutineSymbol* hook = find_rand_hook(clt, name)) {
        b.call(function_id(*hook), std::array<ir::Index, 1>{ self }, l);
    }
}

// Build `clt`'s `$randomize(this, mask) -> int` (LRM 18.4). `mask` is a FILTER
// over rand ordinals, not the active set itself: the body intersects it with the
// object's own rand_mode mask, so a caller that does not know the callee's
// dynamic type (the recursion into a `rand` handle, and every plain
// `obj.randomize()`) passes all-ones and still honours modes the callee owns.
// `randomize(x, y)` passes the subset, `randomize(null)` passes zero.
//
// Step 1 solves nothing, so the middle is a straight-line draw per active
// property and the result is always 1. A constraint solver replaces exactly that
// middle: the hook calls, the mask intersection and the recursion are
// constraint-independent.
void Lowerer::build_randomize_body(const slang::ast::ClassType& clt, ir::Ref class_ref, ir::FunctionId id) {
    const ir::SourceLoc l   = to_source_loc(clt.location);
    const ir::Ref       u64 = make_int_type(64, false);
    const ir::Ref       i32 = make_int_type(32, false);
    ir::Builder         b(module_);
    const ClassRand&    rd   = class_rand_.at(&clt);
    if (!rd.unsupported.empty()) {
        // The chain carries a randomization surface step 1 cannot honour (see
        // ClassRand::unsupported). The refusal lives here, in the one body every
        // randomize() of this class must reach -- the dispatch is virtual, so a
        // base-typed handle over a constrained derived object lands here too --
        // rather than at the type or the call site: eager static initialization
        // lowers every factory-registered class and builds every vtable body,
        // so a compile-time fail would refuse a design for merely CONTAINING
        // such a class (all of UVM's register layer). Loud per PRINCIPLES.md:
        // report + terminate, never a silent unconstrained draw.
        const std::string banner = banner_at(clt.location);
        b.severity(static_cast<uint32_t>(ir::Severity::Fatal), severity_banner(clt.location), rd.unsupported, {},
                   l);
        b.finish(banner + "$fatal", 1, l);
        b.unary(ir::Inst::Tag::Ret, b.constant(module_.pool.put_int(i32, 0), l), l);
        module_.set_function(id,
                             ir::Function{ .name        = std::string(clt.name) + ".randomize",
                                           .return_type = i32,
                                           .param_types = std::vector<ir::Ref>{ class_ref, u64 },
                                           .arg_dirs = std::vector<ir::ArgDir>{ ir::ArgDir::In, ir::ArgDir::In },
                                           .local_types = std::vector<ir::Ref>{ class_ref, u64 },
                                           .span        = span_of(clt),
                                           .ir          = b.finish() });
        return;
    }
    const ir::Index     self = b.unary(ir::Inst::Tag::Load, b.local(0, class_ref, l), l);
    emit_rand_hook(clt, "pre_randomize", self, b, l);
    if (rd.present) {
        const ir::Index body      = b.handle_deref(self, class_ref, l);
        const ir::Index arg_mask  = b.unary(ir::Inst::Tag::Load, b.local(1, u64, l), l);
        const ir::Index mode_slot = b.member_select(body, rd.mode_field, class_ref, l);
        const ir::Index mode_mask = b.unary(ir::Inst::Tag::Load, mode_slot, l);
        const ir::Index eff       = b.binary(ir::Inst::Tag::Band, arg_mask, mode_mask, l);
        for (const RandProp& p : rd.props) {
            b.open_region();
            emit_rand_assign(p, rd, body, class_ref, b, l);
            const ir::ExtraSlice then_region = b.close_region();
            b.open_region();
            const ir::ExtraSlice else_region = b.close_region();
            const ir::Index      ord         = b.constant(module_.pool.put_int(u64, p.ordinal), l);
            const ir::Index      shifted     = b.binary(ir::Inst::Tag::Srl, eff, ord, l);
            const ir::Index      one         = b.constant(module_.pool.put_int(u64, 1), l);
            const ir::Index      bit         = b.binary(ir::Inst::Tag::Band, shifted, one, l);
            b.cond(b.binary(ir::Inst::Tag::Ne, bit, b.constant(module_.pool.put_int(u64, 0), l), l),
                   then_region,
                   else_region,
                   l);
        }
    }
    emit_rand_hook(clt, "post_randomize", self, b, l);
    b.unary(ir::Inst::Tag::Ret, b.constant(module_.pool.put_int(i32, 1), l), l);
    module_.set_function(id,
                         ir::Function{ .name        = std::string(clt.name) + ".randomize",
                                       .return_type = i32,
                                       .param_types = std::vector<ir::Ref>{ class_ref, u64 },
                                       .arg_dirs = std::vector<ir::ArgDir>{ ir::ArgDir::In, ir::ArgDir::In },
                                       .local_types = std::vector<ir::Ref>{ class_ref, u64 },
                                       .span        = span_of(clt),
                                       .ir          = b.finish() });
}

// One active rand property's assignment (LRM 18.4, unconstrained).
void Lowerer::emit_rand_assign(const RandProp&  p,
                               const ClassRand& rd,
                               ir::Index        body,
                               ir::Ref          class_ref,
                               ir::Builder&     b,
                               ir::SourceLoc    l) {
    const ir::Index         lv = b.member_select(body, p.field, class_ref, l);
    const slang::ast::Type& ty = p.sym->getType().getCanonicalType();
    if (p.cyclic) {
        // randc (LRM 18.6): the next value of a random permutation of the type's
        // range, tracked by this property's own 256-bit history in the object.
        const ir::Index seen = b.member_select(body, p.randc_field, class_ref, l);
        const ir::Index rng  = b.member_select(body, rd.rng_field, class_ref, l);
        const ir::Index v    = b.randc_next(seen, rng, p.randc_vals, l);
        b.binary(
          ir::Inst::Tag::Store, lv, b.convert(v, lower_type(p.sym->getType()), p.sym->getType().isSigned(), l), l);
        return;
    }
    const RandDraw draw = [&] { return b.obj_random(b.member_select(body, rd.rng_field, class_ref, l), l); };
    emit_rand_value(lv, ty, draw, b, l);
}

// Draw into the lvalue `lv` of type `ty`, recursing structurally.
void Lowerer::emit_rand_value(ir::Index               lv,
                              const slang::ast::Type& ty,
                              const RandDraw&         draw,
                              ir::Builder&            b,
                              ir::SourceLoc           l) {
    if (ty.isClass()) {
        // A `rand` class handle is not assigned: the object it refers to is
        // randomized instead, and a null handle is skipped (LRM 18.4). The call
        // is virtual, so a base-typed handle randomizes the derived object it
        // actually holds.
        emit_rand_recurse(lv, ty.as<slang::ast::ClassType>(), b, l);
        return;
    }
    if (const auto* arr = ty.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
        const ir::Ref                aty = lower_type(ty);
        const slang::ConstantRange   r   = arr->range;
        const slang::ast::Type&      et  = arr->elementType.getCanonicalType();
        for (int32_t c = r.lower(); c <= r.upper(); ++c) {
            const ir::Index idx = b.constant(
              module_.pool.put_int(make_int_type(32, false), static_cast<uint64_t>(static_cast<int64_t>(c))), l);
            emit_rand_value(b.elem_select(lv, idx, aty, l), et, draw, b, l);
        }
        return;
    }
    const ir::Ref target = lower_type(ty);
    b.binary(ir::Inst::Tag::Store, lv, emit_rand_scalar(ty, target, draw, b, l), l);
}

// `if (h != null) h.$randomize(all)`.
void Lowerer::emit_rand_recurse(ir::Index lv, const slang::ast::ClassType& pointee, ir::Builder& b, ir::SourceLoc l) {
    const ir::Ref   u64    = make_int_type(64, false);
    const ir::Ref   ref    = lower_class_type(pointee);
    const ir::Index handle = b.unary(ir::Inst::Tag::Load, lv, l);
    b.open_region();
    const std::array<ir::Index, 2> args{ handle, b.constant(module_.pool.put_int(u64, ~0ull), l) };
    b.call_virtual(randomize_fn_.at(&pointee), ensure_vtable(pointee).at(kRandomizeSlot), ref, args, l);
    const ir::ExtraSlice then_region = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_region = b.close_region();
    b.cond(b.binary(ir::Inst::Tag::Ne, handle, b.constant(module_.pool.put_int(u64, 0), l), l),
           then_region,
           else_region,
           l);
}

// One scalar draw of type `ty` from the object's own RNG (LRM 18.14.2). An
// integral value is uniform over its full range and always 2-state — LRM 18.4
// gives an unconstrained rand variable a value, never x or z. An enum draws
// uniformly over its DISTINCT declared values, not over the base type's range.
ir::Index Lowerer::emit_rand_scalar(const slang::ast::Type& ty,
                                    ir::Ref                 target,
                                    const RandDraw&         draw,
                                    ir::Builder&            b,
                                    ir::SourceLoc           l) {
    if (const auto* et = ty.as_if<slang::ast::EnumType>()) {
        return emit_rand_enum(*et, target, draw(), b, l);
    }
    const uint32_t width = ty.getBitWidth();
    if (width <= 32) {
        return b.convert(draw(), target, ty.isSigned(), l);
    }
    // Wider than one draw: one 32-bit draw per chunk, filling from the LSB up.
    // The chunk order is part of the reproducible stream, so it is fixed here
    // rather than left to concat's operand order (which is MSB-first).
    const ir::Ref          u32   = make_int_type(32, false);
    const uint32_t         n     = (width + 31) / 32;
    std::vector<ir::Index> parts(n);
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t chunk_w = (k + 1 == n) ? width - 32 * (n - 1) : 32;
        parts[n - 1 - k]       = chunk_w == 32 ? draw() : b.convert(draw(), make_int_type(chunk_w, false), false, l);
    }
    (void)u32;
    return b.convert(b.concat(parts, l), target, ty.isSigned(), l);
}

// Uniform over the enum's distinct values: one draw, mapped onto [0, n) by the
// same multiply-shift $urandom_range uses, then a select chain. A chain rather
// than a constant table because an enum's member count is small and a table
// would be a second mechanism to keep alive.
ir::Index Lowerer::emit_rand_enum(const slang::ast::EnumType& et,
                                  ir::Ref                     target,
                                  ir::Index                   draw,
                                  ir::Builder&                b,
                                  ir::SourceLoc               l) {
    std::vector<slang::SVInt> values;
    for (const EnumMember& m : enum_members(et)) {
        if (std::ranges::none_of(values, [&](const slang::SVInt& v) { return (bool)(v == m.value); })) {
            values.push_back(m.value);
        }
    }
    if (values.empty()) {
        fail("internal: rand enum with no members");
    }
    const ir::Ref   u64   = make_int_type(64, false);
    const ir::Index wide  = b.convert(draw, u64, false, l);
    const ir::Index count = b.constant(module_.pool.put_int(u64, values.size()), l);
    const ir::Index prod  = b.binary(ir::Inst::Tag::Mul, wide, count, l);
    const ir::Index shift = b.constant(module_.pool.put_int(u64, 32), l);
    const ir::Index idx   = b.binary(ir::Inst::Tag::Srl, prod, shift, l);
    ir::Index       acc   = b.constant(svint_to_ref(values.back(), target), l);
    for (size_t k = values.size() - 1; k-- > 0;) {
        const ir::Index eq =
          b.binary(ir::Inst::Tag::Eq, idx, b.constant(module_.pool.put_int(u64, k), l), l);
        acc = b.select(eq, b.constant(svint_to_ref(values[k], target), l), acc, l);
    }
    return acc;
}

// Build `clt`'s `$randops(this, op, arg) -> longint`: the per-object built-ins
// whose target layout is only known to the object's own class — `srandom`
// (LRM 18.13.3) and both `rand_mode` forms (LRM 18.8). One slot with an opcode
// rather than four slots: these are rare calls, and the vtable is per class.
void Lowerer::build_randops_body(const slang::ast::ClassType& clt, ir::Ref class_ref, ir::FunctionId id) {
    const ir::SourceLoc l   = to_source_loc(clt.location);
    const ir::Ref       u64 = make_int_type(64, false);
    ir::Builder         b(module_);
    const ClassRand&    rd = class_rand_.at(&clt);
    if (rd.present) {
        const ir::Index self = b.unary(ir::Inst::Tag::Load, b.local(0, class_ref, l), l);
        const ir::Index body = b.handle_deref(self, class_ref, l);
        const ir::Index op   = b.unary(ir::Inst::Tag::Load, b.local(1, u64, l), l);
        const ir::Index arg  = b.unary(ir::Inst::Tag::Load, b.local(2, u64, l), l);
        const ir::Index zero = b.constant(module_.pool.put_int(u64, 0), l);
        const ir::Index one  = b.constant(module_.pool.put_int(u64, 1), l);
        const auto      mode_lv = [&] { return b.member_select(body, rd.mode_field, class_ref, l); };
        const auto      is_op   = [&](uint64_t which) {
            return b.binary(ir::Inst::Tag::Eq, op, b.constant(module_.pool.put_int(u64, which), l), l);
        };
        const auto arm = [&](uint64_t which, auto&& emit) {
            b.open_region();
            emit();
            const ir::ExtraSlice then_region = b.close_region();
            b.open_region();
            const ir::ExtraSlice else_region = b.close_region();
            b.cond(is_op(which), then_region, else_region, l);
        };
        arm(kRandOpSrandom, [&] { b.obj_rand_seed(b.member_select(body, rd.rng_field, class_ref, l), arg, l); });
        arm(kRandOpModeAll, [&] {
            const ir::Index all = b.constant(module_.pool.put_int(u64, ~0ull), l);
            const ir::Index on  = b.binary(ir::Inst::Tag::Ne, arg, zero, l);
            const ir::Index dst = mode_lv();
            const ir::Index val = b.select(on, all, zero, l);
            b.binary(ir::Inst::Tag::Store, dst, val, l);
        });
        arm(kRandOpModeOne, [&] {
            const ir::Index ord = b.binary(ir::Inst::Tag::Srl, arg, one, l);
            const ir::Index bit = b.binary(ir::Inst::Tag::Sll, one, ord, l);
            const ir::Index cur = b.unary(ir::Inst::Tag::Load, mode_lv(), l);
            const ir::Index on  = b.binary(ir::Inst::Tag::Ne, b.binary(ir::Inst::Tag::Band, arg, one, l), zero, l);
            const ir::Index set = b.binary(ir::Inst::Tag::Bor, cur, bit, l);
            const ir::Index clr = b.binary(ir::Inst::Tag::Band, cur, b.unary(ir::Inst::Tag::Bnot, bit, l), l);
            const ir::Index dst = mode_lv();
            const ir::Index val = b.select(on, set, clr, l);
            b.binary(ir::Inst::Tag::Store, dst, val, l);
        });
        arm(kRandOpModeQuery, [&] {
            const ir::Index cur = b.unary(ir::Inst::Tag::Load, mode_lv(), l);
            b.unary(ir::Inst::Tag::Ret,
                    b.binary(ir::Inst::Tag::Band, b.binary(ir::Inst::Tag::Srl, cur, arg, l), one, l),
                    l);
        });
    }
    b.unary(ir::Inst::Tag::Ret, b.constant(module_.pool.put_int(u64, 0), l), l);
    module_.set_function(id,
                         ir::Function{ .name        = std::string(clt.name) + ".randops",
                                       .return_type = u64,
                                       .param_types = std::vector<ir::Ref>{ class_ref, u64, u64 },
                                       .arg_dirs    = std::vector<ir::ArgDir>{ ir::ArgDir::In,
                                                                               ir::ArgDir::In,
                                                                               ir::ArgDir::In },
                                       .local_types = std::vector<ir::Ref>{ class_ref, u64, u64 },
                                       .span        = span_of(clt),
                                       .ir          = b.finish() });
}

// `obj.randomize()`, a bare `randomize()` in a class method, and
// `std::randomize(...)` (LRM 18.4 / 18.12). slang rewrites all of them into one
// system call named "randomize" (CallExpression::fromLookup), so they are
// separated here: a scope randomize's arguments are lvalue-bound, which slang
// wraps in an AssignmentExpression.
ir::Index Lowerer::lower_randomize_call(const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
    const auto& sci = std::get<1>(call.subroutine);
    if (const auto* rc = std::get_if<slang::ast::CallExpression::RandomizeCallInfo>(&sci.extraInfo);
        rc != nullptr && rc->inlineConstraints != nullptr) {
        // TODO: inline constraints (LRM 18.7) — step 2, the solver.
        fail("inline constraints on randomize() are not supported");
    }
    const auto args = call.arguments();
    if (!args.empty() && args[0]->kind == ExpressionKind::Assignment) {
        return lower_scope_randomize(call, b, l);
    }
    ir::Index                    self{};
    const slang::ast::ClassType* clt = nullptr;
    size_t                       k   = 0;
    if (!args.empty() && args[0]->type->isClass()) {
        self = lower_expr(*args[0], b);
        clt  = &args[0]->type->getCanonicalType().as<slang::ast::ClassType>();
        k    = 1;
    } else {
        std::tie(self, clt) = implicit_this(b, l);
    }
    // The active-set filter (LRM 18.11): every rand property by default, the
    // named subset when arguments are given, and none for `randomize(null)`.
    // The callee intersects it with the object's own rand_mode mask.
    uint64_t mask = ~0ull;
    if (k < args.size()) {
        mask = 0;
        for (; k < args.size(); ++k) {
            if (args[k]->type->isNull()) {
                continue;
            }
            const slang::ast::Symbol* sym = args[k]->getSymbolReference();
            const auto                it  = sym != nullptr ? rand_ordinal_.find(sym) : rand_ordinal_.end();
            if (it == rand_ordinal_.end()) {
                // TODO: LRM 18.11 lets randomize(x) name a non-rand property,
                // making it random for that call only; the mask has no bit for one.
                fail("randomize() argument naming a non-rand property is not supported");
            }
            mask |= 1ull << it->second;
        }
    }
    return emit_randomize_dispatch(*clt, self, mask, b, l);
}

// `std::randomize(a, b, ...)` (LRM 18.12). The targets are ordinary variables, so
// there is no object and the draws come from the CALLING THREAD's RNG — which is
// also why this needs no new machinery: `$urandom`'s existing analysis already
// gives the process its own stream.
ir::Index Lowerer::lower_scope_randomize(const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
    const auto  draw = [&] { return b.urandom(l); };
    std::function<ir::Index()> drawer = draw;
    for (const Expression* arg : call.arguments()) {
        const Expression& target = arg->kind == ExpressionKind::Assignment
                                     ? arg->as<slang::ast::AssignmentExpression>().left()
                                     : *arg;
        const slang::ast::Type& ty = target.type->getCanonicalType();
        if (const char* why = rand_type_unsupported(ty); why != nullptr) {
            fail(why);
        }
        if (ty.isClass()) {
            // TODO: std::randomize of a class handle (LRM 18.12) — the recursion
            // needs the handle's own randomize, which is a virtual dispatch here.
            fail("std::randomize of a class handle is not supported");
        }
        emit_rand_value(lower_lvalue(target, b), ty, drawer, b, loc(target));
    }
    return b.constant(module_.pool.put_int(lower_type(*call.type), 1), l);
}

// `x.rand_mode()` / `x.rand_mode(on)` on one rand property (LRM 18.8). slang
// models this as a system method whose receiver is arguments()[0]; the property's
// ordinal is static, only the address of the mask it lives in is not, so the work
// is done by the object's own `$randops`.
ir::Index Lowerer::lower_rand_mode_call(const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
    const auto args = call.arguments();
    if (args.empty()) {
        fail("rand_mode requires a property receiver");
    }
    const Expression&         target = *args[0];
    const slang::ast::Symbol* sym    = target.getSymbolReference();
    const auto                it     = sym != nullptr ? rand_ordinal_.find(sym) : rand_ordinal_.end();
    if (it == rand_ordinal_.end()) {
        if (const auto* prop = sym != nullptr ? sym->as_if<slang::ast::ClassPropertySymbol>() : nullptr;
            prop != nullptr && prop->randMode != slang::ast::RandMode::None) {
            // A WHOLE rand property with no ordinal: its declaring class's rand
            // surface is unsupported (ClassRand::unsupported), so no rand block
            // -- and no mode mask -- was ever laid out. Same run-time refusal as
            // that class's generated $randomize body, and for the same reason:
            // uvm_reg_field::get_rand_mode is `value.rand_mode()` in a class the
            // factory registers whether or not the design ever randomizes it.
            if (const auto* declaring = prop->getParentScope()->asSymbol().as_if<slang::ast::ClassType>()) {
                if (const auto rit = class_rand_.find(declaring);
                    rit != class_rand_.end() && !rit->second.unsupported.empty()) {
                    const std::string banner = banner_at(target.sourceRange.start());
                    b.severity(static_cast<uint32_t>(ir::Severity::Fatal),
                               severity_banner(target.sourceRange.start()), rit->second.unsupported, {}, l);
                    b.finish(banner + "$fatal", 1, l);
                    return b.constant(module_.pool.put_int(lower_type(*call.type), 0), l);
                }
            }
        }
        // TODO: rand_mode on a SUBELEMENT of a rand property (LRM 18.8) — it
        // needs a per-element enable, not one bit per property.
        fail("rand_mode is supported on a whole rand property only");
    }
    const uint32_t               ordinal = it->second;
    ir::Index                    self{};
    const slang::ast::ClassType* clt = nullptr;
    if (target.kind == ExpressionKind::MemberAccess) {
        const Expression& recv = target.as<slang::ast::MemberAccessExpression>().value();
        if (!recv.type->isClass()) {
            fail("rand_mode on a non-class receiver is not supported");
        }
        self = lower_expr(recv, b);
        clt  = &recv.type->getCanonicalType().as<slang::ast::ClassType>();
    } else {
        std::tie(self, clt) = implicit_this(b, l);
    }
    const ir::Ref u64 = make_int_type(64, false);
    ir::Index     op  = b.constant(module_.pool.put_int(u64, kRandOpModeQuery), l);
    ir::Index     arg = b.constant(module_.pool.put_int(u64, ordinal), l);
    if (args.size() > 1) {
        // The task form: `ordinal << 1 | (on != 0)`.
        const ir::Index raw  = b.convert(lower_expr(*args[1], b), u64, false, l);
        const ir::Index zero = b.constant(module_.pool.put_int(u64, 0), l);
        const ir::Index on   = b.binary(ir::Inst::Tag::Ne, raw, zero, l);
        op                   = b.constant(module_.pool.put_int(u64, kRandOpModeOne), l);
        const ir::Index tag  = b.constant(module_.pool.put_int(u64, uint64_t{ ordinal } << 1), l);
        const ir::Index flag = b.convert(on, u64, false, l);
        arg                  = b.binary(ir::Inst::Tag::Bor, tag, flag, l);
    }
    return b.convert(
      emit_randops_dispatch(*clt, self, op, arg, b, l), lower_type(*call.type), call.type->isSigned(), l);
}

// The vtable slot of one of `clt`'s randomization entry points, and the
// FunctionId that supplies the slot's signature. Both are set by compute_vtable,
// which a class still laying itself out has not reached — reachable only through
// a property-type cycle whose method body randomizes the very class it is a
// property of.
//
// TODO: randomize() of a class whose layout is still in progress. It needs the
// call itself deferred, the way the generated bodies already are.
uint32_t Lowerer::rand_slot_of(const slang::ast::ClassType& clt, std::string_view slot) {
    const auto it = class_vslot_.find(&clt);
    if (it == class_vslot_.end()) {
        fail("randomization of a class that is still being laid out is not supported");
    }
    return it->second.at(slot);
}

ir::FunctionId Lowerer::rand_slot_sig(
  const slang::ast::ClassType&                                                      clt,
  const ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::FunctionId>& map) {
    const auto it = map.find(&clt);
    if (it == map.end()) {
        fail("randomization of a class that is still being laid out is not supported");
    }
    return it->second;
}

// Signature source for a randomization dispatch through an INTERFACE-class
// handle (LRM 8.26.9 makes `randomize()` legal there). Every class's synthesized
// body has the same shape -- `(this, mask) -> int` for randomize,
// `(this, op, arg) -> u64` for randops -- but no concrete class is named at such
// a site, so a never-executed stub over the interface Ref supplies the
// FunctionType, exactly as build_pure_stub does for a pure virtual.
ir::FunctionId Lowerer::iface_rand_sig(ir::Ref iface_ref, bool ops) {
    auto& cache = ops ? iface_randops_sig_ : iface_randomize_sig_;
    if (const auto it = cache.find(iface_ref); it != cache.end()) {
        return it->second;
    }
    const ir::Ref        u64 = make_int_type(64, false);
    const ir::Ref        i32 = make_int_type(32, false);
    const ir::Ref        ret = ops ? u64 : i32;
    std::vector<ir::Ref> params =
      ops ? std::vector<ir::Ref>{ iface_ref, u64, u64 } : std::vector<ir::Ref>{ iface_ref, u64 };
    std::vector<ir::ArgDir> dirs(params.size(), ir::ArgDir::In);
    std::vector<ir::Ref>    locals = params;
    ir::Builder             b(module_);
    b.unary(ir::Inst::Tag::Ret, b.constant(module_.pool.put_int(ret, 0), ir::SourceLoc{}), ir::SourceLoc{});
    const ir::FunctionId id = module_.reserve_function(ret);
    module_.set_function(id,
                         ir::Function{ .name        = ops ? "$iface.randops" : "$iface.randomize",
                                       .return_type = ret,
                                       .param_types = std::move(params),
                                       .arg_dirs    = std::move(dirs),
                                       .local_types = std::move(locals),
                                       .ir          = b.finish() });
    cache.emplace(iface_ref, id);
    return id;
}

ir::Index Lowerer::emit_randops_dispatch(const slang::ast::ClassType& clt,
                                         ir::Index                    self,
                                         ir::Index                    op,
                                         ir::Index                    arg,
                                         ir::Builder&                 b,
                                         ir::SourceLoc                l) {
    const ir::Ref                  ref = lower_class_type(clt);
    const std::array<ir::Index, 3> args{ self, op, arg };
    if (clt.isInterface) {
        // Through an interface handle the reserved slot is unreachable by index
        // (it is numbered per hierarchy), so the design-wide interface index of
        // the same reserved NAME carries the dispatch.
        return b.call_virtual(iface_rand_sig(ref, /*ops=*/true), 0, ref, args, l, iface_slot(kRandOpsSlot));
    }
    return b.call_virtual(rand_slot_sig(clt, randops_fn_), rand_slot_of(clt, kRandOpsSlot), ref, args, l);
}

ir::Index Lowerer::emit_randomize_dispatch(const slang::ast::ClassType& clt,
                                           ir::Index                    self,
                                           uint64_t                     mask,
                                           ir::Builder&                 b,
                                           ir::SourceLoc                l) {
    const ir::Ref                  ref = lower_class_type(clt);
    const ir::Ref                  u64 = make_int_type(64, false);
    const std::array<ir::Index, 2> args{ self, b.constant(module_.pool.put_int(u64, mask), l) };
    if (clt.isInterface) {
        return b.call_virtual(
          iface_rand_sig(ref, /*ops=*/false), 0, ref, args, l, iface_slot(kRandomizeSlot));
    }
    return b.call_virtual(rand_slot_sig(clt, randomize_fn_), rand_slot_of(clt, kRandomizeSlot), ref, args, l);
}

} // namespace clotho::lower
