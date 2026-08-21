#include <doctest/doctest.h>

#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

// The acceptance artifact for docs/design/data-model.md: the full cartesian
// product the design doc §9 asks for, generated rather than hand-written.
//
//   axis E  the inner element type          (19 rows, `elems`)
//   axis C  the aggregate or container       (8 rows, `conts`)
//   axis P  the passing mode                (16 rows, `modes`)
//
// 19 x 8 x 18 = 2736 cells. Each cell is one complete SystemVerilog design, and
// each cell has exactly one of two acceptable outcomes:
//
//   supported  lowering succeeds, the design runs, and stdout equals the axis-E
//              row's expected text.
//   refused    lowering fails AND the diagnostic matches a row of `allowlist`,
//              the explicit table of exclusions below. Every row cites
//              data-model.md §7 or an LRM clause for why it is refused rather
//              than missing.
//
// Anything else is a failure that names the cell: a refusal off the allowlist
// quotes the diagnostic, and a run with the wrong output quotes both texts. That
// is what makes the matrix maintainable — closing a gap means deleting an
// allowlist row, and regressing a supported cell fails loudly instead of
// quietly widening the allowlist.
//
// The matrix is split into one TEST_CASE per axis-C value so a failure
// localizes, plus a final summary TEST_CASE that prints the compact table of
// every unexpected cell.

namespace {

// stdout redirected to a temp file for the lifetime of the object. This is the
// idiom of tests/queue_test.cpp:20-48 made scope-bound: the matrix runs
// thousands of designs in one process, so an exception escaping the JIT must not
// leave the test runner's stdout pointing at a temp file nobody reads again.
struct CapturedStdout {
    CapturedStdout() {
        tmp = std::tmpfile();
        REQUIRE(tmp != nullptr);
        std::fflush(stdout);
        saved = dup(fileno(stdout));
        dup2(fileno(tmp), fileno(stdout));
    }

    CapturedStdout(const CapturedStdout&)            = delete;
    CapturedStdout& operator=(const CapturedStdout&) = delete;

    ~CapturedStdout() {
        restore();
        if (tmp != nullptr) {
            std::fclose(tmp);
        }
    }

    void restore() {
        if (saved < 0) {
            return;
        }
        std::fflush(stdout);
        dup2(saved, fileno(stdout));
        close(saved);
        saved = -1;
    }

    // Everything the design wrote. Ends the redirect.
    std::string text() {
        restore();
        std::rewind(tmp);
        std::string out;
        char        buf[256];
        size_t      n;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
            out.append(buf, n);
        }
        return out;
    }

    std::FILE* tmp   = nullptr;
    int        saved = -1;
};

// JIT-run a module and capture stdout.
std::string jit_capture(ir::Module& m) {
    CapturedStdout cap;
    ir::run_jit(m);
    return cap.text();
}

// Lower, run and capture, requiring success. For the hand-written cases below;
// the matrix inspects a lowering failure instead of aborting the walk on it.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    return jit_capture(*r.module);
}

// A design the frontend must refuse, and the guard text it must name.
void expect_fail(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CAPTURE(r.diagnostic_text());
    CHECK(r.diagnostic_text().find(needle) != std::string::npos);
}

// -- Source generation --

// Every axis snippet spells the subject it acts on as `$$`, so one snippet
// composes into a variable declaration, a container slot, a formal argument and
// a foreach body without knowing which of them it got. `$$` cannot occur in
// valid SystemVerilog, so the substitution is unambiguous.
std::string subst(std::string_view fmt, std::string_view subject) {
    std::string out;
    out.reserve(fmt.size() + subject.size());
    for (size_t i = 0; i < fmt.size();) {
        if (fmt.compare(i, 2, "$$") == 0) {
            out += subject;
            i += 2;
        } else {
            out += fmt[i];
            ++i;
        }
    }
    return out;
}

// `text` with every non-empty line prefixed by `n` spaces, so a multi-statement
// snippet lands at the indentation of the block it is spliced into.
std::string indent(std::string_view text, size_t n) {
    std::string out;
    bool        at_line_start = true;
    for (const char ch : text) {
        if (at_line_start && ch != '\n') {
            out.append(n, ' ');
        }
        out += ch;
        at_line_start = ch == '\n';
    }
    if (!text.empty() && text.back() != '\n') {
        out += '\n';
    }
    return out;
}

// -- Axis E: the inner element type --

// Storage class per data-model.md §1, plus the two properties the exclusion
// allowlist tests. Plane is the absence of every flag.
enum : unsigned {
    t_managed = 1u << 0, // §1 Managed: a POD handle owning at most one allocation
    t_agg     = 1u << 1, // §1 Aggregate: a recursive image addressed by GEP path
    t_real    = 1u << 2,
    t_handle  = 1u << 3, // §1 a class handle is a Plane with reference semantics
    t_event   = 1u << 4, // the one deliberate exclusion, §7
};

struct Elem {
    const char* name;
    // Named types this row needs, declared at compilation-unit scope so a child
    // module's port list can name them too.
    const char* pre;
    // `typedef <this row's type> ET;` — a typedef because an unpacked dimension
    // binds after the declarator, so only a typedef composes uniformly.
    const char* type_def;
    // Statements giving `$$` this row's literal value.
    const char* init;
    // One $display of `$$`.
    const char* show;
    // Exactly what `show` prints once.
    const char* expect;
    unsigned    traits;
};

// clang-format off
const Elem elems[] = {
    { "bit", "", "typedef bit ET;",
      "$$ = 1'b1;", "$display(\"%0d\", $$);", "1\n", 0 },
    { "logic", "", "typedef logic ET;",
      "$$ = 1'b1;", "$display(\"%0d\", $$);", "1\n", 0 },
    // Signed and narrower than a word: the rendered text proves the sign
    // extension survived every copy on the way through.
    { "byte", "", "typedef byte ET;",
      "$$ = -3;", "$display(\"%0d\", $$);", "-3\n", 0 },
    { "int", "", "typedef int ET;",
      "$$ = 42;", "$display(\"%0d\", $$);", "42\n", 0 },
    { "logic [31:0]", "", "typedef logic [31:0] ET;",
      "$$ = 32'hdeadbeef;", "$display(\"%0d\", $$);", "3735928559\n", 0 },
    // Wider than one word: a multi-word Plane, so the `{i(2W), iW}` slot is not
    // a register-sized special case anywhere on the path.
    { "logic [95:0]", "", "typedef logic [95:0] ET;",
      "$$ = 96'd1234567890123456789;", "$display(\"%0d\", $$);",
      "1234567890123456789\n", 0 },
    // A fraction that %0d rounds unambiguously, so a copy that truncated the
    // value to an integer (2) is distinguishable from one that kept it (3).
    { "real", "", "typedef real ET;",
      "$$ = 2.75;", "$display(\"%0d\", $$);", "3\n", t_real },
    { "string", "", "typedef string ET;",
      "$$ = \"hi\";", "$display(\"%s\", $$);", "hi\n", t_managed },
    // A packed struct is a Plane (§1): member access is a part select, so this
    // row needs none of the aggregate machinery the unpacked struct row does.
    { "packed struct", "typedef struct packed { byte a; byte b; } ps_t;",
      "typedef ps_t ET;",
      "$$ = ps_t'{a: 8'd1, b: 8'd2};", "$display(\"%0d\", $$.b);", "2\n", 0 },
    // Written through one member and read through the other: the two members
    // must share the one flat bit vector.
    { "packed union", "typedef union packed { byte a; byte b; } pu_t;",
      "typedef pu_t ET;",
      "$$.a = 8'd5;", "$display(\"%0d\", $$.b);", "5\n", 0 },
    { "tagged packed union", "typedef union tagged packed { byte i; byte j; } tu_t;",
      "typedef tu_t ET;",
      "$$ = tagged i 8'd6;", "$display(\"%0d\", $$.i);", "6\n", 0 },
    { "enum", "typedef enum int { E_A = 3, E_B = 4 } en_t;", "typedef en_t ET;",
      "$$ = E_B;", "$display(\"%0d\", $$);", "4\n", 0 },
    { "unpacked struct", "typedef struct { int a; int b; } us_t;", "typedef us_t ET;",
      "$$ = '{a: 8, b: 9};", "$display(\"%0d\", $$.b);", "9\n", t_agg },
    { "int [2]", "", "typedef int ET[2];",
      "$$ = '{5, 6};", "$display(\"%0d\", $$[1]);", "6\n", t_agg },
    { "int []", "", "typedef int ET[];",
      "$$ = new[2];\n$$[1] = 7;", "$display(\"%0d\", $$[1]);", "7\n", t_managed },
    { "int [$]", "", "typedef int ET[$];",
      "$$ = '{3, 4};", "$display(\"%0d\", $$[1]);", "4\n", t_managed },
    { "int [string]", "", "typedef int ET[string];",
      "$$[\"a\"] = 11;", "$display(\"%0d\", $$[\"a\"]);", "11\n", t_managed },
    { "class handle", "class EC;\n    int v;\nendclass", "typedef EC ET;",
      "$$ = new;\n$$.v = 13;", "$display(\"%0d\", $$.v);", "13\n", t_handle },
    // An event is a 64-bit handle with reference semantics, exactly like a class
    // handle (docs/design/event-handles.md): a bare event variable assigns, holds
    // null and reads `.triggered`. `$$ = null` then `.triggered` is 0 -- the null
    // object never triggers. The shapes that still refuse (events in aggregates,
    // across ports, as subroutine arguments) stay on the allowlist below.
    { "event", "", "typedef event ET;",
      "$$ = null;", "$display(\"%0d\", $$.triggered);", "0\n", t_event },
};
// clang-format on

// -- Axis C: the aggregate or container wrapping the element --

enum class C {
    none,
    farray,
    ustruct,
    dynarr,
    queue,
    assoc_int,
    assoc_str,
    cprop,
};

struct Cont {
    const char* name;
    C           kind;
    // Declarations this row needs inside the design's module.
    const char* pre;
    // Declaration of the storage variable `v`, in terms of the axis-E typedef.
    const char* decl;
    // Statements copying `tmp` into every slot.
    const char* place;
    // The expression naming the element sub-object of `v`.
    const char* access;
    // The `foreach` subject, or empty when the row is not iterable.
    const char* iter;
    // Slots a `foreach` over `iter` visits.
    int         slots;
};

// clang-format off
const Cont conts[] = {
    { "none", C::none, "", "ET v;", "v = tmp;", "v", "", 1 },
    // Two slots, both filled: the foreach mode then also proves the second slot
    // is a distinct object rather than an alias of the first.
    { "int fixed [2]", C::farray, "", "ET v[2];",
      "v[0] = tmp;\nv[1] = tmp;", "v[1]", "v[i]", 2 },
    { "unpacked struct member", C::ustruct, "typedef struct { ET m; } WS;", "WS v;",
      "v.m = tmp;", "v.m", "", 1 },
    { "dynarr element", C::dynarr, "", "ET v[];",
      "v = new[1];\nv[0] = tmp;", "v[0]", "v[i]", 1 },
    { "queue element", C::queue, "", "ET v[$];",
      "v.push_back(tmp);", "v[0]", "v[i]", 1 },
    { "assoc value [int]", C::assoc_int, "", "ET v[int];",
      "v[7] = tmp;", "v[7]", "v[i]", 1 },
    { "assoc value [string]", C::assoc_str, "", "ET v[string];",
      "v[\"k\"] = tmp;", "v[\"k\"]", "v[i]", 1 },
    { "class property", C::cprop, "class WC;\n    ET p;\nendclass", "WC v;",
      "v = new;\nv.p = tmp;", "v.p", "", 1 },
};
// clang-format on

// -- Axis P: the passing mode --

enum class P {
    var_decl,
    port_in,
    port_out,
    port_inout,
    func_in,
    func_out,
    func_inout,
    func_ref,
    func_cref,
    task_in,
    task_out,
    task_inout,
    task_ref,
    task_cref,
    func_return,
    blocking,
    nonblocking,
    nba_target,
    foreach_read,
};

struct Mode {
    const char* name;
    P           kind;
};

// clang-format off
//
// The three MODULE PORT modes are deliberately not generated. A port connection
// is not a procedural context, so slang rejects `sub u(v[0])` for any dynamic-type
// place outright ("cannot refer to an element or member of a dynamic type outside
// of a procedural context"), and a port-output cell cannot also assign its actual
// procedurally ("cannot mix continuous and procedural assignments"). Generating
// those cells produces ILLEGAL SystemVerilog, which is a generator bug and not a
// finding: the allowlist must only ever hold constructs that are legal SV and that
// Clotho deliberately refuses, or it stops meaning anything. Module ports are
// covered by the hand-written cases at the end of this file instead.
const Mode modes[] = {
    { "var decl", P::var_decl },
    { "function input", P::func_in },
    { "function output", P::func_out },
    { "function inout", P::func_inout },
    { "function ref", P::func_ref },
    { "function const ref", P::func_cref },
    { "task input", P::task_in },
    { "task output", P::task_out },
    { "task inout", P::task_inout },
    { "task ref", P::task_ref },
    { "task const ref", P::task_cref },
    { "function return", P::func_return },
    { "blocking assign", P::blocking },
    { "nonblocking assign", P::nonblocking },
    { "nonblocking target", P::nba_target },
    { "foreach read", P::foreach_read },
};
// clang-format on

struct Cell {
    const Elem* e;
    const Cont* c;
    const Mode* m;
};

std::string cell_name(const Cell& k) {
    return std::string(k.e->name) + " / " + k.c->name + " / " + k.m->name;
}

// A subroutine formal mode: the keyword pair and the direction, or nothing.
struct Formal {
    const char* kw     = nullptr; // "function" / "task"
    const char* dir    = nullptr; // "input" / "output" / "inout" / "ref" / "const ref"
    bool        writes = false;   // the callee produces the value; the caller shows it
};

Formal formal_of(P p) {
    switch (p) {
    case P::func_in: return { "function", "input", false };
    case P::func_out: return { "function", "output", true };
    case P::func_inout: return { "function", "inout", false };
    case P::func_ref: return { "function", "ref", false };
    case P::func_cref: return { "function", "const ref", false };
    case P::task_in: return { "task", "input", false };
    case P::task_out: return { "task", "output", true };
    case P::task_inout: return { "task", "inout", false };
    case P::task_ref: return { "task", "ref", false };
    case P::task_cref: return { "task", "const ref", false };
    default: return {};
    }
}

// The whole design for one cell. Layout, for every mode:
//
//     <axis-E named types>            compilation-unit scope, so a child
//     typedef <axis-E type> ET;       module's port list can name ET too
//     [module dm_sub(<port> ET p); ... endmodule]
//     module m;
//         <axis-C declarations>
//         <axis-C storage `v`>
//         ET tmp;
//         <mode declarations>
//         initial begin
//             <axis-E init on tmp>
//             <axis-C place of tmp into v>
//             <mode action>
//         end
//     endmodule
//
// `tmp` exists so the element's literal is built once, in one place, and then
// deposited into the container by a whole-element copy — which is itself a
// data-model operation (§1 Copy) that every cell therefore exercises.
//
// An `output` formal or port has the caller pre-fill the slot before the callee
// writes it. The slot must exist for the call to be legal at all (a dynamic
// array element, an assoc entry), and pre-filling it keeps the caller's later
// read of a Managed or handle element well defined even if the copy-out path is
// broken: such a cell asserts that the round trip lowers, runs, and leaves the
// object intact, not that copy-out happened.
std::string build_cell(const Cell& k) {
    const Elem& e   = *k.e;
    const Cont& c   = *k.c;
    const std::string acc = c.access;

    std::string outer; // before `module m`
    std::string decls; // module-scope declarations the mode adds
    std::string subs;  // the mode's subroutine
    std::string act;   // statements after `place`, inside the parent's initial

    const Formal f = formal_of(k.m->kind);
    if (f.kw != nullptr) {
        // A `const ref` formal is read-only (LRM 13.5.2), so its body only
        // reads. `inout` and `ref` read and then write back the same value, so
        // one cell covers both halves of the copy without changing the text.
        std::string body = f.writes ? subst(e.init, "a") : subst(e.show, "a");
        if (std::string_view(f.dir) == "inout" || std::string_view(f.dir) == "ref") {
            body += "\n" + subst(e.init, "a");
        }
        const bool is_task = std::string_view(f.kw) == "task";
        subs = std::string("    ") + f.kw + " automatic " + (is_task ? "" : "void ") +
               "fn(" + f.dir + " ET a);\n" + indent(body, 8) + "    end" +
               (is_task ? "task" : "function") + "\n";
        act  = "fn(" + acc + ");\n";
        if (f.writes) {
            act += subst(e.show, acc) + "\n";
        }
    } else {
        switch (k.m->kind) {
        case P::var_decl:
            act = subst(e.show, acc) + "\n";
            break;
        case P::port_in:
            outer = "module dm_sub(input var ET p);\n"
                    "    initial begin\n"
                    "        #1;\n" +
                    indent(subst(e.show, "p"), 8) +
                    "    end\n"
                    "endmodule\n";
            decls = "    dm_sub u(" + acc + ");\n";
            break;
        case P::port_out:
            outer = "module dm_sub(output var ET p);\n"
                    "    initial begin\n" +
                    indent(subst(e.init, "p"), 8) +
                    "    end\n"
                    "endmodule\n";
            decls = "    dm_sub u(" + acc + ");\n";
            act   = "#1;\n" + subst(e.show, acc) + "\n";
            break;
        case P::port_inout:
            // Deliberately no `var`: `inout` is the one direction left at the
            // LRM 23.2.2.3 default, so what a bidirectional port of this type
            // resolves to is what is under test (§6: the net mode is Plane-only).
            outer = "module dm_sub(inout ET p);\n"
                    "    initial begin\n"
                    "        #1;\n" +
                    indent(subst(e.show, "p"), 8) +
                    "    end\n"
                    "endmodule\n";
            decls = "    dm_sub u(" + acc + ");\n";
            break;
        case P::func_return:
            subs = "    function automatic ET fn();\n"
                   "        ET t;\n" +
                   indent(subst(e.init, "t"), 8) +
                   "        return t;\n"
                   "    endfunction\n";
            act  = acc + " = fn();\n" + subst(e.show, acc) + "\n";
            break;
        case P::blocking:
            decls = "    ET w;\n";
            act   = "w = " + acc + ";\n" + subst(e.show, "w") + "\n";
            break;
        case P::nonblocking:
            decls = "    ET w;\n";
            act   = "w <= " + acc + ";\n#1;\n" + subst(e.show, "w") + "\n";
            break;
        // The place under test as the nonblocking TARGET, not the source. A
        // separate mode because the two exercise different machinery entirely:
        // reading stages nothing, while writing has to shadow the place through the
        // NBA region and commit it, which every container kind does differently.
        case P::nba_target:
            decls = "    ET w;\n";
            act   = "w = " + acc + ";\n" + acc + " <= w;\n#1;\n" + subst(e.show, acc) + "\n";
            break;
        case P::foreach_read:
            if (std::string_view(c.iter).empty()) {
                // A bare element, a struct member and a class property are not
                // iterable, so the element is read out of a one-slot array: the
                // mode under test is the foreach element read, not the container.
                decls = "    ET fa[1];\n";
                act   = "fa[0] = " + acc + ";\n" + "foreach (fa[j]) " +
                      subst(e.show, "fa[j]") + "\n";
            } else {
                act = std::string("foreach (") + c.iter + ") " + subst(e.show, c.iter) + "\n";
            }
            break;
        default:
            break;
        }
    }

    std::string src;
    if (e.pre[0] != '\0') {
        src += std::string(e.pre) + "\n";
    }
    src += std::string(e.type_def) + "\n";
    src += outer;
    src += "module m;\n";
    if (c.pre[0] != '\0') {
        src += indent(c.pre, 4);
    }
    src += std::string("    ") + c.decl + "\n";
    src += "    ET tmp;\n";
    src += decls;
    src += subs;
    src += "    initial begin\n";
    src += indent(subst(e.init, "tmp"), 8);
    src += indent(c.place, 8);
    src += indent(act, 8);
    src += "    end\n";
    src += "endmodule\n";
    return src;
}

// What a supported cell must print. Only the foreach mode over an iterable
// container prints more than once — once per slot the container holds.
std::string expected_text(const Cell& k) {
    if (k.m->kind == P::foreach_read && !std::string_view(k.c->iter).empty()) {
        std::string out;
        for (int i = 0; i < k.c->slots; ++i) {
            out += k.e->expect;
        }
        return out;
    }
    return k.e->expect;
}

// -- The exclusion allowlist --

// A refusal this matrix accepts: the cells it covers, the diagnostic substring
// it must be refused with, and why the refusal is correct rather than a gap.
//
// A cell that fails to lower is accepted only if some row whose `when` holds
// also matches the diagnostic. Closing a gap means deleting a row; regressing a
// supported cell fails loudly rather than silently widening the table.
struct Allow {
    bool (*when)(const Cell&);
    const char* needle;
    const char* reason;
};

bool is_event(const Cell& k) {
    return (k.e->traits & t_event) != 0;
}

bool is_port(const Cell& k) {
    return k.m->kind == P::port_in || k.m->kind == P::port_out ||
           k.m->kind == P::port_inout;
}

// clang-format off
const Allow allowlist[] = {
    // An event ARRAY needs one object per element, and the object slot array is
    // sized at compile time from the declared events -- there is no per-element
    // binding yet. `foreach read` builds an array of the element even in the `none`
    // column, so it lands here too.
    { [](const Cell& k) { return is_event(k) &&
                                 (k.c->kind == C::farray || k.m->kind == P::foreach_read); },
      "unpacked array of events is not supported",
      "an event array needs one object per element (LRM 15.5.5)." },
    { [](const Cell& k) { return is_event(k) && k.c->kind == C::ustruct; },
      "event struct member is not supported",
      "§7: an event in an aggregate has no per-member trigger state (LRM 15.5)." },
    { [](const Cell& k) { return is_event(k) && k.c->kind == C::cprop; },
      "a per-instance event class property is not supported",
      "an event object per class object needs a runtime slot pool (LRM 15.5.5)." },
    { [](const Cell& k) { return is_event(k) &&
                                 (k.c->kind == C::dynarr || k.c->kind == C::queue); },
      "element type is not supported",
      "§7: an event element needs per-element trigger state (LRM 15.5)." },
    // An event passes and triggers like any other 64-bit handle, so the remaining
    // exclusions are narrow and each has its own reason.
    //
    // A function return needs a local to hold the result, and an automatic-lifetime
    // event local would need a FRESH object per activation.
    { [](const Cell& k) { return is_event(k) && k.m->kind == P::func_return; },
      "an automatic-lifetime event local in a subroutine is not supported",
      "a per-call event needs an object per activation (LRM 15.5.5)." },
    { [](const Cell& k) { return is_event(k) && (k.c->kind == C::assoc_int ||
                                                 k.c->kind == C::assoc_str); },
      "associative array value element type is not supported",
      "§7: an event value element needs per-element trigger state (LRM 15.5)." },

    // An inout port aliases whole storage, and a sub-object of an aggregate or
    // a container has no independent net identity to resolve drivers on.
    { [](const Cell& k) { return k.m->kind == P::port_inout && k.c->kind != C::none; },
      "inout/ref port with a non-whole-signal connection is not supported",
      "§6: the net mode is Plane-only; a container or aggregate sub-object is "
      "not a whole net (LRM 23.3.3.7)." },
    // -- The three remaining gaps, each its own separately tracked item. Every one
    // is a shape whose value is a heap handle that some path cannot stage or copy;
    // none is an element x container x mode interaction. --
    //
    // A string's rvalue materializes into a body temp rather than resolving to a
    // handle lvalue, so it does not fit HandleCopyNb's record staging.
    { [](const Cell& k) {
          return (k.m->kind == P::nonblocking || k.m->kind == P::nba_target) &&
                 k.e->name == std::string("string");
      },
      "nonblocking assignment to a string is not supported",
      "§7: a string rvalue is a body temp, not a stageable handle lvalue." },
    // Every NBA staging buffer -- the scalar shadow slot, the aggregate image and
    // the container-handle record -- is indexed by Signal. An object body is a heap
    // block with no signal, so a class property reaches none of them.
    { [](const Cell& k) { return k.m->kind == P::nba_target && k.c->kind == C::cprop; },
      "nonblocking assignment to a class property is not supported",
      "§7: NBA staging is signal-keyed; a per-object shadow does not exist "
      "(docs/design/classes.md §8)." },
    // `a[k][i]` where the entry is itself a container: the select would have to
    // pick get_ptr or put_ptr, a direction that belongs to its own consumer.
    { [](const Cell& k) {
          return (k.c->kind == C::assoc_int || k.c->kind == C::assoc_str) &&
                 (k.e->traits & t_managed) != 0;
      },
      "element select through an associative-array element that is itself a queue "
      "or dynamic array is not supported",
      "§7: needs composed deferral; put_ptr would create an entry on a read "
      "(LRM 7.8.6)." },
    // LRM 7.9 defines an associative-array right-hand side as a variable, a call
    // result or a literal; anything else has no defined whole-array value.
    { [](const Cell& k) {
          return k.e->name == std::string("assoc") &&
                 (k.m->kind == P::blocking || k.m->kind == P::nonblocking ||
                  k.m->kind == P::var_decl);
      },
      "associative array assignment from a non-variable value is not supported",
      "LRM 7.9 admits a variable, a call result or a literal as the source; the "
      "generated cell is none of those." },
};
// clang-format on

// -- The walk --

struct Bad {
    std::string cell;
    std::string what;   // why the cell is unexpected
    std::string detail; // the diagnostic, or both texts
};

// Run one cell and record it if its outcome is neither of the two acceptable
// ones. A cell never aborts the walk, so one regression cannot hide the other
// 2735 results.
//
// The cell is lowered in this process, because the diagnostic is what the
// exclusion allowlist matches on, but SIMULATED in a child process. A cell that
// segfaults or exits under a runtime fatal is then recorded as a loud failure
// with its exit status instead of taking the whole test runner down and
// reporting nothing at all — which is exactly what the `int [2]` / bare element
// / `function ref` cell did before this walked out of process.
void run_cell(const Cell& k, std::vector<Bad>& bad) {
    const std::string src = build_cell(k);
    const LowerResult r   = lower_text(src);

    if (!r.ok || r.module == nullptr) {
        const std::string diag = r.diagnostic_text();
        for (const Allow& a : allowlist) {
            if (a.when(k) && diag.find(a.needle) != std::string::npos) {
                return;
            }
        }
        bad.push_back({ cell_name(k), "refused, and not on the exclusion allowlist", diag });
        return;
    }

    // Out of process, so this asserts the child's exit code. Under an ASan build
    // that leaves leak detection on, LeakSanitizer exits non-zero for the two
    // documented positions ($finish's longjmp out of live coroutine frames, and
    // class objects, which have no GC by direction) and every cell reports here.
    // Run the matrix with ASAN_OPTIONS=detect_leaks=0; the walk itself is clean.
    const test::DesignRun run = test::run_design_captured(src);
    if (test::design_exit_code(run) != 0) {
        bad.push_back({ cell_name(k), "lowered, but the run did not exit cleanly",
                        "rc " + std::to_string(test::design_exit_code(run)) + ", stderr \"" +
                          run.err + "\"" });
        return;
    }

    const std::string want = expected_text(k);
    if (run.out != want) {
        bad.push_back({ cell_name(k), "wrong output",
                        "want \"" + want + "\", got \"" + run.out + "\"" });
    }
}

// Every cell of one axis-C column, walked once per process and memoized so the
// summary TEST_CASE reports the same results the per-column ones checked.
const std::vector<Bad>& column(const Cont& c) {
    static std::map<const Cont*, std::vector<Bad>> memo;
    const auto found = memo.find(&c);
    if (found != memo.end()) {
        return found->second;
    }
    std::vector<Bad>& bad = memo[&c];
    for (const Elem& e : elems) {
        for (const Mode& m : modes) {
            // LRM 10.4.2 forbids a nonblocking assignment to an element of a
            // dynamically sized array, and slang rejects it outright. Generating
            // these would be ILLEGAL SystemVerilog, which is a generator bug rather
            // than a finding -- the same rule that keeps the port modes out (above).
            // Whole-container NBA (`q <= r`) IS legal and is covered by the
            // hand-written cases; it is the element form that is not.
            const bool dyn_elem = c.kind == C::dynarr || c.kind == C::queue ||
                                  c.kind == C::assoc_int || c.kind == C::assoc_str;
            if (m.kind == P::nba_target && dyn_elem) {
                continue;
            }
            run_cell(Cell{ &e, &c, &m }, bad);
        }
    }
    return bad;
}

// The compact table of unexpected cells, capped so doctest output stays
// readable when a whole column regresses at once.
std::string table(const std::vector<Bad>& bad) {
    constexpr size_t shown = 40;
    std::string      out   = "\n" + std::to_string(bad.size()) + " unexpected cell(s):\n";
    for (size_t i = 0; i < bad.size() && i < shown; ++i) {
        out += "  " + bad[i].cell + "\n      " + bad[i].what + ": " + bad[i].detail + "\n";
    }
    if (bad.size() > shown) {
        out += "  ... and " + std::to_string(bad.size() - shown) + " more\n";
    }
    return out;
}

void check_column(const Cont& c) {
    const std::vector<Bad>& bad = column(c);
    CHECK_MESSAGE(bad.empty(), table(bad));
}

} // namespace

// -- The matrix, one TEST_CASE per axis-C column --

TEST_CASE("data model matrix: bare element") {
    check_column(conts[0]);
}

TEST_CASE("data model matrix: fixed unpacked array") {
    check_column(conts[1]);
}

TEST_CASE("data model matrix: unpacked struct member") {
    check_column(conts[2]);
}

TEST_CASE("data model matrix: dynamic array element") {
    check_column(conts[3]);
}

TEST_CASE("data model matrix: queue element") {
    check_column(conts[4]);
}

TEST_CASE("data model matrix: associative array value, int key") {
    check_column(conts[5]);
}

TEST_CASE("data model matrix: associative array value, string key") {
    check_column(conts[6]);
}

TEST_CASE("data model matrix: class property") {
    check_column(conts[7]);
}

// The whole product in one place, so a failure anywhere is summarized once
// rather than read out of eight separate reports. Columns already walked are
// reused, so this costs nothing after the eight above; run alone, it walks
// everything itself.
TEST_CASE("data model matrix: summary") {
    std::vector<Bad> bad;
    for (const Cont& c : conts) {
        const std::vector<Bad>& col = column(c);
        bad.insert(bad.end(), col.begin(), col.end());
    }
    const size_t cells = std::size(elems) * std::size(conts) * std::size(modes);
    // One local: CHECK_MESSAGE streams its message, so a `+` chain would bind to
    // doctest's builder instead of to std::string.
    const std::string summary = std::to_string(cells) + " cells walked" + table(bad);
    CHECK_MESSAGE(bad.empty(), summary);
}

// -- The other two §7 exclusions, which the matrix's axes do not reach --

// LRM 7.8.5: "real or shortreal data types, or a type containing a real or
// shortreal, shall be an illegal index type". Illegal, not a gap (§7), so it is
// pinned here rather than allowlisted: the matrix enumerates int and string keys
// only. The refusal is slang's, which reaches a bare `real` key before Clotho's
// own guard_agg_key_leaves does, so the LRM clause is cited here rather than
// quoted from the diagnostic.
TEST_CASE("a real associative-array key is illegal") {
    expect_fail(R"(
        module m;
            int v[real];
            initial v[1.5] = 1;
        endmodule
    )",
                "index type cannot be or contain floating point types");
}

// §6: a net's value is resolved from competing drivers, which needs a bit-level
// strength fold; a Managed or Aggregate type has no such fold. The refusal is
// the LRM's (6.7.1 admits only a 4-state integral net type), so this pins that
// the declaration is refused, not the wording of whose refusal it is.
TEST_CASE("a net of a managed or aggregate type is refused") {
    expect_fail(R"(
        module m;
            wire string s;
            initial $display("%s", s);
        endmodule
    )",
                "net");
    expect_fail(R"(
        module m;
            typedef struct { int a; } us_t;
            wire us_t s;
            initial $display("%0d", s.a);
        endmodule
    )",
                "net");
}

// -- ref lifetime: the positive and negative epoch cases of §3 / §9 --

// §3: `ref` binds the formal to the actual's storage, so a queue element written
// by the callee is the caller's element (LRM 13.5.2). The queue is not mutated,
// so the epoch the ref recorded still matches and the check is silent.
TEST_CASE("ref to a queue element: the callee's write is the caller's element") {
    const std::string out = run_capture(R"(
        module m;
            int q[$];
            int i;
            task automatic bump(ref int a);
                a = a + 1;
            endtask
            initial begin
                q.push_back(41);
                i = 0;
                bump(q[i]);
                $display("%0d", q[i]);
            end
        endmodule
    )");
    CHECK(out == "42\n");
}

// The write is visible across a #delay, which is the property that forces a
// pointer rather than copy-in/copy-out (§3). Nothing mutates the queue while the
// ref is live, so this must work: the epoch is unchanged at the deref.
TEST_CASE("ref to a queue element survives a task delay with no mutation") {
    const std::string out = run_capture(R"(
        module m;
            int q[$];
            task automatic bump_slow(ref int a);
                #2;
                a = a + 1;
            endtask
            initial begin
                q.push_back(41);
                bump_slow(q[0]);
                $display("%0d", q[0]);
            end
        endmodule
    )");
    CHECK(out == "42\n");
}

// §3.1: push_back bumps the queue's epoch, because it changes len and hence the
// index-to-address mapping, whether or not the buffer moved. The ref formal
// re-reads *epoch at its next deref, finds it advanced past `expect`, and the
// run ends in the located rt_ref_stale fatal. Out of process because the fatal
// exits, and because the report is the payload (the model is class_test.cpp's
// null-handle case).
TEST_CASE("a push_back under a live ref to a queue element is a located fatal") {
    constexpr std::string_view src = "module m;\n"
                                     "    int q[$];\n"
                                     "    task automatic bump_slow(ref int a);\n"
                                     "        #2;\n"
                                     "        a = a + 1;\n"
                                     "    endtask\n"
                                     "    initial begin\n"
                                     "        q.push_back(41);\n"
                                     "        $display(\"before\");\n"
                                     "        fork\n"
                                     "            bump_slow(q[0]);\n"
                                     "            begin #1; q.push_back(7); end\n"
                                     "        join\n"
                                     "        $display(\"unreachable\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    // The write through the stale ref never happens, and nothing after it runs.
    CHECK(run.out == "before\n");
    CHECK(test::design_exit_code(run) != 0);
    // The headline names the ref formal, then the container that moved under it,
    // then both epochs. The container's name is not shipped with the call: the
    // runtime recovers 'q' from the epoch pointer the guard was watching (§3.3).
    CHECK(run.err.find("fatal: ref '") != std::string::npos);
    CHECK(run.err.find("' is stale: 'q' was restructured (epoch ") != std::string::npos);
    CHECK(run.err.find(", ref formed at epoch ") != std::string::npos);
    // Located at the DEREFERENCE, inside the callee -- the site record is the
    // callee's own, so it points at the access that faulted. The formal's name in
    // the headline is what ties that back to the binding.
    CHECK(run.err.find("5 |         a = a + 1;\n") != std::string::npos);
    CHECK(run.err.find("bump_slow argument 1") != std::string::npos);
    // And it names the operation that invalidated the pointer, and the fix.
    CHECK(run.err.find("moved the element ") != std::string::npos);
    CHECK(run.err.find("do not restructure 'q' while the ref is live") != std::string::npos);
    CHECK(run.err.find("(LRM 13.5.2)") != std::string::npos);
}

// §3.3: the container's name is recovered from the epoch pointer, through a table
// the design emits once. That table can only key on containers whose handle has a
// fixed address -- a signal. A queue that is a CLASS PROPERTY lives in a heap
// object body, so its epoch address is a runtime value and the lookup misses.
//
// The report must then describe the container rather than name it. Naming is not
// merely unavailable here, it would be wrong: the only name in scope is the
// object's, and the object did not move -- its queue's buffer did.
TEST_CASE("a stale ref to a container with no static address is reported unnamed") {
    constexpr std::string_view src = "class C;\n"
                                     "    int q[$];\n"
                                     "endclass\n"
                                     "module m;\n"
                                     "    task automatic bump_slow(ref int a);\n"
                                     "        #2;\n"
                                     "        a = a + 1;\n"
                                     "    endtask\n"
                                     "    initial begin\n"
                                     "        C c;\n"
                                     "        c = new;\n"
                                     "        c.q.push_back(41);\n"
                                     "        fork\n"
                                     "            bump_slow(c.q[0]);\n"
                                     "            begin #1; c.q.push_back(7); end\n"
                                     "        join\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);
    CHECK(test::design_exit_code(run) != 0);
    // Still fatal, still located, still names the formal -- only the container's
    // name is missing, and it is described instead of guessed at or left blank.
    CHECK(run.err.find("is stale: its container was restructured (epoch ") != std::string::npos);
    CHECK(run.err.find("bump_slow argument 1") != std::string::npos);
    CHECK(run.err.find("7 |         a = a + 1;\n") != std::string::npos);
    CHECK(run.err.find("do not restructure its container while the ref is live") !=
          std::string::npos);
    // No empty quotes anywhere: the old shape passed "" for this case and printed
    // `'' was restructured`.
    CHECK(run.err.find("''") == std::string::npos);
}

// §3: a `ref` formal's frame slot and the actual's storage slot must agree on
// shape. They are the same object seen from two sides, so if one side is the
// packed `{i(2W), iW}` plane and the other the narrow 2-state `{iW}` slot, the
// callee's store is the wrong width and writes past the element.
//
// A fixed unpacked array element is where that bites: the elements are adjacent,
// so an oversized store through a ref to `a[i]` lands in a[i+1] or in whatever
// signal the state struct placed next. Asserting the NEIGHBOUR is untouched is
// what catches it with a single activation — the value read back through the ref
// itself looks perfectly correct.
TEST_CASE("a ref to an array element does not overrun its neighbour") {
    const std::string out = run_capture(R"(
        module m;
            int a[2];
            int nb;
            int i;
            task automatic bump(ref int r);
                r = r + 1;
            endtask
            initial begin
                a[0] = 1;
                a[1] = 10;
                nb = 77;
                i = 1;
                bump(a[i]);
                $display("%0d %0d %0d", a[0], a[1], nb);
            end
        endmodule
    )");
    CHECK(out == "1 11 77\n");
}

// Two live activations of one automatic task, each with its own actual, make a
// slot-shape disagreement visible as a wrong value rather than as a write into
// padding: here the oversized store through the ref to `a[1]` reaches `s`, the
// next signal in the state struct, so `s` reads 11 instead of 21.
TEST_CASE("concurrent ref activations write only their own actual") {
    const std::string out = run_capture(R"(
        module m;
            int a[2];
            int s;
            task automatic bump_slow(ref int r);
                #2;
                r = r + 1;
            endtask
            initial begin
                a[1] = 10;
                s = 20;
                fork
                    bump_slow(a[1]);
                    bump_slow(s);
                join
                $display("%0d %0d", a[1], s);
            end
        endmodule
    )");
    CHECK(out == "11 21\n");
}

// §3: a stable root points `epoch` at the immutable zero global and passes
// expect == 0, so a fixed unpacked array element and a module signal are never
// reported stale, however much any container is restructured while the ref is
// live. That is the correct answer, and the reason the check costs a load and a
// compare rather than a null test.
//
// Out of process and one ref activation per design: a stale report exits(1), so
// the absence of one is exactly rc == 0 plus clean stderr, and asserting that
// in-process would take the whole runner down on a regression.
void check_stable_root(std::string_view source, std::string_view want) {
    const test::DesignRun run = test::run_design_captured(source);
    CHECK(run.out == want);
    CHECK(test::design_exit_code(run) == 0);
    CHECK(run.err.find("is stale") == std::string::npos);
}

TEST_CASE("ref to a fixed unpacked array element is never stale") {
    check_stable_root(R"(
        module m;
            int a[2];
            int q[$];
            task automatic bump_slow(ref int r);
                #2;
                r = r + 1;
            endtask
            initial begin
                a[1] = 10;
                q.push_back(1);
                fork
                    bump_slow(a[1]);
                    begin
                        #1;
                        q.push_back(2);
                        q.push_back(3);
                        q.delete();
                    end
                join
                $display("%0d", a[1]);
            end
        endmodule
    )",
                       "11\n");
}

TEST_CASE("ref to a module signal is never stale") {
    check_stable_root(R"(
        module m;
            int s;
            int q[$];
            task automatic bump_slow(ref int r);
                #2;
                r = r + 1;
            endtask
            initial begin
                s = 20;
                q.push_back(1);
                fork
                    bump_slow(s);
                    begin
                        #1;
                        q.push_back(2);
                        q.delete();
                    end
                join
                $display("%0d", s);
            end
        endmodule
    )",
                       "21\n");
}

// §3: a Managed ref passes the handle itself, so the formal aliases the caller's
// container rather than copying it. One case per managed kind, plus the two
// non-Plane storage classes that reach the callee by image pointer.
TEST_CASE("ref string: the callee's mutation is the caller's string") {
    const std::string out = run_capture(R"(
        module m;
            string s;
            task automatic app(ref string a);
                a = {a, "!"};
            endtask
            initial begin
                s = "hi";
                app(s);
                $display("%s", s);
            end
        endmodule
    )");
    CHECK(out == "hi!\n");
}

TEST_CASE("ref queue: the callee's push is the caller's queue") {
    const std::string out = run_capture(R"(
        module m;
            int q[$];
            task automatic app(ref int a[$]);
                a.push_back(9);
            endtask
            initial begin
                q.push_back(8);
                app(q);
                $display("%0d %0d %0d", q.size(), q[0], q[1]);
            end
        endmodule
    )");
    CHECK(out == "2 8 9\n");
}

TEST_CASE("ref assoc: the callee's entry is the caller's entry") {
    const std::string out = run_capture(R"(
        module m;
            int mm[string];
            task automatic app(ref int a[string]);
                a["b"] = 5;
            endtask
            initial begin
                mm["a"] = 4;
                app(mm);
                $display("%0d %0d %0d", mm.num(), mm["a"], mm["b"]);
            end
        endmodule
    )");
    CHECK(out == "2 4 5\n");
}

TEST_CASE("ref unpacked struct: the callee's member write is the caller's member") {
    const std::string out = run_capture(R"(
        module m;
            typedef struct { int a; int b; } us_t;
            us_t s;
            task automatic app(ref us_t r);
                r.b = 6;
            endtask
            initial begin
                s = '{a: 1, b: 2};
                app(s);
                $display("%0d %0d", s.a, s.b);
            end
        endmodule
    )");
    CHECK(out == "1 6\n");
}

// A class handle is a Plane with reference semantics (§1), so `ref` of one must
// alias the handle word: reassigning the formal must retarget the caller's
// variable, not just the object it used to point at.
TEST_CASE("ref class handle: the callee's reassignment is the caller's handle") {
    const std::string out = run_capture(R"(
        module m;
            class EC;
                int v;
            endclass
            EC h;
            task automatic swap(ref EC a);
                a = new;
                a.v = 7;
            endtask
            initial begin
                h = new;
                h.v = 3;
                swap(h);
                $display("%0d", h.v);
            end
        endmodule
    )");
    CHECK(out == "7\n");
}
