// Design and flatten (src/ir/Design.h): units compiled once, composed by child
// slots and roots, expanded per instance into the flat Module the backend runs.
// The designs are built by hand because the frontend does not produce units yet;
// each runs through the real backend so a mis-resolved import shows up as wrong
// simulation output, not only as a wrong table entry.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

#include "ir/Builder.h"
#include "ir/Design.h"
#include "ir/Jit.h"
#include "ir/Partition.h"

using namespace clotho::ir;

namespace {

constexpr SourceLoc kLoc{ 1 };

std::string run_capture(Module& m) {
    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));
    run_jit(m);
    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

Ref byte_type(InternPool& pool) {
    return pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.logic_type, .left = 7, .right = 0 });
}

Signal local(Unit& u, std::string name, Ref type, SignalInfo::Direction dir = SignalInfo::Direction::Internal) {
    u.signals.push_back({ .info = { .name = std::move(name), .type = type, .dir = dir } });
    return static_cast<Signal>(u.signals.size() - 1);
}

Signal extern_signal(Unit& u, std::string name, Ref type, Import imp) {
    u.imports.push_back(std::move(imp));
    u.signals.push_back(
      { .info = { .name = std::move(name), .type = type }, .import = static_cast<uint32_t>(u.imports.size() - 1) });
    return static_cast<Signal>(u.signals.size() - 1);
}

Import member(InstanceRef inst, std::string name) {
    return Import{ .kind = Import::Kind::Member, .inst = std::move(inst), .member = std::move(name) };
}

// An 8-bit constant, converted to the 4-state byte type the test signals use.
Index byte_const(Builder& b, InternPool& pool, uint64_t value) {
    const Ref bits = pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.bit_type, .left = 7, .right = 0 });
    return b.convert(b.constant(pool.put_int(bits, value), kLoc), byte_type(pool), /*sign_extend=*/false, kLoc);
}

// dst = value.
void store_const(Builder& b, InternPool& pool, Signal dst, uint64_t value) {
    b.binary(Inst::Tag::Store, b.signal(dst, kLoc), byte_const(b, pool, value), kLoc);
}

void delay(Builder& b, InternPool& pool, uint64_t ticks) {
    const Ref int_ty =
      pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.bit_type, .left = 31, .right = 0 });
    b.unary(Inst::Tag::Delay, b.constant(pool.put_int(int_ty, ticks), kLoc), kLoc);
}

void display(Builder& b, std::string_view fmt, std::span<const Signal> sigs) {
    std::vector<FmtArg> args;
    for (const Signal s : sigs) {
        args.push_back(FmtArg{ b.unary(Inst::Tag::Load, b.signal(s, kLoc), kLoc), /*is_signed=*/false });
    }
    b.display(fmt, true, args, kLoc);
}

// An always process: `dst = src` whenever src changes; self-starting like a
// continuous assignment or a port copy.
Process copy_process(Module& scratch, Signal dst, Signal src) {
    Builder b(scratch);
    b.binary(Inst::Tag::Store, b.signal(dst, kLoc), b.unary(Inst::Tag::Load, b.signal(src, kLoc), kLoc), kLoc);
    return Process{ .activation  = Activation::Always,
                    .wait_events = { WaitEvent{ .signal = src, .edge = Edge::Any } },
                    .self_start  = true,
                    .ir          = b.finish() };
}

// A Builder types a body from the signal and function tables of the module the
// body belongs to (docs/design/kernel.md), and the bodies of a hand-built unit
// name handles of the unit's own tables, which differ from unit to unit. So the
// bodies of one design are all built against one scratch module that has every
// handle any unit of these tests names (eight byte signals, two functions that
// return a byte), and holds the design's pool until the bodies are built.
class Scratch {
public:
    explicit Scratch(Design& d)
      : design_(d)
      , module_(std::move(d.pool)) {
        const Ref byte = byte_type(module_.pool);
        for (uint32_t i = 0; i < 8; ++i) {
            module_.add_signal({ .name = "s" + std::to_string(i), .type = byte });
        }
        for (uint32_t i = 0; i < 2; ++i) {
            module_.reserve_function(byte);
        }
    }

    Module& module() {
        return module_;
    }

    // Returns the pool to the design. Every body is finished, and the scratch
    // module is not used again.
    void give_back() {
        design_.pool = module_.release_pool();
    }

private:
    Design& design_;
    Module  module_;
};

} // namespace

TEST_CASE("flatten copies a unit once per instance and resolves child member imports") {
    Design      d;
    Scratch     scratch(d);
    InternPool& pool = scratch.module().pool;
    const Ref   byte = byte_type(pool);

    // leaf: always @(d) q = d;
    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "leaf" });
    const UnitId leaf_id{ 0 };
    {
        Unit&        leaf = d.units.back();
        const Signal in   = local(leaf, "d", byte, SignalInfo::Direction::Input);
        const Signal out  = local(leaf, "q", byte, SignalInfo::Direction::Output);
        Builder      b(scratch.module());
        b.binary(Inst::Tag::Store, b.signal(out, kLoc), b.unary(Inst::Tag::Load, b.signal(in, kLoc), kLoc), kLoc);
        leaf.processes.push_back(Process{ .activation  = Activation::Always,
                                          .wait_events = { WaitEvent{ .signal = in, .edge = Edge::Any } },
                                          .ir          = b.finish() });
    }

    // top: two leaves, each driven and observed through value-flow port copies
    // that import the child's port signals by name.
    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "top" });
    {
        Unit&        top = d.units.back();
        const Signal a0  = local(top, "a0", byte);
        const Signal a1  = local(top, "a1", byte);
        const Signal b0  = local(top, "b0", byte);
        const Signal b1  = local(top, "b1", byte);
        const Signal d0  = extern_signal(top, "u0.d", byte, member({ .slots = { 0 } }, "d"));
        const Signal q0  = extern_signal(top, "u0.q", byte, member({ .slots = { 0 } }, "q"));
        const Signal d1  = extern_signal(top, "u1.d", byte, member({ .slots = { 1 } }, "d"));
        const Signal q1  = extern_signal(top, "u1.q", byte, member({ .slots = { 1 } }, "q"));
        top.children.push_back({ .unit = leaf_id, .name = "u0" });
        top.children.push_back({ .unit = leaf_id, .name = "u1" });
        top.processes.push_back(copy_process(scratch.module(), d0, a0));
        top.processes.push_back(copy_process(scratch.module(), b0, q0));
        top.processes.push_back(copy_process(scratch.module(), d1, a1));
        top.processes.push_back(copy_process(scratch.module(), b1, q1));

        Builder b(scratch.module());
        store_const(b, pool, a0, 3);
        store_const(b, pool, a1, 5);
        delay(b, pool, 1);
        const Signal shown[] = { b0, b1 };
        display(b, "%0d %0d", shown);
        top.processes.push_back(Process{ .activation = Activation::Initial, .ir = b.finish() });
    }
    d.roots.push_back({ .unit = UnitId{ 1 }, .name = "top", .prefix = "" });
    d.name = d.top_name = "top";

    scratch.give_back();
    auto m = flatten(std::move(d));

    // One flat signal per local signal per instance, named as the per-instance
    // lowering names them; the imports created none of their own.
    std::vector<std::string> names;
    for (const SignalInfo& s : m->signals()) {
        names.push_back(s.name);
    }
    CHECK(names == std::vector<std::string>{ "a0", "a1", "b0", "b1", "u0.d", "u0.q", "u1.d", "u1.q" });
    CHECK(m->processes().size() == 7);

    REQUIRE(m->tops().size() == 1);
    const InstanceScope& top = m->tops()[0];
    CHECK(top.name == "top");
    CHECK(top.def_name == "top");
    REQUIRE(top.children.size() == 2);
    CHECK(top.children[0].name == "u0");
    CHECK(top.children[1].name == "u1");
    CHECK(top.children[1].def_name == "leaf");
    CHECK(top.children[1].signals == std::vector<Signal>{ Signal{ 6 }, Signal{ 7 } });

    CHECK(run_capture(*m) == "3 5\n");
}

TEST_CASE("flatten aliases a bound port to the signal each parent binds") {
    Design      d;
    Scratch     scratch(d);
    InternPool& pool = scratch.module().pool;
    const Ref   byte = byte_type(pool);

    // drv: inout io; initial io = 6;
    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "drv" });
    {
        Unit& drv = d.units.back();
        drv.bound_ports.push_back({ .name = "io", .kind = BoundPort::Kind::Signal });
        const Signal io = extern_signal(drv, "io", byte, Import{ .kind = Import::Kind::Port, .port = 0 });
        Builder      b(scratch.module());
        store_const(b, pool, io, 6);
        drv.processes.push_back(Process{ .activation = Activation::Initial, .ir = b.finish() });
    }

    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "top" });
    {
        Unit&        top = d.units.back();
        const Signal w0  = local(top, "w0", byte);
        const Signal w1  = local(top, "w1", byte);
        top.children.push_back({ .unit = UnitId{ 0 }, .name = "u0", .bindings = { static_cast<uint32_t>(w0) } });
        top.children.push_back({ .unit = UnitId{ 0 }, .name = "u1", .bindings = { static_cast<uint32_t>(w1) } });
        Builder b(scratch.module());
        delay(b, pool, 1);
        const Signal shown[] = { w0, w1 };
        display(b, "%0d %0d", shown);
        top.processes.push_back(Process{ .activation = Activation::Initial, .ir = b.finish() });
    }
    d.roots.push_back({ .unit = UnitId{ 1 }, .name = "top", .prefix = "" });

    scratch.give_back();
    auto m = flatten(std::move(d));

    // The port's storage is the parent's: no per-instance signal backs it.
    REQUIRE(m->signals().size() == 2);
    CHECK(m->tops()[0].children[0].signals.empty());
    CHECK(run_capture(*m) == "6 6\n");
}

TEST_CASE("flatten resolves package members through a static root") {
    Design      d;
    Scratch     scratch(d);
    InternPool& pool = scratch.module().pool;
    const Ref   byte = byte_type(pool);

    // package p; logic [7:0] pv = 9; function f; return 7; endfunction
    d.units.push_back(Unit{ .kind = UnitKind::Static, .def_name = "p" });
    {
        Unit&        p  = d.units.back();
        const Signal pv = local(p, "pv", byte);
        Builder      init(scratch.module());
        store_const(init, pool, pv, 9);
        p.processes.push_back(Process{ .activation = Activation::PreInit, .ir = init.finish() });

        Builder fb(scratch.module());
        fb.unary(Inst::Tag::Ret, byte_const(fb, pool, 7), kLoc);
        p.functions.push_back({ .fn = Function{ .name = "f", .return_type = byte, .ir = fb.finish() }, .name = "f" });
    }

    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "top" });
    {
        Unit&        top = d.units.back();
        const Signal x   = local(top, "x", byte);
        const Signal pv  = extern_signal(top, "pv", byte, member({ .base = InstanceRef::Base::Root, .index = 0 }, "pv"));
        top.imports.push_back(member({ .base = InstanceRef::Base::Root, .index = 0 }, "f"));
        top.functions.push_back({ .fn = Function{ .name = "f", .return_type = byte },
                                  .import = static_cast<uint32_t>(top.imports.size() - 1) });
        Builder b(scratch.module());
        b.binary(Inst::Tag::Store, b.signal(x, kLoc), b.call(FunctionId{ 0 }, {}, kLoc), kLoc);
        const Signal shown[] = { x, pv };
        display(b, "%0d %0d", shown);
        top.processes.push_back(Process{ .activation = Activation::Initial, .ir = b.finish() });
    }
    d.roots.push_back({ .unit = UnitId{ 0 }, .name = "p", .prefix = "" });
    d.roots.push_back({ .unit = UnitId{ 1 }, .name = "top", .prefix = "" });

    scratch.give_back();
    auto m = flatten(std::move(d));

    // A static unit's instance is no InstanceScope: its signal stays unscoped.
    REQUIRE(m->tops().size() == 1);
    CHECK(m->tops()[0].signals == std::vector<Signal>{ Signal{ 1 } });
    CHECK(m->function_count() == 1);
    CHECK(run_capture(*m) == "7 9\n");
}

TEST_CASE("flatten resolves interface-port and upward references") {
    Design      d;
    Scratch     scratch(d);
    InternPool& pool = scratch.module().pool;
    const Ref   byte = byte_type(pool);

    // interface bus; logic [7:0] data; endinterface
    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "bus" });
    local(d.units.back(), "data", byte);

    // drv(bus b): initial b.data = 4;
    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "drv" });
    {
        Unit& drv = d.units.back();
        drv.bound_ports.push_back({ .name = "b", .kind = BoundPort::Kind::Instance });
        const Signal data =
          extern_signal(drv, "b.data", byte, member({ .base = InstanceRef::Base::Port, .index = 0 }, "data"));
        Builder b(scratch.module());
        store_const(b, pool, data, 4);
        drv.processes.push_back(Process{ .activation = Activation::Initial, .ir = b.finish() });
    }

    // mon: initial #1 $display(top.i.data), reached as the parent's first child.
    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "mon" });
    {
        Unit&        mon  = d.units.back();
        const Signal data = extern_signal(mon, "i.data", byte, member({ .index = 1, .slots = { 0 } }, "data"));
        Builder      b(scratch.module());
        delay(b, pool, 1);
        const Signal shown[] = { data };
        display(b, "%0d", shown);
        mon.processes.push_back(Process{ .activation = Activation::Initial, .ir = b.finish() });
    }

    d.units.push_back(Unit{ .kind = UnitKind::Module, .def_name = "top" });
    {
        Unit& top = d.units.back();
        top.inst_refs.push_back({ .slots = { 0 } });
        top.children.push_back({ .unit = UnitId{ 0 }, .name = "i" });
        top.children.push_back({ .unit = UnitId{ 1 }, .name = "u", .bindings = { 0 } });
        top.children.push_back({ .unit = UnitId{ 2 }, .name = "m" });
    }
    d.roots.push_back({ .unit = UnitId{ 3 }, .name = "top", .prefix = "" });

    scratch.give_back();
    auto m = flatten(std::move(d));

    REQUIRE(m->signals().size() == 1);
    CHECK(m->signals()[0].name == "i.data");
    CHECK(run_capture(*m) == "4\n");
}

TEST_CASE("rebased maps every cross-table handle and leaves the rest alone") {
    // Built against a module that has the handles it names: two byte signals
    // and two functions.
    Module      scratch;
    InternPool& pool = scratch.pool;
    const Ref   byte = byte_type(pool);
    for (uint32_t i = 0; i < 2; ++i) {
        scratch.add_signal({ .name = "s" + std::to_string(i), .type = byte });
        scratch.reserve_function(byte);
    }
    Builder b(scratch);

    const Index sig  = b.signal(Signal{ 0 }, kLoc);
    const Index val  = b.unary(Inst::Tag::Load, sig, kLoc);
    const Index call = b.call(FunctionId{ 1 }, std::span<const Index>{ &val, 1 }, kLoc);
    b.task_call(FunctionId{ 0 }, {}, kLoc);
    const WaitEdge edges[] = { { .signal = Signal{ 1 }, .edge = 0 },
                               { .signal = Signal{ 0 }, .edge = 0, .handle = val } };
    const Index    wait    = b.wait(edges, kLoc);
    const IntraTrigger trigs[] = { { .signal = Signal{ 1 }, .edge = 0 } };
    const Index        nbe     = b.store_nb_event(sig, val, val, b.intra_trigger_run(trigs), kLoc);
    const Index        detach  = b.event_trigger_nb_event(val, Signal{ 1 }, 0, 1, kLoc);
    b.open_region();
    const Index dis = b.disable(2, kLoc);
    const Index blk = b.named_block(b.close_region(), 2, true, kLoc);
    const Index rem = b.disable_remote(1, kLoc);
    const Index df  = b.defer_flush(kNoDisableTarget, kLoc);
    const Index ew  = b.expect_wait(0, kLoc);
    const Index ek  = b.expect_wake(val, 1, kLoc);
    const Index sp  = b.strobe_pend(FunctionId{ 1 }, kLoc);
    const Index sw  = b.sync_wait(3, {}, kLoc);
    const Index      ct     = b.cover_query(CoverOp::TypeTotal, {}, byte, 2, 0, kLoc);
    const Index cc  = b.cover_ctl(CoverOp::Start, {}, 2, 0, kLoc);
    const Index ir_ = b.iface_ref(0, kLoc);
    const Index is  = b.iface_signal(val, Signal{ 1 }, kLoc);
    const FunctionId kids[] = { FunctionId{ 0 }, FunctionId{ 1 } };
    const Index      fork   = b.fork(kids, {}, ForkKind::Join, kLoc);
    const Index      k      = b.convert(b.constant(pool.put_int(pool.bit_type, 1), kLoc), byte, /*sign_extend=*/false, kLoc);
    const Ir         body   = b.finish();

    // Signals map to 10 + h, functions to 20 + h, interface scopes to 30 + h,
    // layouts to 40 + h and the remaining families to their own hundred.
    struct Offsets final : HandleMapper {
        uint32_t map(HandleFamily family, uint32_t h) override {
            switch (family) {
                case HandleFamily::Signal:
                    return 10 + h;
                case HandleFamily::Layout:
                    return 40 + h;
                case HandleFamily::Function:
                    return 20 + h;
                case HandleFamily::IfaceScope:
                    return 30 + h;
                case HandleFamily::Monitor:
                    return 100 + h;
                case HandleFamily::CoverType:
                    return 200 + h;
                case HandleFamily::DisableTarget:
                    return 300 + h;
            }
            return h;
        }
    } offsets;
    const Ir out = body.rebased(offsets);

    REQUIRE(out.inst_count() == body.inst_count());
    CHECK(out.payload(sig).signal == Signal{ 10 });
    CHECK(out.extra_data<CallInfo>(out.payload(call).extra).func == FunctionId{ 21 });
    CHECK(out.extra_data<CallInfo>(out.payload(call).extra).args.start ==
          body.extra_data<CallInfo>(body.payload(call).extra).args.start);

    const auto we = out.wait_edges(out.payload(wait).extra);
    REQUIRE(we.size() == 2);
    CHECK(we[0].signal == Signal{ 11 });
    // A handle wait's placeholder signal names nothing and is left as built.
    CHECK(we[1].signal == Signal{ 0 });
    CHECK(out.intra_trigs(out.payload(nbe).op_extra.extra)[0].signal == Signal{ 11 });
    CHECK(out.extra_data<EventDetachInfo>(out.payload(detach).op_extra.extra).signal == Signal{ 11 });

    CHECK(out.payload(dis).label == 302);
    CHECK(out.extra_data<BlockInfo>(out.payload(blk).extra).label == 302);
    CHECK(out.extra_data<BlockInfo>(out.payload(blk).extra).remote == 1);
    CHECK(out.payload(rem).label == 301);
    CHECK(out.payload(df).label == kNoDisableTarget);

    CHECK(out.extra_data<ExpectInfo>(out.payload(ew).extra).monitor == 100);
    CHECK(out.payload(ek).op_imm.imm == 101);
    CHECK(out.extra_data<StrobePendInfo>(out.payload(sp).extra).func == FunctionId{ 21 });
    // A wait site is an identity within its body, not a handle.
    CHECK(out.extra_data<SyncInfo>(out.payload(sw).extra).site == 3);
    CHECK(out.extra_data<CoverInfo>(out.payload(ct).extra).type_idx == 202);
    // An instance op takes its type from the handle; its index is not one.
    CHECK(out.extra_data<CoverInfo>(out.payload(cc).extra).type_idx == 2);
    CHECK(out.extra_data<IfaceInst>(out.payload(ir_).extra).scope == 30);
    // A virtual interface member names a layout, not storage.
    CHECK(out.extra_data<IfaceMember>(out.payload(is).op_extra.extra).signal == Signal{ 41 });

    const ForkInfo fi    = out.extra_data<ForkInfo>(out.payload(fork).extra);
    const auto     words = out.extra_raw();
    CHECK(words[static_cast<uint32_t>(fi.children.start)] == 20);
    CHECK(words[static_cast<uint32_t>(fi.children.start) + 1] == 21);

    // Constants and types are not handles.
    CHECK(out.payload(k).op_type.type == body.payload(k).op_type.type);
    // The source body is untouched.
    CHECK(body.payload(sig).signal == Signal{ 0 });
    CHECK(body.payload(dis).label == 2);
}

// A flat module of three instances of one specialization, the third storing a
// different value: partition gives the two identical ones one unit and keeps
// the third apart, and flattening the result runs the same design.
TEST_CASE("partition merges the identical instances of a specialization and no others") {
    auto        m    = std::make_unique<Module>();
    InternPool& pool = m->pool;
    const Ref   byte = byte_type(pool);

    InstanceScope       top{ .id = m->alloc_scope_id(), .name = "top", .def_name = "top" };
    std::vector<Signal> qs;
    for (const uint64_t value : { 4u, 4u, 9u }) {
        InstanceScope u{ .id = m->alloc_scope_id(), .name = "u" + std::to_string(qs.size()), .def_name = "leaf" };
        const Signal  q = m->add_signal({ .name = u.name + ".q", .type = byte });
        u.signals.push_back(q);
        Builder b(*m);
        store_const(b, pool, q, value);
        m->add_process(Process{ .activation = Activation::Initial, .ir = b.finish(), .scope = u.id });
        top.children.push_back(std::move(u));
        qs.push_back(q);
    }
    Builder b(*m);
    delay(b, pool, 1);
    display(b, "%0d %0d %0d", qs);
    m->add_process(Process{ .activation = Activation::Initial, .ir = b.finish(), .scope = top.id });
    m->add_top(std::move(top));

    Attribution           a{ .scope_specs = { 0, 1, 1, 1 } };
    std::vector<uint32_t> specs;
    auto                  d = partition(*m, a, &specs);

    // Glue, top, one leaf for u0 and u1, one for u2.
    REQUIRE(d->units.size() == 4);
    const Unit& t = d->units[1];
    REQUIRE(t.children.size() == 3);
    CHECK(t.children[0].unit == t.children[1].unit);
    CHECK(t.children[0].unit != t.children[2].unit);
    CHECK(specs[static_cast<uint32_t>(t.children[0].unit)] == specs[static_cast<uint32_t>(t.children[2].unit)]);

    auto flat = flatten(std::move(*d));
    CHECK(run_capture(*flat) == "4 4 9\n");
}

// Where an instance's name is written into a body, instances of one specialization
// stop being identical, so the name is read instead (Inst::Tag::InstPath). The
// path then has to come out as the path of the instance the process or function
// was flattened into, for a unit shared by two instances like any other.
TEST_CASE("inst_path names the instance its process or function is flattened into") {
    auto        m    = std::make_unique<Module>();
    InternPool& pool = m->pool;

    Attribution         a{ .scope_specs = { 0, 1, 1 } };
    InstanceScope       top{ .id = m->alloc_scope_id(), .name = "top", .def_name = "top" };
    for (const char* name : { "u0", "u1" }) {
        InstanceScope u{ .id = m->alloc_scope_id(), .name = name, .def_name = "leaf" };

        const FunctionId who = m->reserve_function(pool.void_type);
        Builder          fb(*m);
        const FmtArg     fp{ fb.inst_path(kLoc) };
        fb.display("fn %s", true, { &fp, 1 }, kLoc);
        fb.ret_void(kLoc);
        m->set_function(who, Function{ .name = "who", .return_type = pool.void_type, .ir = fb.finish() });
        a.functions.push_back(Owner{ .kind = Owner::Kind::Scope, .index = u.id });
        a.function_names.push_back("who");

        Builder      b(*m);
        const FmtArg p{ b.inst_path(kLoc) };
        b.display("proc %s", true, { &p, 1 }, kLoc);
        b.call(who, {}, kLoc);
        m->add_process(Process{ .activation = Activation::Initial, .ir = b.finish(), .scope = u.id });
        top.children.push_back(std::move(u));
    }
    m->add_top(std::move(top));

    auto d = partition(*m, a);
    // Glue, top, and ONE leaf for both instances: no path is baked into either body.
    CHECK(d->units.size() == 3);

    auto flat = flatten(std::move(*d));
    // The two instances start at the same time, so only the set of lines is fixed.
    std::vector<std::string> lines;
    const std::string        out = run_capture(*flat);
    for (size_t at = 0; at < out.size();) {
        const size_t eol = out.find('\n', at);
        lines.push_back(out.substr(at, eol - at));
        at = eol + 1;
    }
    std::ranges::sort(lines);
    CHECK(lines == std::vector<std::string>{ "fn top.u0", "fn top.u1", "proc top.u0", "proc top.u1" });
}
