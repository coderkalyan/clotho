// Subroutines: calls, argument passing, capture boxes, task and function bodies, and DPI.

#include "lower/Lowerer.h"

namespace clotho::lower {

void Lowerer::lower_call(const CallExpression& call, ir::Builder& b) {
    if (!call.isSystemCall()) {
        // A user subroutine invoked as a statement: a function (void, or a
        // non-void whose result is discarded) or a task enable. Lower for side
        // effects; lower_user_call routes a task to lower_task_call.
        lower_user_call(call, b);
        return;
    }
    const std::string_view name = call.getSubroutineName();
    // A string method used as a statement (s.putc(i, c); the others are
    // legal-but-pointless): lower for its effect, discard the value.
    if (!name.empty() && name[0] != '$' && !call.arguments().empty() && is_string_expr(*call.arguments()[0])) {
        lower_string_method(name, call, b);
        return;
    }
    // A container method as a statement (arr.delete(), push/pop/insert, a
    // manipulation method whose result is unused): lower for effect, discard
    // the value.
    if (lower_container_method(call, b, loc(call))) {
        return;
    }
    // A randomization system call as a statement: `obj.randomize();` with the
    // result discarded, and the task forms of rand_mode / constraint_mode.
    if (name == "randomize" || name == "rand_mode" || name == "constraint_mode") {
        lower_sysfunc(call, b);
        return;
    }
    if (const char rx = display_radix(name, "$display")) {
        lower_display(call, /*newline=*/true, b, rx);
        return;
    }
    if (const char rx = display_radix(name, "$write")) {
        lower_display(call, /*newline=*/false, b, rx);
        return;
    }
    if (const char rx = display_radix(name, "$strobe")) {
        lower_strobe(call, b, rx, /*fd_arg=*/false);
        return;
    }
    if (const char rx = display_radix(name, "$fstrobe")) {
        lower_strobe(call, b, rx, /*fd_arg=*/true);
        return;
    }
    if (const char rx = display_radix(name, "$monitor")) {
        lower_monitor(call, b, rx, /*fd_arg=*/false);
        return;
    }
    if (const char rx = display_radix(name, "$fmonitor")) {
        lower_monitor(call, b, rx, /*fd_arg=*/true);
        return;
    }
    if (name == "$monitoron" || name == "$monitoroff") {
        b.monitor_enable(name == "$monitoron", loc(call));
        return;
    }
    if (const char rx = display_radix(name, "$fdisplay")) {
        lower_fdisplay(call, /*newline=*/true, b, rx);
        return;
    }
    if (const char rx = display_radix(name, "$fwrite")) {
        lower_fdisplay(call, /*newline=*/false, b, rx);
        return;
    }
    if (const char rx = display_radix(name, "$swrite")) {
        lower_sformat(call, /*is_sformat=*/false, b, rx);
        return;
    }
    if (name == "$sformat") {
        lower_sformat(call, /*is_sformat=*/true, b);
        return;
    }
    if (name == "$fclose") {
        if (call.arguments().size() != 1) {
            fail("$fclose requires a single file-descriptor argument");
        }
        b.fclose(lower_expr(*call.arguments()[0], b), loc(call));
        return;
    }
    if (name == "$fgets" || name == "$fgetc" || name == "$ungetc" || name == "$feof" || name == "$ftell" ||
        name == "$fseek" || name == "$rewind" || name == "$ferror" || name == "$fscanf" || name == "$sscanf" ||
        name == "$fread" || name == "$test$plusargs" || name == "$value$plusargs") {
        // File read / positioning / error / scan functions, and plusargs,
        // used in statement position: lower for the side effect (file
        // position, write-back) and drop the result.
        lower_sysfunc(call, b);
        return;
    }
    if (name == "$system") {
        // Task form: run the command, discard the return value.
        lower_system_call(call, b);
        return;
    }
    if (name == "$finish" || name == "$stop") {
        // Both end the simulation and exit. LRM §20.2 defines $stop as
        // suspending the run (vs $finish exiting to the OS) and mandates no
        // interactive mode; Clotho cannot resume a suspended run, so a
        // suspend-forever equals exit and the two are treated identically
        // apart from the banner's task name.
        //
        // The optional 0/1/2 argument selects the exit-message verbosity
        // (LRM Table 20-1); slang guarantees it is a constant, defaulting to
        // 1 when omitted. The runtime prints the banner (time + location) for
        // level >= 1.
        uint32_t level = 1;
        if (!call.arguments().empty()) {
            slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
            slang::ast::EvalContext    ectx(actx);
            const slang::ConstantValue cv = call.arguments()[0]->eval(ectx);
            if (cv.isInteger()) {
                level = cv.integer().as<uint32_t>().value_or(1);
            }
        }
        b.finish(finish_banner(call, name), level, loc(call));
        return;
    }
    if (name == "$info" || name == "$warning" || name == "$error" || name == "$fatal") {
        lower_severity_task(call, name, b);
        return;
    }
    if (name == "$fflush") {
        // No-argument form flushes every open output buffer; the runtime reads
        // a 0 descriptor as "flush all" (LRM §21.3.6). The single-argument form
        // flushes one MCD / FD.
        ir::Index handle;
        if (call.arguments().empty()) {
            const ir::Ref u32 = make_int_type(32, false, false);
            handle            = b.constant(module_.pool.put_int(u32, 0), loc(call));
        } else {
            handle = lower_expr(*call.arguments()[0], b);
        }
        b.fflush(handle, loc(call));
        return;
    }
    if (name == "$printtimescale") {
        lower_printtimescale(call, b);
        return;
    }
    if (name == "$timeformat") {
        lower_timeformat(call, b);
        return;
    }
    if (name == "$readmemh" || name == "$readmemb") {
        lower_readmem(call, name == "$readmemh", b);
        return;
    }
    if (name == "$dumpfile") {
        // $dumpfile(name) (LRM §21.7.1.1): set the output path.
        if (call.arguments().size() != 1) {
            fail("$dumpfile requires one filename argument");
        }
        b.dump_file(lower_expr(*call.arguments()[0], b), loc(call));
        return;
    }
    if (name == "$dumpvars") {
        lower_dumpvars(call, b);
        return;
    }
    if (name == "$dumpoff") {
        b.dump_control(ir::Inst::Tag::DumpOff, loc(call));
        return;
    }
    if (name == "$dumpon") {
        b.dump_control(ir::Inst::Tag::DumpOn, loc(call));
        return;
    }
    if (name == "$dumpall") {
        b.dump_control(ir::Inst::Tag::DumpAll, loc(call));
        return;
    }
    if (name == "$dumpflush") {
        b.dump_control(ir::Inst::Tag::DumpFlush, loc(call));
        return;
    }
    if (name == "$dumplimit") {
        // $dumplimit(n) (LRM §21.7.1.5): cap the dump file at n bytes.
        if (call.arguments().size() != 1) {
            fail("$dumplimit requires one byte-count argument");
        }
        b.dump_limit(lower_expr(*call.arguments()[0], b), loc(call));
        return;
    }
    if (name == "$cast") {
        // Task form (LRM 6.24.2): the same conditional assignment, but a failed
        // cast is a run-time error rather than a 0 the caller reads.
        (void)lower_cast(call, b, /*task_form=*/true);
        return;
    }
    if (is_assert_control_task(name)) {
        // LRM 20.12. slang registers all ten names on one AssertControlTask,
        // so the arity and the shape of every argument are already checked.
        lower_assert_control(name, call, b);
        return;
    }
    if (name == "$exit") {
        // LRM 24.7: "Calling $exit from a thread or its descendent thread
        // originating in an initial procedure of a program block shall terminate
        // all initial procedures and their descendent threads within that
        // originating program block. Calling $exit from a thread or its
        // descendent thread that does not originate in an initial procedure in a
        // program shall be ignored, and a warning may be issued to indicate that
        // the call to $exit has been ignored."
        //
        // The clause scopes the effect by the THREAD, not by the lexical site, so
        // only one case is decidable here: a call lexically in a design PROCESS
        // (not inside a program, not inside a subroutine) can only ever be
        // reached by a design thread, so it is the "shall be ignored" arm and
        // nothing is emitted. Every other site emits the op and the backend names
        // the owning program instance from the body the op lands in -- which is
        // what a fork child of a program `initial`, and a subroutine, need.
        if (!in_program_ && fn_ctx_ == nullptr) {
            report(Severity::Warning,
                   "program-exit-ignored",
                   "$exit outside a program block is ignored (LRM 24.7): it terminates the "
                   "`initial` procedures of the program the calling thread originates in, and "
                   "this thread originates in design code",
                   to_source_span(call.sourceRange));
            return;
        }
        b.program_exit(loc(call));
        return;
    }
    fail("unsupported system task '" + std::string(name) + "'");
}

// The synthetic one-field class a captured automatic of type `inner` lives in
// (docs/design/fork-join.md V4). Interned per inner type.
//
// A class and not a new kind of heap cell because a class already is everything a
// box needs: ClassNew allocates it on the GC fast path, its trace map falls out of
// the field table (so a boxed handle is traced precisely and a boxed `int` is
// traced not at all), a handle has reference semantics -- which IS the sharing
// 9.3.2 asks for -- and the place is HandleDeref + MemberSelect 0. It is the shape
// lower_process_class already uses for the PCB handle.
//
// Nothing here is keyed on a slang type: set_class_desc takes a bare Ref, and the
// id is minted straight off the counter without joining the symbol map. A box has
// no methods, is never a base and is never `$cast` to, so its slot table is empty.
ir::Ref Lowerer::box_class_type(ir::Ref inner) {
    return box_class_impl(inner, /*notified=*/false, /*container=*/false);
}

// The notified variant: field 1 is the box's change-notifier event, registered
// with the backend's store path (set_change_notify) exactly like a waited class
// property's notifier, so every store / mutation of the boxed value fires it.
// `container` selects the mutation-fired flavour (a queue / dynamic / assoc
// array's value is its contents, so there is no old -> new word to classify).
ir::Ref Lowerer::box_class_type_notified(ir::Ref inner, bool container) {
    return box_class_impl(inner, /*notified=*/true, container);
}

ir::Ref Lowerer::box_class_impl(ir::Ref inner, bool notified, bool container) {
    auto& interned = notified ? box_class_notified_ : box_class_;
    if (const auto it = interned.find(inner); it != interned.end()) {
        return it->second;
    }
    const uint32_t fid = module_.pool.reserve_struct_fields();
    ir::Type       t;
    t.kind      = ir::Type::Kind::Class;
    t.fields_id = fid;
    t.element   = ir::Ref::Invalid;
    const ir::Ref ref = module_.pool.put(ir::Item{ t });
    // Named, like the `$capture` field it holds, so the IR dump reads
    // `class $capture` rather than a bare Ref. A design that boxes two different
    // types mints two of these, and the printer then qualifies both labels with
    // their Ref (see InternPool::class_label).
    module_.pool.set_class_name(ref, kCaptureBoxField);
    std::vector<ir::FieldRow> fields;
    fields.push_back({ kCaptureBoxField, inner, 0 });
    if (notified) {
        fields.push_back({ kCaptureBoxChg, module_.pool.event_type, 1 });
        // Any-change only: `wait` re-tests its condition on every wake (LRM
        // 9.4.3), and `@` on a captured automatic is not routed here.
        module_.set_change_notify(ref, 0, ir::Edge::Any, 1, container);
        notified_box_refs_.insert(static_cast<uint32_t>(ref));
    }
    module_.pool.set_struct_fields(fid, std::move(fields));
    module_.set_class_desc(ref, std::vector<ir::FunctionId>{}, class_ids_->next++);
    interned.emplace(inner, ref);
    return ref;
}

// The box class for one captured symbol: the notified variant iff some `wait`
// condition in the design reads the symbol AND its type can carry a notifier.
// The design-wide pre-pass and the wait lowering MUST agree on notifiability,
// so both go through box_can_notify; a waited symbol of a type that cannot
// notify gets the plain box here and lower_wait_stmt refuses it by name.
ir::Ref Lowerer::box_class_for(const ValueSymbol& vs, ir::Ref inner) {
    if (waited_props_ == nullptr || !waited_props_->waits_local(vs)) {
        return box_class_type(inner);
    }
    const slang::ast::Type& ct = vs.getType().getCanonicalType();
    if (!box_can_notify(ct)) {
        return box_class_type(inner);
    }
    return box_class_type_notified(inner, is_notified_container(ct));
}

// Whether `cls` is a notified box class, i.e. carries the `$chg.$capture`
// field at index 1. Read by the wait lowering to build the park edge.
bool Lowerer::box_is_notified(ir::Ref cls) const {
    return notified_box_refs_.contains(static_cast<uint32_t>(cls));
}

// The lvalue of a box's single field, given the box HANDLE as a value. Split out
// because a box's home is a frame slot for a captured automatic and a design-state
// signal for a fork-scope declaration in a process body (proc_fork_boxes_).
ir::Index Lowerer::box_field_lvalue(ir::Index handle, ir::Ref cls, ir::Builder& b, ir::SourceLoc l) {
    return b.member_select(b.handle_deref(handle, cls, l), 0, cls, l);
}

// The lvalue of a boxed local: dereference the handle in its frame slot and select
// the single field. Re-emitted per access, like every other place.
ir::Index Lowerer::boxed_lvalue(const FnCtx::Boxed& bx, ir::Builder& b, ir::SourceLoc l) {
    return box_field_lvalue(b.unary(ir::Inst::Tag::Load, b.local(bx.local, bx.cls, l), l), bx.cls, b, l);
}

// Whether `vs` resolves through a capture box at this point in lowering: a
// boxed automatic of the current frame (a fork child's capture formal included)
// or a fork-scope declaration of a fork in a PROCESS body. Same precedence as
// value_lvalue's, so the wait lowering and the access path answer identically.
bool Lowerer::is_boxed_symbol(const ValueSymbol& vs) const {
    if (fn_ctx_ != nullptr && fn_ctx_->boxed.contains(&vs)) {
        return true;
    }
    return proc_fork_boxes_.contains(&vs);
}

// The loaded event handle of a boxed local's change notifier (box field 1),
// for a `wait` edge. The box handle is loaded HERE, inside the wait loop's
// body, so a re-entered fork that re-boxes is re-read on every pass.
ir::Index Lowerer::boxed_notifier_handle(const ValueSymbol& vs, ir::Builder& b, ir::SourceLoc l) {
    ir::Index handle{};
    ir::Ref   cls = ir::Ref::Invalid;
    if (fn_ctx_ != nullptr) {
        if (const auto bx = fn_ctx_->boxed.find(&vs); bx != fn_ctx_->boxed.end()) {
            handle = b.unary(ir::Inst::Tag::Load, b.local(bx->second.local, bx->second.cls, l), l);
            cls    = bx->second.cls;
        }
    }
    if (cls == ir::Ref::Invalid) {
        const auto fb = proc_fork_boxes_.find(&vs);
        if (fb == proc_fork_boxes_.end()) {
            fail("internal: no capture box for waited automatic '" + std::string(vs.name) + "'");
        }
        handle = b.unary(ir::Inst::Tag::Load, b.signal(fb->second.sig, l), l);
        cls    = fb->second.cls;
    }
    if (!box_is_notified(cls)) {
        // The pre-pass and box_class_for read the same resolver, so a plain box
        // here means the two disagreed -- a lowering bug, not a design refusal.
        fail("internal: waited automatic '" + std::string(vs.name) +
             "' was boxed without a change notifier");
    }
    return b.unary(ir::Inst::Tag::Load,
                   b.member_select(b.handle_deref(handle, cls, l), 1, cls, l), l);
}

// The enclosing method's `this` handle, as a value. One helper rather than four
// copies of the load, because `this` is now a capturable automatic like any other:
// when a fork child reads a class property of the enclosing method, `this` is boxed
// and the handle has to be read out of the box.
ir::Index Lowerer::this_handle(ir::Builder& b, ir::SourceLoc l) {
    if (fn_ctx_->this_var != nullptr) {
        if (const auto bx = fn_ctx_->boxed.find(fn_ctx_->this_var); bx != fn_ctx_->boxed.end()) {
            return b.unary(ir::Inst::Tag::Load, boxed_lvalue(bx->second, b, l), l);
        }
    }
    return b.unary(ir::Inst::Tag::Load, b.local(fn_ctx_->this_local, fn_ctx_->this_class, l), l);
}

// Allocate a box for every automatic of THIS activation that a detaching fork's
// children reference, before the body is lowered (docs/design/fork-join.md V4).
//
// Entry, not first use: a box has to exist on every path that can reach an access,
// and first use may sit inside a conditional region. One ClassNew per boxed
// variable per activation is the cost, paid only by a body that actually spawns a
// detached child touching its locals.
//
// A boxed symbol keeps its localmap entry -- for a formal that is the ABI's
// copy-in slot, which stays formal k == Local k; for a declared local it is what
// ForkCaptureDetector matches against, so the child still receives it. value_lvalue
// consults `boxed` first, so the localmap slot of a boxed local is never read.
void Lowerer::install_capture_boxes(const SubroutineSymbol& sub, const Statement& body, ir::Builder& b) {
    BoxCandidateScan scan;
    body.visit(scan);
    if (scan.candidates.empty() && !scan.implicit_this) {
        // `implicit_this` on its own is enough: a child whose only reference to the
        // enclosing activation is a BARE METHOD CALL (`bump();`) names no value
        // symbol at all, so `candidates` is empty while `this` still has to travel.
        return;
    }
    const ir::SourceLoc l = to_source_loc(sub.location);
    // Formals first, in declaration order, then automatics declared in the body:
    // a stable order keeps the emitted IR deterministic.
    for (const FormalArgumentSymbol* fa : sub.getArguments()) {
        if (!scan.candidates.contains(fa)) {
            continue;
        }
        if (fa->direction != slang::ast::ArgumentDirection::In) {
            // TODO: capture of an output / inout formal. Its copy-out reads the
            // formal's own frame slot, which does not see writes made through the
            // box, so the box would have to be copied back before every return.
            // Raised here, where the fork's join kind is not in scope, so the
            // wording names the construct rather than a kind.
            fail("a fork child that reads the enclosing subroutine's '" + std::string(fa->name) +
                 "' is not supported: capturing an output or inout formal would lose "
                 "the writes made through it");
        }
        box_one_capture(*fa, /*copy_in=*/true, b, l);
    }
    for (const ValueSymbol* vs : body_automatics(sub, body)) {
        // A fork's OWN block_item_declaration is skipped here: lower_fork gives it
        // a fresh box per ENTRY to the fork, and a per-activation box would make it
        // one variable shared by every entry (LRM 9.3.2; the clause's example would
        // print 333 instead of 123).
        if (scan.candidates.contains(vs) && !scan.fork_scope_decls.contains(vs)) {
            box_one_capture(*vs, /*copy_in=*/false, b, l);
        }
    }
    // `this` last, and only if a child needs it. It is an ordinary automatic of
    // this frame -- slang makes it a VariableSymbol with Automatic lifetime -- but
    // it is in neither getArguments() nor the body's declarations, so neither loop
    // above reaches it. `copy_in` is right for the same reason it is right for a
    // formal: splice_this already gave it Local 0 and the backend's prologue fills
    // that slot from argument 0. It is also `const`, so the box's only job is
    // lifetime -- but uniformity is worth more here than saving one indirection in
    // a method that spawns.
    if (sub.thisVar != nullptr && (scan.implicit_this || scan.candidates.contains(sub.thisVar))) {
        box_one_capture(*sub.thisVar, /*copy_in=*/true, b, l);
    }
}

// One box: a frame slot holding the handle, a ClassNew into it, and for a formal
// the one store that moves the incoming argument into the box.
void Lowerer::box_one_capture(const ValueSymbol& vs, bool copy_in, ir::Builder& b, ir::SourceLoc l) {
    if (fn_ctx_->boxed.contains(&vs)) {
        return;
    }
    const ir::Ref  inner = lower_type(vs.getType());
    const ir::Ref  cls   = box_class_for(vs, inner);
    const uint32_t slot  = static_cast<uint32_t>(fn_ctx_->local_types.size());
    fn_ctx_->local_types.push_back(cls);
    const ir::Index dst = b.local(slot, cls, l);
    const ir::Index obj = b.class_new(cls, l);
    b.binary(ir::Inst::Tag::Store, dst, obj, l);
    if (copy_in) {
        // The formal's own Local, which the backend's prologue has filled.
        const uint32_t  k       = fn_local(vs);
        const ir::Index box_lv  = boxed_lvalue({ slot, cls }, b, l);
        const ir::Index arg_val = b.unary(ir::Inst::Tag::Load, b.local(k, inner, l), l);
        b.binary(ir::Inst::Tag::Store, box_lv, arg_val, l);
    }
    // Deliberately NOT registered in localmap for a declared local: a boxed local
    // has no frame slot of its own, so lower_var_decl cannot initialize the
    // variable through a slot typed as the box. value_lvalue routes every access
    // (the declaration's own initializer included) through the box.
    fn_ctx_->boxed[&vs] = { slot, cls };
}

// Give one of a fork's own block_item_declarations (LRM 9.3.2) its per-ENTRY box,
// and emit the allocation. Called from lower_fork at the fork statement, so the
// ClassNew below runs on every EXECUTION of the fork while the home holding the
// handle is allocated once, at lower time. That split is the whole mechanism:
//
//   for (int j = 1; j <= 3; ++j) fork automatic int k = j; ... join_none
//
// executes the fork three times, so three boxes exist and each iteration's
// children hold the one made for them -- the clause's "local copy, k, for each
// value of j", which prints 123. A per-activation box (install_capture_boxes) would
// be ONE box written three times and every child would read the last value, 3.
//
// The handle is stored into its home immediately, before any initializer can
// allocate, so the box is reachable from a GC root for its whole life: a frame slot
// in a subroutine (a conservative pinned frame root) or a design-state signal in a
// process body, where there is no frame (a precise root -- an `initial` block's
// storage is design state, docs/design/garbage-collection.md 5.4).
void Lowerer::install_fork_decl_box(const VariableSymbol& vs, ir::Builder& b, ir::SourceLoc l) {
    const ir::Ref cls = box_class_for(vs, lower_type(vs.getType()));
    if (fn_ctx_ != nullptr) {
        // Reuse the ordinary capture-box home so value_lvalue, ForkCaptureDetector
        // and a nested fork's own capture of this variable all need nothing new.
        const auto it = fn_ctx_->boxed.find(&vs);
        const uint32_t slot = it != fn_ctx_->boxed.end() ? it->second.local
                                                         : static_cast<uint32_t>(fn_ctx_->local_types.size());
        if (it == fn_ctx_->boxed.end()) {
            fn_ctx_->local_types.push_back(cls);
            fn_ctx_->boxed[&vs] = { slot, cls };
        }
        const ir::Index dst = b.local(slot, cls, l);
        const ir::Index obj = b.class_new(cls, l);
        b.binary(ir::Inst::Tag::Store, dst, obj, l);
        return;
    }
    const auto it = proc_fork_boxes_.find(&vs);
    if (it == proc_fork_boxes_.end()) {
        ir::SignalInfo info;
        info.name = prefix_ + std::string(vs.name) + ".$forkbox";
        info.loc  = to_source_loc(vs.location);
        info.type = cls;
        info.dir  = ir::SignalInfo::Direction::Internal;
        proc_fork_boxes_[&vs] = { register_signal(std::move(info)), cls };
    }
    const ProcForkBox& pb  = proc_fork_boxes_.at(&vs);
    const ir::Index    dst = b.signal(pb.sig, l);
    const ir::Index    obj = b.class_new(cls, l);
    b.binary(ir::Inst::Tag::Store, dst, obj, l);
}

// The box handle a fork child's formal takes, by value: an rvalue, so nothing the
// child holds points into the parent's activation (docs/design/fork-join.md V4).
ir::Index Lowerer::capture_handle(const ValueSymbol& vs, ir::Builder& b, ir::SourceLoc l) {
    if (fn_ctx_ != nullptr) {
        if (const auto bx = fn_ctx_->boxed.find(&vs); bx != fn_ctx_->boxed.end()) {
            return b.unary(ir::Inst::Tag::Load, b.local(bx->second.local, bx->second.cls, l), l);
        }
    }
    if (const auto fb = proc_fork_boxes_.find(&vs); fb != proc_fork_boxes_.end()) {
        return b.unary(ir::Inst::Tag::Load, b.signal(fb->second.sig, l), l);
    }
    // The two walks that decide captures disagreed, which is a lowering bug rather
    // than a source-level refusal.
    fail("a fork child that reads '" + std::string(vs.name) +
         "' is not supported (internal: no capture box)");
}

// The automatic-lifetime variables `body` declares, in declaration order. Only
// these can box: a static local keeps persistent storage that already outlives
// every child, and anything else in the candidate set is not this frame's.
std::vector<const ValueSymbol*> Lowerer::body_automatics(const SubroutineSymbol& sub, const Statement& body) {
    // Declarations are reached through their STATEMENTS -- a statement visitor does
    // not visit a VariableSymbol on its own -- so both forms that introduce an
    // automatic are picked up here: a declaration statement and a `for`'s own loop
    // variables, which is the shape LRM 9.3.2's example uses.
    struct Scan : ASTVisitor<Scan, VisitFlags::AllGood> {
        std::vector<const ValueSymbol*> out;
        void take(const VariableSymbol& vs) {
            if (vs.lifetime == slang::ast::VariableLifetime::Automatic) {
                out.push_back(&vs);
            }
        }
        void handle(const VariableDeclStatement& d) {
            take(d.symbol);
            visitDefault(d);
        }
        void handle(const ForLoopStatement& f) {
            for (const VariableSymbol* v : f.loopVars) {
                take(*v);
            }
            visitDefault(f);
        }
    };
    Scan sc;
    body.visit(sc);
    (void)sub;
    return std::move(sc.out);
}

// True for a subroutine declared directly in a package or `$unit` scope
// (LRM 6.8's "no instance multiplicity" applies to these exactly as it
// does to a variable): walks up from the subroutine's declaring scope,
// stopping at the first Package/CompilationUnit (global) or InstanceBody
// (module-scope, possibly through an intervening generate block).
bool Lowerer::is_global_subroutine(const SubroutineSymbol& sub) {
    const Scope* s = sub.getParentScope();
    while (s != nullptr) {
        switch (s->asSymbol().kind) {
            case SymbolKind::Package:
            case SymbolKind::CompilationUnit:
                return true;
            case SymbolKind::InstanceBody:
                return false;
            default:
                s = s->asSymbol().getParentScope();
        }
    }
    return false;
}

// Resolve a subroutine (function or task) to a FunctionId, lowering its body
// on first reference. The id is reserved before the body is built so a
// recursive call resolves it. A package/$unit-scope subroutine has no
// instance multiplicity (LRM 6.8): delegate its whole lowering to the
// shared globals_ Lowerer instead of duplicating it (and any static
// local's storage) per top instance.
// The DPI-C C type of a formal / return type (LRM 35.5.6): a by-value scalar
// for the small named types, else the by-pointer chunk-array form (BitVec /
// LogicVec) for a packed vector. nullopt for a type Clotho does not marshal
// (string, chandle, open array, unpacked aggregate) — the caller loud-fails
// those. Never returns Void.
std::optional<ir::DpiCType> Lowerer::dpi_ctype(const slang::ast::Type& type) {
    const slang::ast::Type& ct = type.getCanonicalType();
    if (ct.isFloating()) {
        return ct.as<slang::ast::FloatingType>().floatKind == slang::ast::FloatingType::ShortReal
                 ? ir::DpiCType::Shortreal
                 : ir::DpiCType::Real;
    }
    if (ct.isPredefinedInteger()) {
        switch (ct.as<slang::ast::PredefinedIntegerType>().integerKind) {
            case slang::ast::PredefinedIntegerType::Byte:
                return ir::DpiCType::Byte;
            case slang::ast::PredefinedIntegerType::ShortInt:
                return ir::DpiCType::Shortint;
            case slang::ast::PredefinedIntegerType::Int:
                return ir::DpiCType::Int;
            case slang::ast::PredefinedIntegerType::LongInt:
                return ir::DpiCType::Longint;
            default:
                // `integer` (4-state 32) / `time` (2-state 64): the vec form.
                return ct.isFourState() ? ir::DpiCType::LogicVec : ir::DpiCType::BitVec;
        }
    }
    if (ct.isScalar()) {
        return ct.isFourState() ? ir::DpiCType::LogicScalar : ir::DpiCType::BitScalar;
    }
    if (ct.isCHandle()) {
        return ir::DpiCType::Chandle; // opaque void*
    }
    if (ct.isString()) {
        return ir::DpiCType::String; // char*
    }
    if (ct.isIntegral()) {
        // A packed vector / packed struct / enum: the svBitVecVal /
        // svLogicVecVal chunk-array form (slang has already checked it is a
        // legal DPI arg type via isValidForDPIArg).
        return ct.isFourState() ? ir::DpiCType::LogicVec : ir::DpiCType::BitVec;
    }
    return std::nullopt; // chandle, string, unpacked array / struct, ...
}

// Lower a DPI-C import (LRM 35) into the reserved slot. There is no IR body:
// the backend emits an external C declaration keyed on the linkage name and
// marshals each scalar argument at the call site (lower_dpi_call). Functions
// and tasks; scalar / packed-vector / chandle / string / 1-D open-array
// formals in every direction -- everything else loud-fails with a TODO (see
// docs/design/dpi-c.md).
void Lowerer::build_dpi_import(const SubroutineSymbol& sub, ir::FunctionId id) {
    const bool is_task = sub.subroutineKind == SubroutineKind::Task;
    if (is_task && sub.flags.has(MethodFlags::Pure)) {
        // LRM 35.5.1.3: "An imported task can never be declared pure." slang
        // only warns (-Wdpi-pure-task), and the mistake is not benign here --
        // is_pure marks the C declaration readnone + willreturn, so the
        // optimizer would CSE or delete calls to a task whose whole purpose is
        // its side effects. Refuse rather than silently miscompile.
        fail("an imported DPI task cannot be declared `pure` (LRM 35.5.1.3)");
    }
    // TODO: T2 -- an import task whose C calls an exported task that consumes
    // time. The call site below stays an ordinary plain `Call`, which cannot
    // suspend, so the C frame is not suspendable; the export-task thunk reports
    // a located fatal if the task it drives does not complete in zero time
    // (rt_dpi_export_task_suspended). Lifting that needs the baton-pool stack
    // handoff and a task call site here (docs/design/dpi-c.md, "DPI tasks").
    // `context` (LRM 35.5.3) is accepted and lowered identically to a plain
    // import: its purpose is to let the C call exported SV functions (and the
    // svScope APIs), and an export thunk needs no scope to find its instance —
    // the lowered SV function it calls already addresses that instance's state
    // absolutely. The svScope C API is provided (svGetScope / svSetScope /
    // svGetScopeFromName / user data, exposed as JIT symbols in Jit.cpp) with one
    // scope per top; a handle naming a nested instance is the deferred piece.

    // A DPI import has no body, so slang leaves returnValVar null even for a
    // non-void function; decide void-ness from the declared return type.
    ir::DpiCType ret_ct = ir::DpiCType::Void;
    ir::Ref      ret_ty = module_.pool.void_type;
    if (!sub.getReturnType().isVoid()) {
        const auto c = dpi_ctype(sub.getReturnType());
        if (!c.has_value() || *c == ir::DpiCType::BitVec || *c == ir::DpiCType::LogicVec) {
            // A DPI return must be a "small value" (LRM 35.5.5): slang already
            // rejects a packed-array return, so a vec here is defensive. string
            // / chandle returns are the deferred marshal.
            fail("DPI return type is not a supported scalar type (only "
                 "byte/shortint/int/longint/real/shortreal/bit/logic)");
        }
        ret_ct = *c;
        ret_ty = lower_type(sub.getReturnType());
    }

    std::vector<ir::Ref>      param_types;
    std::vector<ir::ArgDir>   arg_dirs;
    std::vector<ir::DpiCType> arg_cts;
    std::vector<ir::DpiCType> arg_elem_cts;
    for (const FormalArgumentSymbol* fa : sub.getArguments()) {
        if (fa->direction == slang::ast::ArgumentDirection::Ref) {
            // DPI-C has no `ref` formals (LRM 35.5.6); slang rejects DPIRefArg,
            // so this is defensive against a future slang change.
            fail("a DPI import argument cannot be `ref` (LRM 35.5.6)");
        }
        // Open array (LRM 35.5.6.2): an unsized `[]` formal arrives as an
        // svOpenArrayHandle. The element's own DpiCType is recorded separately;
        // the bounds/storage come from the actual at each call.
        if (const auto* oa = fa->getType().getCanonicalType().as_if<slang::ast::DPIOpenArrayType>()) {
            // input / output / inout all supported: the C reads (input/inout) and
            // writes (output/inout) elements through svGet/PutArrElem. The backend
            // marshals in before the call and back out after (or, for a named-
            // scalar zero-copy element, the C writes the design array directly).
            build_dpi_open_array_arg(*oa, to_arg_dir(fa->direction), param_types, arg_dirs, arg_cts, arg_elem_cts);
            continue;
        }
        const auto c = dpi_ctype(fa->getType());
        if (!c.has_value()) {
            // multi-dim open array / unpacked aggregate — deferred.
            fail("DPI argument type is not supported (scalar, packed vector, "
                 "chandle, string, 1-D open array only; not "
                 "multi-dim-open-array/unpacked)");
        }
        if (*c == ir::DpiCType::String && fa->direction != slang::ast::ArgumentDirection::In) {
            // TODO: output / inout string. The C writes an unbounded NUL-
            // terminated char* back and DPI gives no size contract, so there is
            // no safe fixed buffer — deferred (see docs/design/dpi-c.md).
            fail("DPI output/inout string arguments are not supported");
        }
        // input / output / inout: the direction drives the C ABI (by value vs
        // by pointer) in declare_dpi_import / lower_dpi_call. build_call_args
        // already makes the caller temp + CopyOutArg for a non-In direction.
        param_types.push_back(lower_type(fa->getType()));
        arg_dirs.push_back(to_arg_dir(fa->direction));
        arg_cts.push_back(*c);
        arg_elem_cts.push_back(ir::DpiCType::Void);
    }

    // The C linkage name is the import's c_identifier, or the SV name when
    // omitted (LRM 35.5.4). slang does not store it on the symbol — read it
    // off the syntax node.
    std::string cid(sub.name);
    if (const auto* syn = sub.getSyntax()) {
        if (const auto* d = syn->as_if<slang::syntax::DPIImportSyntax>()) {
            if (d->c_identifier) {
                cid = std::string(d->c_identifier.valueText());
            }
        }
    }

    // Emit a trivial void Ret so the (never-lowered) Ir is well-formed for the
    // whole-function scans in the backend (nba shadow, ptr-arg signals).
    ir::Builder         b(module_.pool);
    const ir::SourceLoc el = to_source_loc(sub.location);
    b.ret_void(el);

    // Mirror param_types into local_types (formal k == Local k), the invariant
    // every whole-function analysis assumes (e.g. compute_may_z joins param_z
    // into local_z). The backend never materializes these — lower_function
    // early-returns for a DPI import — so they are phantom slots.
    std::vector<ir::Ref> local_types = param_types;

    module_.set_function(id,
                         ir::Function{ .name                = std::string(sub.name),
                                       .return_type         = ret_ty,
                                       .param_types         = std::move(param_types),
                                       .arg_dirs            = std::move(arg_dirs),
                                       .local_types         = std::move(local_types),
                                       .is_dpi              = true,
                                       .is_pure             = sub.flags.has(MethodFlags::Pure),
                                       .is_dpi_task         = is_task,
                                       .extern_name         = std::move(cid),
                                       .dpi_arg_ctypes      = std::move(arg_cts),
                                       .dpi_ret_ctype       = ret_ct,
                                       .dpi_arg_elem_ctypes = std::move(arg_elem_cts),
                                       .span                = span_of(sub),
                                       .ir                  = b.finish() });
}

// Classify a 1-D DPI open-array formal (LRM 35.5.6.2). Records DpiCType::OpenArray
// in arg_cts and the element's own DpiCType in arg_elem_cts; param_types gets the
// element's lowered type (a phantom slot — the backend reads the actual's array
// type at each call, but the whole-function scans want a benign non-handle Ref).
// 1-D, with a marshalable element (named scalar / packed bit / packed logic), in
// any direction; everything else loud-fails.
void Lowerer::build_dpi_open_array_arg(const slang::ast::DPIOpenArrayType& oa,
                                       ir::ArgDir                          dir,
                                       std::vector<ir::Ref>&               param_types,
                                       std::vector<ir::ArgDir>&            arg_dirs,
                                       std::vector<ir::DpiCType>&          arg_cts,
                                       std::vector<ir::DpiCType>&          arg_elem_cts) {
    const slang::ast::Type& elem = oa.elementType.getCanonicalType();
    if (elem.isUnpackedArray() || elem.getCanonicalType().as_if<slang::ast::DPIOpenArrayType>()) {
        // TODO: multi-dimensional open array (element itself an array). Deferred —
        // the descriptor + marshal are 1-D only.
        fail("DPI multi-dimensional open-array arguments are not supported");
    }
    const auto ec = dpi_ctype(elem);
    // V1 elements: the named integers (Byte/Shortint/Int/Longint — zero-copy,
    // Clotho's narrow element storage is already a contiguous C array) and packed
    // bit/logic vectors (marshalled via rt_dpi_pack_arr_*). Scalar bit/logic, real,
    // string, chandle elements are deferred (docs/design/dpi-c.md).
    const bool ok = ec.has_value() &&
                    (*ec == ir::DpiCType::Byte || *ec == ir::DpiCType::Shortint || *ec == ir::DpiCType::Int ||
                     *ec == ir::DpiCType::Longint || *ec == ir::DpiCType::BitVec || *ec == ir::DpiCType::LogicVec);
    if (!ok) {
        fail("DPI open-array element type is not supported (byte/shortint/int/"
             "longint or packed bit/logic vector only)");
    }
    // Any element width is supported: <= 64 bits marshals through the
    // autovectorized C runtime (1 word for <= 32, 2 for 33..64), wider elements
    // through inline LLVM IR (native iW/i2W ops) at the call site.
    param_types.push_back(lower_type(elem));
    arg_dirs.push_back(dir);
    arg_cts.push_back(ir::DpiCType::OpenArray);
    arg_elem_cts.push_back(*ec);
}

// The root (top-level) instance whose hierarchy ultimately contains `sub`, or
// null when the subroutine sits in no elaborated instance. A module's members
// hang off an InstanceBodySymbol whose own parent scope is the *declaration*
// site, so climbing a level steps across the body/InstanceSymbol pair through
// parentInstance — the same walk control_depth uses.
const InstanceSymbol* Lowerer::owning_top(const SubroutineSymbol& sub) {
    const InstanceSymbol* top   = nullptr;
    const Scope*          scope = sub.getParentScope();
    while (scope != nullptr) {
        const Symbol& sym = scope->asSymbol();
        if (sym.kind != SymbolKind::InstanceBody) {
            scope = sym.getParentScope();
            continue;
        }
        const InstanceSymbol* parent = sym.as<InstanceBodySymbol>().parentInstance;
        if (parent == nullptr) {
            return nullptr; // a body slang elaborated without an instance
        }
        top   = parent;
        scope = parent->getParentScope();
    }
    return top;
}

void Lowerer::collect_dpi_exports(const slang::ast::Compilation& comp,
                                  const InstanceSymbol*          top,
                                  bool                           collect_globals) {
    for (const auto& ex : comp.getDPIExports()) {
        const SubroutineSymbol& sub = *ex.subroutine;
        if (is_global_subroutine(sub)) {
            if (!collect_globals) {
                continue; // collected once for the design, not once per top
            }
        } else if (top == nullptr || owning_top(sub) != top) {
            continue; // another top's export (or no elaborated instance at all)
        }
        const bool is_task = sub.subroutineKind == SubroutineKind::Task;
        // An exported TASK (LRM 35.8) is admitted here and lowers as an ordinary
        // task coroutine; the backend thunk drives it (emit_dpi_export). Only the
        // ZERO-TIME case works: a task that actually suspends is a located
        // runtime fatal, since the C frame that called it cannot be parked.
        // TODO: T2 -- suspension across the C frame (docs/design/dpi-c.md).
        ir::DpiCType ret_ct = ir::DpiCType::Void;
        if (!sub.getReturnType().isVoid()) {
            const auto c = dpi_ctype(sub.getReturnType());
            if (!c.has_value() || *c == ir::DpiCType::BitVec || *c == ir::DpiCType::LogicVec ||
                *c == ir::DpiCType::String) {
                // slang rejects a packed-array return; a string return is deferred
                // (the returned char* would dangle past the thunk).
                fail("DPI export return type is not supported "
                     "(scalar/chandle/real; string return is deferred)");
            }
            ret_ct = *c;
        }
        std::vector<ir::DpiCType> arg_cts;
        std::vector<ir::ArgDir>   arg_dirs;
        for (const FormalArgumentSymbol* fa : sub.getArguments()) {
            if (fa->direction == slang::ast::ArgumentDirection::Ref) {
                // As for an import: illegal per LRM 35.5.6 and already rejected
                // by slang, guarded defensively here.
                fail("a DPI export argument cannot be `ref` (LRM 35.5.6)");
            }
            const auto c = dpi_ctype(fa->getType());
            if (!c.has_value()) {
                fail("DPI export argument type is not supported "
                     "(open-array / unpacked aggregate)");
            }
            if (*c == ir::DpiCType::String && fa->direction != slang::ast::ArgumentDirection::In) {
                // TODO: output / inout string (unbounded C-written char*).
                fail("DPI export output/inout string arguments are not supported");
            }
            arg_cts.push_back(*c);
            arg_dirs.push_back(to_arg_dir(fa->direction));
        }
        // One C symbol cannot name two SV functions: llvm::Function::Create
        // would silently rename the second thunk to `<name>.1`, leaving C bound
        // to an arbitrary instance. slang's own c_identifier dedup makes this
        // unreachable today, but the invariant has to be Clotho's.
        for (const ir::DpiExport& prev : module_.dpi_exports) {
            if (prev.extern_name == ex.cIdentifier) {
                fail("duplicate DPI export c_identifier '" + ex.cIdentifier +
                     "' (one C name cannot bind two SystemVerilog functions)");
            }
        }
        // function_id force-lowers the SV subroutine: a task takes build_task,
        // so the thunk has a real coroutine ramp to drive.
        const ir::FunctionId id = function_id(sub);
        module_.dpi_exports.push_back(ir::DpiExport{ id, std::string(ex.cIdentifier), std::move(arg_cts),
                                                     std::move(arg_dirs), ret_ct, is_task });
    }
}

// Point the delay / $time multipliers at `scope`'s effective timescale. A
// `#5` written there means 5 of that scope's time units (LRM 3.14.2), so the
// unit multiplier is unit/tick; a real delay first rounds to the scope's
// precision, hence precision/tick. Both ratios are exact integers: the tick
// is the design-wide GCD of every unit and of every precision that a real
// delay rounds to (design_tick_fs / global_scope_tick_fs), and every
// timescale value is a power of ten. Callers guard on tick_fs_ != 0; unitless
// mode leaves both at 1 and scales nothing.
//
// TODO: a scope slang hands back no timescale for falls into that unitless
// branch even when the design HAS a tick, and then reads/writes time in raw
// wheel ticks. The case that reaches it is a subroutine or class declared
// directly in `$unit`: a `timescale directive applies to design elements
// (LRM 3.14.2.2), so CompilationUnitSymbol::timeScale stays unset unless
// `$unit` carries its own `timeunit` / `timeprecision`. So a `$unit` function
// under `timescale 1ns/1ps` reports 3000 where its 1ns caller reports 3 --
// and the number moves with unrelated code, since dropping a `#0.5` anywhere
// shifts the tick from 1ns to 1ps. iverilog reports 0 there (its own 1s
// default). The fix is a fixed tool-default timescale for such a scope, folded
// into the wheel tick the way global_scope_tick_fs already folds the scopes
// that DO declare one. Declaring in a package or module is unaffected.
void Lowerer::apply_time_scale(const Scope& scope) {
    const auto ts   = scope.getTimeScale();
    time_mul_       = ts ? tsv_to_fs(ts->base) / tick_fs_ : 1;
    prec_over_tick_ = ts ? tsv_to_fs(ts->precision) / tick_fs_ : 1;
}

// The FunctionId of a subroutine reached through a virtual interface handle:
// the REPRESENTATIVE instance's body, lowered a second time with the handle as
// formal 0 (LRM 25.7 through 25.9). One body serves every instance, which is
// what the instance-context indirection is for; the ordinary per-instance
// lowering (fnmap_) is untouched and still serves direct calls.
ir::FunctionId Lowerer::vif_function_id(const SubroutineSymbol& ph_sub) {
    if (const auto it = vif_fnmap_.find(&ph_sub); it != vif_fnmap_.end()) {
        return it->second;
    }
    const auto& placeholder =
      ph_sub.getParentScope()->asSymbol().as<slang::ast::InstanceBodySymbol>();
    for (const VifInstance& inst : vif_defs_->insts) {
        if (!inst.body->hasSameType(placeholder)) {
            continue;
        }
        if (inst.syms.empty()) {
            fail("a subroutine call through a virtual interface is not supported for an "
                 "interface with no signals: there is no instance layout to address");
        }
        const slang::ast::Symbol* rep_sym = inst.body->find(ph_sub.name);
        if (rep_sym == nullptr || rep_sym->kind != SymbolKind::Subroutine) {
            fail("internal: virtual interface subroutine missing from the representative");
        }
        // The representative's signals may have been registered by a different
        // Lowerer (another top, or the shared globals one). Seed them so the
        // body's own references resolve; a symbol maps to one signal for the
        // whole design, so this can only add.
        for (const auto& [sym, sig] : inst.syms) {
            sigmap_.emplace(sym, sig);
        }
        const ir::FunctionId id = module_.reserve_function();
        vif_fnmap_[&ph_sub]     = id;
        const Restore<std::optional<ir::Signal>> ctx(vif_build_ctx_, inst.syms.begin()->second);
        const Restore<const Scope*> scope(scope_, inst.body);
        const auto&                 rep = rep_sym->as<SubroutineSymbol>();
        if (rep.subroutineKind == SubroutineKind::Task) {
            build_task(rep, id);
        } else {
            build_function(rep, id);
        }
        return id;
    }
    fail("a subroutine call through a virtual interface is not supported: no elaborated "
         "instance of interface '" + std::string(placeholder.getDefinition().name) + "' exists");
}

ir::FunctionId Lowerer::function_id(const SubroutineSymbol& sub) {
    if (globals_ != nullptr && this != globals_ && is_global_subroutine(sub)) {
        const Restore<const Scope*> scope(globals_->scope_, sub.getParentScope());
        return globals_->function_id(sub);
    }

    const auto cached = fnmap_.find(&sub);
    if (cached != fnmap_.end()) {
        return cached->second;
    }
    if (sub.flags.has(MethodFlags::DPIImport)) {
        const ir::FunctionId id = module_.reserve_function();
        fnmap_[&sub]            = id;
        build_dpi_import(sub, id);
        return id;
    }
    // No per-formal type guard here: every storage class now passes in every
    // direction. `ref` aliases the actual's live storage through a fat pointer
    // (docs/design/data-model.md §3); a Managed handle — string, dynamic array,
    // queue, assoc — passes as one handle pointer with a deep copy in and a
    // move / deep copy back (§6); an aggregate passes as one storage pointer.
    // The residual restrictions are per-ACTUAL and live at the call site
    // (build_call_args) or on the formal's own type in build_function.

    const ir::FunctionId id = module_.reserve_function();
    fnmap_[&sub]            = id;
    if (defer_bodies_) {
        // Reserved, not built: we are inside lowering a TYPE (compute_vtable), and
        // a body reaches arbitrary code -- another class still laying itself out,
        // a global not yet registered. The id is all the caller needs; the body is
        // built by drain_pending_bodies at the next phase boundary.
        body_pending_.emplace_back(&sub, id);
        return id;
    }
    build_reserved_body(sub, id);
    return id;
}

// Lower one reserved body. The time scale comes from the scope that DECLARES the
// subroutine, not the one that calls it (LRM 3.14.2.3) -- and the same for a
// `$time` read in its body. The body is lowered once and shared by every call
// site (fnmap_), so this is not merely more correct but the only self-consistent
// choice: scaled by the caller, at most one caller could ever be right.
// Scope::getTimeScale walks out to the enclosing module / package / compilation
// unit, so a class method picks up its declaring unit's scale too.
//
// `scope_` follows the declaring scope for the same reason function_id's globals
// delegation sets it: a constant fold inside the body resolves against it, and at
// drain time the scope we were lowering when the body was reserved is long gone.
void Lowerer::build_reserved_body(const SubroutineSymbol& sub, ir::FunctionId id) {
    const Restore<const Scope*> scope(scope_, sub.getParentScope());
    const Restore<uint64_t>     mul(time_mul_);
    const Restore<uint64_t>     prec(prec_over_tick_);
    if (tick_fs_ != 0) {
        if (const Scope* decl = sub.getParentScope()) {
            apply_time_scale(*decl);
        }
    }
    if (sub.subroutineKind == SubroutineKind::Task) {
        build_task(sub, id);
    } else {
        build_function(sub, id);
    }
}

bool Lowerer::has_pending_bodies() const {
    return !body_pending_.empty();
}

// Build every body reserved but not yet built, until none is left: a body reaches
// classes of its own, whose vtables reserve more. Runs at a phase boundary -- the
// end of lower_globals, the end of each top -- which is the whole point, because
// that is where the state a body reads is complete.
//
// The shared globals Lowerer owns package/$unit bodies (function_id delegates
// them), so a top's drain has to reach into it too, and either drain can feed the
// other: a global body lowers a class, whose vtable reserves a method this
// Lowerer owns. Hence the loop rather than two calls.
void Lowerer::drain_pending_bodies() {
    const Restore<bool> immediate(defer_bodies_, false);
    // A body built from here was forced into existence by vtable completeness,
    // not by a call the design executes -- so an unsupported construct inside
    // one becomes a warning plus a run-time fatal stub (build_function /
    // build_task) instead of a whole-design refusal. Eager static class
    // property initialization (LRM 6.21) is what makes this reachable at
    // scale: every factory-registered UVM class now lowers, register layer
    // included, whether or not the design ever calls into it.
    const Restore<bool> stub(stub_unsupported_bodies_, true);
    while (true) {
        if (!body_pending_.empty()) {
            const auto [sub, id] = body_pending_.back();
            body_pending_.pop_back();
            build_reserved_body(*sub, id);
            continue;
        }
        if (globals_ != nullptr && globals_ != this && globals_->has_pending_bodies()) {
            globals_->drain_pending_bodies();
            continue;
        }
        return;
    }
}

// Whether a call's receiver is a virtual interface handle, so the callee is the
// ctx-parameterized body rather than one instance's.
bool Lowerer::is_vif_call(const CallExpression& call) {
    return call.thisClass() != nullptr &&
           call.thisClass()->type->getCanonicalType().isVirtualInterface();
}

// Map a slang argument direction to the IR direction.
ir::ArgDir Lowerer::to_arg_dir(slang::ast::ArgumentDirection dir) {
    switch (dir) {
        case slang::ast::ArgumentDirection::In:
            return ir::ArgDir::In;
        case slang::ast::ArgumentDirection::Out:
            return ir::ArgDir::Out;
        case slang::ast::ArgumentDirection::InOut:
            return ir::ArgDir::Inout;
        case slang::ast::ArgumentDirection::Ref:
            return ir::ArgDir::Ref;
        default:
            return ir::ArgDir::In;
    }
}

// Splice the virtual interface handle of a subroutine lowered for handle
// dispatch (LRM 25.9) as frame Local 0, exactly as splice_this does for a class
// method. The body never names it: the backend reads it at entry and makes it
// the instance every signal access resolves against. Returns the interface
// member that identifies the definition, or Signal{0} outside such a lowering.
std::optional<ir::Signal> Lowerer::splice_iface_ctx(std::vector<ir::Ref>&    param_types,
                                                    std::vector<ir::ArgDir>& arg_dirs) {
    if (!vif_build_ctx_) {
        return std::nullopt;
    }
    param_types.push_back(module_.pool.viface_type);
    arg_dirs.push_back(ir::ArgDir::In);
    fn_ctx_->local_types.push_back(module_.pool.viface_type);
    // Consumed: a body this one lowers on the way (an ordinary package function,
    // say) is not itself ctx-parameterized. `in_vif_body_` is what stays set.
    const ir::Signal member = *vif_build_ctx_;
    vif_build_ctx_.reset();
    return member;
}

// Whether `sub` is declared directly in an interface body, so a BARE call to it
// from inside a ctx-parameterized body is a call on the same instance and must
// carry the same context (LRM 25.7).
bool Lowerer::is_iface_body_subroutine(const SubroutineSymbol& sub) {
    const Scope* owner = sub.getParentScope();
    return owner != nullptr && owner->asSymbol().kind == SymbolKind::InstanceBody &&
           owner->asSymbol().as<slang::ast::InstanceBodySymbol>().getDefinition().definitionKind ==
             slang::ast::DefinitionKind::Interface;
}

// The instance-context argument for a call made from inside a ctx-parameterized
// body: frame Local 0, the handle this activation was entered with.
bool Lowerer::vif_ctx_arg_wanted(const SubroutineSymbol& sub) const {
    return in_vif_body_ && is_iface_body_subroutine(sub);
}

std::optional<ir::Index> Lowerer::vif_ctx_arg(const SubroutineSymbol& sub, ir::Builder& b, ir::SourceLoc l) {
    if (!vif_ctx_arg_wanted(sub)) {
        return std::nullopt;
    }
    return b.unary(ir::Inst::Tag::Load, b.local(0, module_.pool.viface_type, l), l);
}

// Splice the implicit `this` handle of a class method (LRM 8.11) as frame Local
// 0. Called at the start of build_function / build_task before the real formals,
// so `this` occupies Local 0 and every real formal / return var shifts up. A
// free subroutine (thisVar == nullptr) is a no-op. `this` is an In pointer passed
// by value (reference semantics): it must NOT take the aggregate copy-in, and
// does not — a class handle is a scalar i64, routed through the scalar-arg ABI.
void Lowerer::splice_this(const SubroutineSymbol&  sub,
                          std::vector<ir::Ref>&    param_types,
                          std::vector<ir::ArgDir>& arg_dirs) {
    if (sub.thisVar == nullptr) {
        return;
    }
    const ir::Ref hty = lower_type(sub.thisVar->getType());
    param_types.push_back(hty);
    arg_dirs.push_back(ir::ArgDir::In);
    fn_ctx_->this_local = fn_local(*sub.thisVar);
    fn_ctx_->has_this   = true;
    fn_ctx_->this_class = hty;
    fn_ctx_->this_var   = sub.thisVar;
}

// The lvalue of a class property `prop` reached through an object handle value
// `handle_val` (the loaded i64 pointer bits of a class of type `class_ref`):
// dereference the handle to the object body, then a MemberSelect on the
// property's field index. Shared by member access (`obj.prop`) and a bare
// property reference inside a method (rooted at `this`).
//
// `handle_span` locates the object expression itself when the source spells
// one out. The backend's null-handle guard underlines it, so pointing at
// `obj` rather than at the whole `obj.prop` is what makes the fatal name the
// handle that was null. An implicit `this` has no such span; the diagnostic
// then falls back to `l` (the property reference).
ir::Index Lowerer::class_member_lvalue(ir::Index                 handle_val,
                                       const slang::ast::Symbol& prop,
                                       ir::Ref                   class_ref,
                                       ir::Builder&              b,
                                       ir::SourceLoc             l,
                                       ir::SourceSpan            handle_span) {
    const ir::Index body = b.handle_deref(handle_val, class_ref, handle_span.valid() ? handle_span.start : l);
    if (handle_span.end != ir::SourceLoc{ 0 }) {
        b.set_end(body, handle_span.end);
    }
    // Every non-static property of a lowered class is indexed by lower_class_type,
    // so a miss is a lowering bug, not an unsupported construct. Reported instead
    // of `at()`-throwing so it arrives as a located diagnostic naming the property
    // rather than an uncaught std::out_of_range with no source at all.
    const auto it = class_prop_index_.find(&prop);
    if (it == class_prop_index_.end()) {
        const slang::ast::Scope* sc = prop.getParentScope();
        fail("internal: class property '" + std::string(prop.name) + "' of '" +
             std::string(sc != nullptr ? sc->asSymbol().name : "?") + "' has no field index");
    }
    return b.member_select(body, it->second, class_ref, l);
}

// Whether a body-build failure in `sub` may compile to a run-time fatal stub:
// only a CLASS METHOD built on the vtable-forced path (drain_pending_bodies).
// A module/package subroutine reaches lowering only from a real call site, so
// its compile diagnostic stands.
bool Lowerer::stub_eligible(const SubroutineSymbol& sub) const {
    return stub_unsupported_bodies_ && sub.getParentScope() != nullptr &&
           sub.getParentScope()->asSymbol().kind == SymbolKind::ClassType;
}

// Turn an abandoned body into the stub: roll the half-emitted statements back
// to the (empty) pre-body mark, record a compile-time warning naming the
// construct, and emit report + terminate so an EXECUTION of the body dies
// loudly with the same message. The caller's fall-through return and
// set_function tail then complete the record with the real signature, which is
// what keeps every call site ABI-correct without rebuilding it here.
void Lowerer::emit_unsupported_body_stub(const SubroutineSymbol& sub,
                                         const LowerError&       e,
                                         ir::Builder&            b,
                                         ir::Builder::Mark       body_mark) {
    b.rollback(body_mark);
    report(Severity::Warning,
           "unsupported-construct",
           e.message + "; '" + std::string(sub.name) + "' compiles to a run-time fatal stub",
           e.span);
    const ir::SourceLoc el     = to_source_loc(sub.location);
    const std::string   banner = banner_at(sub.location);
    b.severity(static_cast<uint32_t>(ir::Severity::Fatal), severity_banner(sub.location), e.message, {}, el);
    b.finish(banner + "$fatal", 1, el);
}

// Lower one task body into the reserved slot. Like build_function, but the task
// is void and becomes a coroutine: the backend emits the parameter copy-in /
// copy-out (output / inout pass by pointer) and the completion handshake, so
// here we only allocate a frame Local per formal (index k == formal position)
// and lower the body. Every path ends in a void Ret, which the backend turns
// into the copy-out + caller-resume epilogue.
void Lowerer::build_task(const SubroutineSymbol& sub, ir::FunctionId id) {
    FnCtx ctx;
    ctx.name_prefix = std::string(sub.name) + ".";
    ctx.is_task     = true;
    ctx.sym         = &sub;
    // A body may be lowered lazily from inside a property initializer or a
    // default-argument expression, where `ctor_this_` / `default_arg_this_`
    // name an object as an ir::Index of the CALLER's builder. Inside this body
    // those Indexes belong to another builder entirely, and the body's own
    // `this` formal is the correct handle, so both scopes stop at the call
    // boundary.
    const Restore<std::optional<CtorThis>> no_ctor_this(ctor_this_, std::nullopt);
    const Restore<std::optional<CtorThis>> no_dflt_this(default_arg_this_, std::nullopt);
    const FnScope fn_scope(*this, &ctx);
    ir::Builder             b(module_.pool);
    std::vector<ir::Ref>    param_types;
    std::vector<ir::ArgDir> arg_dirs;

    // A class method (LRM 8.11): splice the implicit `this` handle as formal 0,
    // before the real formals, so they (and the return var) keep formal k ==
    // Local k with `this` at Local 0.
    splice_this(sub, param_types, arg_dirs);
    // A subroutine reached through a virtual interface handle (LRM 25.9): the
    // handle is formal 0 instead, and the backend installs it as the instance
    // context for the whole body.
    const std::optional<ir::Signal> ctx_member = splice_iface_ctx(param_types, arg_dirs);
    const Restore<bool>             in_vif(in_vif_body_, ctx_member.has_value());

    // Allocate the formals' frame Locals first so formal k maps to Local k (the
    // backend's prologue / epilogue rely on this). No Arg copy is emitted: the
    // backend copies parameters in (and output / inout back out) per direction.
    for (const FormalArgumentSymbol* fa : sub.getArguments()) {
        param_types.push_back(lower_type(fa->getType()));
        arg_dirs.push_back(to_arg_dir(fa->direction));
        fn_local(*fa);
    }

    const ir::Builder::Mark body_mark = b.mark();
    try {
        install_capture_boxes(sub, sub.getBody(), b);
        lower_stmt(sub.getBody(), b);
    } catch (const LowerError& e) {
        if (!stub_eligible(sub)) {
            throw;
        }
        emit_unsupported_body_stub(sub, e, b, body_mark);
    }

    // Fall-through return: the void Ret drives the backend's copy-out epilogue.
    const ir::SourceLoc el = to_source_loc(sub.location);
    b.ret_void(el);

    const auto tgt = disable_targets_.find(&sub);
    module_.set_function(id,
                         ir::Function{ .name            = std::string(sub.name),
                                       .return_type     = module_.pool.void_type,
                                       .param_types     = std::move(param_types),
                                       .arg_dirs        = std::move(arg_dirs),
                                       .local_types     = std::move(ctx.local_types),
                                       .takes_iface_ctx = ctx_member.has_value(),
                                       .ctx_member      = ctx_member.value_or(ir::Signal{ 0 }),
                                       .is_task         = true,
                                       // A `disable` naming this task ends every
                                       // activation, so each links itself onto the
                                       // target's list (LRM 9.6.2).
                                       .disable_target =
                                         tgt != disable_targets_.end() ? tgt->second : ir::kNoDisableTarget,
                                       .span        = span_of(sub),
                                       .ir          = b.finish() });
}

// Lower one `fork` child statement (LRM 9.3.2) into a synthetic fork-child
// task: a coroutine spawned at the fork, whose completion decrements the shared
// join record. Mirrors build_task, but takes a bare Statement and runs in the
// *parent's* scope context (scope_ / prefix_ / time_mul_ unchanged) so the
// child's references to parent variables resolve to the parent's signals, while
// the child's own declared locals get per-spawn frame slots (value_lvalue:
// localmap first, then signal_of).
//
// `captures` are the enclosing subroutine's frame symbols the fork's children
// touch (lower_fork collected them). Each becomes an ArgDir::In formal carrying
// the variable's GC box handle, registered in this child's `boxed` map under the
// SAME ValueSymbol -- which is what makes the child's reference to it resolve
// through the box instead of falling through to signal_of. Every child of one fork
// takes the same list, so an unused capture costs one dead scalar parameter.
// Returns the reserved FunctionId.
ir::FunctionId Lowerer::build_fork_child(const Statement&                    body,
                                         std::span<const ValueSymbol* const> captures,
                                         ir::ForkKind                        kind) {
    const ir::FunctionId id = module_.reserve_function();

    FnCtx ctx;
    ctx.name_prefix = "fork.child.";
    ctx.is_task     = true;
    const FnScope fn_scope(*this, &ctx);

    // Formals first, so formal k == Local k (the invariant build_task maintains
    // and the backend's prologue / ref binding rely on).
    std::vector<ir::Ref>    param_types;
    std::vector<ir::ArgDir> arg_dirs;
    param_types.reserve(captures.size());
    arg_dirs.reserve(captures.size());
    for (const ValueSymbol* sym : captures) {
        // The box handle, by value. Reference semantics come from the box, not
        // from the ABI, so nothing points into the parent's frame and the child
        // may outlive it (docs/design/fork-join.md V4). box_class_for, not
        // box_class_type: the parent minted the NOTIFIED variant for a symbol
        // some `wait` reads, and the child's stores must resolve the same
        // (class, field) change-notify entry or they would fire nothing.
        const ir::Ref cls = box_class_for(*sym, lower_type(sym->getType()));
        param_types.push_back(cls);
        arg_dirs.push_back(ir::ArgDir::In);
        const uint32_t k   = fn_local(*sym);
        ctx.local_types[k] = cls;
        ctx.boxed[sym]     = { k, cls };
        // A captured `this` makes this child a method body for lookup purposes: a
        // bare property reference inside it re-roots at the handle exactly as it
        // did in the enclosing method (value_lvalue -> this_handle), which reads
        // the box rather than a frame slot.
        if (sym->name == "this") {
            ctx.has_this   = true;
            ctx.this_var   = sym;
            ctx.this_class = lower_type(sym->getType());
        }
    }

    ir::Builder                b(module_.pool);
    const ir::Builder::Mark    body_mark = b.mark();
    std::optional<LowerError>  failure;
    try {
        lower_stmt(body, b);
    } catch (const LowerError& e) {
        // `id` is already committed to module_'s function table --
        // reserve_function has no rollback -- and the caller (lower_fork,
        // which built `ids` from this call's return value) never gets `id`
        // back once this throws: nothing downstream will ever call
        // module_.set_function(id, ...) for it. Left unset, the backend's
        // unconditional declare_function pass over every reserved id
        // segfaults on this one's Ref::Invalid return type the first time
        // codegen reaches it, regardless of whether an OUTER stub_eligible
        // catch later downgrades the enclosing method to a stub of its own
        // (that stub never references this dead id either). The body is
        // unreachable either way -- rethrowing after completing the
        // reservation preserves the existing "whole enclosing method becomes
        // a fatal stub" behavior the sibling capture-box check above already
        // gets for the same construct.
        b.rollback(body_mark);
        failure = e;
    }
    const ir::SourceLoc el = loc(body);
    b.ret_void(el);

    module_.set_function(id,
                         ir::Function{ .name          = "fork.child",
                                       .return_type   = module_.pool.void_type,
                                       .param_types   = std::move(param_types),
                                       .arg_dirs      = std::move(arg_dirs),
                                       .local_types   = std::move(ctx.local_types),
                                       .is_task       = true,
                                       .is_fork_child = true,
                                       .fork_kind     = kind,
                                       .ir            = b.finish() });
    if (failure) {
        throw *failure;
    }
    return id;
}

// Lower a `$strobe` argument list (LRM 21.2.2) into a synthetic zero-formal
// deferred-print task: same shape as build_fork_child (a fresh, empty
// FnCtx; runs in the *parent's* scope context — scope_ / prefix_ /
// time_mul_ unchanged — so a reference to the caller's own module signals
// still resolves), but the body is a $display built from `args`, not a
// statement. The fresh, never-populated localmap is what makes this safe
// without a separate eligibility check: any argument referencing the
// caller's own automatic local, formal, or `this` misses that lookup
// (value_lvalue) and falls through to signal_of, which fails loudly on a
// symbol never registered as a persistent signal — storage the deferred
// body, resumed after the caller's own activation may have ended, cannot
// reach anyway. Returns the reserved FunctionId; the call site pends it.
// Synthesize the deferred-print body task for $strobe / $fstrobe / $monitor /
// $fmonitor. The task is zero-formal: it evaluates the argument list at the
// Postponed region and prints it. `fd_arg` (for $f* forms) makes args[0] the
// destination descriptor and args[1..] the message; `monitor` routes the print
// through the change-detecting monitor emit (else a plain display/fdisplay).
ir::FunctionId Lowerer::build_deferred_body(std::span<const Expression* const> args,
                                            char                               default_conv,
                                            ir::SourceLoc                      l,
                                            bool                               fd_arg,
                                            bool                               monitor) {
    const ir::FunctionId id = module_.reserve_function();

    FnCtx ctx;
    ctx.name_prefix = monitor ? "monitor." : "strobe.";
    ctx.is_task     = true;
    const FnScope fn_scope(*this, &ctx);

    ir::Builder                b(module_.pool);
    const ir::Builder::Mark    body_mark = b.mark();
    std::optional<LowerError>  failure;
    try {
        // $display / $strobe target the standard-output MCD (descriptor 1); the
        // $f* forms carry an explicit descriptor as the first argument.
        ir::Index desc =
          b.constant(svint_to_ref(slang::SVInt(32, 1, true), make_int_type(32, false, true)), l);
        if (fd_arg) {
            if (args.empty()) {
                fail("$fstrobe / $fmonitor requires a file-descriptor argument");
            }
            desc = lower_expr(*args[0], b);
            args = args.subspan(1);
        }
        std::string            fmt;
        std::vector<ir::Index> fargs;
        build_format(args, fmt, fargs, b, default_conv);
        if (monitor) {
            b.monitor_emit(desc, fmt, fargs, l);
        } else if (fd_arg) {
            b.fdisplay(desc, fmt, /*newline=*/true, fargs, l);
        } else {
            b.display(fmt, /*newline=*/true, fargs, l);
        }
    } catch (const LowerError& e) {
        // See build_fork_child: `id` is already committed with no rollback, and
        // this call's return value is what the pending-print call site would
        // have used to reference it -- once this throws, nothing else ever
        // will. Complete the reservation with a dead body before propagating.
        b.rollback(body_mark);
        failure = e;
    }
    b.ret_void(l);

    module_.set_function(id,
                         ir::Function{ .name        = monitor ? "monitor" : "strobe",
                                       .return_type = module_.pool.void_type,
                                       .param_types = {},
                                       .arg_dirs    = {},
                                       .local_types = std::move(ctx.local_types),
                                       .is_task     = true,
                                       .ir          = b.finish() });
    if (failure) {
        throw *failure;
    }
    return id;
}

// The return variable's lvalue: the per-call frame Local (fresh each call).
ir::Index Lowerer::ret_var_ref(FnCtx& ctx, ir::SourceLoc l, ir::Builder& b) {
    return b.local(ctx.ret_local, ctx.ret_type, l);
}

// Lower one function body into the reserved slot. (Ir has no default ctor and
// is not assignable, so every field is built locally and the Function is
// constructed once, at the end.)
void Lowerer::build_function(const SubroutineSymbol& sub, ir::FunctionId id) {
    const bool    is_void     = sub.returnValVar == nullptr;
    const ir::Ref return_type = is_void ? module_.pool.void_type : lower_type(sub.returnValVar->getType());

    FnCtx ctx;
    ctx.name_prefix = std::string(sub.name) + ".";
    const FnScope fn_scope(*this, &ctx);
    // Stops the enclosing construction's / default-argument's `this` at the
    // call boundary, as build_task does and for the same reason (each handle is
    // another builder's Index; this body's own `this` formal is the right one).
    const Restore<std::optional<CtorThis>> no_ctor_this(ctor_this_, std::nullopt);
    const Restore<std::optional<CtorThis>> no_dflt_this(default_arg_this_, std::nullopt);
    ir::Builder             b(module_.pool);
    std::vector<ir::Ref>    param_types;
    std::vector<ir::ArgDir> arg_dirs;

    // A class method (LRM 8.11): splice the implicit `this` handle as formal 0.
    splice_this(sub, param_types, arg_dirs);
    // A subroutine reached through a virtual interface handle (LRM 25.9): the
    // handle is formal 0 instead, and the backend installs it as the instance
    // context for the whole body.
    const std::optional<ir::Signal> ctx_member = splice_iface_ctx(param_types, arg_dirs);
    const Restore<bool>             in_vif(in_vif_body_, ctx_member.has_value());

    // Allocate a frame Local per formal (formal k == Local k), like build_task.
    // The backend copies parameters into these Locals at entry (emit_copyin) and
    // copies output / inout formals back at each return (emit_copyout); a ref
    // formal's Local is redirected to the passed actual pointer. No IR-level Arg
    // copy is emitted for any direction. An aggregate formal is passed by pointer
    // and copied into its Local by the backend's aggregate copy-in.
    for (const FormalArgumentSymbol* fa : sub.getArguments()) {
        if (is_agg_abi_type(fa->getType()) && fa->direction != slang::ast::ArgumentDirection::Ref) {
            // An aggregate output / inout formal (LRM 13.5): the backend copies
            // the frame Local back through the passed pointer at each return. A
            // struct-bearing actual passes its own storage pointer directly; a
            // plain array actual passes a canonical caller temp that
            // build_call_args copies back into the actual after the call. A
            // managed leaf (string / container) rides the same pointers — the copy
            // is per-leaf rather than a memcpy (emit_agg_deep_copy).
        }
        param_types.push_back(lower_type(fa->getType()));
        arg_dirs.push_back(to_arg_dir(fa->direction));
        fn_local(*fa);
    }

    if (!is_void) {
        ctx.has_ret_local = true;
        ctx.ret_type      = return_type;
        // The implicit return variable is fresh per call even for a static
        // (non-automatic) function: an unassigned return path yields X (LRM
        // 13.4.2 makes a static function's *named* locals persist, but iverilog
        // resets the function-name return variable each call -- a scalar
        // returns X, a handle returns an empty container). A frame Local gets
        // exactly that (init_frame X-fills scalars / empties handles per call);
        // persisting it would leak the prior call's value on the unwritten path.
        ctx.ret_local = fn_local(*sub.returnValVar);
    }

    const ir::Builder::Mark body_mark = b.mark();
    try {
        install_capture_boxes(sub, sub.getBody(), b);

        // A constructor constructs its base part first (LRM 8.15), before the
        // body: implicitly (no-arg), via `extends B(args)`, or via an explicit
        // `super.new` (all three surface through getBaseConstructorCall()). Emit
        // it here, in the constructor's own frame, where any base-arg that
        // references a ctor formal is in scope. slang requires super.new to be
        // the first statement, so entry-emission is order-equivalent; the body's
        // super.new is a no-op.
        if (sub.flags.has(slang::ast::MethodFlags::Constructor) && sub.thisVar != nullptr) {
            const auto& clt = sub.thisVar->getType().getCanonicalType().as<slang::ast::ClassType>();
            if (clt.getBaseClass() != nullptr) {
                const ir::SourceLoc bl = to_source_loc(sub.location);
                emit_base_construction(clt, this_handle(b, bl), b, bl);
            }
        }

        lower_stmt(sub.getBody(), b);
    } catch (const LowerError& e) {
        if (!stub_eligible(sub)) {
            throw;
        }
        emit_unsupported_body_stub(sub, e, b, body_mark);
    }

    // Fall-through return: yields the function variable (X if never assigned).
    // An aggregate result's Ret operand is its lvalue (the backend copies its
    // storage out through the trailing return out-param), not a loaded value.
    const ir::SourceLoc el = to_source_loc(sub.location);
    if (is_void) {
        b.ret_void(el);
    } else if (is_agg_abi_type(sub.returnValVar->getType())) {
        b.unary(ir::Inst::Tag::Ret, ret_var_ref(ctx, el, b), el);
    } else {
        // A dynamic-array/queue/assoc result is returned as its handle lvalue
        // (the backend deep-copies it out); mirrors the explicit-`return;` path
        // in lower_return. Without this a queue return lowers to Ret(Load(handle)),
        // an uninitialized-lvalue read that SIGSEGVs in emit_handle_assign.
        const ir::Index rv = ret_var_ref(ctx, el, b);
        b.unary(ir::Inst::Tag::Ret, is_handle_ref(ctx.ret_type) ? rv : b.unary(ir::Inst::Tag::Load, rv, el), el);
    }

    module_.set_function(id,
                         ir::Function{ .name            = std::string(sub.name),
                                       .return_type     = return_type,
                                       .param_types     = std::move(param_types),
                                       .arg_dirs        = std::move(arg_dirs),
                                       .local_types     = std::move(ctx.local_types),
                                       .takes_iface_ctx = ctx_member.has_value(),
                                       .ctx_member      = ctx_member.value_or(ir::Signal{ 0 }),
                                       .span            = span_of(sub),
                                       .ir              = b.finish() });
}

// Lower a user function call: the actuals (resized to the parameter types)
// become a `call`. Returns the result value, or nullopt for a void function.
std::optional<ir::Index> Lowerer::lower_user_call(const CallExpression& call, ir::Builder& b) {
    if (call.subroutine.index() != 0) {
        fail("system call used where a user subroutine was expected");
    }
    const SubroutineSymbol& sub = *std::get<0>(call.subroutine);
    if (is_process_method(sub)) {
        // The nine `process` methods (LRM 9.7) have no lowerable body -- slang
        // synthesizes signatures only -- so they never reach the call path
        // below. Their receiver is resolved here rather than through
        // resolve_method_this, because `self()` is static and the rest take the
        // control block out of the receiver's object.
        return lower_process_method(call, sub, b);
    }
    if (is_sync_method(sub)) {
        // The `semaphore` methods (LRM 15.3) are the same shape of bodyless
        // built-in: each expands into ordinary IR over the object's synthetic
        // body, so none of the call path below applies.
        return lower_sync_method(call, sub, b);
    }
    // A covergroup / coverpoint built-in method (LRM 19.8). Intercepted here for
    // the same reason the `std` classes are: slang gives these a signature and
    // no body, and a covergroup is not a class, so nothing below applies.
    if (is_cover_method(sub)) {
        const ir::Index v = lower_cover_method(call, sub, b);
        if (v == ir::Index{}) {
            return std::nullopt;
        }
        return v;
    }
    // A class built-in slang synthesizes with an empty body (srandom, rand_mode,
    // the randstate pair, the default randomization hooks). Intercepted before
    // anything else: lowering slang's degenerate body is silently wrong.
    if (sub.flags.has(slang::ast::MethodFlags::BuiltIn) && sub.getParentScope() != nullptr
        && sub.getParentScope()->asSymbol().kind == SymbolKind::ClassType) {
        return lower_builtin_class_method(call, sub, b);
    }
    // A class method (LRM 8.11): resolve the `this` object handle, spliced as
    // arg 0 (the callee splices it as formal 0). An explicit receiver
    // (`obj.method()`) is call.thisClass(); a bare in-method call uses `this`.
    std::optional<ir::Index> this_val = resolve_method_this(call, sub, b);
    // A bare call to a sibling subroutine of the same interface, from inside a
    // ctx-parameterized body: the context travels as arg 0, like a receiver.
    const bool bare_vif = !this_val.has_value() && vif_ctx_arg_wanted(sub);
    if (bare_vif) {
        this_val = vif_ctx_arg(sub, b, loc(call));
    }
    // A DPI import task is a task to SystemVerilog (a statement, no result, and
    // slang already refuses it inside a function) but NOT a coroutine: it has no
    // IR body to ramp, only a native C symbol. In T1 nothing on this path can
    // consume time -- an exported task the C reaches must complete in zero time
    // or the thunk reports a located fatal -- so the call site stays the ordinary
    // plain `Call` that lower_dpi_call marshals, and never grows a task ramp,
    // record slot or suspend arm it could not use.
    // TODO: T2 makes this a real task call site, so the caller can be parked
    // while the C frame waits (docs/design/dpi-c.md, "DPI tasks").
    if (sub.subroutineKind == SubroutineKind::Task && !sub.flags.has(MethodFlags::DPIImport)) {
        // A task is a statement, never a value; it blocks the caller until done.
        lower_task_call(call, sub, b, this_val);
        return std::nullopt;
    }
    const bool is_virtual = this_val && is_user_virtual(sub) && !is_super_call(call);
    // Resolve (and lazily lower) a direct callee up front — this also surfaces
    // formal-side guards (e.g. ref-arg restrictions) before arg marshaling. A
    // virtual call instead sources its callee from the vtable below, never
    // function_id(sub), which would lower slang's degenerate pure subroutine.
    const ir::FunctionId direct_id =
      is_virtual ? ir::FunctionId{}
                 : ((is_vif_call(call) || bare_vif) ? vif_function_id(sub) : function_id(sub));

    const auto formals = sub.getArguments();
    const auto actuals = call.arguments();
    if (formals.size() != actuals.size()) {
        fail("function argument count mismatch");
    }
    const ir::SourceLoc     l = loc(call);
    std::vector<CopyOutArg> outs;
    std::vector<ir::Index>  args =
      build_call_args(formals, actuals, outs, b, l, default_arg_recv(sub, this_val));
    if (this_val) {
        args.insert(args.begin(), *this_val);
    }
    // A virtual method invoked through an object handle dispatches dynamically
    // (LRM 8.20), except a `super.method()` call, which binds statically to the
    // base method. The receiver's static class type selects the vtable layout.
    //
    // Through an INTERFACE-class handle (LRM 8.26) the same call dispatches on
    // the design-wide index of the method's name instead of a vtable slot: slot
    // numbers are per single-inheritance hierarchy, so two unrelated implementers
    // of one interface do not agree on one (docs/design/interface-classes.md §5).
    ir::Index c;
    if (is_virtual) {
        const ir::Ref    recv  = virtual_recv_class(call);
        const uint32_t   iface = iface_dispatch_index(sub);
        const uint32_t   slot  = iface == ir::kNoIface ? virtual_slot_of(sub) : 0;
        c = b.call_virtual(virtual_sig_id(sub), slot, recv, args, l, iface);
    } else {
        c = b.call(direct_id, args, l);
    }
    // Output / inout formals copy back to their actuals after the call (a
    // function is zero-time, so this is the same instant as the call).
    emit_call_copyouts(outs, b, l);

    // A DPI import has no returnValVar even when non-void, so key the result
    // on the declared return type (void == no value). Uses the slang type, not
    // the lowered Function record, which is not yet set during a recursive
    // call (build_function sets the slot only after lowering its body).
    return sub.getReturnType().isVoid() ? std::nullopt : std::optional<ir::Index>(c);
}

// Lower a task enable (LRM 13.3). The backend drives the task coroutine and
// blocks the caller until it completes. Argument passing (13.5) is shared with
// function calls: build_call_args prepares each operand and records output /
// inout copy-backs, emit_call_copyouts applies them after the call. An unpacked
// struct / whole-array formal passes one storage pointer, exactly as it does for
// a function: the task's prologue copies it into the frame Local and its
// completion epilogue copies an output / inout Local back through it, so the
// copy-out lands when the task returns even if it consumed time.
void Lowerer::lower_task_call(const CallExpression&    call,
                              const SubroutineSymbol&  sub,
                              ir::Builder&             b,
                              std::optional<ir::Index> this_val) {
    // A virtual task invoked through a handle dispatches dynamically (LRM 8.20),
    // like a virtual function but over the task (coroutine) ABI. Skipping
    // function_id(sub) is load-bearing, exactly as on the function path: a PURE
    // virtual task's callee is slang's degenerate associated subroutine, which has
    // no body to lower — its prototype stub supplies the signature instead
    // (virtual_sig_id).
    const bool is_virtual = this_val && is_user_virtual(sub) && !is_super_call(call);
    // A receiver that is a virtual interface handle, or a bare sibling call from
    // inside a ctx-parameterized body, both dispatch to the one shared body.
    const bool           through_vif = is_vif_call(call) || vif_ctx_arg_wanted(sub);
    const ir::FunctionId id =
      is_virtual ? ir::FunctionId{} : (through_vif ? vif_function_id(sub) : function_id(sub));
    const auto           formals    = sub.getArguments();
    const auto           actuals    = call.arguments();
    if (formals.size() != actuals.size()) {
        fail("task argument count mismatch");
    }
    const ir::SourceLoc     l = loc(call);
    std::vector<CopyOutArg> outs;
    std::vector<ir::Index>  args =
      build_call_args(formals, actuals, outs, b, l, default_arg_recv(sub, this_val));
    if (this_val) {
        args.insert(args.begin(), *this_val);
    }
    if (is_virtual) {
        const ir::Ref  recv  = virtual_recv_class(call);
        const uint32_t iface = iface_dispatch_index(sub);
        const uint32_t slot  = iface == ir::kNoIface ? virtual_slot_of(sub) : 0;
        b.task_call_virtual(virtual_sig_id(sub), slot, recv, args, l, iface);
    } else {
        b.task_call(id, args, l);
    }
    emit_call_copyouts(outs, b, l);
}

// The receiver class type that selects a virtual call's vtable layout (LRM 8.20).
// Mirrors resolve_method_this' receiver shapes IN THE SAME PRECEDENCE, so the
// handle it returns and the layout chosen here always describe one object: an
// explicit `obj.f()`, else the receiver a default-argument expression evaluates
// against, else the object under construction in a property initializer, else
// the enclosing method's own `this`. Only reached when resolve_method_this
// produced a handle, so one of the four holds — which is what makes the final
// fn_ctx_ deref safe.
ir::Ref Lowerer::virtual_recv_class(const CallExpression& call) {
    if (call.thisClass() != nullptr) {
        return lower_type(*call.thisClass()->type);
    }
    if (default_arg_this_) {
        return default_arg_this_->cls;
    }
    if (ctor_this_) {
        return ctor_this_->cls;
    }
    return fn_ctx_->this_class;
}

// The design-wide interface-method index a dispatch bound through an
// interface-class handle uses (LRM 8.26), or kNoIface for an ordinary virtual
// call.
//
// slang hands a call through an interface handle the PROTOTYPE's fabricated stub
// subroutine -- bodyless, flagged Pure|Virtual, parented by the interface class
// (SubroutineSymbols.cpp:1177, Lookup.cpp:1988-1992) -- so the declaring class
// is what distinguishes the two dispatch kinds. A method inherited from a parent
// interface resolves to that parent's prototype, which yields the same name
// index, as LRM 8.26.6.1 requires.
uint32_t Lowerer::iface_dispatch_index(const slang::ast::SubroutineSymbol& sub) {
    if (!declaring_class(sub).isInterface) {
        return ir::kNoIface;
    }
    return iface_slot(sub.name);
}

// Resolve the `this` object handle for a possible method call (LRM 8.11). An
// explicit receiver (`obj.method()`) is call.thisClass(); a bare call uses the
// enclosing `this` — a method's own `this` Local, or the object under
// construction when the call sits in a property declaration initializer.
// Returns nullopt for a free subroutine (no thisVar) and for a static method.
std::optional<ir::Index> Lowerer::resolve_method_this(const CallExpression&   call,
                                                      const SubroutineSymbol& sub,
                                                      ir::Builder&            b) {
    // A static method has no `this` (LRM 8.10), and slang gives it no thisVar, so
    // build_function splices no receiver formal -- whichever way the call was
    // written. Written with an object handle (`p.f()`, which LRM 8.9 allows for
    // any static member: its own example reads `p.fileID` through a handle that
    // was never assigned), slang still reports the receiver as thisClass(), and
    // splicing it shifts every actual one formal to the left. The backend then
    // reads the handle under the NEXT formal's direction, so a `ref` formal asked
    // a value operand for its lvalue -- a segfault with no diagnostic.
    if (sub.flags.has(slang::ast::MethodFlags::Static)) {
        if (call.thisClass() != nullptr) {
            // Evaluated for its side effects, then dropped: the receiver can be any
            // expression (`make_pool().f()`), and the static-PROPERTY path
            // (value_lvalue) evaluates its prefix the same way before resolving to
            // the shared slot.
            lower_expr(*call.thisClass(), b);
        }
        return std::nullopt;
    }
    if (call.thisClass() != nullptr) {
        // For a virtual interface receiver this is the handle, which the
        // ctx-parameterized body takes as formal 0 (vif_function_id) exactly as
        // a class method takes `this`.
        return lower_expr(*call.thisClass(), b);
    }
    // A bare in-method call to an instance method uses the enclosing `this`. A
    // pure virtual has a null thisVar (slang's degenerate rep) yet is still an
    // instance method, so a virtual callee also takes this path (a free function
    // has neither a thisVar nor virtualness).
    if (sub.thisVar == nullptr && !is_user_virtual(sub)) {
        return std::nullopt;
    }
    if (default_arg_this_) {
        // A bare method call inside a DEFAULT argument expression (LRM 13.5.3):
        // the default was bound in the callee's declaration scope, so its
        // implicit `this` is the receiver of the call being made — not the
        // object under construction and not the enclosing method's `this`,
        // both of which are different objects whenever the receiver was
        // spelled out (`obj.f()` with a defaulted argument).
        return default_arg_this_->handle;
    }
    if (ctor_this_) {
        // A bare method call in a property declaration initializer (`int a =
        // f();`). The initializer runs as part of construction (LRM 8.7), so the
        // object being constructed IS `this` — but it is lowered at the `new`
        // site, so the handle comes from there rather than from a frame Local.
        // Checked BEFORE the frame: when the `new` sits inside some other
        // method, that method's `this` is a different object entirely.
        return ctor_this_->handle;
    }
    if (fn_ctx_ == nullptr || !fn_ctx_->has_this) {
        fail("class method call without an object handle is not supported");
    }
    const ir::SourceLoc l = loc(call);
    return this_handle(b, l);
}

// The receiver context a default-argument expression of a call to `sub`
// evaluates against (LRM 13.5.3), threaded into build_call_args: the resolved
// receiver handle plus the callee's DECLARING class, which types a bare
// property select and selects the vtable layout of a bare virtual call inside
// the default — the static type `this` has in the callee's own body. Nullopt
// for a free subroutine and for a virtual-interface receiver (an interface
// subroutine has no ClassType scope), whose defaults reference no class state.
std::optional<Lowerer::CtorThis> Lowerer::default_arg_recv(const SubroutineSymbol&  sub,
                                                           std::optional<ir::Index> this_val) {
    if (!this_val.has_value()) {
        return std::nullopt;
    }
    const slang::ast::Scope* sc = sub.getParentScope();
    while (sc != nullptr && sc->asSymbol().kind != SymbolKind::ClassType) {
        sc = sc->asSymbol().getParentScope();
    }
    if (sc == nullptr) {
        return std::nullopt;
    }
    return CtorThis{ .handle = *this_val,
                     .cls    = lower_type(sc->asSymbol().as<slang::ast::ClassType>()) };
}

// The enclosing `this` handle and its class, for a built-in randomization call
// written without a receiver (`randomize()`, `x.rand_mode(0)` inside a method).
std::pair<ir::Index, const slang::ast::ClassType*> Lowerer::implicit_this(ir::Builder& b, ir::SourceLoc l) {
    if (default_arg_this_) {
        // Same precedence, and the same reason, as resolve_method_this.
        return { default_arg_this_->handle, class_of_ref(default_arg_this_->cls) };
    }
    if (ctor_this_) {
        return { ctor_this_->handle, class_of_ref(ctor_this_->cls) };
    }
    if (fn_ctx_ == nullptr || !fn_ctx_->has_this) {
        fail("a randomization method call outside a class method needs an object handle");
    }
    return { this_handle(b, l), class_of_ref(fn_ctx_->this_class) };
}

// The ClassType a lowered Kind::Class Ref came from (class_type_cache_ inverted).
const slang::ast::ClassType* Lowerer::class_of_ref(ir::Ref ref) {
    for (const auto& [clt, r] : class_type_cache_) {
        if (r == ref) {
            return clt;
        }
    }
    fail("internal: class ref without a symbol");
}

// Build the call-argument operands for a user function / task (LRM 13.5): an
// input passes a value resized to the formal; a ref passes the actual's lvalue;
// an output / inout uses a fresh caller temp (copied in from the actual for
// inout) the backend passes by pointer. Each output / inout is recorded in
// `outs` for copy-out after the call. Shared by function and task calls — the
// copy-in / copy-out semantics are identical and delicate to correctness.
//
// `recv` is the receiver context of a class-method call, or nullopt for a free
// subroutine. An actual that IS the formal's default expression (pointer
// identity: slang binds an omitted argument as the formal's cached default,
// CallExpression.cpp bindArgs) lowers under that context (default_arg_this_),
// because LRM 13.5.3 evaluates the default in the callee's declaration scope —
// its implicit `this` is the receiver, not whatever object encloses the call.
// A written actual keeps the enclosing context, even when this call is itself
// nested inside another call's default expression.
std::vector<ir::Index> Lowerer::build_call_args(std::span<const FormalArgumentSymbol* const> formals,
                                                std::span<const Expression* const>           actuals,
                                                std::vector<CopyOutArg>&                     outs,
                                                ir::Builder&                                 b,
                                                ir::SourceLoc                                l,
                                                const std::optional<CtorThis>&               recv) {
    std::vector<ir::Index> args;
    args.reserve(formals.size());
    for (size_t i = 0; i < formals.size(); ++i) {
        const bool is_default =
          recv.has_value() && actuals[i] == formals[i]->getDefaultValue();
        const Restore<std::optional<CtorThis>> dflt(default_arg_this_,
                                                    is_default ? recv : default_arg_this_);
        const auto dir = formals[i]->direction;
        // A DPI open-array formal (LRM 35.5.6.2): pass the actual array's lvalue
        // leaf. The backend builds the svOpenArray descriptor from the actual's
        // array type at the call site. lower_type would fail on DPIOpenArrayType,
        // so intercept before it. Input-only (guarded in build_dpi_open_array_arg).
        if (formals[i]->getType().getCanonicalType().as_if<slang::ast::DPIOpenArrayType>()) {
            const Expression* actual = actuals[i];
            if (actual->kind == ExpressionKind::Assignment) {
                actual = &actual->as<AssignmentExpression>().left();
            }
            args.push_back(lower_lvalue(unwrap_conversion(*actual), b));
            continue;
        }
        // A `ref` actual is decided by DIRECTION, not by type: every storage
        // class aliases through the same fat ref, so this must precede the
        // by-value aggregate and handle paths below (which would otherwise
        // capture a ref aggregate / container formal and copy it).
        if (dir == slang::ast::ArgumentDirection::Ref) {
            args.push_back(lower_ref_actual(*actuals[i], b));
            continue;
        }
        const ir::Ref pty = lower_type(formals[i]->getType());
        if (is_agg_abi_ref(pty)) {
            // An unpacked aggregate actual passes by pointer (LRM 13.5.1 copies at
            // the boundary; the callee copies the pointee into its frame Local).
            // slang wraps an output / inout actual in an AssignmentExpression;
            // unwrap to the bound lvalue.
            //
            // A struct-bearing type has canonical aggregate storage already, so an
            // `input` hands over the actual's own storage. Everything else goes
            // through a canonical temp:
            //   - a flat (scalar-element) array's module storage may be
            //     may-z-narrowed (z-elided) relative to the callee's canonical
            //     formal, so the copy-in must be layout-converting; and
            //   - an `output` / `inout` write must reach the actual through the
            //     ordinary store path (emit_call_copyouts), or the callee's raw
            //     memcpy through the actual's pointer would leave the signal's
            //     wait queue unserviced -- `always @(s.a)` would not wake.
            const bool        flat   = !contains_struct_ref(pty);
            const Expression* actual = actuals[i];
            if (actual->kind == ExpressionKind::Assignment) {
                actual = &actual->as<AssignmentExpression>().left();
            }
            const Expression& src = unwrap_conversion(*actual);
            // A slice is a legal whole-array actual (LRM 7.4.5): same element
            // count, so it copies element-wise into the temp like any array.
            const bool lvalue_shape =
              is_struct_lvalue_shape(src) ||
              (flat && src.kind == ExpressionKind::RangeSelect && is_unpacked_array(src));
            if (!lvalue_shape) {
                if (dir != slang::ast::ArgumentDirection::In) {
                    // An output / inout actual must be valid on the left-hand
                    // side of a procedural assignment (LRM 13.5), and slang
                    // enforces that upstream ("expression is not assignable"),
                    // so what reaches here is an assignable form this path
                    // cannot decompose into a copy-back home.
                    // TODO: an aggregate-typed streaming lvalue actual
                    // (LRM 11.4.14.3).
                    fail(flat ? "whole-array argument must be a variable or slice"
                              : "aggregate argument must be a variable");
                }
                args.push_back(agg_input_actual(src, pty, b, l));
                continue;
            }
            if (dir == slang::ast::ArgumentDirection::In && !flat) {
                args.push_back(lower_lvalue(src, b));
                continue;
            }
            const ir::Index temp = make_canonical_agg_temp(pty, b, l);
            if (dir != slang::ast::ArgumentDirection::Out) {
                // An indexed slice fills the temp position-wise (it has no
                // anchored lvalue to copy from); anything else copies whole.
                if (is_positional_array_slice(src)) {
                    emit_indexed_slice_copy(src.as<RangeSelectExpression>(), temp,
                                            /*into_slice=*/false, ir::Inst::Tag::Store, b, l);
                } else {
                    emit_agg_copy(temp, lower_lvalue(src, b), pty, ir::Inst::Tag::Store, b, l);
                }
            }
            if (dir != slang::ast::ArgumentDirection::In) {
                outs.push_back(CopyOutArg{ temp, &src, pty, actual->type, /*is_agg=*/true, pty });
            }
            args.push_back(temp);
            continue;
        }
        // A dynamic-array actual passes one handle lvalue for every direction:
        // the callee deep-copies it in (input / inout), copies its frame handle
        // back out (output / inout), or aliases it (ref). No caller temp — the
        // actual is always a whole handle. slang wraps an output / inout actual
        // in an AssignmentExpression; unwrap to the bound lvalue.
        if (is_handle_ref(pty)) {
            const Expression* actual = actuals[i];
            if (actual->kind == ExpressionKind::Assignment) {
                actual = &actual->as<AssignmentExpression>().left();
            }
            args.push_back(lower_lvalue(*actual, b));
            continue;
        }
        if (dir == slang::ast::ArgumentDirection::In) {
            // A string actual passes as-is (no width to resize to); the callee's
            // copy-in gives value semantics.
            args.push_back(is_string_ref(pty) ? lower_expr(*actuals[i], b)
                                              : coerce(lower_expr(*actuals[i], b), *actuals[i]->type, pty, b, l));
            continue;
        }
        // slang models an output / inout actual as an AssignmentExpression whose
        // left side is the caller lvalue; unwrap it to the bound expression.
        const Expression* actual = actuals[i];
        if (actual->kind == ExpressionKind::Assignment) {
            actual = &actual->as<AssignmentExpression>().left();
        }
        if (is_string_ref(pty) != is_string_expr(*actual)) {
            // Backstop, not a gap: the copy-out is an assignment from the formal
            // to the actual, and neither direction of string <-> packed is
            // assignment compatible (LRM 6.16 wants an explicit cast), so slang
            // already rejects the mismatch at the call ("value of type 'string'
            // cannot be assigned to ..."). If one ever reached here the caller
            // temp would carry the FORMAL's type and both hops would need a
            // conversion -- StrFromPacked in, StrToPacked back -- which neither
            // `coerce` (a plane resize) nor a plain Store performs.
            fail("an output/inout string argument requires a string actual");
        }
        const ir::Index temp = make_arg_temp(pty, b, l);
        if (dir == slang::ast::ArgumentDirection::InOut) {
            // Copy-in: temp = (parameter type) actual. A string handle passes with
            // no resize (the Store deep-copies it), like the In case above.
            const ir::Index in = is_string_ref(pty) ? lower_expr(*actual, b)
                                                    : coerce(lower_expr(*actual, b), *actual->type, pty, b, l);
            b.binary(ir::Inst::Tag::Store, temp, in, l);
        }
        args.push_back(temp);
        outs.push_back(CopyOutArg{ temp, actual, lower_type(*actual->type), &formals[i]->getType() });
    }
    return args;
}

// A non-lvalue aggregate actual bound to an `input` formal, as the pointer
// operand the by-value ABI wants. LRM 13.5 allows ANY expression as an input
// actual, and 13.5.1 copies it into the subroutine area, so the value only
// needs a home the callee's copy-in can read.
//
// A call result already has one: the callee wrote it through the caller-side
// out-param temp that is the call's lvalue (the fact agg_return_operand also
// relies on), so it passes directly, evaluated once, and the callee's copy-in
// is the only copy. Everything else — a `'{...}` pattern, an aggregate `?:`,
// a bare concatenation literal — is built into a fresh canonical temp by the
// ordinary aggregate store path, which also owns the diagnostic for any form
// assignment cannot lower either. Each call site gets its own temp
// (make_canonical_agg_temp): a frame Local inside a subroutine, fresh per
// frame, so recursion and two actuals in one expression cannot alias; or a
// module signal in a process, safe to reuse across calls because the callee
// copies the pointee into its own frame before the caller can refill it.
ir::Index Lowerer::agg_input_actual(const Expression& src, ir::Ref pty, ir::Builder& b, ir::SourceLoc l) {
    if (src.kind == ExpressionKind::Call) {
        return lower_lvalue(src, b);
    }
    if (src.kind == ExpressionKind::RangeSelect) {
        // TODO: a slice of a struct-bearing array as a whole-array actual
        // (LRM 7.4.5). The backend has no aggregate-element slice copy
        // (agg_return_operand hits the same wall), so staging it here would
        // only move the failure later. A flat array's slice never arrives:
        // it is an lvalue shape at the call site.
        fail("a slice of a struct-bearing array argument is not supported");
    }
    const ir::Index temp = make_canonical_agg_temp(pty, b, l);
    emit_agg_store(temp, pty, src, ir::Inst::Tag::Store, b, l);
    return temp;
}

// Copy each output / inout temp back to its actual after the call: actual =
// (actual type) temp. Reuses the normal store path so width conversion and
// complex actual lvalues (bit / part selects) work.
void Lowerer::emit_call_copyouts(const std::vector<CopyOutArg>& outs, ir::Builder& b, ir::SourceLoc l) {
    for (const CopyOutArg& oc : outs) {
        if (oc.is_agg && is_positional_array_slice(*oc.actual)) {
            // An indexed-slice output / inout actual: write the callee-written
            // temp back position-wise, since the actual has no anchored lvalue.
            // An out-of-range position drops its write (LRM 7.4.5).
            emit_indexed_slice_copy(oc.actual->as<RangeSelectExpression>(), oc.temp,
                                    /*into_slice=*/true, ir::Inst::Tag::Store, b, l);
            continue;
        }
        const ir::Index dst = lower_lvalue(*oc.actual, b);
        if (oc.is_agg) {
            // A whole-array output / inout: copy the callee-written temp back
            // into the actual element-wise (layout-converting), the reverse of
            // the copy-in above.
            emit_agg_copy(dst, oc.temp, oc.agg_ty, ir::Inst::Tag::Store, b, l);
            continue;
        }
        const ir::Index tv = b.unary(ir::Inst::Tag::Load, oc.temp, l);
        // A string temp's Load is its handle, and formal type == actual type (a
        // mixed string / packed argument is refused in build_call_args), so there
        // is nothing to coerce: the Store below deep-copies the handle into the
        // actual, releasing whatever it held.
        const bool      str = is_string_ref(oc.actual_ty);
        const ir::Index val = str ? tv : coerce(tv, *oc.formal_ty, oc.actual_ty, b, l);
        // The copy-back is an ordinary procedural write to the actual, so a
        // forced / `assign`-driven actual drops it and keeps the override's
        // value (LRM 10.6.2, matching iverilog 13); the call itself and the
        // callee's write to its own temp are unaffected.
        store_or_gate(pca_info_of_lvalue(*oc.actual), ir::Inst::Tag::Store, dst, val, b, l);
    }
}

// Whether an expression names a whole PLACE a `ref` can alias (LRM 13.5.2), as
// opposed to a partial selection of one. A place is a named variable, an
// element of an unpacked array / dynamic array / queue / associative array, a
// member of an unpacked struct, or a class property — composed to any depth.
//
// The exclusion is a bit or part select of a packed value: that is a sub-slot
// bit range, not an addressable object, so no pointer can name it. slang
// already rejects it ("only variables, class properties, and members of
// unpacked structs and arrays are allowed"); this is the backstop.
bool Lowerer::is_whole_ref_actual(const Expression& a) {
    switch (a.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            return true;
        case ExpressionKind::ElementSelect: {
            // An element of any unpacked collection is a whole place; a select of
            // a PACKED value is a bit select, which is not.
            const auto& es = a.as<ElementSelectExpression>();
            return is_unpacked_container(*es.value().type) && is_whole_ref_actual(es.value());
        }
        case ExpressionKind::MemberAccess: {
            // An unpacked-struct member or a class property is a whole place. A
            // PACKED struct / union member is a part select of the flat value, so
            // it is not.
            const auto& ma = a.as<MemberAccessExpression>();
            if (ma.member.kind == SymbolKind::ClassProperty) {
                // The object body is reached through the handle, so the handle
                // expression itself need not be a place.
                return true;
            }
            return is_unpacked_struct(*ma.value().type) && is_whole_ref_actual(ma.value());
        }
        default:
            return false;
    }
}

// Lower a ref actual (LRM 13.5.2) to its lvalue: the backend turns that lvalue
// into the fat ref the callee aliases live (ref_actual_arg), including the
// relocation epoch when the place is rooted inside a container's heap storage.
ir::Index Lowerer::lower_ref_actual(const Expression& actual, ir::Builder& b) {
    const Expression* a = &actual;
    // slang may wrap a ref actual in an AssignmentExpression (as for out/inout);
    // unwrap to the bound lvalue.
    if (a->kind == ExpressionKind::Assignment) {
        a = &a->as<AssignmentExpression>().left();
    }
    if (!is_whole_ref_actual(*a)) {
        // A bit / part select of a packed value is a bit range, not an object a
        // pointer can name. There is nothing to implement here: LRM 13.5.2 does
        // not allow it and slang rejects it first.
        fail("ref argument actual must be a whole variable, element, member or "
             "class property, not a bit or part select");
    }
    return lower_lvalue(*a, b);
}

// A fresh, anonymous storage cell for an output / inout call argument, returned
// as an lvalue leaf. Inside a function / task caller it is a per-call frame
// Local; in a process it is a module Signal (a process has no frame). The same
// lvalue Index is reused for copy-in, the call operand, and copy-out.
ir::Index Lowerer::make_arg_temp(ir::Ref type, ir::Builder& b, ir::SourceLoc l) {
    if (fn_ctx_ != nullptr) {
        const auto idx = static_cast<uint32_t>(fn_ctx_->local_types.size());
        fn_ctx_->local_types.push_back(type);
        return b.local(idx, type, l);
    }
    ir::SignalInfo info;
    info.name = prefix_ + "__taskarg" + std::to_string(task_temp_seq_++);
    info.type = type;
    info.dir  = ir::SignalInfo::Direction::Internal;
    return b.signal(register_signal(std::move(info)), l);
}

// A fresh canonical-aggregate array temp for a by-value whole-array actual: a
// per-call frame Local inside a function / task (which build_frame_layout gives
// canonical aggregate storage), or a force_agg module Signal in a process
// (whose flat storage would otherwise be may-z-narrowed). Either way its layout
// is memcpy-compatible with the callee's canonical aggregate formal.
ir::Index Lowerer::make_canonical_agg_temp(ir::Ref type, ir::Builder& b, ir::SourceLoc l) {
    if (fn_ctx_ != nullptr) {
        const auto idx = static_cast<uint32_t>(fn_ctx_->local_types.size());
        fn_ctx_->local_types.push_back(type);
        return b.local(idx, type, l);
    }
    ir::SignalInfo info;
    info.name      = prefix_ + "__aggarg" + std::to_string(task_temp_seq_++);
    info.type      = type;
    info.dir       = ir::SignalInfo::Direction::Internal;
    info.force_agg = true;
    return b.signal(register_signal(std::move(info)), l);
}

} // namespace clotho::lower
