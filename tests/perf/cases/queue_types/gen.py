#!/usr/bin/env python3
"""Generate the queue benchmark designs.

usage: gen.py types|types_loops|modules|ring > design.sv

types        56 distinct queue element types in one module, every operation (push_back,
             push_front, insert, q[i], delete, pop_front, pop_back) used once per type, in
             straight-line code. Compile cost of the queue engine linked into a design,
             where it is the number of element types and sites that count.
types_loops  The same operations of the same types, each inside a loop that repeats them.
modules      The 56 types spread over 16 modules (plus one shared type per module), each
             module with its own operations: the engine linked once per module.
ring         Queue operations in loops, sized to run for a fraction of a second: an int
             queue pushed and popped, a string queue, a struct with a string member, and
             insert / delete in the middle of a queue. Simulation cost of the engine.
"""

import sys

TYPES = 56


def elem_type(i):
    w = i + 1
    return [
        f"logic [{w}:0]",
        f"bit [{w + 3}:0]",
        f"struct {{ int a; string s{i}; logic [{w}:0] b; }}",
        f"struct {{ string s; byte b[{i % 5 + 1}]; }}",
        f"struct packed {{ logic [{w}:0] a; bit [3:0] b; }}",
        f"struct {{ int q[$]; shortint k{i}; }}",
        f"struct {{ string a; string b; int c{i}; }}",
    ][i % 7]


def declare(indices):
    lines = []
    for i in indices:
        lines.append(f"  typedef {elem_type(i)} T{i};")
        lines.append(f"  T{i} q{i}[$]; T{i} v{i};")
    return lines


def operations(indices):
    lines = []
    for i in indices:
        lines += [
            f"    q{i}.push_back(v{i});",
            f"    q{i}.push_back(v{i}); q{i}.push_front(v{i});",
            f"    q{i}.insert(1, v{i});",
            f"    v{i} = q{i}[1];",
            f"    q{i}.delete(1);",
            f"    v{i} = q{i}.pop_front(); v{i} = q{i}.pop_back();",
        ]
    return lines


def sizes(indices):
    return " + ".join(f"q{i}.size()" for i in indices)


def types(loops):
    body = operations(range(TYPES))
    if loops:
        body = ["    for (int rep = 0; rep < 2; rep++) begin"] + ["  " + l for l in body] + ["    end"]
    out = ["module top;"] + declare(range(TYPES)) + ["  initial begin"] + body
    out += [f'    $display("done %0d", {sizes(range(TYPES))});', "  end", "endmodule"]
    return out


def modules():
    out = []
    members = []
    for m in range(16):
        idx = sorted({i for i in range(TYPES) if i % 16 == m} | {(m * 3 + 1) % TYPES})
        members.append(idx)
        out += [f"module m{m};"] + declare(idx) + ["  int total;", "  initial begin"] + operations(idx)
        out += [f"    total = {sizes(idx)};", "  end", "endmodule"]
    out += ["module top;"] + [f"  m{m} u{m}();" for m in range(16)]
    out += ["  initial begin", "    #1;", '    $display("done %0d", ' + " + ".join(f"u{m}.total" for m in range(16)) + ");",
            "  end", "endmodule"]
    return out


RING = """module top;
  int iq[$]; longint isum = 0;
  string sq[$]; string s; longint ssum = 0;
  typedef struct { int a; string s; } rec_t;
  rec_t rq[$]; rec_t r; longint rsum = 0;
  int mq[$]; longint msum = 0;
  initial begin
    for (int i = 0; i < 16; i++) iq.push_back(i);
    for (int i = 0; i < 3000000; i++) begin
      iq.push_back(i);
      isum += iq.pop_front();
    end
    for (int i = 0; i < 16; i++) sq.push_back($sformatf("s%0d", i));
    s = "hello world";
    for (int i = 0; i < 700000; i++) begin
      sq.push_back(s);
      s = sq.pop_front();
      ssum += s.len();
    end
    for (int i = 0; i < 16; i++) begin r.a = i; r.s = "abc"; rq.push_back(r); end
    for (int i = 0; i < 700000; i++) begin
      r.a = i;
      rq.push_back(r);
      r = rq.pop_front();
      rsum += r.a + r.s.len();
    end
    for (int i = 0; i < 64; i++) mq.push_back(i);
    for (int i = 0; i < 700000; i++) begin
      mq.insert(32, i);
      msum += mq[33];
      mq.delete(32);
    end
    $display("done %0d %0d %0d %0d", isum, ssum, rsum, msum);
  end
endmodule"""


def main():
    kind = sys.argv[1] if len(sys.argv) == 2 else ""
    if kind == "types":
        out = types(False)
    elif kind == "types_loops":
        out = types(True)
    elif kind == "modules":
        out = modules()
    elif kind == "ring":
        out = RING.split("\n")
    else:
        sys.exit(__doc__)
    print("\n".join(out))


main()
