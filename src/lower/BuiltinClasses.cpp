// Built-in classes: process, semaphore and mailbox types and methods.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Lower the built-in `process` class (LRM 9.7). Unlike a user class, its body is
// not a property table: slang synthesizes only method signatures, so the object
// carries ONE hidden `chandle` naming the thread's control block (a native,
// untraced record — see docs/design/process-class.md §4). Everything else about
// the handle is an ordinary class handle: a 64-bit 2-state word, `null` == 0,
// `==` an identity compare, storable in a variable or a class property.
//
// The class is `final` and `abstract` in slang, so there is no derivation and no
// user `new` (slang rejects it), and every method is `MethodFlags::BuiltIn`, so
// compute_vtable mints no dispatch slot for one — only the shallow-copy thunk
// every class gets, which is unreachable here for the same reason `new` is.
//
// It does get the two RESERVED randomization slots, because compute_vtable
// reserves them for every class (LRM 18 makes `randomize` a member of every
// class, `process` included). So this path owes what the user-class path owes:
// an empty `ClassRand` for the generated bodies to read, and the same
// outermost-frame drain that builds them -- without both, the two FunctionIds
// compute_vtable reserved stay unbuilt and the backend declares a function with
// no return type.
ir::Ref Lowerer::lower_process_class(const slang::ast::ClassType& clt) {
    const uint32_t fid = module_.pool.reserve_struct_fields();
    ir::Type       t;
    t.kind      = ir::Type::Kind::Class;
    t.fields_id = fid;
    t.element   = ir::Ref::Invalid;
    const ir::Ref ref = module_.pool.put(ir::Item{ t });
    bind_class_type(clt, ref);
    module_.set_process_class(ref);
    std::vector<ir::FieldRow> fields;
    fields.push_back({ kProcessCtlField, make_int_type(64, false), 0 });
    module_.pool.set_struct_fields(fid, std::move(fields));
    class_rand_.emplace(&clt, ClassRand{});
    compute_vtable(clt, ref);
    if (class_depth_ == 1) {
        drain_rand_bodies();
    }
    return ref;
}

// Lower the built-in `semaphore` class (LRM 15.3). Like `process` its body is
// not a property table -- slang synthesizes signatures only -- so it is
// synthesized here: the key count, plus the two words of its FIFO wait queue.
// Everything else is an ordinary class: the handle is a 64-bit 2-state word,
// `null` is 0, `==` is identity, and it is storable in a variable, a class
// property or a container. See docs/design/mailbox-and-semaphore.md §4.1.
//
// The key count is SIGNED 64-bit, not `int`: LRM 15.3.1 says the count "can
// increase beyond keyCount when more keys are put into the semaphore than are
// removed", so a 32-bit accumulator would wrap in a long run of unbalanced
// `put`s. The two queue words are chandle-shaped (2-state, 64-bit), so no
// collector ever sees the coroutine-frame pointers they hold.
ir::Ref Lowerer::lower_semaphore_class(const slang::ast::ClassType& clt) {
    const uint32_t fid = module_.pool.reserve_struct_fields();
    ir::Type       t;
    t.kind      = ir::Type::Kind::Class;
    t.fields_id = fid;
    t.element   = ir::Ref::Invalid;
    const ir::Ref ref = module_.pool.put(ir::Item{ t });
    bind_class_type(clt, ref);
    std::vector<ir::FieldRow> fields;
    push_sync_body(clt, fields);
    module_.pool.set_struct_fields(fid, std::move(fields));
    register_sync_notifier(clt, ref);
    // Same two obligations lower_process_class has: the reserved randomization
    // slots need an (empty) rand block for the generated bodies to read, and the
    // outermost frame drains the ids compute_vtable reserved.
    class_rand_.emplace(&clt, ClassRand{});
    compute_vtable(clt, ref);
    if (class_depth_ == 1) {
        drain_rand_bodies();
    }
    return ref;
}

// How many body fields the built-in synchronization class `sync` takes.
uint32_t Lowerer::sync_body_fields(const slang::ast::ClassType& sync) {
    return sync.name == "mailbox" ? kMbxPeekTail + 1 : kSemQTail + 1;
}

// Give `class_ref`, which is `sync` or a class derived from it, the design-wide
// notifier of the state a function of `sync` reads: a mailbox's FIFO, a semaphore's
// key count. One exists when some `@` or `wait` calls such a function, at its top
// (`wait (mb.num() != 0)`) or through a subroutine (CalleeReads::builtins), and
// then every `put`, `get` and `try_*` fires it by storing where it stores.
void Lowerer::register_sync_notifier(const slang::ast::ClassType& sync, ir::Ref class_ref) {
    const bool mailbox = sync.name == "mailbox";
    register_shared_notifier(sync, class_ref, mailbox ? kMbxFifo : kSemKeys, /*container=*/mailbox);
}

// Append the synthetic body of `semaphore` or of one `mailbox #(T)` to `fields`:
// the body of that class, and the first fields of every class derived from it
// (LRM 8.13), which is what lets a built-in method reach them through a handle
// of either.
void Lowerer::push_sync_body(const slang::ast::ClassType& sync, std::vector<ir::FieldRow>& fields) {
    const ir::Ref word = make_int_type(64, false);
    if (sync.name != "mailbox") {
        fields.push_back({ kSemKeysField, word, kSemKeys });
        fields.push_back({ kSemHeadField, word, kSemQHead });
        fields.push_back({ kSemTailField, word, kSemQTail });
        return;
    }
    const slang::ast::Symbol* tsym = sync.find("T");
    if (tsym == nullptr || tsym->kind != SymbolKind::TypeParameter) {
        fail("internal: std::mailbox without its type parameter");
    }
    const slang::ast::Type& mt = tsym->as<slang::ast::TypeParameterSymbol>().targetType.getType();
    if (mt.isError() || mt.getCanonicalType().kind == SymbolKind::UntypedType) {
        // TODO: the typeless mailbox (LRM 15.4.9's `dynamic_type`). It needs a
        // dynamically typed element -- a per-slot descriptor plus a run-time
        // type identity for 15.4.5's type check, 15.4.6's negative return and
        // 15.4.8's -- which is a new rt_elem kind and a new notion of type
        // equality (docs/design/mailbox-and-semaphore.md §8).
        fail("the typeless std::mailbox (LRM 15.4.9) is not supported; use mailbox #(T)");
    }
    const ir::Ref elem = lower_type(mt);
    guard_container_element(mt.getCanonicalType(), elem, "mailbox message");
    ir::Type qt;
    qt.kind            = ir::Type::Kind::Queue;
    qt.element         = elem;
    qt.left            = 0; // unbounded; the mailbox bound is enforced in emitted IR
    const ir::Ref fifo = module_.pool.put(ir::Item{ qt });
    fields.push_back({ kMbxBoundField, word, kMbxBound });
    fields.push_back({ kMbxFifoField, fifo, kMbxFifo });
    fields.push_back({ kMbxGetHeadField, word, kMbxGetHead });
    fields.push_back({ kMbxGetTailField, word, kMbxGetTail });
    fields.push_back({ kMbxPutHeadField, word, kMbxPutHead });
    fields.push_back({ kMbxPutTailField, word, kMbxPutTail });
    fields.push_back({ kMbxPeekHeadField, word, kMbxPeekHead });
    fields.push_back({ kMbxPeekTailField, word, kMbxPeekTail });
}

// `semaphore::new(int keyCount = 0)` and `mailbox::new(int bound = 0)` (LRM 15.3.1,
// 15.4.1), run against `handle`, an object of `class_ref`, which is `sync` or a
// class derived from it. Both are BODYLESS built-in constructors: emit_ctor_call
// would lower slang's empty prototype and die on an out-of-range formal index, so
// the argument is stored into the synthetic body here instead. The rest of the
// body powers on all-zero, which is exactly an empty wait queue. `where` names the
// construction in the report a negative bound gets.
void Lowerer::emit_sync_ctor(const slang::ast::ClassType&       sync,
                             std::span<const Expression* const> actuals,
                             ir::Index                          handle,
                             ir::Ref                            class_ref,
                             slang::SourceLocation              where,
                             ir::Builder&                       b,
                             ir::SourceLoc                      l) {
    const ir::Ref i64 = make_int_type(64, false);
    ir::Index     arg = b.constant(module_.pool.put_int(i64, 0), l);
    if (actuals.size() == 1) {
        const Expression& a = *actuals[0];
        // coerce, not resize: the actual is a signed `int`, and a
        // negative bound / key count must sign-extend to be seen as one.
        arg = coerce(lower_expr(a, b), *a.type, i64, b, l);
    }
    const bool is_mbx = sync.name == "mailbox";
    // Only a bound that was written can be negative, and only that has a place to
    // name: the default is 0.
    if (is_mbx && actuals.size() == 1) {
        // LRM 15.4.1: "The bound shall be positive. Negative bounds are
        // illegal and can result in indeterminate behavior, but
        // implementations can issue a warning." Clotho never silently
        // accepts, so it reports -- at run time, because the argument need
        // not be constant.
        const ir::Index bad = b.binary(ir::Inst::Tag::SLt, arg, b.constant(module_.pool.put_int(i64, 0), l), l);
        b.open_region();
        {
            const std::string banner = banner_at(where);
            emit_report(b, static_cast<uint32_t>(ir::Severity::Fatal), banner,
                        "mailbox bound is negative (LRM 15.4.1)", {}, l);
            b.finish(banner + "mailbox", 1, l);
        }
        const ir::ExtraSlice then_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice else_r = b.close_region();
        b.cond(bad, then_r, else_r, l, /*unlikely=*/true);
    }
    b.binary(ir::Inst::Tag::Store,
             b.member_select(b.handle_deref(handle, class_ref, l), is_mbx ? kMbxBound : kSemKeys, class_ref, l),
             arg,
             l);
}

// Whether `sub` is one of the built-in synchronization classes' methods (LRM
// 15.3, 15.4). Keyed on the owning ClassType's name and package for the reason
// is_process_method is: a method call can be lowered before any variable of the
// type has been declared.
bool Lowerer::is_sync_method(const slang::ast::SubroutineSymbol& sub) {
    if (!sub.flags.has(slang::ast::MethodFlags::BuiltIn)) {
        return false;
    }
    const slang::ast::Scope* owner = sub.getParentScope();
    if (owner == nullptr || owner->asSymbol().kind != SymbolKind::ClassType ||
        (owner->asSymbol().name != "semaphore" && owner->asSymbol().name != "mailbox")) {
        return false;
    }
    const slang::ast::Scope* pkg = owner->asSymbol().getParentScope();
    return pkg != nullptr && pkg->asSymbol().kind == SymbolKind::Package && pkg->asSymbol().name == "std";
}

// Whether `clt` is a built-in synchronization class of the `std` package.
bool Lowerer::is_sync_class(const slang::ast::ClassType& clt) {
    return is_sync_builtin_class(clt);
}

// Lower one `mailbox #(T)` specialization (LRM 15.4). slang monomorphizes the
// generic class, so each specialization is an independent nominal class here --
// no different from a user `C #(T)` -- and the body is synthesized the way
// `semaphore`'s is: the bound, the message FIFO, and the three wait queues.
//
// The FIFO is an UNBOUNDED queue type whatever the mailbox bound is. A bounded
// queue type means something else entirely: an overflowing push silently drops
// the element at the opposite end (Ir.h's QueuePushBack), where a full mailbox
// must BLOCK (15.4). So the bound is tested in the emitted code instead.
//
// `get` waiters and `peek` waiters are separate queues because a peek consumes
// nothing: LRM 15.4.7 unblocks every peeker while a message remains, which is
// the one place the FIFO discipline of 15.4.5 must not apply (§6.2 of the
// design doc).
ir::Ref Lowerer::lower_mailbox_class(const slang::ast::ClassType& clt) {
    // The body first: it lowers the message type, which is what can refuse.
    std::vector<ir::FieldRow> fields;
    push_sync_body(clt, fields);
    const uint32_t fid = module_.pool.reserve_struct_fields();
    ir::Type       t;
    t.kind      = ir::Type::Kind::Class;
    t.fields_id = fid;
    t.element   = ir::Ref::Invalid;
    const ir::Ref ref = module_.pool.put(ir::Item{ t });
    bind_class_type(clt, ref);
    module_.pool.set_struct_fields(fid, std::move(fields));
    register_sync_notifier(clt, ref);
    class_rand_.emplace(&clt, ClassRand{});
    compute_vtable(clt, ref);
    if (class_depth_ == 1) {
        drain_rand_bodies();
    }
    return ref;
}

// How many body fields `std::weak_reference` takes in this design: the referent,
// and its change notifier when some `@` or `wait` reads `get()` of this
// specialization (WaitPropScan). Answered from the design-wide pre-pass alone, so
// the class and every class derived from it agree before either is laid out.
uint32_t Lowerer::weak_body_fields(const slang::ast::ClassType& weak) const {
    return waited_props_ != nullptr && waited_props_->waits_weak(weak) ? 2 : 1;
}

// Append the synthetic body of `std::weak_reference #(T)` (LRM 8.30) to `fields`,
// and register what the backend needs to know about `class_ref`, which is that
// class or one derived from it.
//
// The referent is a property of type T like any handle property: `get()` loads it,
// `clear()` stores null into it, a shallow copy copies it. What makes it weak is one
// fact about the class, recorded here: the backend leaves this field out of the
// class's trace map and names it there as the weak referent instead, and the
// collector stores null into it when the referent is reachable no other way
// (ir::WeakClass, rt_gc_map::weak_referent).
//
// The notifier is the any-change notifier a waited-on property has
// (ir::ChangeNotify), so a store through `clear()` fires it as any store to a
// waited-on property does, and the collector fires it when it clears the referent.
void Lowerer::push_weak_body(const slang::ast::ClassType& weak, ir::Ref class_ref, std::vector<ir::FieldRow>& fields) {
    const slang::ast::Symbol* tsym = weak.find("T");
    if (tsym == nullptr || tsym->kind != SymbolKind::TypeParameter) {
        fail("internal: std::weak_reference without its type parameter");
    }
    // A class type: slang refuses any other argument (LRM 8.30.1).
    const slang::ast::Type& referent = tsym->as<slang::ast::TypeParameterSymbol>().targetType.getType();
    fields.push_back({ kWeakReferentField, lower_type(referent), kWeakReferent });
    module_.set_weak_class(class_ref, kWeakReferent);
    // The referents' design-wide notifier, when a subroutine some wait calls reads
    // get() (CalleeReads::builtins).
    register_shared_notifier(weak, class_ref, kWeakReferent, /*container=*/false);
    if (weak_body_fields(weak) > 1) {
        fields.push_back({ kWeakNotifyField, module_.pool.event_type, kWeakNotify });
        module_.set_change_notify(class_ref, kWeakReferent, ir::Edge::Any, kWeakNotify, /*container=*/false);
    }
}

// Lower one `std::weak_reference #(T)` specialization (LRM 8.30). Like `mailbox` it
// is a generic class slang monomorphizes and gives signatures only, so the body is
// synthesized (push_weak_body) and the four methods are lowered in place
// (lower_weak_method). In every other way it is an ordinary class: an instance is
// collected like any object (8.30.1), two instances with one referent are two
// objects (8.30.2), and a user class may derive from it.
//
// Interned before the referent type is lowered, as a user class is before its
// properties: `class C; weak_reference #(C) w; endclass` reaches this class from
// inside C's own layout, and lowering C from here must find it.
ir::Ref Lowerer::lower_weak_class(const slang::ast::ClassType& clt) {
    const uint32_t fid = module_.pool.reserve_struct_fields();
    ir::Type       t;
    t.kind            = ir::Type::Kind::Class;
    t.fields_id       = fid;
    t.element         = ir::Ref::Invalid;
    const ir::Ref ref = module_.pool.put(ir::Item{ t });
    bind_class_type(clt, ref);
    // An entry for a class that failed to lower must not survive to be reused
    // (lower_class_type's InternGuard says why).
    struct Abandon {
        ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::Ref>& table;
        const slang::ast::ClassType*                                         key;
        bool                                                                 committed = false;
        ~Abandon() {
            if (!committed) {
                table.erase(key);
            }
        }
    } abandon{ class_type_cache_, &clt };
    std::vector<ir::FieldRow> fields;
    push_weak_body(clt, ref, fields);
    module_.pool.set_struct_fields(fid, std::move(fields));
    // The two obligations lower_process_class has: an (empty) rand block for the
    // reserved randomization slots, and the outermost frame's drain.
    class_rand_.emplace(&clt, ClassRand{});
    compute_vtable(clt, ref);
    if (class_depth_ == 1) {
        drain_rand_bodies();
    }
    abandon.committed = true;
    return ref;
}

// `weak_reference::new(T referent)` (LRM 8.30.2), run against `handle`, an object of
// `class_ref` or of a class derived from it. A bodyless built-in constructor, so the
// one thing it does is stored here. A null referent is stored as it is and reported
// by nothing (8.30.2).
void Lowerer::emit_weak_ctor(std::span<const Expression* const> actuals,
                             ir::Index                          handle,
                             ir::Ref                            class_ref,
                             ir::Builder&                       b,
                             ir::SourceLoc                      l) {
    if (actuals.size() != 1) {
        fail("internal: weak_reference::new takes the referent and nothing else");
    }
    const ir::Index referent = lower_expr(*actuals[0], b);
    b.binary(ir::Inst::Tag::Store,
             b.member_select(b.handle_deref(handle, class_ref, l), kWeakReferent, class_ref, l),
             referent,
             l);
}

// The object a built-in method of `weak_reference`, `semaphore` or `mailbox` acts
// on, and its static class: the receiver the call names, or `this` for an
// unqualified call (or one through `super`) in a method of a class derived from the
// built-in one.
std::pair<ir::Index, ir::Ref> Lowerer::builtin_receiver(const CallExpression& call, ir::Builder& b) {
    if (const Expression* recv = call.thisClass(); recv != nullptr) {
        return { lower_expr(*recv, b), lower_type(*recv->type) };
    }
    if (ctor_this_) {
        return { ctor_this_->handle, ctor_this_->cls };
    }
    if (fn_ctx_ != nullptr && fn_ctx_->has_this) {
        return { this_handle(b, loc(call)), fn_ctx_->this_class };
    }
    // slang resolves a non-static method call against an object or not at all.
    fail("internal: built-in method '" + std::string(call.getSubroutineName()) + "' called on no object");
}

// Lower one of the four built-in `weak_reference` methods (LRM 8.30.2 to 8.30.5);
// `new` is emit_weak_ctor.
std::optional<ir::Index> Lowerer::lower_weak_method(const CallExpression&   call,
                                                    const SubroutineSymbol& sub,
                                                    ir::Builder&            b) {
    const ir::SourceLoc l = loc(call);
    if (sub.name == "get_id") {
        // LRM 8.30.5, static: 0 for null, otherwise a value that is the object's own
        // whatever class the handle is typed as, and no other live object's. That is
        // the object's identity number (rt_gc_identity), which is never handed out
        // twice in a run, so the reuse 8.30.5 allows after collection does not happen.
        if (call.arguments().size() != 1) {
            fail("internal: weak_reference::get_id takes one object");
        }
        return b.handle_identity(lower_expr(*call.arguments()[0], b), l);
    }
    const auto [handle, cls] = builtin_receiver(call, b);
    const auto referent      = [&, handle = handle, cls = cls] {
        return b.member_select(b.handle_deref(handle, cls, l), kWeakReferent, cls, l);
    };
    if (sub.name == "get") {
        // LRM 8.30.3: the referent, or null once the reference has been cleared, by
        // clear() or by the collector. Both store null where this loads.
        return b.unary(ir::Inst::Tag::Load, referent(), l);
    }
    if (sub.name == "clear") {
        // LRM 8.30.4. An ordinary store, so a wait on get() sees it.
        const ir::Index dst  = referent();
        const ir::Index null = b.constant(module_.pool.put_int(make_int_type(64, false), 0), l);
        b.binary(ir::Inst::Tag::Store, dst, null, l);
        return std::nullopt;
    }
    // Every method of LRM 8.30 is handled above; a fifth would come from a slang
    // release, not from a design.
    fail("internal: unknown weak_reference method '" + std::string(sub.name) + "'");
}

// Load the handle of the change notifier of the weak reference `get` is called on,
// dereferencing that weak reference where the wait is, as prop_notifier_handle does
// for a property.
ir::Index Lowerer::weak_notifier_handle(const CallExpression& get, ir::Builder& b) {
    const ir::SourceLoc l      = loc(get);
    const auto [handle, cls]   = builtin_receiver(get, b);
    const ir::ChangeNotify* cn = module_.change_notify(cls, kWeakReferent);
    if (cn == nullptr || cn->field[static_cast<uint32_t>(ir::Edge::Any)] == ir::kNoNotify) {
        // The design-wide pre-pass decides which weak reference classes carry a
        // notifier and this decides which are used; they read the same resolver.
        fail("internal: no change notifier for weak_reference::get()");
    }
    return b.unary(
      ir::Inst::Tag::Load,
      b.member_select(b.handle_deref(handle, cls, l), cn->field[static_cast<uint32_t>(ir::Edge::Any)], cls, l),
      l);
}

// Whether `sub` is one of `process`'s nine built-in methods (LRM 9.7). Keyed on
// the owning ClassType's name and package rather than on a remembered Ref: a
// method call can be lowered before any `process` variable has been declared.
bool Lowerer::is_process_method(const slang::ast::SubroutineSymbol& sub) {
    if (!sub.flags.has(slang::ast::MethodFlags::BuiltIn)) {
        return false;
    }
    const slang::ast::Scope* owner = sub.getParentScope();
    if (owner == nullptr || owner->asSymbol().kind != SymbolKind::ClassType ||
        owner->asSymbol().name != "process") {
        return false;
    }
    const slang::ast::Scope* pkg = owner->asSymbol().getParentScope();
    return pkg != nullptr && pkg->asSymbol().kind == SymbolKind::Package && pkg->asSymbol().name == "std";
}

// Lower one of the nine built-in `process` methods (LRM 9.7). `self` is static
// (no receiver); every other method takes the receiver's control block. Each
// lowers to a ProcQuery / ProcCtl / ProcWait instruction, never to a call: slang
// gives these methods no body.
std::optional<ir::Index> Lowerer::lower_process_method(const CallExpression&   call,
                                                       const SubroutineSymbol& sub,
                                                       ir::Builder&            b) {
    const ir::SourceLoc l = loc(call);
    // The receiver handle, for every method except the static `self`.
    const auto recv = [&]() -> ir::Index {
        if (call.thisClass() == nullptr) {
            fail("process::" + std::string(sub.name) + " needs an explicit process handle");
        }
        return lower_expr(*call.thisClass(), b);
    };
    if (sub.name == "self") {
        // `process::self()` is static: its return type IS the process class, so
        // lowering that type is also what interns the class body (and is a no-op
        // once some `process` variable has been declared).
        return b.proc_query(ir::ProcOp::Self, {}, lower_type(sub.getReturnType()), l);
    }
    if (sub.name == "status") {
        // The `state` typedef over an int enum, so an ordinary 32-bit result.
        const std::array<ir::Index, 1> args{ recv() };
        return b.proc_query(ir::ProcOp::Status, args, lower_type(sub.getReturnType()), l);
    }
    if (sub.name == "kill") {
        // LRM 9.7: end the process and every descendant it forked.
        const std::array<ir::Index, 1> args{ recv() };
        b.proc_ctl(ir::ProcOp::Kill, args, l);
        return std::nullopt;
    }
    if (sub.name == "srandom") {
        // LRM 18.14: reseed that thread's generator.
        const std::array<ir::Index, 2> args{ recv(), lower_expr(*call.arguments()[0], b) };
        b.proc_ctl(ir::ProcOp::Srandom, args, l);
        return std::nullopt;
    }
    if (sub.name == "get_randstate") {
        const std::array<ir::Index, 1> args{ recv() };
        return b.proc_query(ir::ProcOp::RandstateGet, args, module_.pool.string_type, l);
    }
    if (sub.name == "set_randstate") {
        const std::array<ir::Index, 2> args{ recv(), lower_expr(*call.arguments()[0], b) };
        b.proc_ctl(ir::ProcOp::RandstateSet, args, l);
        return std::nullopt;
    }
    if (sub.name == "suspend" || sub.name == "resume") {
        // LRM 9.7: suspend this thread or another one; resume restarts a suspended
        // thread. Both are functions, not tasks -- even though suspending the
        // CALLER parks it, which is the suspension itself rather than a wait for a
        // value, and is why `suspend` needs no ProcWait.
        const std::array<ir::Index, 1> args{ recv() };
        b.proc_ctl(sub.name == "suspend" ? ir::ProcOp::Suspend : ir::ProcOp::Resume, args, l);
        return std::nullopt;
    }
    if (sub.name == "await") {
        // A task: it parks the calling thread until the target completes, so it
        // is a statement with no value (LRM 9.7).
        const std::array<ir::Index, 1> args{ recv() };
        b.proc_wait(ir::ProcOp::Await, args, l);
        return std::nullopt;
    }
    // Every method of LRM 9.7 is handled above; a new one would arrive with a new
    // slang release rather than from a design.
    fail("process::" + std::string(sub.name) + "() is not supported");
}

// Lower a call to a built-in synchronization method (LRM 15.3, 15.4). Each
// expands HERE, into ordinary IR over the object's synthetic body plus the two
// wait-queue primitives: the resource test is a frontend compare, the park /
// wake discipline is the backend's. See docs/design/mailbox-and-semaphore.md §6.
std::optional<ir::Index> Lowerer::lower_sync_method(const CallExpression&   call,
                                                    const SubroutineSymbol& sub,
                                                    ir::Builder&            b) {
    const std::string_view cls = sub.getParentScope()->asSymbol().name;
    if (cls == "mailbox") {
        return lower_mailbox_method(call, sub, b);
    }
    const ir::SourceLoc l      = loc(call);
    const auto [handle, ref]   = builtin_receiver(call, b);
    const ir::Ref   i64    = make_int_type(64, false);
    const ir::Ref   int_ty = make_int_type(32, true);
    // The body lvalue is re-derived per access rather than held across the park:
    // a suspend may have run in between, and an lvalue is a place, not a copy.
    const auto field = [&](uint32_t idx) {
        return b.member_select(b.handle_deref(handle, ref, l), idx, ref, l);
    };
    const auto keys     = [&] { return b.unary(ir::Inst::Tag::Load, field(kSemKeys), l); };
    const auto qwords   = [&] {
        return std::array<ir::Index, 2>{ field(kSemQHead), field(kSemQTail) };
    };
    // `keyCount` defaults to 1 (`put`/`get`/`try_get`); slang materialises the
    // default actual, so an absent one is an internal error rather than a case.
    const auto count = [&]() -> ir::Index {
        if (call.arguments().size() != 1) {
            fail("semaphore::" + std::string(sub.name) + " expects one argument");
        }
        const Expression& a = *call.arguments()[0];
        return coerce(lower_expr(a, b), *a.type, i64, b, l);
    };

    if (sub.name == "put") {
        // LRM 15.3.2: return the keys, then "if a process has been suspended
        // waiting for a key, that process shall execute if enough keys have been
        // returned" -- the woken head re-tests and takes them.
        const ir::Index n   = count();
        const ir::Index dst = field(kSemKeys);
        const ir::Index val = b.binary(ir::Inst::Tag::Add, keys(), n, l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
        b.sync_ctl(ir::SyncOp::WakeHead, sync_site_seq_++, qwords(), l);
        return std::nullopt;
    }
    if (sub.name == "try_get") {
        // LRM 15.3.4 defines this on availability alone: it never joins the
        // waiting queue, so 15.3.3's FIFO rule -- which is about that queue --
        // does not constrain it, and a queued waiter does not block it. Recorded
        // in the design doc §15; Verilator agrees.
        const ir::Index n     = count();
        const ir::Index ok    = b.binary(ir::Inst::Tag::SGe, keys(), n, l);
        const ir::Index dst   = field(kSemKeys);
        const ir::Index taken = b.binary(ir::Inst::Tag::Sub, keys(), n, l);
        const ir::Index kept  = keys();
        const ir::Index val   = b.select(ok, taken, kept, l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
        const ir::Index granted = b.constant(module_.pool.put_int(int_ty, 1), l);
        const ir::Index denied  = b.constant(module_.pool.put_int(int_ty, 0), l);
        return b.select(ok, granted, denied, l);
    }
    if (sub.name == "get") {
        const uint32_t  site = sync_site_seq_++;
        const ir::Index n    = count();
        // The fast path is taken only when NOBODY is queued: LRM 15.3.3's FIFO
        // rule (and Accellera's Interpretation #1 of it) makes a blocked head
        // block later arrivals even when the keys would satisfy them.
        const ir::Index head              = b.unary(ir::Inst::Tag::Load, field(kSemQHead), l);
        const ir::Index                none = b.constant(module_.pool.put_int(make_int_type(64, false), 0), l);
        const ir::Index empty             = b.binary(ir::Inst::Tag::Eq, head, none, l);
        const std::array<ir::Index, 2> qw = qwords();
        emit_sync_park_loop(
          site,
          qw,
          empty,
          [&] { return b.binary(ir::Inst::Tag::SGe, keys(), n, l); },
          /*require_head=*/true,
          b,
          l);
        const ir::Index dst = field(kSemKeys);
        const ir::Index val = b.binary(ir::Inst::Tag::Sub, keys(), n, l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
        return std::nullopt;
    }
    // Every method of LRM 15.3 is handled above; a new one would arrive with a
    // new slang release rather than from a design.
    fail("semaphore::" + std::string(sub.name) + "() is not supported");
}

// Lower a call to a built-in `mailbox #(T)` method (LRM 15.4). The FIFO is an
// ordinary queue member, so `num` / the `try_*` trio are the queue tags plus one
// bound test, and the three blocking methods wrap those in the same park /
// retest / leave loop `semaphore::get` uses. §6.2 of the design doc.
std::optional<ir::Index> Lowerer::lower_mailbox_method(const CallExpression&   call,
                                                       const SubroutineSymbol& sub,
                                                       ir::Builder&            b) {
    const ir::SourceLoc l    = loc(call);
    const auto [handle, ref] = builtin_receiver(call, b);
    const ir::Ref   fifo_ty =
      module_.pool.struct_fields(std::get<ir::Type>(module_.pool.get(ref)).fields_id)[kMbxFifo].type;
    const ir::Ref elem_ty = std::get<ir::Type>(module_.pool.get(fifo_ty)).element;
    const ir::Ref i64     = make_int_type(64, false);
    const ir::Ref int_ty  = make_int_type(32, true);
    const ir::Ref word    = make_int_type(64, false);

    const auto field = [&](uint32_t idx) {
        return b.member_select(b.handle_deref(handle, ref, l), idx, ref, l);
    };
    const auto size  = [&] { return b.queue_size(field(kMbxFifo), fifo_ty, l); };
    const auto bound = [&] { return b.unary(ir::Inst::Tag::Load, field(kMbxBound), l); };
    const auto zero64 = [&] { return b.constant(module_.pool.put_int(i64, 0), l); };
    const auto one32  = [&] { return b.constant(module_.pool.put_int(int_ty, 1), l); };
    const auto zero32 = [&] { return b.constant(module_.pool.put_int(int_ty, 0), l); };
    const auto qempty = [&](uint32_t head) {
        const ir::Index first = b.unary(ir::Inst::Tag::Load, field(head), l);
        const ir::Index none  = b.constant(module_.pool.put_int(word, 0), l);
        return b.binary(ir::Inst::Tag::Eq, first, none, l);
    };
    const auto qwords = [&](uint32_t head, uint32_t tail) {
        return std::array<ir::Index, 2>{ field(head), field(tail) };
    };
    // Room for one more message: an unbounded mailbox (bound 0) never blocks a
    // send (LRM 15.4.1 / 15.4).
    const auto has_room = [&] {
        const ir::Index limit     = bound();
        const ir::Index none      = zero64();
        const ir::Index unbounded = b.binary(ir::Inst::Tag::Eq, limit, none, l);
        const ir::Index used      = b.convert(size(), i64, true, l);
        const ir::Index cap       = bound();
        const ir::Index under     = b.binary(ir::Inst::Tag::SLt, used, cap, l);
        return b.binary(ir::Inst::Tag::Lor, unbounded, under, l);
    };
    const auto has_msg = [&] {
        const ir::Index used = b.convert(size(), i64, true, l);
        const ir::Index none = zero64();
        return b.binary(ir::Inst::Tag::SGt, used, none, l);
    };
    // The message argument of `put` / `try_put`, coerced to the element type.
    const auto message = [&]() -> ir::Index {
        if (call.arguments().size() != 1) {
            fail("mailbox::" + std::string(sub.name) + " expects one argument");
        }
        // The same rule a queue push follows: an ordinary value for a plane,
        // string or handle element, and a POINTER to its image for an aggregate
        // or a nested container.
        return lower_container_element_value(*call.arguments()[0], fifo_ty, b, l);
    };
    // The destination of `get` / `peek` and their non-blocking forms. LRM 15.4.5
    // wants "a valid left-hand expression", so it is lowered as an ordinary
    // assignment target -- which admits a bit or part select, where the `ref`
    // argument ABI would refuse one.
    const auto dest = [&]() -> const Expression& {
        if (call.arguments().size() != 1) {
            fail("mailbox::" + std::string(sub.name) + " expects one argument");
        }
        const Expression* a = call.arguments()[0];
        if (a->kind == ExpressionKind::Assignment) {
            a = &a->as<AssignmentExpression>().left();
        }
        return *a;
    };
    // Copy one retrieved message into the destination. slang types the actual as
    // T exactly (it is a `ref T` formal), so nothing is converted -- but WHICH
    // copy verb applies is the element type's business, exactly as it is for a
    // queue element: a plane or a string stores, a nested container deep-copies
    // through its own runtime verb, and an aggregate goes leaf-wise (or one
    // memcpy when it owns no heap).
    const auto store_dest = [&](const Expression& d, ir::Index value) {
        const ir::Index dst = lower_lvalue(d, b);
        switch (value_cat(elem_ty)) {
            case ValueCat::scalar:
            case ValueCat::string:
                b.binary(ir::Inst::Tag::Store, dst, value, l);
                return;
            case ValueCat::handle: {
                const ir::Type et = std::get<ir::Type>(module_.pool.get(elem_ty));
                if (et.kind == ir::Type::Kind::Queue) {
                    b.queue_copy(dst, value, elem_ty, l);
                } else if (et.kind == ir::Type::Kind::Assoc) {
                    b.assoc_copy(dst, value, elem_ty, l);
                } else {
                    b.dyn_array_copy(dst, value, elem_ty, l);
                }
                return;
            }
            case ValueCat::aggregate:
                emit_port_agg_copy(dst, value, elem_ty, b, l);
                return;
        }
    };

    if (sub.name == "num") {
        // LRM 15.4.2. The value is valid only until the next get / put, which is
        // the caller's problem, not a reason to synchronize anything here.
        return size();
    }
    if (sub.name == "try_put") {
        // LRM 15.4.4: place the message if the mailbox is not full, else 0.
        const ir::Index m    = message();
        const ir::Index room = has_room();
        const ir::Index res  = make_arg_temp(int_ty, b, l);
        b.open_region();
        {
            b.queue_push(field(kMbxFifo), m, /*front=*/false, fifo_ty, l);
            const ir::Index get_head  = field(kMbxGetHead);
            const ir::Index get_tail  = field(kMbxGetTail);
            const ir::Index peek_head = field(kMbxPeekHead);
            const ir::Index peek_tail = field(kMbxPeekTail);
            emit_mbx_message_arrived(get_head, get_tail, peek_head, peek_tail, b, l);
            b.binary(ir::Inst::Tag::Store, res, one32(), l);
        }
        const ir::ExtraSlice then_r = b.close_region();
        b.open_region();
        b.binary(ir::Inst::Tag::Store, res, zero32(), l);
        const ir::ExtraSlice else_r = b.close_region();
        b.cond(room, then_r, else_r, l);
        return b.unary(ir::Inst::Tag::Load, res, l);
    }
    if (sub.name == "try_get" || sub.name == "try_peek") {
        // LRM 15.4.6 / 15.4.8: 0 on an empty mailbox, else retrieve (or copy)
        // and return a positive integer. A negative return is 15.4.9's typeless
        // mailbox only -- here slang has already type-checked the actual.
        const bool        consume = sub.name == "try_get";
        const Expression& d       = dest();
        const ir::Index   res     = make_arg_temp(int_ty, b, l);
        const ir::Index   avail   = has_msg();
        b.open_region();
        {
            if (consume) {
                store_dest(d, b.queue_pop(field(kMbxFifo), /*front=*/true, fifo_ty, l));
                // Room freed: hand off to the head of the send queue.
                b.sync_ctl(ir::SyncOp::WakeHead, sync_site_seq_++, qwords(kMbxPutHead, kMbxPutTail), l);
            } else {
                const ir::Index fifo = field(kMbxFifo);
                const ir::Index at0  = zero64();
                store_dest(d, b.unary(ir::Inst::Tag::Load, b.elem_select(fifo, at0, fifo_ty, l), l));
            }
            b.binary(ir::Inst::Tag::Store, res, one32(), l);
        }
        const ir::ExtraSlice then_r = b.close_region();
        b.open_region();
        b.binary(ir::Inst::Tag::Store, res, zero32(), l);
        const ir::ExtraSlice else_r = b.close_region();
        b.cond(avail, then_r, else_r, l);
        return b.unary(ir::Inst::Tag::Load, res, l);
    }
    if (sub.name == "put") {
        // LRM 15.4.3: store in strict FIFO order, suspending while the mailbox
        // is full. Head-of-line like `semaphore::get`: the fast path is taken
        // only when no other sender is queued (15.4.5's arrival-order rule).
        const uint32_t                 site  = sync_site_seq_++;
        const ir::Index                m     = message();
        const std::array<ir::Index, 2> qw    = qwords(kMbxPutHead, kMbxPutTail);
        const ir::Index                empty = qempty(kMbxPutHead);
        emit_sync_park_loop(site, qw, empty, [&] { return has_room(); }, /*require_head=*/true, b, l);
        b.queue_push(field(kMbxFifo), m, /*front=*/false, fifo_ty, l);
        const ir::Index get_head  = field(kMbxGetHead);
        const ir::Index get_tail  = field(kMbxGetTail);
        const ir::Index peek_head = field(kMbxPeekHead);
        const ir::Index peek_tail = field(kMbxPeekTail);
        emit_mbx_message_arrived(get_head, get_tail, peek_head, peek_tail, b, l);
        return std::nullopt;
    }
    if (sub.name == "get") {
        const uint32_t                 site  = sync_site_seq_++;
        const Expression&              d     = dest();
        const std::array<ir::Index, 2> qw    = qwords(kMbxGetHead, kMbxGetTail);
        const ir::Index                empty = qempty(kMbxGetHead);
        emit_sync_park_loop(site, qw, empty, [&] { return has_msg(); }, /*require_head=*/true, b, l);
        store_dest(d, b.queue_pop(field(kMbxFifo), /*front=*/true, fifo_ty, l));
        b.sync_ctl(ir::SyncOp::WakeHead, sync_site_seq_++, qwords(kMbxPutHead, kMbxPutTail), l);
        return std::nullopt;
    }
    if (sub.name == "peek") {
        // LRM 15.4.7's own sentence -- "As long as a message remains in the
        // mailbox queue, any process blocked in either a peek() or get()
        // operation shall become unblocked" -- is why a peeker is NOT subject to
        // the arrival-order rule of 15.4.5: it consumes nothing, so it can
        // starve nobody. Hence its own queue, woken as a set, and a fast path
        // that only asks whether a message is there.
        const uint32_t    site = sync_site_seq_++;
        const Expression& d    = dest();
        const ir::Index   any  = b.constant(module_.pool.put_int(make_int_type(1, false), 1), l);
        emit_sync_park_loop(site, qwords(kMbxPeekHead, kMbxPeekTail), any, [&] { return has_msg(); },
                            /*require_head=*/false, b, l);
        const ir::Index fifo = field(kMbxFifo);
        const ir::Index at0  = zero64();
        store_dest(d, b.unary(ir::Inst::Tag::Load, b.elem_select(fifo, at0, fifo_ty, l), l));
        return std::nullopt;
    }
    // Every method of LRM 15.4 is handled above; a new one would arrive with a
    // new slang release rather than from a design.
    fail("mailbox::" + std::string(sub.name) + "() is not supported");
}

// A message was placed: hand off to the head of the receive queue, and wake
// every peeker (LRM 15.4.7 unblocks all of them while a message remains).
void Lowerer::emit_mbx_message_arrived(ir::Index     get_head,
                                       ir::Index     get_tail,
                                       ir::Index     peek_head,
                                       ir::Index     peek_tail,
                                       ir::Builder&  b,
                                       ir::SourceLoc l) {
    const std::array<ir::Index, 2> getq{ get_head, get_tail };
    const std::array<ir::Index, 2> peekq{ peek_head, peek_tail };
    b.sync_ctl(ir::SyncOp::WakeAll, sync_site_seq_++, peekq, l);
    b.sync_ctl(ir::SyncOp::WakeHead, sync_site_seq_++, getq, l);
}

// The class built-ins slang synthesizes on EVERY class with an empty body
// (ClassSymbols.cpp's populate): `srandom`, `get_randstate`, `set_randstate`, the
// object-level `rand_mode` / `constraint_mode`, and the default
// `pre_randomize` / `post_randomize`. They are ordinary SubroutineSymbols, so
// without this intercept they lower as a call into slang's degenerate empty body
// — which is both silently wrong and, because the formal list does not match the
// spliced receiver, a crash in the argument walk. Whitelisted, so a future slang
// adding a built-in fails loud instead of no-oping.
std::optional<ir::Index> Lowerer::lower_builtin_class_method(const CallExpression&   call,
                                                             const SubroutineSymbol& sub,
                                                             ir::Builder&            b) {
    const ir::SourceLoc l    = loc(call);
    const ir::Ref       u64  = make_int_type(64, false);
    const auto          args = call.arguments();
    if (sub.name == "pre_randomize" || sub.name == "post_randomize") {
        // The synthesized default is empty (a user override is not BuiltIn and
        // never reaches here), so an explicit call — typically
        // `super.pre_randomize()` — has nothing to run.
        return std::nullopt;
    }
    if (sub.name == "srandom" || sub.name == "rand_mode") {
        const auto [self, clt] = call.thisClass() != nullptr
                                   ? std::pair<ir::Index, const slang::ast::ClassType*>{
                                       lower_expr(*call.thisClass(), b),
                                       &call.thisClass()->type->getCanonicalType().as<slang::ast::ClassType>() }
                                   : implicit_this(b, l);
        if (clt->isInterface && sub.name == "rand_mode") {
            // LRM 8.26.9: "Use of rand_mode and constraint_mode shall not be
            // legal as a consequence of the name resolution rules and the fact
            // that interface classes are not permitted to contain data members."
            // slang installs the built-in on every class including an interface
            // one (ClassSymbols.cpp:233-249), so this is clotho's to reject.
            fail("rand_mode through an interface class handle is not legal (LRM 8.26.9)");
        }
        if (args.size() != 1) {
            fail("built-in class method '" + std::string(sub.name) + "' takes one argument");
        }
        const uint64_t  which = sub.name == "srandom" ? kRandOpSrandom : kRandOpModeAll;
        const ir::Index op    = b.constant(module_.pool.put_int(u64, which), l);
        emit_randops_dispatch(*clt, self, op, b.convert(lower_expr(*args[0], b), u64, false, l), b, l);
        return std::nullopt;
    }
    if (sub.name == "constraint_mode") {
        // TODO: constraint_mode (LRM 18.9) — nothing to enable until constraints
        // are solved at all.
        fail("constraint_mode is not supported");
    }
    if (sub.name == "get_randstate" || sub.name == "set_randstate") {
        // TODO: the object randstate codec (LRM 18.13.4/18.13.5) — two runtime
        // helpers formatting the four state words as a string.
        fail("get_randstate / set_randstate are not supported");
    }
    fail("unsupported built-in class method '" + std::string(sub.name) + "'");
}

} // namespace clotho::lower
