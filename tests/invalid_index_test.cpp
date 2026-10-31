#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// LRM 7.4.5 / 7.8.6 / 7.10.1: an element access with an invalid index (outside the
// container, or with an x/z bit) reads the Table 7-1 default and writes nothing.
//
// The backend guarantees it by type (Guarded in src/ir/llvm/Backend.h): a place
// whose select may be invalid cannot reach memory except through a guard. These
// tests are the other half: every consumer of an element place -- load, store,
// compound assignment, nonblocking write, whole-aggregate copy, `ref` and by-value
// arguments, a returned element, `==`, container push / insert, a runtime write-back
// -- over each container kind (fixed array, dynamic array, queue, associative
// array) and each kind of invalid index. Before the guard was structural, each
// consumer had to remember it, and the ones that forgot read or wrote past the
// container: a segfault for a wild index, silent corruption otherwise.
//
// Expectations follow the LRM text. iverilog is the oracle for the integer
// consumers it supports; it has no unpacked structs, `ref` ports or a
// writable $sscanf target, so those are checked against the LRM alone.

namespace {

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module);

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

struct Container {
    const char*              decl;
    const char*              init;
    const char*              show;
    std::vector<const char*> indices;
};

// The 4-state scalar element: a, then 10 11 12 13.
const Container kInt[] = {
    { "integer a [0:3];", "a[0] = 10; a[1] = 11; a[2] = 12; a[3] = 13;",
      R"($display("  a=%0d %0d %0d %0d", a[0], a[1], a[2], a[3]);)", { "7", "-1", "'bx", "'bz" } },
    { "integer a [];", "a = new[4]; a[0] = 10; a[1] = 11; a[2] = 12; a[3] = 13;",
      R"($display("  sz=%0d a=%0d %0d %0d %0d", a.size(), a[0], a[1], a[2], a[3]);)",
      { "7", "-1", "'bx", "'bz" } },
    { "integer a [$];", "a = {}; a.push_back(10); a.push_back(11); a.push_back(12); a.push_back(13);",
      R"($display("  sz=%0d a=%0d %0d %0d %0d", a.size(), a[0], a[1], a[2], a[3]);)",
      { "7", "-1", "'bx", "'bz" } },
    { "integer a [integer];", "a.delete(); a[0] = 10; a[1] = 11; a[2] = 12; a[3] = 13;",
      R"($display("  sz=%0d a=%0d %0d %0d %0d", a.num(), a[0], a[1], a[2], a[3]);)", { "'bx", "'bz" } },
};

// The unpacked-struct element: x / y, then 0/0 1/10 2/20 3/30.
const Container kStruct[] = {
    { "S a [0:3];", "for (int j = 0; j < 4; j++) begin a[j].x = j; a[j].y = 10*j; end",
      R"($display("  a=%0d/%0d %0d/%0d %0d/%0d %0d/%0d", a[0].x, a[0].y, a[1].x, a[1].y, a[2].x, a[2].y, a[3].x, a[3].y);)",
      { "7", "-1", "'bx", "'bz" } },
    { "S a [];", "a = new[4]; for (int j = 0; j < 4; j++) begin a[j].x = j; a[j].y = 10*j; end",
      R"($display("  sz=%0d a=%0d/%0d %0d/%0d %0d/%0d %0d/%0d", a.size(), a[0].x, a[0].y, a[1].x, a[1].y, a[2].x, a[2].y, a[3].x, a[3].y);)",
      { "7", "-1", "'bx", "'bz" } },
    { "S a [$];",
      "a = {}; for (int j = 0; j < 4; j++) begin S t; t.x = j; t.y = 10*j; a.push_back(t); end",
      R"($display("  sz=%0d a=%0d/%0d %0d/%0d %0d/%0d %0d/%0d", a.size(), a[0].x, a[0].y, a[1].x, a[1].y, a[2].x, a[2].y, a[3].x, a[3].y);)",
      { "7", "-1", "'bx", "'bz" } },
    { "S a [integer];", "a.delete(); for (int j = 0; j < 4; j++) begin a[j].x = j; a[j].y = 10*j; end",
      R"($display("  sz=%0d a=%0d/%0d %0d/%0d %0d/%0d %0d/%0d", a.num(), a[0].x, a[0].y, a[1].x, a[1].y, a[2].x, a[2].y, a[3].x, a[3].y);)",
      { "'bx", "'bz" } },
};

const char* const kSource = R"(
module m;
  typedef struct { integer x; integer y; } S;
  @@DECL@@
  S tmp;
  S qq [$];
  integer i, n, k;
  task automatic bump(ref integer r);
    $display("  ref in=%0d", r);
    r = 77;
  endtask
  task automatic bumps(ref S r);
    $display("  refs in=%0d/%0d", r.x, r.y);
    r.x = 77;
  endtask
  function automatic integer sum(S v); return v.x + v.y; endfunction
  function automatic S get(S v); return v; endfunction
  initial begin
    for (k = 0; k < @@N@@; k++) begin
      @@INIT@@
      case (k)
@@CASES@@
      endcase
      @@BODY@@
      $display("  i=%0d", i);
      @@SHOW@@
    end
  end
endmodule
)";

std::string replace(std::string s, std::string_view from, std::string_view to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
    }
    return s;
}

// Runs `body` once per invalid index of `c` and returns the output the LRM
// prescribes: the consumer's own line(s) `per_index`, then the index and the
// container, which must be untouched.
std::string run_case(const Container& c, std::string_view body, std::string_view per_index,
                     std::string& expected) {
    std::string cases;
    for (size_t k = 0; k < c.indices.size(); ++k) {
        cases += std::string(k + 1 < c.indices.size() ? "        " + std::to_string(k) + ": i = "
                                                      : "        default: i = ") +
                 c.indices[k] + ";\n";
    }
    std::string src = replace(kSource, "@@DECL@@", c.decl);
    src             = replace(src, "@@INIT@@", c.init);
    src             = replace(src, "@@SHOW@@", c.show);
    src             = replace(src, "@@BODY@@", body);
    src             = replace(src, "@@N@@", std::to_string(c.indices.size()));
    src             = replace(src, "@@CASES@@", cases);

    // What the container shows is its initial content, so run the same program
    // with an empty body: the difference between the two is the consumer's lines.
    std::string untouched = replace(src, std::string(body), "");
    std::string shown     = run_capture(untouched);
    expected.clear();
    size_t pos = 0;
    for (size_t k = 0; k < c.indices.size(); ++k) {
        const size_t      end = shown.find("  i=", pos);
        REQUIRE(end != std::string::npos);
        const size_t next = shown.find('\n', shown.find('\n', end) + 1);
        expected += std::string(per_index) + shown.substr(end, next + 1 - end);
        pos = next + 1;
    }
    return run_capture(src);
}

void check_each(const Container* set, size_t count, std::string_view body, std::string_view per_index) {
    for (size_t c = 0; c < count; ++c) {
        std::string expected;
        const std::string got = run_case(set[c], body, per_index, expected);
        CHECK_MESSAGE(got == expected, set[c].decl, "\n got:\n", got, "\n expected:\n", expected);
    }
}

} // namespace

TEST_CASE("an invalid index reads the element default and a blocking write does nothing") {
    check_each(kInt, 4, "a[i] = 99; a[i] += 1; a[i]++;", "");
    check_each(kInt, 4, R"($display("  read=%0d", a[i]);)", "  read=x\n");
}

TEST_CASE("an invalid index of a ref actual binds a discard slot") {
    check_each(kInt, 4, "bump(a[i]);", "  ref in=x\n");
}

TEST_CASE("an invalid index of a $sscanf target writes nothing") {
    check_each(kInt, 4, R"(n = $sscanf("42", "%d", a[i]);)", "");
}

TEST_CASE("an invalid index of a struct element: copy, member write and read") {
    check_each(kStruct, 4, "a[i].x = 99;", "");
    check_each(kStruct, 4, R"(tmp = a[i]; $display("  tmp=%0d/%0d", tmp.x, tmp.y);)", "  tmp=x/x\n");
    check_each(kStruct, 4, "tmp.x = 5; tmp.y = 6; a[i] = tmp;", "");
}

TEST_CASE("an invalid index of a struct element: ref, by-value argument, result and ==") {
    check_each(kStruct, 4, "bumps(a[i]);", "  refs in=x/x\n");
    check_each(kStruct, 4, R"($display("  byval=%0d", sum(a[i]));)", "  byval=x\n");
    check_each(kStruct, 4, R"(tmp = get(a[i]); $display("  ret=%0d/%0d", tmp.x, tmp.y);)",
               "  ret=x/x\n");
    check_each(kStruct, 4, R"(tmp.x = 0; tmp.y = 0; $display("  eq=%b", a[i] == tmp);)", "  eq=x\n");
}

// An associative-array element cannot be pushed whole (a separate, reported refusal).
TEST_CASE("an invalid index of a struct element pushed or inserted into a queue yields x") {
    check_each(kStruct, 3,
               R"(qq.delete(); qq.push_back(a[i]); $display("  qq=%0d/%0d", qq[0].x, qq[0].y);)",
               "  qq=x/x\n");
    check_each(kStruct, 3,
               R"(qq.delete(); qq.insert(0, a[i]); $display("  qq=%0d/%0d", qq[0].x, qq[0].y);)",
               "  qq=x/x\n");
}

TEST_CASE("an invalid index of a ref to a struct member binds a discard slot") {
    check_each(kStruct, 4, "bump(a[i].y);", "  ref in=x\n");
}
