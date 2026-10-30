// System tasks and functions: formatting, display, file I/O, casts and string and enum methods.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Lower a $system shell-exec call (LRM §20.17.1), returning the i32 result
// (the raw system() return). Shared by the task-statement and function-call
// forms. Gated by allow_system_: Off refuses it at compile time (no exec
// code is emitted); Const requires the command to constant-fold at
// elaboration — which proves it cannot depend on any runtime input, since
// plusargs / file contents / $random are not constant-evaluable; Full allows
// a runtime-computed command. Default-off is a deliberate, security-motivated
// deviation from LRM compliance (docs/LRM_NOTES.md, note 29).
ir::Index Lowerer::lower_system_call(const CallExpression& call, ir::Builder& b) {
    const ir::SourceLoc l    = loc(call);
    const auto          args = call.arguments();
    if (args.size() > 1) {
        fail("$system expects at most one argument (a command string)");
    }
    if (allow_system_ == ir::SystemAccess::Off) {
        fail("$system requires --allow-system=const or --allow-system=full "
             "(shell command execution is disabled by default)");
    }
    if (args.empty()) {
        // No-arg $system(): exec system(NULL). The command operand is an
        // unused empty-byte constant (has_arg = 0 tells the runtime to pass
        // NULL).
        const ir::Index empty = b.constant(module_.pool.put_int(make_int_type(8, false), 0), l);
        return b.system(empty, /*has_arg=*/0, l);
    }
    const Expression& cmd = *args[0];
    if (allow_system_ == ir::SystemAccess::Const) {
        if (fold_const(cmd, Fold::Runtime).bad()) {
            fail("$system with --allow-system=const requires a constant command "
                 "string; use --allow-system=full for a runtime-computed command");
        }
    }
    return b.system(lower_string_arg(cmd, b), /*has_arg=*/1, l);
}

// $dumpvars (LRM §21.7.1.3). Phase 0 supports only the whole-design form:
// $dumpvars, $dumpvars(0), or $dumpvars(0, <top>...) whose listed scopes are
// exactly the design's top-module instances (so the request equals the whole
// design). A level > 0 limits hierarchy depth and any other scope / variable
// argument selects a subset; both are deferred (docs/VCD.md, sparse dumping) and
// loud-fail rather than dumping the wrong set.
void Lowerer::lower_dumpvars(const CallExpression& call, ir::Builder& b) {
    const auto args = call.arguments();
    if (!args.empty()) {
        const std::optional<int32_t> level = try_const_int(*args[0]);
        if (!level.has_value() || *level != 0) {
            fail("$dumpvars with a nonzero level is not supported (whole-design dump only)");
        }
        if (args.size() > 1) {
            const auto                             tops = scope_->getCompilation().getRoot().topInstances;
            std::vector<const slang::ast::Symbol*> listed;
            for (size_t k = 1; k < args.size(); ++k) {
                const auto* as = args[k]->as_if<slang::ast::ArbitrarySymbolExpression>();
                if (as == nullptr || as->symbol->kind != slang::ast::SymbolKind::Instance) {
                    fail("$dumpvars of a scope/variable subset is not supported (whole-design dump only)");
                }
                bool is_top = false;
                for (const slang::ast::InstanceSymbol* t : tops) {
                    if (t == as->symbol.get()) {
                        is_top = true;
                        break;
                    }
                }
                if (!is_top) {
                    fail("$dumpvars of a non-top scope is not supported (whole-design dump only)");
                }
                listed.push_back(as->symbol.get());
            }
            // The list must cover every top, else the whole-design dump would be
            // a superset of what was requested.
            for (const slang::ast::InstanceSymbol* top : tops) {
                bool covered = false;
                for (const slang::ast::Symbol* l : listed) {
                    if (l == top) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    fail("$dumpvars must list every top module to dump the whole design");
                }
            }
        }
    }
    dumps_vcd_ = true;
    b.dump_vars(loc(call));
}

// $readmemh / $readmemb (LRM §21.4): load a file into an unpacked-array memory.
// Forms: (file, mem), (file, mem, start), (file, mem, start, finish). The memory
// must be a whole unpacked array (a plain name or hierarchical reference); the
// filename is a string literal, string-typed expression, or integral value
// holding packed ASCII (LRM §21.4), and the optional address bounds are general
// runtime values.
void Lowerer::lower_readmem(const CallExpression& call, bool hex, ir::Builder& b) {
    const auto args = call.arguments();
    if (args.size() < 2 || args.size() > 4) {
        fail("$readmem requires 2 to 4 arguments");
    }
    // slang (post-8.0) binds the memory as a system-task output argument: an
    // AssignmentExpression whose left side is the target lvalue, the same
    // shape a user task's output actual has. Unwrap to the target.
    const Expression* mem_arg = args[1];
    if (mem_arg->kind == ExpressionKind::Assignment) {
        mem_arg = &mem_arg->as<AssignmentExpression>().left();
    }
    const Expression& mem = *mem_arg;
    const auto*       vsb = mem.as_if<slang::ast::ValueExpressionBase>();
    if (vsb == nullptr || mem.type->getCanonicalType().as_if<slang::ast::FixedSizeUnpackedArrayType>() == nullptr) {
        // TODO: a sliced memory target (mem[a:b]) or a multidimensional slice.
        fail("$readmem target must be a whole unpacked-array memory");
    }
    const ir::Signal target     = signal_of(vsb->symbol);
    const ir::Index  filename   = lower_expr(*args[0], b);
    ir::Index        start      = filename; // placeholder; unused unless has_start
    ir::Index        finish     = filename;
    const bool       has_start  = args.size() >= 3;
    const bool       has_finish = args.size() >= 4;
    if (has_start) {
        start = lower_expr(*args[2], b);
    }
    if (has_finish) {
        finish = lower_expr(*args[3], b);
    }
    b.read_mem(target, filename, start, finish, hex, has_start, has_finish, loc(call));
}

// $timeformat(units, precision, suffix, min_width): set the %t format for all
// later %t specifiers (LRM §20.4.3). units / precision / min_width are general
// runtime rvalues; the suffix must be a string literal.
void Lowerer::lower_timeformat(const CallExpression& call, ir::Builder& b) {
    const auto args = call.arguments();
    // The no-argument form resets to the defaults; slang accepts it but
    // Clotho's runtime default is already seeded at startup, so it is unused
    // in practice. Require the full four-argument form for now.
    // TODO: the zero-argument $timeformat resets to the LRM default values.
    if (args.size() != 4) {
        fail("$timeformat requires four arguments (units, precision, suffix, min_width)");
    }

    // The suffix must be a literal string: dynamic string values have no
    // runtime representation yet, so only a compile-time literal is passed to
    // the runtime as (ptr, len).
    // TODO: a non-literal suffix (a string variable / expression) needs full
    // dynamic string support, which Clotho lacks.
    const Expression* suffix = args[2];
    while (suffix->kind == ExpressionKind::Conversion) {
        suffix = &suffix->as<ConversionExpression>().operand();
    }
    if (suffix->kind != ExpressionKind::StringLiteral) {
        fail("$timeformat suffix must be a string literal");
    }

    const ir::Index units     = lower_expr(*args[0], b);
    const ir::Index precision = lower_expr(*args[1], b);
    const ir::Index min_width = lower_expr(*args[3], b);
    b.timeformat(units, precision, suffix->as<StringLiteral>().getValue(), min_width, loc(call));
}

// $printtimescale [hier]: print "Time scale of (<path>) is <unit> / <prec>"
// for a scope (LRM §21.3.1.2). With no argument the scope is the calling
// module; the timescale and path are compile-time constants, so this folds to
// a literal $display.
void Lowerer::lower_printtimescale(const CallExpression& call, ir::Builder& b) {
    std::string                     path = instance_path();
    std::optional<slang::TimeScale> ts   = scope_->getTimeScale();
    if (!call.arguments().empty()) {
        // The argument names another scope. That scope's timescale is one
        // Scope::getTimeScale() call away and no signal is involved, so the
        // cross-hierarchy reference never reaches the IR. slang's
        // PrintTimeScaleTask binds the argument as a symbol reference and
        // rejects any kind other than Instance / CompilationUnit / Root, so
        // those three are all that can arrive here.
        const auto* as = call.arguments()[0]->as_if<slang::ast::ArbitrarySymbolExpression>();
        if (as == nullptr) {
            fail("$printtimescale argument must name a module instance, $unit or $root");
        }
        const slang::ast::Symbol& sym = *as->symbol;
        switch (sym.kind) {
            case SymbolKind::Instance:
                ts   = sym.as<InstanceSymbol>().body.getTimeScale();
                path = sym.getHierarchicalPath();
                break;
            case SymbolKind::CompilationUnit:
                // getHierarchicalPath() spells this scope "$unit"; the symbol
                // itself is unnamed.
                ts   = sym.as<CompilationUnitSymbol>().timeScale;
                path = sym.getHierarchicalPath();
                break;
            case SymbolKind::Root:
                // $root declares no timescale of its own, so it reports the
                // simulator default. Its path text is spelled out because
                // slang's path formatter escapes the leading '$' of the root
                // symbol's name into "\$root ".
                ts   = scope_->getCompilation().getDefaultTimeScale();
                path = "$root";
                break;
            default:
                fail("$printtimescale argument must name a module instance, $unit or $root");
        }
    }

    // A scope with no timescale reports the simulator default, which for
    // Clotho is the unitless base: one tick is one second (see the tick fold
    // in lower_compilation). iverilog prints the same "1s / 1s" here.
    const std::string scale = ts ? ts->toString() : "1s / 1s";
    const std::string text  = "Time scale of (" + path + ") is " + scale;
    b.display(text, /*newline=*/true, {}, loc(call));
}

// A real count of ticks as the unsigned 64-bit one the wheel takes, times
// `tick_mul` (1 when the real is already in ticks).
//
// What a count that does not fit a 64-bit time means is not defined by the LRM.
// Converting it as an integer would keep its low 64 bits, and a delay of 2**64
// ticks would become 0 and fire at once, so it saturates instead: a huge delay
// never wraps into an early event. The ceiling is INT64_MAX rather than
// UINT64_MAX for headroom, not for the wheel (which saturates its own
// `now + delay`, rt_wheel_schedule_rel): the bound below is a double, and the
// largest count whose product with `tick_mul` fits UINT64_MAX would have to be
// rounded down to keep that product from wrapping, whereas the 2**63 slack above
// INT64_MAX absorbs the rounding. A delay between the two is 2**63 ticks or more,
// which no run reaches.
//
// The test is on the real, against the largest value whose product with
// `tick_mul` stays within the ceiling, so the multiply cannot wrap either. Both
// signs saturate, and so does an infinity; NaN compares false and keeps the
// conversion's own answer, zero. A negative count inside the ceiling is left
// alone: LRM 9.4.1 reads a negative delay as a two's-complement unsigned
// integer, which is what the conversion's cast already gives.
//
// `is_signed` gives the count as a signed 64-bit one instead, for a %t operand
// (scale_to_ticks). There a negative real is a negative time, not a delay read
// as an unsigned integer, so it converts as signed and each sign saturates at
// the ceiling of its own sign, -INT64_MAX and INT64_MAX.
ir::Index Lowerer::saturating_ticks(ir::Index     scaled,
                                    uint64_t      tick_mul,
                                    ir::Builder&  b,
                                    ir::SourceLoc l,
                                    bool          is_signed) {
    using T                    = ir::Inst::Tag;
    const ir::Ref      ty      = make_int_type(64, false);
    constexpr uint64_t ceiling = INT64_MAX;
    const uint64_t     mul     = std::max<uint64_t>(tick_mul, 1);
    const double       bound   = static_cast<double>(ceiling / mul);
    const ir::Ref      real_ty = module_.pool.real_type;

    ir::Index ticks = b.float_cvt(T::FtoI, scaled, ty, l);
    if (mul > 1) {
        ticks = b.binary(T::Mul, ticks, b.constant(module_.pool.put_int(ty, mul), l), l);
    }
    const ir::Index above = b.binary(T::FGe, scaled, b.constant(module_.pool.put_real(real_ty, bound), l), l);
    const ir::Index below = b.binary(T::FLe, scaled, b.constant(module_.pool.put_real(real_ty, -bound), l), l);
    const ir::Index top   = b.constant(module_.pool.put_int(ty, ceiling), l);
    if (!is_signed) {
        return b.select(b.binary(T::Lor, above, below, l), top, ticks, l);
    }
    const ir::Index bottom =
      b.constant(module_.pool.put_int(ty, static_cast<uint64_t>(-static_cast<int64_t>(ceiling))), l);
    return b.select(above, top, b.select(below, bottom, ticks, l), l);
}

// Lower a #delay amount, scaled from the module's time unit to wheel ticks.
// The unit multiplier is a compile-time constant; the amount is widened to 64
// bits first and then multiplied, so a large unit (e.g. seconds against fs
// precision) cannot overflow the amount's declared width. An integer delay times
// the unit is always an exact multiple of the tick (LRM §3.14), so no rounding is
// needed. The result is always a 64-bit unsigned tick count for an integral amount,
// and 0 for one with an unknown bit (LRM 9.4.1, below).
ir::Index Lowerer::scaled_delay(const Expression& amount, ir::Builder& b) {
    const ir::SourceLoc l   = loc(amount);
    const ir::Ref       u64 = make_int_type(64, false);

    // Real (fractional) delay (LRM 3.14 / 22.7): round to this module's time
    // precision, then scale to ticks. The wheel tick is the design-wide GCD of
    // all units and every real-bearing module's precision (design_tick_fs), so
    // unit/precision and precision/tick are both exact integers and the tick
    // grid represents the rounded value exactly. Constant-fold below collapses
    // a literal `#1.5` to a single tick constant; a runtime real delay costs
    // one round (FtoI, half away from zero — LRM 6.12.1) and an integer
    // multiply, and a count beyond what 64-bit time holds saturates
    // (saturating_ticks). time_mul_ == unit/precision * prec_over_tick_, so
    // recover the unit/precision factor by division.
    if (amount.type->isFloating()) {
        // A shortreal widens exactly (LRM 6.12.2). Everything below, the unit
        // multiplier and saturating_ticks' range test, is in `real`: a float
        // operand against those double constants fails the LLVM verifier.
        const ir::Index rv =
          coerce(lower_expr(amount, b), *amount.type, module_.pool.real_type, b, l);
        const uint64_t unit_over_prec = prec_over_tick_ != 0 ? time_mul_ / prec_over_tick_ : 1;
        ir::Index      scaled         = rv;
        if (unit_over_prec > 1) {
            const ir::Index m =
              b.constant(module_.pool.put_real(module_.pool.real_type, static_cast<double>(unit_over_prec)), l);
            scaled = b.binary(ir::Inst::Tag::FMul, rv, m, l);
        }
        return saturating_ticks(scaled, prec_over_tick_, b, l);
    }

    // LRM 9.4.1: a delay that evaluates to a negative value "shall be interpreted
    // as a two's-complement unsigned integer of the same size as a time variable",
    // which is 64 bits. An amount narrower than that therefore extends by its OWN
    // signedness, as in an assignment (coerce, LRM 11.8.3): `int d = -1` is
    // 2**64 - 1, not 2**32 - 1. An unsigned amount zero-extends and a wider one
    // truncates. The backend consumes the count as is, so this is the one place
    // the extension is decided.
    //
    // LRM 9.4.1: "If the delay expression evaluates to an unknown or high-impedance
    // value, it shall be interpreted as zero delay." The clause does not say whether
    // a value with only some bits x or z is unknown. Any such bit makes it so, as in
    // 11.4.4 ("contains an unknown (x) or high-impedance (z) value"), and as iverilog
    // reads it: `#(4'b1x01)` waits 0, not the 9 that the 4-state to 2-state
    // conversion below would keep from its known bits. 9.4.5 and 15.5.1 take their
    // delay from this same delay control. 10.3.3 and 28.16 do not repeat the rule
    // for a continuous-assignment, gate or net delay, but those take the same
    // `delay_value` / `mintypmax_expression` (A.2.2.3, A.6.5), so they follow it
    // (iverilog agrees for the first two and implements no net delay;
    // docs/LRM_NOTES.md, note 39).
    const ir::Index amt   = lower_expr(amount, b);
    ir::Index       ticks = coerce(amt, *amount.type, u64, b, l);
    if (amount.type->isFourState()) {
        ticks =
          b.select(b.unary(ir::Inst::Tag::IsUnknown, amt, l), b.constant(module_.pool.put_int(u64, 0), l), ticks, l);
    }
    if (time_mul_ <= 1) {
        return ticks;
    }
    const ir::Index mul = b.constant(module_.pool.put_int(u64, time_mul_), l);
    // The product saturates at the largest count instead of wrapping, like a real
    // delay's (saturating_ticks): `longint unsigned d = 2**61; #(d)` in a 1ns/1ps
    // module is 2**61 * 1000 ticks, whose low 64 bits are 0, and a negative amount
    // is 2**64 - 1 units. Neither may fire early. A constant amount folds away.
    const ir::Index limit = b.constant(module_.pool.put_int(u64, UINT64_MAX / time_mul_), l);
    const ir::Index huge  = b.binary(ir::Inst::Tag::UGt, ticks, limit, l);
    return b.select(huge, b.constant(module_.pool.put_int(u64, UINT64_MAX), l),
                    b.binary(ir::Inst::Tag::Mul, ticks, mul, l), l);
}

// Lower a string-valued system-task argument (a $fopen filename / mode, a
// $sscanf source). slang types these as the dynamic `string` type, wrapping a
// string literal in a string conversion; strip those conversions to recover
// the packed byte vector (byte 0 = last character) whenever the underlying
// value is packed. A genuine dynamic-string value is lowered to its rt_string
// handle — the backend (spill_ascii_arg / $readmem / $dumpfile) reads its
// bytes in natural order via rt_string_data/len, so both forms are accepted.
ir::Index Lowerer::lower_string_arg(const Expression& expr, ir::Builder& b) {
    const Expression* e = &expr;
    while (e->kind == ExpressionKind::Conversion && e->type->getCanonicalType().isString()) {
        e = &e->as<ConversionExpression>().operand();
    }
    return lower_expr(*e, b);
}

// Collect the non-suppressed conversion characters of a $fscanf / $sscanf
// format literal, in argument order, rejecting unsupported conversions.
// Mirrors the runtime scanner: a leading '*' suppresses (consumes no arg), an
// optional field width follows, then the conversion char. Blocks %f/%e/%g (no
// reals) and %v/%m/%t/%u/%z. The caller pairs each returned char with its
// output argument to check the destination kind.
std::vector<char> Lowerer::scan_conversion_chars(std::string_view fmt) {
    std::vector<char> convs;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            continue;
        }
        ++i;
        if (i >= fmt.size()) {
            break;
        }
        if (fmt[i] == '%') {
            continue;
        }
        bool suppress = false;
        if (fmt[i] == '*') {
            suppress = true;
            ++i;
        }
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
            ++i;
        }
        if (i >= fmt.size()) {
            break;
        }
        switch (fmt[i]) {
            case 'd':
            case 'o':
            case 'b':
            case 'h':
            case 'x':
            case 'c':
            case 's':
                break;
            default:
                // TODO: %f/%e/%g need real support; %v/%m/%t/%u/%z unimplemented.
                fail(std::string("$fscanf / $sscanf conversion %") + fmt[i] + " is not supported");
        }
        if (!suppress) {
            convs.push_back(fmt[i]);
        }
    }
    return convs;
}

// Lower $fscanf / $sscanf (LRM §21.3.4): the source is the first argument (an
// FD for $fscanf, a string for $sscanf), the format is the second (a string
// literal), and the rest are output lvalues, one per conversion.
ir::Index Lowerer::lower_scanf(const CallExpression& call, bool is_sscanf, ir::Builder& b, ir::SourceLoc l) {
    const auto args = call.arguments();
    if (args.size() < 2) {
        fail("$fscanf / $sscanf requires a source and a format string");
    }
    const Expression* fmt_e = args[1];
    while (fmt_e->kind == ExpressionKind::Conversion) {
        fmt_e = &fmt_e->as<ConversionExpression>().operand();
    }
    if (fmt_e->kind != ExpressionKind::StringLiteral) {
        // TODO: a runtime (non-literal) format string needs dynamic string support.
        fail("$fscanf / $sscanf format must be a string literal");
    }
    const std::string_view  fmt      = fmt_e->as<StringLiteral>().getValue();
    const std::vector<char> convs    = scan_conversion_chars(fmt);
    const auto              out_args = args.subspan(2);
    if (out_args.size() != convs.size()) {
        fail("$fscanf / $sscanf has " + std::to_string(convs.size()) + " conversion(s) but " +
             std::to_string(out_args.size()) + " output argument(s)");
    }
    // The source lowers as a value (FD) or a packed string ($sscanf).
    const ir::Index        source = is_sscanf ? lower_string_arg(*args[0], b) : lower_expr(*args[0], b);
    std::vector<ir::Index> outs;
    outs.reserve(out_args.size());
    for (size_t k = 0; k < out_args.size(); ++k) {
        const Expression* a = out_args[k];
        // The scan engine writes every conversion as must/may plane words
        // (rt_scan_arg), and a real slot holds an IEEE-754 double -- the parsed
        // bits would land in its mantissa and the design would read a silently
        // wrong number ($sscanf("12", "%d", r) left r at 0.000000 and still
        // returned 1). The clause's own real conversions (%f/%e/%g) are already
        // refused by scan_conversion_chars, so this closes the integral spelling
        // of the same gap. Same guard, same reason, as $value$plusargs's.
        if (a->type->isFloating()) {
            fail("$fscanf / $sscanf into a real variable is not supported");
        }
        // A `string` object destination is sanctioned for %s alone (LRM
        // 21.3.4.3: "The string format specifier %s (or %S) may be used to
        // read into a variable of integral, unpacked array of byte, or string
        // data types" -- the integer specifiers list integral types only). The
        // scan engine's string store relies on this pairing: it assigns the %s
        // token verbatim and never converts.
        if (a->type->getCanonicalType().isString() && convs[k] != 's') {
            fail(std::string("$fscanf / $sscanf %") + convs[k] +
                 " into a string object is not supported");
        }
        // An unpacked aggregate destination. The integer specifiers "shall not
        // be used with any unpacked aggregate data type" (LRM 21.3.4.3), and
        // the flat plane store the engine does would land a %s token
        // right-justified -- reversed across the elements of the clause's own
        // sanctioned "unpacked array of byte" form ($sscanf("hello", "%s", b)
        // put 'o' in b[0]) -- so either pairing was a silent wrong answer.
        // TODO: %s into an unpacked array of byte needs an element-ordered
        // store (a descriptor variant like the string one).
        const slang::ast::Type& ct = a->type->getCanonicalType();
        if (ct.isUnpackedArray() || ct.isUnpackedStruct() || ct.isUnpackedUnion()) {
            fail("$fscanf / $sscanf into an unpacked aggregate destination is not supported");
        }
        outs.push_back(lower_lvalue(*a, b));
    }
    return b.scanf(source, is_sscanf, fmt, outs, l);
}

// Collect an enum type's members in declaration order (LRM 6.19: the order
// the methods first/last/next/prev/name walk). Every member value is a clean
// compile-time constant of the base type.
std::vector<Lowerer::EnumMember> Lowerer::enum_members(const slang::ast::EnumType& et) {
    std::vector<EnumMember> members;
    for (const slang::ast::EnumValueSymbol& ev : et.values()) {
        members.push_back({ ev.getValue().integer(), ev.name });
    }
    return members;
}

Lowerer::EnumNameTable Lowerer::enum_name_table(const slang::ast::EnumType& et) {
    // Each member value becomes its must-plane bytes ((width+7)/8), so a base
    // wider than 64 bits round-trips; the runtime byte-compares.
    const std::vector<EnumMember> members = enum_members(et);
    const uint32_t                width   = et.bitWidth;
    const uint32_t                vbytes  = (width + 7) / 8;
    const uint32_t                words   = (width + 63) / 64;
    EnumNameTable                 tbl;
    tbl.valbufs.reserve(members.size());
    tbl.rows.reserve(members.size());
    for (const EnumMember& m : members) {
        const slang::SVInt&   v      = m.value;
        const uint64_t*       raw    = v.getRawPtr();
        const bool            unk    = v.hasUnknown();
        const uint32_t        vwords = (static_cast<uint32_t>(v.getBitWidth()) + 63) / 64;
        std::vector<uint64_t> vplane(std::max(words, vwords), 0);
        for (uint32_t i = 0; i < vwords; ++i) {
            // VPI aval (must plane): value bits, with unknown bits cleared.
            vplane[i] = unk ? (raw[i] ^ raw[vwords + i]) : raw[i];
        }
        std::vector<uint8_t> vb(vbytes, 0);
        std::memcpy(vb.data(), vplane.data(), vbytes);
        tbl.valbufs.push_back(std::move(vb));
        tbl.rows.push_back({ std::span<const uint8_t>(tbl.valbufs.back()), m.name });
    }
    return tbl;
}

// Lower an enum method (LRM 6.19.2-6.19.5). The receiver is arguments()[0].
ir::Index Lowerer::lower_enum_method(std::string_view            name,
                                     const slang::ast::EnumType& et,
                                     const CallExpression&       call,
                                     ir::Builder&                b,
                                     ir::SourceLoc               l) {
    const std::vector<EnumMember> members = enum_members(et);
    const ir::Ref                 base_ty = lower_type(et);

    // num(): the member count (LRM 6.19.4), an int, independent of the value.
    if (name == "num") {
        return b.constant(module_.pool.put_int(lower_type(*call.type), members.size()), l);
    }
    // first()/last(): the first/last member value (LRM 6.19.5), constant.
    if (name == "first") {
        return b.constant(svint_to_ref(members.front().value, base_ty), l);
    }
    if (name == "last") {
        return b.constant(svint_to_ref(members.back().value, base_ty), l);
    }

    // name() (LRM 6.19.2): the source name of the matching member, or "" if
    // the value is not a member. Delegated to a runtime scan of a per-type
    // member table (a string result rules out a frontend select).
    if (name == "name") {
        // `.name()` yields "" for a non-member value (LRM 6.19.2), so no
        // format fallback — that is `%p`'s rule, not this method's.
        const EnumNameTable tbl = enum_name_table(et);
        return b.enum_name(lower_expr(*call.arguments()[0], b),
                           tbl.rows,
                           /*fmt_fallback=*/false,
                           call.arguments()[0]->type->isSigned(),
                           l);
    }

    // next(N)/prev(N) (LRM 6.19.5): the Nth member after/before the current
    // one in declaration order, wrapping. N is an optional constant (default
    // 1); a non-member receiver yields X (matches iverilog; LRM leaves it
    // undefined). Lowered as a branchless packed select chain over the member
    // set — an integer select, so any base width works.
    if (name == "next" || name == "prev") {
        int64_t step = 1;
        if (call.arguments().size() >= 2) {
            const slang::ConstantValue cv = fold_const(*call.arguments()[1], Fold::Runtime);
            if (!cv.isInteger()) {
                // TODO: a runtime (non-constant) step needs value->index lookup
                // plus modular index arithmetic; rare, so reject for now.
                fail("enum next/prev with a non-constant step is not supported");
            }
            step = cv.integer().as<int64_t>().value_or(0);
        }

        const int64_t num   = static_cast<int64_t>(members.size());
        const int64_t delta = (name == "next") ? step : -step;
        const auto    wrap  = [num](int64_t k) { return ((k % num) + num) % num; };

        const ir::Index recv = lower_expr(*call.arguments()[0], b);
        // Default (no member matched): X of the base type (2-state base cannot
        // hold X, so it degrades to 0 via svint_to_ref's must plane).
        ir::Index res = b.constant(svint_to_ref(et.isFourState ? slang::SVInt::createFillX(et.bitWidth, et.isSigned)
                                                               : slang::SVInt(et.bitWidth, 0, et.isSigned),
                                                base_ty),
                                   l);
        for (size_t i = members.size(); i-- > 0;) {
            const ir::Index memberc = b.constant(svint_to_ref(members[i].value, base_ty), l);
            const ir::Index targetc = b.constant(
              svint_to_ref(members[static_cast<size_t>(wrap(static_cast<int64_t>(i) + delta))].value, base_ty), l);
            const ir::Index eq = b.binary(ir::Inst::Tag::CaseEq, recv, memberc, l);
            res                = b.select(eq, targetc, res, l);
        }
        return res;
    }

    // LRM 6.19.2-6.19.5 defines six enum methods -- first, last, next, prev,
    // name, num -- and all six are handled above. slang binds no other name on
    // an enum receiver, so this is an invariant, not a feature gap.
    fail("internal: unhandled enum method '" + std::string(name) + "'");
}

// Format a type as its LRM 20.6.1 $typename string.
//
// slang implements $typename in full (`TypenameFunction`, QueryFuncs.cpp) over its
// own `TypePrinter`, so Clotho asks for that string rather than deriving a second
// one. This used to be hand-rolled for the kinds Clotho lowers, with everything
// else failing loudly on the grounds that Clotho could not vouch for a name for a
// type it did not lower; that premise expired once classes, enums, structs, unions
// and the containers landed, and the formatter's output was byte-identical to
// slang's for every kind it did cover (including a resolved typedef and the `$`
// placeholder of an anonymous unpacked dimension).
//
// The options are pinned rather than defaulted because `TypePrinter` is also the
// diagnostics printer: `printAKA` appends "aka" glosses, and `FriendlyName` spells
// an anonymous type structurally where 20.6.1 wants the implementation-generated
// unique name -- and uniqueness is load-bearing here, since $typename strings are
// specified to compare equal only for equivalent types.
std::string Lowerer::format_typename(const slang::ast::Type& type) {
    slang::ast::TypePrinter printer;
    printer.options.printAKA = false;
    printer.options.anonymousTypeStyle = slang::ast::TypePrintingOptions::SystemName;
    printer.options.elideScopeNames = false;
    printer.options.skipScopedTypeNames = false;
    printer.append(type);
    return printer.toString();
}

// Classify a $countbits control-bit argument (LRM 20.9): a constant 1-bit
// value naming which 4-state value that control tallies. A non-constant
// control is rejected (it would need a runtime class-mask select).
uint32_t Lowerer::count_control_class(const Expression& ctrl) {
    const slang::ConstantValue cv = fold_const(ctrl, Fold::Runtime);
    if (!cv.isInteger()) {
        fail("$countbits control bit must be a constant 0/1/x/z");
    }
    const slang::logic_t bit = cv.integer()[0];
    if (bit.value == slang::logic_t::X_VALUE) {
        return ir::CountX;
    }
    if (bit.value == slang::logic_t::Z_VALUE) {
        return ir::CountZ;
    }
    return bit.value != 0 ? ir::CountOne : ir::CountZero;
}

// Runtime lowering for the utility bit-query functions (LRM 20.9; $bits on a
// dynamic string per 20.6.2) when the operand is not constant. A constant
// operand folds in slang and never reaches here.
ir::Index Lowerer::lower_bit_query(std::string_view      name,
                                   const CallExpression& call,
                                   ir::Builder&          b,
                                   const ir::SourceLoc   l) {
    const auto    args = call.arguments();
    const ir::Ref i32  = make_int_type(32, false);

    // $bits: the only non-constant operand Clotho stores is a dynamic
    // string, whose bit count is 8 per current character (LRM 6.16, 20.6.2).
    // A fixed-size operand would have const-folded above; any other dynamic
    // type (queue / dynamic array / class handle) is unsupported storage.
    if (name == "$bits") {
        if (args.size() != 1 || !is_string_expr(*args[0])) {
            fail("$bits of a non-constant, non-string operand is not supported");
        }
        const ir::Index len   = b.unary(ir::Inst::Tag::StrLen, lower_expr(*args[0], b), l);
        const ir::Index eight = b.constant(module_.pool.put_int(i32, 8), l);
        return b.convert(b.binary(ir::Inst::Tag::Mul, len, eight, l), lower_type(*call.type), call.type->isSigned(), l);
    }

    // The 20.9 counting family. The first argument is the counted value;
    // $countbits additionally carries constant control-bit arguments naming
    // which 4-state values to tally.
    const ir::Index val = lower_expr(*args[0], b);

    // $onehot / $onehot0 / $isunknown carry their qualifier in the tag.
    if (name == "$onehot") {
        return b.convert(b.unary(ir::Inst::Tag::OneHot, val, l), lower_type(*call.type), call.type->isSigned(), l);
    }
    if (name == "$onehot0") {
        return b.convert(b.unary(ir::Inst::Tag::OneHot0, val, l), lower_type(*call.type), call.type->isSigned(), l);
    }
    if (name == "$isunknown") {
        return b.convert(b.unary(ir::Inst::Tag::IsUnknown, val, l), lower_type(*call.type), call.type->isSigned(), l);
    }

    uint32_t mask = ir::CountOne; // $countones counts definite-1 bits
    if (name == "$countbits") {
        mask = 0;
        for (size_t k = 1; k < args.size(); ++k) {
            mask |= count_control_class(*args[k]);
        }
    }
    return b.convert(b.count_bits(val, mask, l), lower_type(*call.type), call.type->isSigned(), l);
}

// A failed $cast called as a task is a run-time error (LRM 6.24.2), reported
// and terminated through the same severity + finish pair `$fatal` uses. The
// function form never reports: it answers 0.
void Lowerer::emit_cast_fatal(const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
    const std::string banner = banner_at(call.sourceRange.start());
    b.severity(static_cast<uint32_t>(ir::Severity::Fatal),
               severity_banner(call.sourceRange.start()),
               "$cast: the source is not assignment compatible with the destination",
               {},
               l);
    b.finish(banner + "$cast", 1, l);
}

// The same report, taken only when the run-time test said the cast failed.
void Lowerer::emit_cast_fatal_unless(ir::Index valid, const CallExpression& call, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index failed = b.unary(ir::Inst::Tag::Lnot, valid, l);
    b.open_region();
    emit_cast_fatal(call, b, l);
    const ir::ExtraSlice then_region = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_region = b.close_region();
    b.cond(failed, then_region, else_region, l, /*unlikely=*/true);
}

// $cast(dest, source) (LRM 6.24.2 / 8.16): a dynamic, run-time-checked cast.
// Assigns and answers 1 on success; on failure the destination keeps its value
// and the function form answers 0 while the task form (`task_form`) reports a
// run-time error.
//
// The destination's type picks the test. A numeric destination always succeeds.
// An enum destination succeeds only for a legal member value. A class
// destination is the dynamic down-cast, whose three success cases LRM 8.16
// spells out; two of them are decided here, at compile time.
ir::Index Lowerer::lower_cast(const CallExpression& call, ir::Builder& b, bool task_form) {
    const ir::SourceLoc l = loc(call);
    if (call.arguments().size() != 2) {
        fail("$cast expects exactly two arguments");
    }
    const Expression&       dest_expr = *call.arguments()[0];
    const Expression&       src_expr  = *call.arguments()[1];
    const slang::ast::Type& dt        = dest_expr.type->getCanonicalType();
    const slang::ast::Type& st        = src_expr.type->getCanonicalType();
    if (dt.isCHandle()) {
        // TODO: $cast to a chandle (LRM 6.14). A chandle carries no type
        // identity at all -- it is an opaque pointer from foreign code -- so
        // there is nothing to check it against.
        fail("$cast of a chandle is not supported");
    }
    const ir::Ref   int_ty  = lower_type(*call.type);
    const ir::Index int1    = b.constant(svint_to_ref(slang::SVInt(32, 1, true), int_ty), l);
    const ir::Index int0    = b.constant(svint_to_ref(slang::SVInt(32, 0, true), int_ty), l);
    const ir::Ref   dest_ty = lower_type(*dest_expr.type);
    const ir::Index dst     = lower_lvalue(dest_expr, b);
    // True for an interface class type (LRM 8.26). `isClass()` holds for one too,
    // so every place that reasons about class identity has to distinguish them.
    const auto is_iface_class = [](const slang::ast::Type& t) {
        const slang::ast::Type& c = t.getCanonicalType();
        return c.isClass() && c.as<slang::ast::ClassType>().isInterface;
    };
    if (dt.isClass()) {
        // The source is evaluated whatever the verdict: it is an expression and
        // may have side effects.
        const ir::Index src = lower_expr(src_expr, b);
        // Case 1 (the destination is the source's own class or an ancestor of
        // it) and case 3 (the source is the literal `null`) are static: the cast
        // cannot fail, so it is a plain assignment. isAssignmentCompatible
        // answers both -- slang types the null literal as `null`, which is
        // assignment compatible with every class handle.
        if (dt.isAssignmentCompatible(st)) {
            b.binary(ir::Inst::Tag::Store, dst, coerce(src, st, dest_ty, b, l), l);
            return int1;
        }
        // Which run-time test applies depends on the DESTINATION: an interface
        // class asks whether the object's class implements it (LRM 8.26.5), any
        // other class asks the ancestor display (LRM 8.16).
        const bool dest_iface = is_iface_class(dt);
        const bool src_iface  = is_iface_class(st);
        // Neither direction relates the two types, so no object can satisfy the
        // cast: it always fails, and slang has already warned (DynamicCastConst).
        // Single inheritance is what makes this conclusive -- with two bases a
        // common descendant could satisfy both.
        //
        // An interface class on either side breaks exactly that: a class the
        // destination descends from can still implement the source interface, and
        // two unrelated interfaces can share an implementer -- which is LRM
        // 8.26.5's own `$cast(get_ref, put_ref)` example. So the fold is
        // restricted to a pair of ordinary classes, and everything else takes the
        // run-time test (docs/design/interface-classes.md §6.2).
        if (!st.isClass() || (!dest_iface && !src_iface && !st.isAssignmentCompatible(dt))) {
            if (task_form) {
                emit_cast_fatal(call, b, l);
            }
            return int0;
        }
        // Case 2: only the object's own class decides. Assign on success, keep
        // the current value on failure.
        const ir::Index valid =
          dest_iface ? b.class_implements(src, dest_ty, l) : b.class_is_a(src, dest_ty, l);
        const ir::Index cur = b.unary(ir::Inst::Tag::Load, dst, l);
        b.binary(ir::Inst::Tag::Store, dst, b.select(valid, src, cur, l), l);
        if (task_form) {
            emit_cast_fatal_unless(valid, call, b, l);
        }
        return b.select(valid, int1, int0, l);
    }
    const ir::Index conv = coerce(lower_expr(src_expr, b), *src_expr.type, dest_ty, b, l);
    const ir::Ref   bit1 = make_int_type(1, false);
    if (const auto* et = dt.as_if<slang::ast::EnumType>()) {
        // Enum destination (LRM 6.19.3): succeed only if the value is a legal
        // member. valid = OR over members of (conv === member).
        ir::Index       valid = b.constant(module_.pool.put_int(bit1, 0), l);
        const ir::Index one   = b.constant(module_.pool.put_int(bit1, 1), l);
        for (const EnumMember& m : enum_members(*et)) {
            const ir::Index mc = b.constant(svint_to_ref(m.value, dest_ty), l);
            const ir::Index eq = b.binary(ir::Inst::Tag::CaseEq, conv, mc, l);
            valid              = b.select(eq, one, valid, l);
        }
        // Assign the value on success; keep the current dest on failure.
        const ir::Index cur = b.unary(ir::Inst::Tag::Load, dst, l);
        b.binary(ir::Inst::Tag::Store, dst, b.select(valid, conv, cur, l), l);
        if (task_form) {
            emit_cast_fatal_unless(valid, call, b, l);
        }
        return b.select(valid, int1, int0, l);
    }
    // Numeric destination: a value cast always succeeds (LRM 6.24.2).
    b.binary(ir::Inst::Tag::Store, dst, conv, l);
    return int1;
}

// Lower a system function call used as an rvalue (e.g. `$time`). User function
// calls are not yet supported.
ir::Index Lowerer::lower_sysfunc(const CallExpression& call, ir::Builder& b) {
    if (!call.isSystemCall()) {
        fail("user function calls are not supported");
    }
    const std::string_view name = call.getSubroutineName();
    const ir::SourceLoc    l    = loc(call);

    // Sampled value functions (LRM 16.9.3). Dispatched ahead of everything
    // else because they are the one family that must NOT lower their operand
    // as an ordinary expression: each reads a shadow or a history ring.
    if (is_sampled_value_fn(name)) {
        return lower_sampled_value(name, call, b, l);
    }

    // Randomization (LRM 18). slang rewrites `obj.randomize()`,
    // `std::randomize(...)` and the per-property `rand_mode` / `constraint_mode`
    // into system calls, so all of them arrive here, receiver first.
    if (name == "randomize") {
        return lower_randomize_call(call, b, l);
    }
    if (name == "rand_mode") {
        return lower_rand_mode_call(call, b, l);
    }
    if (name == "constraint_mode") {
        // TODO: constraint_mode (LRM 18.9) — step 2, the solver.
        fail("constraint_mode is not supported");
    }

    // String built-in methods (s.len(), s.substr(i,j), ...) arrive as system
    // calls whose receiver is arguments()[0]; their names carry no '$'.
    if (!name.empty() && name[0] != '$' && !call.arguments().empty() && is_string_expr(*call.arguments()[0])) {
        return lower_string_method(name, call, b);
    }

    // `item.index` inside a `with` clause reaches here as a builtin method on
    // the iterator variable, whose type may itself be a container -- so it is
    // resolved ahead of the container dispatch below.
    if (!name.empty() && name[0] != '$' && !call.arguments().empty() && name == "index") {
        if (const std::optional<ir::Index> v = lower_iterator_index(call, b, l)) {
            return *v;
        }
    }

    // Dynamic-array (LRM 7.5), queue (7.10.2) and associative-array (7.9)
    // built-in methods, plus the 7.12 manipulation methods on any of them.
    if (const std::optional<ir::Index> v = lower_container_method(call, b, l)) {
        return *v;
    }

    // Enum methods (LRM 6.19.2-6.19.5) arrive the same way, with an enum-typed
    // receiver as arguments()[0].
    if (!name.empty() && name[0] != '$' && !call.arguments().empty()) {
        const slang::ast::Type& rct = call.arguments()[0]->type->getCanonicalType();
        if (const auto* et = rct.as_if<slang::ast::EnumType>()) {
            return lower_enum_method(name, *et, call, b, l);
        }
    }

    // `e.triggered` (LRM 15.5.3): 1-bit "triggered this time step" property.
    if (const Expression* ev = triggered_event(call)) {
        return b.event_triggered(event_handle(*ev, b), l);
    }

    // `<sequence>.triggered` (see sequence_method): the same spelling on a
    // sequence instead of an event, and a different feature. Refused here by
    // name, because "unsupported system function 'triggered'" below would send
    // the reader looking for a missing built-in rather than a missing feature.
    // `.matched` cannot reach this point -- slang rejects it outside an
    // assertion expression -- but it is named the same way where it can.
    // TODO: sequence methods (concurrent-assertions.md 9.3).
    if (const std::string_view method = sequence_method(call); !method.empty()) {
        fail("`." + std::string(method) + "` on a named sequence is not supported");
    }

    // $time: 64-bit, scaled to the time unit of the scope whose code does the
    // read (LRM §20.3.1, narrowed by §3.14.2.3 for a subroutine body -- see
    // function_id, which sets time_mul_ from the DECLARING scope). $stime: the
    // same value truncated to the low 32 bits (LRM §20.3.2). The backend
    // divides the wheel tick by time_mul_ (1 when unitless) to scale the
    // finest-precision clock down to that unit; $stime resizes the result to
    // its 32-bit return type. Legal in a plain function too: the wheel is a
    // module global (clotho.wheel), not a body parameter, so a function reads
    // the clock exactly as a task does.
    if (name == "$time") {
        return b.sim_time(time_mul_, l);
    }
    if (name == "$stime") {
        return b.convert(b.sim_time(time_mul_, l), lower_type(*call.type), call.type->isSigned(), l);
    }

    // The seed operand of a $dist_* (an output arg, slang §hasOutputArgs) is
    // wrapped as Assignment{left = seed, right = EmptyArgument} — the same
    // placeholder shape slang uses for output ports. $random's seed is a
    // plain value arg. Reach the underlying seed lvalue in both cases.
    const auto seed_lvalue_expr = [](const Expression& a) -> const Expression& {
        const Expression& inner = a.kind == ExpressionKind::Assignment ? a.as<AssignmentExpression>().left() : a;
        return unwrap_conversion(inner);
    };

    // A $dist_* / seeded-$random result is a packed {seed:32, draw:32} i64.
    // Store the updated seed back to the inout `seed` lvalue and yield the
    // signed draw. Shared by $random(seed) and every $dist_* (LRM §20.15).
    const auto seed_writeback = [&](const Expression& seed_arg, ir::Index packed) -> ir::Index {
        const ir::Ref   i32s    = make_int_type(32, false);
        const ir::Index sh32    = b.constant(module_.pool.put_int(make_int_type(64, false), 32u), l);
        const ir::Index seed_lv = lower_lvalue(seed_lvalue_expr(seed_arg), b);
        const ir::Index seed    = b.convert(b.binary(ir::Inst::Tag::Srl, packed, sh32, l), i32s, true, l);
        b.binary(ir::Inst::Tag::Store, seed_lv, in_lvalue_domain(seed_lv, seed, b, l), l);
        return b.convert(packed, i32s, true, l);
    };

    // $random (LRM §20.15.1): a 32-bit signed PRNG draw. The plain form
    // returns the next value of the simulator's global random stream; the
    // seeded form `$random(seed)` is rtl_dist_uniform(seed, INT32_MIN,
    // INT32_MAX), lowered through the shared DistCall path so its inout-seed
    // writeback matches $dist_*.
    if (name == "$random") {
        if (call.arguments().empty()) {
            return b.random(l);
        }
        const Expression&              seed_arg = *call.arguments()[0];
        const ir::Ref                  i32s     = make_int_type(32, false);
        const ir::Index                lo       = b.constant(module_.pool.put_int(i32s, 0x80000000ull), l);
        const ir::Index                hi       = b.constant(module_.pool.put_int(i32s, 0x7fffffffull), l);
        const std::array<ir::Index, 3> args{ lower_expr(seed_lvalue_expr(seed_arg), b), lo, hi };
        return seed_writeback(seed_arg, b.dist_call(ir::DistOp::Uniform, args, l));
    }

    // $dist_* (LRM §20.15.2): the IEEE reference distributions, each with an
    // inout `seed` first argument and 1-2 distribution parameters. The
    // rtl_dist_* algorithms are mandated verbatim, so the stream is
    // byte-identical to any conforming tool for a given seed.
    struct DistSpec {
        const char* nm;
        ir::DistOp  op;
        size_t      nparam;
    };
    static constexpr DistSpec kDists[] = {
        { "$dist_uniform", ir::DistOp::Uniform, 2 },         { "$dist_normal", ir::DistOp::Normal, 2 },
        { "$dist_exponential", ir::DistOp::Exponential, 1 }, { "$dist_poisson", ir::DistOp::Poisson, 1 },
        { "$dist_chi_square", ir::DistOp::ChiSquare, 1 },    { "$dist_t", ir::DistOp::StudentT, 1 },
        { "$dist_erlang", ir::DistOp::Erlang, 2 },
    };
    for (const DistSpec& d : kDists) {
        if (name != d.nm) {
            continue;
        }
        const auto argv = call.arguments();
        if (argv.size() != d.nparam + 1) {
            fail(std::string(d.nm) + " requires a seed and " + std::to_string(d.nparam) + " parameter(s)");
        }
        std::vector<ir::Index> args;
        args.reserve(argv.size());
        // The seed (arg 0) is read as a bare integer lvalue; the distribution
        // parameters keep any value conversion slang inserted.
        args.push_back(lower_expr(seed_lvalue_expr(*argv[0]), b));
        for (size_t k = 1; k < argv.size(); ++k) {
            args.push_back(lower_expr(*argv[k], b));
        }
        return seed_writeback(*argv[0], b.dist_call(d.op, args, l));
    }

    // $urandom / $urandom_range (LRM §18.13). The RNG is per THREAD, not per
    // body: a function or task runs in its caller's thread, so a draw inside
    // one continues the calling process's stream. The backend threads that
    // state through the call ABI (declare_function's `rng` parameter).
    if (name == "$urandom") {
        if (call.arguments().empty()) {
            return b.urandom(l);
        }
        return b.urandom_seeded(lower_expr(*call.arguments()[0], b), l);
    }
    if (name == "$urandom_range") {
        const auto argv = call.arguments();
        if (argv.empty() || argv.size() > 2) {
            fail("$urandom_range requires one or two arguments (maxval [, minval])");
        }
        const ir::Index maxv = lower_expr(*argv[0], b);
        const ir::Index minv =
          argv.size() == 2 ? lower_expr(*argv[1], b) : b.constant(module_.pool.put_int(make_int_type(32, false), 0), l);
        return b.urandom_range(maxv, minv, l);
    }

    // $signed / $unsigned (LRM §20.10): reinterpret the self-determined
    // argument with the opposite signedness, preserving its size and value
    // bits. slang gives `call.type` the argument's width with the new sign;
    // a same-width resize leaves the bits untouched but tags the result type
    // so downstream widening sign- vs zero-extends correctly.
    if (name == "$signed" || name == "$unsigned") {
        if (call.arguments().size() != 1) {
            fail("$signed / $unsigned expects exactly one argument");
        }
        return b.convert(lower_expr(*call.arguments()[0], b), lower_type(*call.type), call.type->isSigned(), l);
    }

    // $clog2 (LRM §20.8.1): ceil(log2(arg)) with the argument treated as
    // unsigned and $clog2(0) == 0; the result is a 32-bit integer. slang's
    // constant evaluator implements clog2, so a constant argument (the
    // common `$clog2(PARAM)` / literal usage) folds to a pooled literal here.
    if (name == "$clog2") {
        if (call.arguments().size() != 1) {
            fail("$clog2 expects exactly one argument");
        }
        const slang::ConstantValue cv = fold_const(call, Fold::Runtime);
        if (cv.isInteger()) {
            return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
        }
        // A non-constant argument lowers to a runtime count-leading-zeros
        // (Clog2). The argument is self-determined and treated as unsigned
        // (LRM §20.8.1); its own width bounds the result.
        return b.unary(ir::Inst::Tag::Clog2, lower_expr(*call.arguments()[0], b), l);
    }

    // Utility query / bit-counting system functions (LRM 20.6, 20.9). Each
    // is a compile-time constant for a fixed-size operand: slang's constant
    // evaluator folds $bits / $countones / $countbits / $onehot / $onehot0 /
    // $isunknown to a literal (e.g. $bits queries the operand's type, never
    // its value), so the common constant-arg usage costs nothing at
    // runtime. Only a non-constant (runtime signal / dynamic string)
    // argument reaches the IR lowering.
    if (name == "$bits" || name == "$countones" || name == "$countbits" || name == "$onehot" ||
        name == "$onehot0" || name == "$isunknown") {
        // $bits is "determined without actual evaluation of the expression it
        // encloses" (LRM 20.6.2), so a call inside it never runs and still folds.
        // The others read the operand's value, and a call in it has to run.
        const slang::ConstantValue cv = fold_const(call, name == "$bits" ? Fold::Required : Fold::Runtime);
        if (cv.isInteger()) {
            return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
        }
        return lower_bit_query(name, call, b, l);
    }

    // An array query inspects its operand for the type only, but its dimension
    // argument (LRM 20.7) is a value, so only a call in that one has to run.
    const Fold array_query_fold =
      call.arguments().size() > 1 && has_user_call(*call.arguments()[1]) ? Fold::Runtime : Fold::Required;

    // $size (LRM 20.7): const-folds in slang for a fixed-size array; a runtime
    // queue / dynamic array cannot fold and reaches here — it is exactly the
    // collection's .size() (the sole/first dimension is 0..len-1).
    if (name == "$size" && !call.arguments().empty()) {
        const slang::ConstantValue cv = fold_const(call, array_query_fold);
        if (cv.isInteger()) {
            return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
        }
        const Expression& arr = *call.arguments()[0];
        if (is_dyn_array(arr)) {
            return b.convert(b.dyn_array_size(lower_lvalue(arr, b), lower_type(*arr.type), l),
                             lower_type(*call.type),
                             call.type->isSigned(),
                             l);
        }
        if (is_queue(arr)) {
            return b.convert(b.queue_size(lower_lvalue(arr, b), lower_type(*arr.type), l),
                             lower_type(*call.type),
                             call.type->isSigned(),
                             l);
        }
        if (is_assoc(arr)) {
            return b.convert(b.assoc_num(lower_lvalue(arr, b), lower_type(*arr.type), l),
                             lower_type(*call.type),
                             call.type->isSigned(),
                             l);
        }
        // TODO: $size(coll, dim) on a multidimensional dynamic collection.
        fail("$size of this operand is not supported");
    }

    // Remaining array query functions (LRM 20.7): $dimensions /
    // $unpacked_dimensions / $left / $right / $low / $high / $increment.
    //
    // $dimensions / $unpacked_dimensions are pure TYPE walks (slang's
    // ArrayDimensionFunction::eval never reads the operand's value), so they
    // constant-fold below regardless of whether the operand is dynamically
    // sized: confirmed against slang's QueryFuncs.cpp and empirically (a
    // dynamic array / queue / associative array operand still folds).
    //
    // The other five need the operand's runtime state per slang's own eval
    // (ArrayQueryFunction::getDim always evaluates the operand's value, even
    // though most of the individual results below do not actually depend on
    // it), so a non-constant fold reaches here. Per 20.7, for a queue or
    // dynamic array dimension: $left = $low = 0 and $increment = -1
    // unconditionally (never a runtime $left-vs-$right comparison — the empty
    // case is not a special case, it falls out of the $right formula below);
    // only $right / $high (= size - 1) need the runtime size. Per 20.7's
    // associative-array paragraph (integral-key only; a non-integral key is
    // already refused by slang's checkArguments with QueryOnAssociativeWildcard
    // before Clotho ever sees the call): $left = 0 and $increment = -1
    // unconditionally, $right is the index type's maximum representable value
    // (a type constant, not a "currently allocated" query), and only $low /
    // $high need a runtime key walk (lowest / highest currently allocated key,
    // 'x if the map is empty).
    if (name == "$dimensions" || name == "$unpacked_dimensions" || name == "$left" || name == "$right" ||
        name == "$low" || name == "$high" || name == "$increment") {
        const slang::ConstantValue cv = fold_const(call, array_query_fold);
        if (cv.isInteger()) {
            return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
        }

        // Reachable only for $left/$right/$low/$high/$increment on a
        // dynamically-sized operand. LRM 20.7.1 makes a non-first dimension
        // argument into a dynamically-sized dimension a compile error (slang
        // raises it before Clotho runs), and a fixed inner dimension folds
        // above regardless of an outer dynamic one -- so the only dimension
        // shape here is "absent, or the constant 1", selecting the operand's
        // own dynamic (or associative) dimension.
        if (call.arguments().size() > 1) {
            const slang::ConstantValue   dim_cv = fold_const(*call.arguments()[1], Fold::Runtime);
            const std::optional<int32_t> dim_ix =
              dim_cv.isInteger() ? dim_cv.integer().as<int32_t>() : std::nullopt;
            if (dim_ix != 1) {
                // TODO: a non-constant dimension argument on a dynamically-sized
                // operand (LRM 20.7.1). A constant dimension other than 1 here
                // would already have folded above (a fixed inner dimension) or
                // been a compile error (a nested dynamic dimension), so only a
                // genuinely runtime dimension index reaches this refusal.
                fail("array query function with a non-constant dimension argument on a "
                     "dynamically-sized operand is not supported");
            }
        }

        const Expression& arr       = *call.arguments()[0];
        const ir::Ref     result_ty = lower_type(*call.type);
        if (is_dyn_array(arr) || is_queue(arr)) {
            if (name == "$left" || name == "$low") {
                return b.constant(svint_to_ref(slang::SVInt(32, 0, true), result_ty), l);
            }
            if (name == "$increment") {
                return b.constant(svint_to_ref(slang::SVInt(32, uint64_t(-1), true), result_ty), l);
            }
            // $right / $high: size - 1, computed in the signed RESULT type
            // (never the unsigned size type), so an empty collection's 0 - 1
            // lands on -1 per 20.7 rather than an unsigned wraparound.
            const bool      is_da  = is_dyn_array(arr);
            const ir::Ref   arr_ty = lower_type(*arr.type);
            const ir::Index arr_lv = lower_lvalue(arr, b);
            const ir::Index size = is_da ? b.dyn_array_size(arr_lv, arr_ty, l) : b.queue_size(arr_lv, arr_ty, l);
            const ir::Index one   = b.constant(svint_to_ref(slang::SVInt(32, 1, true), result_ty), l);
            return b.binary(ir::Inst::Tag::Sub, b.convert(size, result_ty, call.type->isSigned(), l), one, l);
        }
        if (is_assoc(arr)) {
            // A non-integral key would already have failed the semantic check
            // above (QueryOnAssociativeWildcard) for this no-dimension-argument
            // shape, so the key here is always integral (LRM 20.7's "restricted
            // to index types with integral values").
            if (name == "$left") {
                return b.constant(svint_to_ref(slang::SVInt(32, 0, true), result_ty), l);
            }
            if (name == "$increment") {
                return b.constant(svint_to_ref(slang::SVInt(32, uint64_t(-1), true), result_ty), l);
            }
            if (name == "$right") {
                // The index type's maximum representable BIT PATTERN (all bits
                // set), matching slang's RightFunction::eval exactly -- for a
                // SIGNED key type this reads back as -1, not the largest
                // positive value. That is 20.7's "highest possible index value"
                // as slang's own reference eval defines it, and Clotho's
                // const-fold path above already returns that same value for any
                // case that folds, so the runtime path here has to match it.
                slang::SVInt max_val(call.type->getBitWidth(), 0, call.type->isSigned());
                max_val.setAllOnes();
                return b.constant(svint_to_ref(max_val, result_ty), l);
            }
            // $low / $high: the lowest / highest currently ALLOCATED key, or
            // 'x if the map is empty (LRM 20.7) -- the same assoc_traverse
            // First/Last primitive a.first(k)/a.last(k) use (LRM 7.9.4), into a
            // compiler temp, muxed against 'x when the traversal finds nothing.
            const ir::Ref    aty  = lower_type(*arr.type);
            const ir::Index  a_lv = lower_lvalue(arr, b);
            const TempCell   key  = make_temp_cell(result_ty, "arrq_key");
            const ir::Ref    bit1 = make_int_type(1, false);
            using Step           = ir::Builder::AssocStep;
            const ir::Index found = b.convert(
              b.assoc_traverse(a_lv, key.lv(b, l), name == "$low" ? Step::First : Step::Last, aty, l), bit1, false, l);
            const ir::Layout lay = module_.pool.layout(result_ty);
            const uint32_t   w   = lay.width == 0 ? 1 : lay.width;
            const ir::Index  x_const =
              lay.domain == ir::Domain::Four
                ? b.constant(svint_to_ref(slang::SVInt::createFillX(w, call.type->isSigned()), result_ty), l)
                : b.constant(module_.pool.put_int(result_ty, 0), l);
            const ir::Index key_val = b.unary(ir::Inst::Tag::Load, key.lv(b, l), l);
            return b.select(found, key_val, x_const, l);
        }

        // TODO: a multidimensional dynamic collection (matches the $size arm's
        // boundary above) and a runtime `string` operand.
        fail("array query function on a dynamically-sized operand is not supported");
    }

    // $isunbounded (LRM 20.6.1): 1 if the operand is the unbounded literal
    // `$` or a parameter whose value is `$`, else 0. slang leaves the
    // argument unevaluated and answers from its type / the referenced
    // ParameterSymbol's value, so the call always folds to a 1-bit constant.
    // The sole non-integer result is a hierarchical operand, which slang
    // refuses to constant-evaluate.
    if (name == "$isunbounded") {
        const slang::ConstantValue cv = fold_const(call, Fold::Required);
        if (cv.isInteger()) {
            return b.constant(svint_to_ref(cv.integer(), lower_type(*call.type)), l);
        }
        // TODO: $isunbounded of a hierarchical reference (LRM 20.6.1). Needs a
        // constant-evaluation context that resolves across instances.
        fail("$isunbounded of a hierarchical reference is not supported");
    }

    // $typename (LRM 20.6.1): the operand's resolved type name as a string
    // constant, from slang's own type printer (format_typename). The argument's
    // self-determined TYPE is named; the argument is resolved during elaboration
    // but never evaluated, so this folds for every type kind -- which is why the
    // same query already worked in a constant context, where slang's own
    // $typename fold answers before this path is reached at all.
    if (name == "$typename") {
        if (call.arguments().size() != 1) {
            fail("$typename expects exactly one argument");
        }
        return b.constant(string_value_ref(format_typename(*call.arguments()[0]->type)), l);
    }

    if (name == "$cast") {
        return lower_cast(call, b, /*task_form=*/false);
    }

    // $fopen (LRM §21.3): one argument opens a multichannel descriptor (MCD)
    // for writing; two arguments open a single file descriptor (FD) with the
    // given C stdio mode string. The name / mode lower to packed-ASCII vectors
    // the backend spills to byte buffers, like $readmem's filename.
    if (name == "$fopen") {
        const auto args = call.arguments();
        if (args.empty() || args.size() > 2) {
            fail("$fopen expects one or two arguments");
        }
        const ir::Index fname = lower_string_arg(*args[0], b);
        if (args.size() == 2) {
            return b.fopen_fd(fname, lower_string_arg(*args[1], b), l);
        }
        return b.fopen_mcd(fname, l);
    }

    // $system (LRM §20.17.1) in expression position: return its i32 result.
    if (name == "$system") {
        return lower_system_call(call, b);
    }

    // $fgetc / $ungetc / $feof / $ftell / $fseek / $rewind (LRM §21.3): file
    // read / positioning functions, each returning a 32-bit integer. Operands
    // (a descriptor, plus a character or offset) are plain integer rvalues in
    // call order.
    if (name == "$fgetc" || name == "$feof" || name == "$ftell" || name == "$rewind" || name == "$ungetc" ||
        name == "$fseek") {
        ir::Inst::Tag tag;
        size_t        want;
        if (name == "$fgetc") {
            tag  = ir::Inst::Tag::Fgetc;
            want = 1;
        } else if (name == "$feof") {
            tag  = ir::Inst::Tag::Feof;
            want = 1;
        } else if (name == "$ftell") {
            tag  = ir::Inst::Tag::Ftell;
            want = 1;
        } else if (name == "$rewind") {
            tag  = ir::Inst::Tag::Rewind;
            want = 1;
        } else if (name == "$ungetc") {
            tag  = ir::Inst::Tag::Ungetc;
            want = 2;
        } else {
            tag  = ir::Inst::Tag::Fseek;
            want = 3;
        }
        const auto args = call.arguments();
        if (args.size() != want) {
            fail(std::string(name) + " expects " + std::to_string(want) + " argument(s)");
        }
        std::vector<ir::Index> vals;
        vals.reserve(args.size());
        for (const Expression* a : args) {
            vals.push_back(lower_expr(*a, b));
        }
        if (want == 1) {
            return b.unary(tag, vals[0], l);
        }
        if (want == 2) {
            return b.binary(tag, vals[0], vals[1], l);
        }
        return b.fseek(vals[0], vals[1], vals[2], l);
    }

    // $fgets(str, fd) (LRM §21.3.4): read a line into the packed-vector lvalue
    // `str`, returning the character count. `str` is an output argument.
    if (name == "$fgets") {
        const auto args = call.arguments();
        if (args.size() != 2) {
            fail("$fgets expects two arguments (str, fd)");
        }
        const ir::Index dst = lower_lvalue(*args[0], b);
        const ir::Index fd  = lower_expr(*args[1], b);
        return b.fgets(dst, fd, l);
    }

    // $ferror(fd, str) (LRM §21.3.6): write the descriptor's error message into
    // the string lvalue `str`, returning the error code.
    if (name == "$ferror") {
        const auto args = call.arguments();
        if (args.size() != 2) {
            fail("$ferror expects two arguments (fd, str)");
        }
        const ir::Index fd  = lower_expr(*args[0], b);
        const ir::Index dst = lower_lvalue(*args[1], b);
        return b.ferror(dst, fd, l);
    }

    // $fscanf(fd, fmt, args) / $sscanf(str, fmt, args) (LRM §21.3.4).
    if (name == "$fscanf" || name == "$sscanf") {
        return lower_scanf(call, name == "$sscanf", b, l);
    }

    // $test$plusargs(str) (LRM §21.6): nonzero iff a command-line plusarg's
    // prefix matches `str`. `str` must be a compile-time string literal for
    // now (a runtime / integral-variable string argument, which the LRM
    // also allows, is a documented gap — matches $fscanf/$sscanf's own
    // literal-format-string requirement above).
    if (name == "$test$plusargs") {
        const auto args = call.arguments();
        if (args.size() != 1) {
            fail("$test$plusargs expects one argument");
        }
        const Expression* e = args[0];
        while (e->kind == ExpressionKind::Conversion) {
            e = &e->as<ConversionExpression>().operand();
        }
        if (e->kind != ExpressionKind::StringLiteral) {
            // TODO: a runtime (non-literal) plusarg-string argument.
            fail("$test$plusargs argument must be a string literal");
        }
        return b.test_plusargs(e->as<StringLiteral>().getValue(), l);
    }

    // $value$plusargs(user_string, var) (LRM §21.6). `user_string` must be
    // a compile-time string literal of the form "plusarg_string%[0-9]*conv"
    // for now (a runtime user_string, which the LRM's own ieee1364_example
    // exercises via a `pstring` variable, is a documented gap). Supported
    // conversions: d/o/b/h/x/s; %e/%f/%g need real support (the same gap
    // as $fscanf/$sscanf's own %f/%e/%g, see scan_conversion_chars).
    if (name == "$value$plusargs") {
        const auto args = call.arguments();
        if (args.size() != 2) {
            fail("$value$plusargs expects two arguments (user_string, var)");
        }
        const Expression* e = args[0];
        while (e->kind == ExpressionKind::Conversion) {
            e = &e->as<ConversionExpression>().operand();
        }
        if (e->kind != ExpressionKind::StringLiteral) {
            // TODO: a runtime (non-literal) user_string argument.
            fail("$value$plusargs user_string must be a string literal");
        }
        const std::string_view us  = e->as<StringLiteral>().getValue();
        const size_t           pct = us.rfind('%');
        if (pct == std::string_view::npos) {
            fail("$value$plusargs user_string must end in a %conversion");
        }
        size_t i = pct + 1;
        while (i < us.size() && us[i] >= '0' && us[i] <= '9') {
            ++i; // "leading 0 forms" (LRM §21.6) — no width semantics here.
        }
        if (i >= us.size() || i + 1 != us.size()) {
            fail("$value$plusargs user_string must end in a %[0-9]*<conv>");
        }
        const char conv = static_cast<char>(std::tolower(static_cast<unsigned char>(us[i])));
        switch (conv) {
            case 'd':
            case 'o':
            case 'b':
            case 'h':
            case 'x':
            case 's':
                break;
            default:
                // TODO: %e/%f/%g need real support.
                fail(std::string("$value$plusargs conversion %") + us[i] + " is not supported");
        }
        // A `real` destination is only ever reachable with %e/%f/%g, which the
        // switch above already refuses. The integral conversions have no plane
        // storage to write here -- a real slot holds an IEEE-754 double, so the
        // parsed bit pattern would land in its mantissa and the design would read
        // a silently wrong number (`+V=12` with %d read back as 0.000000). Refused
        // rather than guessed; the LRM gives no meaning to an integral conversion
        // into a real variable.
        if (args[1]->type->isFloating()) {
            fail(std::string("$value$plusargs %") + us[i] +
                 " into a real variable is not supported");
        }
        const std::string_view prefix = us.substr(0, pct);
        const ir::Index        dst    = lower_lvalue(*args[1], b);
        return b.value_plusargs(dst, prefix, conv, l);
    }

    // $sformatf(fmt, args) (LRM §21.3.3): format into a new string value.
    // $psprintf is a nonstandard tool alias for the identical operation --
    // slang itself models both through the same SFormatFunction class
    // (build/_deps/slang-src/source/ast/builtins/MiscSystemFuncs.cpp:453-454),
    // differing only by the nonstandard-extension warning it emits at bind
    // time; the argument validation, return type and eval semantics are
    // shared, so the two names dispatch to the same lowering.
    if (name == "$sformatf" || name == "$psprintf") {
        return lower_sformatf(call, b);
    }

    // $fread(target, fd[, start[, count]]) (LRM §21.3.4): binary read into a
    // single vector or a whole unpacked-array memory. The memory form (like
    // $readmem) takes optional start-address and element-count bounds.
    if (name == "$fread") {
        const auto args = call.arguments();
        if (args.size() < 2 || args.size() > 4) {
            fail("$fread requires 2 to 4 arguments");
        }
        // slang (post-8.0) binds the target as a system-task output argument;
        // unwrap the AssignmentExpression to the lvalue (see lower_lvalue).
        const Expression* target_arg = args[0];
        if (target_arg->kind == ExpressionKind::Assignment) {
            target_arg = &target_arg->as<AssignmentExpression>().left();
        }
        const Expression& target = *target_arg;
        if (is_unpacked_array(target)) {
            const auto* vsb = target.as_if<slang::ast::ValueExpressionBase>();
            if (vsb == nullptr) {
                // TODO: a sliced memory target (mem[a:b]).
                fail("$fread memory target must be a whole unpacked-array memory");
            }
            const ir::Signal mem       = signal_of(vsb->symbol);
            const ir::Index  fd        = lower_expr(*args[1], b);
            const bool       has_start = args.size() >= 3;
            const bool       has_count = args.size() >= 4;
            ir::Index        start     = fd; // placeholder unless present
            ir::Index        cnt       = fd;
            if (has_start) {
                start = lower_expr(*args[2], b);
            }
            if (has_count) {
                cnt = lower_expr(*args[3], b);
            }
            return b.fread_mem(fd, mem, start, cnt, has_start, has_count, l);
        }
        if (args.size() != 2) {
            fail("$fread into a scalar variable takes exactly two arguments");
        }
        const ir::Index dst = lower_lvalue(target, b);
        const ir::Index fd  = lower_expr(*args[1], b);
        return b.fread_word(dst, fd, l);
    }

    // Real conversion functions (LRM §20.5). Constant contexts never reach
    // here — slang folds them.
    if (name == "$rtoi" || name == "$itor" || name == "$realtobits" || name == "$bitstoreal" ||
        name == "$shortrealtobits" || name == "$bitstoshortreal") {
        if (call.arguments().size() != 1) {
            fail(std::string(name) + " expects exactly one argument");
        }
        // slang has converted the argument to the function's formal type
        // (real / shortreal / 64- or 32-bit vector), so only the result
        // conversion is lowered here.
        const ir::Index v = lower_expr(*call.arguments()[0], b);
        if (name == "$rtoi") {
            // Truncates toward zero — unlike assignment conversion, which
            // rounds (LRM 20.5 vs 6.12.1).
            return b.float_cvt(ir::Inst::Tag::FtoITrunc, v, lower_type(*call.type), l);
        }
        if (name == "$itor") {
            return b.float_cvt(call.arguments()[0]->type->isSigned() ? ir::Inst::Tag::SItoF : ir::Inst::Tag::UItoF,
                               v,
                               lower_type(*call.type),
                               l);
        }
        // The four bit-pattern reinterpretations.
        return b.float_cvt(ir::Inst::Tag::FBitcast, v, lower_type(*call.type), l);
    }

    // Real math functions (LRM §20.8.2, Table 20-4): each matches the
    // equivalent C math library function. slang converts every argument to
    // real. Constant contexts fold in slang and never reach here.
    {
        static constexpr std::pair<std::string_view, ir::RealMathOp> kMath[] = {
            { "$ln", ir::RealMathOp::Ln },       { "$log10", ir::RealMathOp::Log10 },
            { "$exp", ir::RealMathOp::Exp },     { "$sqrt", ir::RealMathOp::Sqrt },
            { "$pow", ir::RealMathOp::Pow },     { "$floor", ir::RealMathOp::Floor },
            { "$ceil", ir::RealMathOp::Ceil },   { "$sin", ir::RealMathOp::Sin },
            { "$cos", ir::RealMathOp::Cos },     { "$tan", ir::RealMathOp::Tan },
            { "$asin", ir::RealMathOp::Asin },   { "$acos", ir::RealMathOp::Acos },
            { "$atan", ir::RealMathOp::Atan },   { "$atan2", ir::RealMathOp::Atan2 },
            { "$hypot", ir::RealMathOp::Hypot }, { "$sinh", ir::RealMathOp::Sinh },
            { "$cosh", ir::RealMathOp::Cosh },   { "$tanh", ir::RealMathOp::Tanh },
            { "$asinh", ir::RealMathOp::Asinh }, { "$acosh", ir::RealMathOp::Acosh },
            { "$atanh", ir::RealMathOp::Atanh },
        };
        for (const auto& [fname, op] : kMath) {
            if (name != fname) {
                continue;
            }
            const bool two =
              op == ir::RealMathOp::Pow || op == ir::RealMathOp::Atan2 || op == ir::RealMathOp::Hypot;
            const auto args = call.arguments();
            if (args.size() != (two ? 2u : 1u)) {
                fail(std::string(name) + " expects " + (two ? "two arguments" : "one argument"));
            }
            std::vector<ir::Index> vals;
            for (const Expression* a : args) {
                vals.push_back(lower_expr(*a, b));
            }
            return b.real_math(op, vals, l);
        }
    }

    // $realtime (LRM §20.3.3): the raw wheel tick count as a real, divided
    // by the unit multiplier so the fractional part below the module time
    // unit survives ($time's round-to-unit does not apply).
    if (name == "$realtime") {
        // The tick count is an unsigned 64-bit value.
        const ir::Index ticks = b.float_cvt(ir::Inst::Tag::UItoF, b.sim_time(1, l), module_.pool.real_type, l);
        if (time_mul_ <= 1) {
            return ticks;
        }
        const ir::Index mul =
          b.constant(module_.pool.put_real(module_.pool.real_type, static_cast<double>(time_mul_)), l);
        return b.binary(ir::Inst::Tag::FDiv, ticks, mul, l);
    }
    // $get_coverage (LRM 19.9): the overall coverage of every covergroup type,
    // as a real in 0..100. 100.0 for a design with no covergroup instance --
    // 19.11's closing sentence -- which is what the runtime answers on an
    // empty model, so this needs no design-side gate.
    if (name == "$get_coverage") {
        return b.cover_query(ir::CoverOp::DesignCoverage, {}, module_.pool.real_type, 0, 0, l);
    }
    fail("unsupported system function '" + std::string(name) + "'");
}

// Lower a built-in string method call (LRM 6.16.x). slang models these as
// system subroutines with the receiver as arguments()[0]. Read-only methods
// take the receiver as an rvalue; putc mutates it in place through its
// lvalue.
ir::Index Lowerer::lower_string_method(std::string_view name, const CallExpression& call, ir::Builder& b) {
    const ir::SourceLoc l    = loc(call);
    const auto          args = call.arguments();
    const auto          recv = [&] { return lower_expr(*args[0], b); };
    using T                  = ir::Inst::Tag;

    if (name == "len") {
        return b.unary(T::StrLen, recv(), l);
    }
    if (name == "getc" && args.size() == 2) {
        const ir::Index s = recv();
        const ir::Index i = lower_index(*args[1], b);
        return b.binary(T::StrGetc, s, i, l);
    }
    if (name == "toupper") {
        return b.unary(T::StrUpper, recv(), l);
    }
    if (name == "tolower") {
        return b.unary(T::StrLower, recv(), l);
    }
    if (name == "compare" && args.size() == 2) {
        const ir::Index s     = recv();
        const ir::Index other = lower_expr(*args[1], b);
        return b.binary(T::StrCmp, s, other, l);
    }
    if (name == "icompare" && args.size() == 2) {
        const ir::Index s     = recv();
        const ir::Index other = lower_expr(*args[1], b);
        return b.binary(T::StrICmp, s, other, l);
    }
    if (name == "substr" && args.size() == 3) {
        const ir::Index s = recv();
        const ir::Index i = lower_expr(*args[1], b);
        const ir::Index j = lower_expr(*args[2], b);
        return b.str_substr(s, i, j, l);
    }
    if (name == "putc" && args.size() == 3) {
        const ir::Index str = lower_lvalue(*args[0], b);
        const ir::Index idx = lower_index(*args[1], b);
        return b.str_putc(str, idx, lower_expr(*args[2], b), l);
    }
    if (name == "atoi") {
        return b.unary(T::StrAtoi, recv(), l);
    }
    if (name == "atohex") {
        return b.unary(T::StrAtohex, recv(), l);
    }
    if (name == "atooct") {
        return b.unary(T::StrAtooct, recv(), l);
    }
    if (name == "atobin") {
        return b.unary(T::StrAtobin, recv(), l);
    }
    if (name == "atoreal") {
        return b.unary(T::StrAtoreal, recv(), l);
    }
    if (name == "itoa" && args.size() == 2) {
        const ir::Index str = lower_lvalue(*args[0], b);
        return b.binary(T::StrItoa, str, lower_expr(*args[1], b), l);
    }
    if (name == "hextoa" && args.size() == 2) {
        const ir::Index str = lower_lvalue(*args[0], b);
        return b.binary(T::StrHextoa, str, lower_expr(*args[1], b), l);
    }
    if (name == "octtoa" && args.size() == 2) {
        const ir::Index str = lower_lvalue(*args[0], b);
        return b.binary(T::StrOctoa, str, lower_expr(*args[1], b), l);
    }
    if (name == "bintoa" && args.size() == 2) {
        const ir::Index str = lower_lvalue(*args[0], b);
        return b.binary(T::StrBintoa, str, lower_expr(*args[1], b), l);
    }
    if (name == "realtoa" && args.size() == 2) {
        const ir::Index str = lower_lvalue(*args[0], b);
        return b.binary(T::StrRealtoa, str, lower_expr(*args[1], b), l);
    }
    // LRM 6.16 defines eighteen string methods and all eighteen are handled
    // above. slang binds no other name on a string receiver, so this is an
    // invariant, not a feature gap.
    fail("internal: unhandled string method '" + std::string(name) + "'");
}

// Build the combined format string and value-arg list of a $display-family
// task from `format_args` (the arguments after any leading descriptor). A
// string-literal argument contributes its text verbatim (its %-specifiers pull
// from following value args, SV-style); a bare value argument not covered by an
// earlier specifier gets a default `%0d` (or `%s` for a string object).
// Hierarchical path of the instance / generate scope currently being lowered
// ("top.u1.blk"): the flattened prefix, with the elided single-top name
// restored.
std::string Lowerer::instance_path() const {
    std::string path = top_elided_ ? module_.name : std::string();
    if (!prefix_.empty()) {
        if (!path.empty()) {
            path += '.';
        }
        path.append(prefix_, 0, prefix_.size() - 1);
    }
    return path;
}

// Hierarchical name of the scope invoking the statement currently being
// lowered (LRM §21.2.1.5 %m): the instance path plus the enclosing
// subroutine and named begin blocks. Static per lowering site, because
// instances are flattened per-instance and subroutine bodies are lowered
// per defining instance.
std::string Lowerer::hier_name() const {
    std::string path;
    if (fn_ctx_ != nullptr && fn_ctx_->invoking_scope) {
        path = *fn_ctx_->invoking_scope;
    } else {
        path = instance_path();
        if (path.empty() && scope_ != nullptr) {
            // A package / $unit subroutine sits under no instance, so the instance
            // path is empty; %m still owes the scope chain (LRM 21.2.1.3, Verilator
            // renders `pkg.fn`). UVM's uvm_instance_scope (base/uvm_misc.svh) scans
            // %m backwards for a '.' / ':' separated prefix and reports SCPSTR --
            // 128 times, then a BUILDERR fatal -- when there is none, so the
            // package segment is load-bearing, not cosmetic.
            for (const Scope* s = scope_; s != nullptr; s = s->asSymbol().getParentScope()) {
                const Symbol& sym = s->asSymbol();
                if (sym.kind == SymbolKind::Package) {
                    path = std::string(sym.name);
                    break;
                }
                if (sym.kind == SymbolKind::CompilationUnit) {
                    path = "$unit";
                    break;
                }
            }
        }
        if (fn_ctx_ != nullptr && !fn_ctx_->name_prefix.empty()) {
            path += '.';
            path.append(fn_ctx_->name_prefix, 0, fn_ctx_->name_prefix.size() - 1);
        }
    }
    for (const OpenBlock& nb : named_blocks_) {
        if (nb.inherited) {
            continue; // already in invoking_scope
        }
        path += '.';
        path += nb.sym->name;
    }
    return path;
}

// "library.cell" of the instance invoking the statement currently being
// lowered (LRM §21.2.1.3 %l): the library from which the current module
// instance was extracted, and its cell (module definition) name. Clotho
// has no library/config system (LRM Clause 33 is unimplemented -- every
// definition compiles from one source set with no binding choice to make),
// so every instance is unconditionally in "work", the conventional default
// library name when no library map is in play. Static per lowering site,
// same as hier_name (%m): the current instance's InstanceScope is the top
// of scope_stack_ for the whole dynamic extent of lowering that instance's
// processes.
std::string Lowerer::lib_cell_name() const {
    return "work." + scope_stack_.back()->def_name;
}

// Validate one non-%% specifier parsed from a display format literal and
// return its lowercase conversion char. Everything the engine does not
// implement fails loudly here, so the runtime renderer only ever sees legal
// specifiers (LRM §21.2.1.1: an undefined specifier is an error).
char Lowerer::check_specifier(const clotho::FormatSpec& spec) {
    if (!spec.ok) {
        fail("incomplete format specifier in display format");
    }
    if (spec.width_overflow) {
        fail("format field width is out of range");
    }
    const char conv = static_cast<char>(spec.conv | 0x20);
    switch (conv) {
        case 'd':
        case 'h':
        case 'x':
        case 'o':
        case 'b':
        case 'c':
        case 's':
        case 't':
        case 'm':
        case 'l':
            break;
        case 'u':
        case 'z':
            if (spec.has_width || spec.left_justify) {
                fail("%u / %z output is unformatted; width and justification do not apply");
            }
            break;
        case 'e':
        case 'f':
        case 'g':
            // Real conversions take the full C formatting specification
            // (LRM §21.2.1.2 Table 21-2), including a precision.
            return conv;
        case 'v':
            // %v prints a per-bit strength descriptor (e.g. St0 / Pu1 / HiZ,
            // LRM §21.2.1.4). It needs a runtime per-bit strength on the net —
            // the 120-value strength model — which Clotho does not keep: drive
            // strengths are resolved away at the resolver into a plain 4-state
            // value. A Strong/HiZ approximation would be silently wrong (the
            // conformance tests probe exactly the Pull/Weak prefixes), so %v
            // stays loud until the strength model lands (docs/TODO.md).
            fail("%v (net signal strength) is not supported");
        case 'p':
            // %p renders an aggregate as an assignment pattern (LRM 21.2.1.6);
            // build_format expands it at lower time into `'{...}` text plus a
            // per-leaf spec.
            //
            // `%0p` is a SEPARATE specifier in the clause, not a width applied to
            // %p: it "shall print aggregate expressions ... in a shorter,
            // implementation specific form". The 0 reads as it does everywhere else
            // (21.2.1.2) -- drop the %d-width padding on the integral leaves -- and
            // Clotho's %p already renders every leaf minimally, scalars included,
            // so the shorter form coincides with the plain one and both accept
            // here. Verilator agrees (byte-identical for the two), so nothing is
            // lost by the coincidence; see docs/TODO.md.
            //
            // A non-zero width, a precision and left-justification are none of them
            // defined for %p by the clause. Unreachable from source: slang checks
            // this first and more precisely (SFormat.cpp, "Pattern args allow the
            // zero-pad specifier" -- `%p` sets widthAllowed = false, and only the
            // bare zero-pad flag is let through for it). Kept as a backstop because
            // this is the only place that knows what Clotho's own expansion accepts.
            if ((spec.has_width && spec.width != 0) || spec.has_prec || spec.left_justify) {
                fail("%p takes no field width, precision, or justification other than "
                     "the '%0p' shorter form (LRM 21.2.1.6)");
            }
            break;
        default:
            fail(std::string("undefined format specifier '%") + spec.conv + "' (LRM 21.2.1.1)");
    }
    if (spec.has_prec) {
        fail("a format precision is only defined for real specifiers (%e / %f / %g)");
    }
    return conv;
}

// Copy one string-literal argument into `fmt`, validating every specifier,
// splicing %m as literal text, and appending each argument-consuming
// conversion char to `pending`.
void Lowerer::append_format_literal(std::string& fmt, std::string_view text, std::vector<char>& pending) {
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '%') {
            fmt += text[i++];
            continue;
        }
        const clotho::FormatSpec spec = clotho::parse_format_spec(text.substr(i + 1));
        if (spec.ok && spec.conv == '%') {
            fmt += "%%";
            i += 2;
            continue;
        }
        const char conv = check_specifier(spec);
        if (conv == 'm' || conv == 'l') {
            // %m / %l consume no argument; both are static at lower time
            // (the invoking scope's hierarchical name, or its library.cell),
            // so splice the text in directly (width-padded).
            std::string name = conv == 'm' ? hier_name() : lib_cell_name();
            if (spec.has_width && name.size() < spec.width) {
                if (spec.left_justify) {
                    name.append(spec.width - name.size(), ' ');
                } else {
                    name.insert(0, spec.width - name.size(), ' ');
                }
            }
            fmt += name;
        } else {
            fmt.append(text, i, 1 + spec.len);
            pending.push_back(conv);
        }
        i += 1 + spec.len;
    }
}

// Expand a %p operand (LRM §21.2.1.6) into assignment-pattern text plus its
// per-leaf value args. An unpacked array / struct renders `'{e0, e1, ...}`
// recursively; the clause's four named singular cases (tagged union, plain
// union, packed struct, enum) render through expand_p_packed; anything else is
// an integral / real / string leaf. The slang type is threaded alongside the
// lowered one because taggedness, member names, and enum membership live only in
// the AST — `ir::Type` keeps a tagged union indistinguishable from a plain packed
// union by design (docs/design/tagged-unions.md).
void Lowerer::expand_p_arg(const Expression& arg, std::string& frag, std::vector<ir::FmtArg>& args, ir::Builder& b) {
    if (const Expression& src = unwrap_conversion(arg); src.kind == ExpressionKind::StringLiteral) {
        // "A string data type or string literal shall print its value as a string
        // enclosed in quotes." A bare literal is an integral packed value (LRM
        // 5.9), so without this it would print as one decimal number; its text is
        // known here, so splice it, doubling any `%` so it stays literal.
        frag += '"';
        for (const char c : src.as<StringLiteral>().getValue()) {
            frag += c;
            if (c == '%') {
                frag += '%';
            }
        }
        frag += '"';
        return;
    }
    const ir::Ref           ty        = lower_type(*arg.type);
    const ir::Type::Kind    k         = std::get<ir::Type>(module_.pool.get(ty)).kind;
    // Every managed container renders through expand_p_lvalue, which walks it.
    // An Assoc is not is_agg_abi_ref either, so without naming it here a bare
    // `$display("%p", my_assoc)` took the singular path and died as an
    // unrenderable handle.
    const bool              container = k == ir::Type::Kind::Queue || k == ir::Type::Kind::DynArray ||
                                        k == ir::Type::Kind::Assoc;
    const slang::ast::Type& sty       = *arg.type;
    const ir::SourceLoc     l         = loc(arg);
    if (!is_agg_abi_ref(ty) && !container) {
        // A singular operand: take it as an rvalue, so an arbitrary expression
        // (a call, a concat) works and not just an addressable variable. Lowered
        // exactly once — a call operand must not be evaluated twice. The last
        // paragraph of 21.2.1.6 formats it "as an element of an aggregate
        // expression", which is what expand_p_scalar renders.
        expand_p_scalar(lower_expr(arg, b), sty, frag, args, b, l);
        return;
    }
    expand_p_lvalue(lower_lvalue(unwrap_conversion(arg), b), ty, &sty, frag, args, b, l);
}

// The Scope side of a union type, whose members carry the names and payload
// widths `%p` and the LRM 11.9 check need. Type itself does not derive Scope, so
// the cast goes through the concrete union type.
const Scope& Lowerer::union_scope(const slang::ast::Type& ct) {
    return ct.isPackedUnion() ? static_cast<const Scope&>(ct.as<slang::ast::PackedUnionType>())
                              : static_cast<const Scope&>(ct.as<slang::ast::UnpackedUnionType>());
}

// Bits [lsb, lsb+width(fty)) of the packed rvalue `v`, as a value of type
// `fty`. Used to walk a packed struct / union member without needing an lvalue,
// so a `%p` operand may be any expression and not just an addressable variable.
ir::Index Lowerer::packed_slice(ir::Index v, uint32_t lsb, ir::Ref fty, ir::Builder& b, ir::SourceLoc l) {
    ir::Index x = v;
    if (lsb != 0) {
        const ir::Index sh = b.constant(module_.pool.put_int(make_int_type(32, false), lsb), l);
        x                  = b.binary(ir::Inst::Tag::Srl, v, sh, l);
    }
    return b.convert(x, fty, false, l);
}

// Render a singular (packed) operand under `%p` from its rvalue, for the four
// cases LRM 21.2.1.6 names explicitly. Returns false for anything else, which
// the caller renders as an ordinary integral / real leaf.
bool Lowerer::expand_p_packed(ir::Index                v,
                              const slang::ast::Type&  sty,
                              std::string&             frag,
                              std::vector<ir::FmtArg>& args,
                              ir::Builder&             b,
                              ir::SourceLoc            l) {
    const slang::ast::Type& ct = sty.getCanonicalType();
    if (ct.isTaggedUnion()) {
        // "In the case of a tagged union, it shall print "tag:value" along with
        // the currently valid element." Which member that is only the run-time
        // tag knows, so this is the one %p shape that renders in the runtime
        // rather than expanding to literal text here. The `'{...}` around it is
        // literal: the clause requires the whole output to be "a legal
        // interpretation of the assignment pattern syntax", which a bare
        // `tag:value` pair is not.
        std::vector<ir::Builder::TaggedFmtRow> rows;
        for (const auto& f : union_scope(ct).membersOfType<FieldSymbol>()) {
            const slang::ast::Type& mt = f.getType();
            rows.push_back({ f.name, mt.isVoid() ? 0u : static_cast<uint32_t>(mt.getBitWidth()), mt.isSigned() });
        }
        frag += "'{%s}";
        args.push_back(
          { b.tagged_fmt_p(v, module_.pool.layout(lower_type(sty)).width, union_tag_bits(ct), rows, l), false });
        return true;
    }
    if (ct.isPackedUnion() || ct.isUnpackedUnion()) {
        // "For unions, only the first declared elements shall be printed." Every
        // member overlays the LSBs, so the first one is the low bits of the flat
        // value. An unpacked union lands here too, because Clotho gives it the
        // same flat representation (docs/LRM_NOTES.md, note 3).
        for (const auto& f : union_scope(ct).membersOfType<FieldSymbol>()) {
            const slang::ast::Type& mt = f.getType();
            frag += "'{";
            frag += f.name;
            frag += ':';
            expand_p_scalar(packed_slice(v, 0, lower_type(mt), b, l), mt, frag, args, b, l);
            frag += "}";
            return true;
        }
        return false; // a member-less union cannot occur, but do not assume it
    }
    if (ct.isStruct()) {
        // "A packed structure data type shall print its value as an assignment
        // pattern with named elements. Each element shall be printed under one of
        // these rules" — so a nested packed struct / union / enum member recurses.
        // (Only the packed form reaches here; an unpacked struct is an aggregate
        // and took the lvalue path.)
        frag += "'{";
        bool first = true;
        for (const auto& f : static_cast<const Scope&>(ct.as<slang::ast::PackedStructType>())
                               .membersOfType<FieldSymbol>()) {
            if (!first) {
                frag += ", ";
            }
            first                      = false;
            const slang::ast::Type& mt = f.getType();
            frag += f.name;
            frag += ':';
            expand_p_scalar(packed_slice(v, static_cast<uint32_t>(f.bitOffset), lower_type(mt), b, l),
                            mt, frag, args, b, l);
        }
        frag += "}";
        return true;
    }
    if (ct.isEnum()) {
        // "An enumerated data type shall print its value as an enumeration name
        // if the value is valid for that type. Otherwise the value shall print
        // according to the base type of the enumeration" — the fallback the
        // runtime applies when no member matches.
        frag += "%s";
        const EnumNameTable tbl = enum_name_table(ct.as<slang::ast::EnumType>());
        args.push_back({ b.enum_name(v, tbl.rows, /*fmt_fallback=*/true, sty.isSigned(), l), false });
        return true;
    }
    return false;
}

// The name `%p` prints for a NON-NULL handle, or nullopt when the type is not a
// handle at all. LRM 21.2.1.6 names five reference types together -- "a chandle,
// class handle, interface class handle, event, or virtual interface" -- and
// leaves their form implementation-dependent, so one rule covers all of them:
// the name of the thing the handle points AT. See expand_p_scalar for why a name
// rather than the pointee's contents.
//
// A class name is the bare `ClassType::name`: unqualified, where Verilator writes
// `pkg::C`, and WITHOUT a specialization's arguments -- slang leaves every
// specialization of a generic class named plainly `C`, since the arguments live in
// genericParameters and only the type printer appends them
// (TypePrinter.cpp:373-379), so `$display("%p", b)` of a `box#(int)` prints `box`.
// Implementation-defined either way (LRM 21.2.1.6). The IR printer's class label
// deliberately differs and takes the arguments, because a label there has to
// identify one class; see bind_class_type.
// An interface class is a ClassType too, so it takes the same arm.
std::optional<std::string_view> Lowerer::handle_p_name(const slang::ast::Type& ct) {
    if (ct.isClass()) {
        return ct.as<slang::ast::ClassType>().name;
    }
    if (ct.isVirtualInterface()) {
        // The interface DEFINITION's name, which is what the handle's TYPE names
        // (`virtual input_if` prints `input_if`). The placeholder body slang
        // caches per (definition, parameters) is the same one a vif member access
        // resolves against (vif_named_body), so this agrees with `vif.sig` about
        // which interface it is.
        return ct.as<slang::ast::VirtualInterfaceType>().iface.body.getDefinition().name;
    }
    if (ct.isCHandle()) {
        // LRM 6.14: an opaque pointer. The type is the only name it has.
        return "chandle";
    }
    if (ct.isEvent()) {
        return "event";
    }
    return std::nullopt;
}

// The `%p` text of a handle value `v`: `null` when the handle word is zero, else
// `name`. A select over two string constants, so nothing dereferences the handle
// and there is no branch to build. Zero is `null` for all five reference types --
// a class handle and a chandle by Table 7-1's default, an event because slot 0 is
// the reserved null object, a virtual interface because the handle is the address
// of an instance's context record.
ir::Index Lowerer::handle_p_render(ir::Index v, std::string_view name, ir::Builder& b, ir::SourceLoc l) {
    const ir::Ref   u64       = make_int_type(64, false);
    const ir::Index null      = b.constant(module_.pool.put_int(u64, 0), l);
    const ir::Index is_null   = b.binary(ir::Inst::Tag::Eq, v, null, l);
    const ir::Index null_text = b.constant(string_value_ref("null"), l);
    const ir::Index name_text = b.constant(string_value_ref(name), l);
    return b.select(is_null, null_text, name_text, l);
}

// A `%p` element that is already an rvalue: one of the four named singular
// cases, else the per-element leaf rules that close LRM 21.2.1.6's list. An
// integral leaf prints "as [it] would unformatted", i.e. minimal decimal signed
// with the operand's own signedness (21.2.1.1); a real the same way (%g, note
// 26); a string "as a string enclosed in quotes".
void Lowerer::expand_p_scalar(ir::Index                v,
                              const slang::ast::Type&  sty,
                              std::string&             frag,
                              std::vector<ir::FmtArg>& args,
                              ir::Builder&             b,
                              ir::SourceLoc            l) {
    if (expand_p_packed(v, sty, frag, args, b, l)) {
        return;
    }
    const ir::Ref lty = lower_type(sty);
    if (module_.pool.is_float(lty)) {
        frag += "%g";
    } else if (value_cat(lty) == ValueCat::string) {
        // The clause says a string operand prints "enclosed in quotes", and
        // that is also how a queue's string elements already print.
        frag += "\"%s\"";
    } else if (!sty.isIntegral()) {
        const ir::Ref cl = lower_type(sty);
        if (const std::optional<std::string_view> hn = handle_p_name(sty.getCanonicalType())) {
            // A handle. LRM 21.2.1.6 REQUIRES this to print -- "a chandle, class
            // handle, interface class handle, event, or virtual interface shall
            // print its value in an implementation-dependent format, except that
            // a null handle value shall print the word null" -- but the clause
            // otherwise describes %p over aggregate VALUES, and a handle is not
            // one. So the non-null form is ours to pick, and this is the choice,
            // recorded in docs/TODO.md:
            //
            //   a null handle prints `null`; any other prints the NAME of what it
            //   points at -- a class name, an interface definition name, or the
            //   type's own name for a chandle / event, which have no other.
            //
            // One rule at every depth -- a direct %p operand, a struct member, a
            // queue element -- which is what makes it cycle-safe by construction.
            // Expanding a class object's properties instead is more informative
            // and is what Verilator does for a DIRECT operand, but it needs a
            // depth cap or a visited set the LRM does not define, and UVM's object
            // graphs are cyclic (uvm_component holds m_parent and m_children, so
            // `%p` on one would not terminate). Verilator is itself inconsistent
            // here -- it expands a direct operand but prints the type name for a
            // handle inside a container -- so there is no reference behaviour to
            // match, only a decision to make.
            //
            // A virtual interface could instead print the hierarchical path of the
            // instance the handle names, which is genuinely more informative. It
            // is rejected for two reasons: it is instance IDENTITY where the class
            // arm prints a TYPE, so the five reference types would stop agreeing
            // with each other; and the handle is a context-record address with no
            // runtime address -> path table behind it, so a path would take a
            // compare chain over every elaborated instance of the definition,
            // emitted per %p. A raw pointer value is rejected outright: it is
            // unstable across runs, which makes it useless in a golden file.
            //
            // TODO: expand the properties of a DIRECT class-handle %p operand (the
            // one depth where a cycle cannot be reached in one step), leaving
            // nested handles as names.
            frag += "%s";
            args.push_back({ handle_p_render(v, *hn, b, l), false });
            return;
        }
        if (is_handle_ref(cl) || is_agg_abi_ref(cl)) {
            // TODO: %p of a container or aggregate ELEMENT. 21.2.1.6 recurses
            // into one, but the runtime renderer works from a flat element
            // descriptor (one integral / real / string per element), so a nested
            // container has no image to hand it. Reported here rather than
            // falling into the case below, whose reason is different.
            fail("%p of a container or aggregate element is not supported");
        }
        // Not one of the five reference types and not an aggregate, so there is
        // neither a value nor a name to print. Nothing in the language reaches
        // here today -- the `null` literal has no lowered type at all and dies
        // earlier, in lower_type -- so this is the backstop, named by type so a
        // future arrival is identifiable from the diagnostic alone.
        fail("%p of a value of type '" + sty.toString() + "' is not supported");
    } else {
        frag += "%0d";
    }
    args.push_back({ v, sty.isSigned() });
}

// `sty`, when non-null, is the slang type matching `ty`; it is threaded down so
// a nested tagged union / packed struct / plain union / enum leaf can be
// rendered per LRM 21.2.1.6 (none of which `ir::Type` can distinguish). Null
// only where the slang type is genuinely unavailable, which falls back to the
// plain integral leaf.
void Lowerer::expand_p_lvalue(ir::Index                lv,
                              ir::Ref                  ty,
                              const slang::ast::Type*  sty,
                              std::string&             frag,
                              std::vector<ir::FmtArg>& args,
                              ir::Builder&             b,
                              ir::SourceLoc            l) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(ty));
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        const slang::ast::Type* est =
          sty != nullptr ? &sty->getCanonicalType().as<slang::ast::FixedSizeUnpackedArrayType>().elementType
                         : nullptr;
        frag += "'{";
        const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        for (uint32_t k = 0; k < n; ++k) {
            if (k != 0) {
                frag += ", ";
            }
            const int32_t coord =
              t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
            const ir::Index idx = b.constant(
              module_.pool.put_int(make_int_type(32, false), static_cast<uint64_t>(static_cast<int64_t>(coord))), l);
            expand_p_lvalue(b.elem_select(lv, idx, ty, l), t.element, est, frag, args, b, l);
        }
        frag += "}";
        return;
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        // `'{field:val, ...}` — LRM 21.2.1.6 requires the member names for an
        // unpacked struct ("an assignment pattern with named elements"); only the
        // white space between them is implementation-defined (docs/LRM_NOTES.md, note 32).
        frag += "'{";
        const auto fields = module_.pool.struct_fields(t.fields_id);
        // Field order is declaration order on both sides, so the slang members
        // line up with the lowered field table index for index.
        std::vector<const slang::ast::Type*> mts;
        if (sty != nullptr) {
            for (const auto& f : static_cast<const Scope&>(
                   sty->getCanonicalType().as<slang::ast::UnpackedStructType>())
                                   .membersOfType<FieldSymbol>()) {
                mts.push_back(&f.getType());
            }
        }
        for (uint32_t i = 0; i < fields.size(); ++i) {
            if (i != 0) {
                frag += ", ";
            }
            frag += module_.pool.field_name(fields[i]);
            frag += ':';
            expand_p_lvalue(b.member_select(lv, i, ty, l), fields[i].type,
                            i < mts.size() ? mts[i] : nullptr, frag, args, b, l);
        }
        frag += "}";
        return;
    }
    if (t.kind == ir::Type::Kind::Queue || t.kind == ir::Type::Kind::DynArray) {
        // A queue / dynamic-array leaf renders over its live element count (LRM
        // 21.2.1.6), spliced in as a `%s` string argument. The runtime formatter
        // does it from a flat descriptor, which covers exactly a single-word
        // integral, a real and a string element; everything else -- an aggregate
        // or nested container (no flat image), an element wider than one word (the
        // runtime extraction is single-word), and a tagged union (the descriptor
        // carries no member names) -- renders through the loop here, where each
        // element goes through the ordinary aggregate expansion.
        frag += "%s";
        const slang::ast::Type* est = sty != nullptr ? sty->getCanonicalType().getArrayElementType() : nullptr;
        // A handle element -- class, chandle, event or virtual interface -- is a
        // 64-bit word, so the flat runtime formatter would take it and print the
        // pointer. It has a name to print instead (handle_p_render), but only the
        // frontend knows it, so route it through the loop like any other element
        // the descriptor cannot describe. A class handle is decidable from the IR
        // type; a chandle is a plain unsigned 64-bit int there and only the AST
        // tells it apart, so both tests are needed.
        const bool              elem_is_handle =
          std::get<ir::Type>(module_.pool.get(t.element)).kind == ir::Type::Kind::Class ||
          (est != nullptr && handle_p_name(est->getCanonicalType()).has_value());
        const bool              loop_render = value_cat(t.element) != ValueCat::scalar ||
                                              module_.pool.layout(t.element).width > 64 ||
                                              elem_is_handle ||
                                              (est != nullptr && est->isTaggedUnion());
        if (loop_render && value_cat(t.element) != ValueCat::string) {
            args.push_back({ build_container_p_string(lv, ty, t, sty, b, l), false });
            return;
        }
        // What is left is exactly what the flat descriptor describes: a scalar
        // of one word or less, a real, or a string.
        args.push_back({ b.container_fmt_p(lv, ty, l), false });
        return;
    }
    if (t.kind == ir::Type::Kind::Assoc) {
        // An associative-array leaf renders over its live entries in ascending
        // key order, spliced in as a `%s` string argument like a queue's. It
        // cannot use the flat runtime formatter at all: that addresses an element
        // as `base + i*stride`, which a keyed table has no answer for, and the
        // ordered walk exists only as the four-call rt_assoc_first / next ABI.
        // So it takes the same IR-loop render a container with a non-flat element
        // already takes -- see build_assoc_p_string.
        frag += "%s";
        args.push_back({ build_assoc_p_string(lv, ty, t, sty, b, l), false });
        return;
    }
    if (value_cat(ty) != ValueCat::scalar && value_cat(ty) != ValueCat::string) {
        // Every managed container has an arm above, so what reaches here is a
        // handle with no aggregate value to render at all.
        fail("%p of a handle aggregate leaf is not supported");
    }
    // A singular leaf. With the slang type in hand this may still be one of LRM
    // 21.2.1.6's named cases (a nested tagged union, packed struct, plain union,
    // or enum); otherwise it is an integral / real / string leaf.
    const ir::Index v = b.unary(ir::Inst::Tag::Load, lv, l);
    if (sty != nullptr) {
        expand_p_scalar(v, *sty, frag, args, b, l);
        return;
    }
    frag += module_.pool.is_float(ty) ? "%g" : value_cat(ty) == ValueCat::string ? "\"%s\"" : "%0d";
    // No slang type: a leaf the walk could not name, which prints as an unsigned number.
    args.push_back({ v, false });
}

// `%p` of a container whose element is an aggregate or a nested container, as a
// string value (LRM 21.2.1.6's `'{e0, e1, ...}` over the live element count).
//
// The runtime formatter renders an element from a flat descriptor (width, domain,
// stride), which an unpacked struct / array or a nested handle simply does not
// have -- and the frontend cannot unroll a runtime length the way it unrolls a
// fixed array. So the render is a loop built here: each element goes through the
// ORDINARY aggregate expansion (so member names, nested arrays, tagged unions and
// string leaves all render exactly as they do outside a container) into one
// $sformatf, and the pieces concatenate into a string cell.
ir::Index Lowerer::build_container_p_string(ir::Index               lv,
                                            ir::Ref                 ty,
                                            const ir::Type&         t,
                                            const slang::ast::Type* sty,
                                            ir::Builder&            b,
                                            ir::SourceLoc           l) {
    using T                           = ir::Inst::Tag;
    const slang::ast::Type* est       = sty != nullptr ? sty->getCanonicalType().getArrayElementType() : nullptr;
    const ir::Ref           i32       = make_int_type(32, false);
    const TempCell          out       = make_temp_cell(module_.pool.string_type, "pstr");
    const TempCell          idx       = make_temp_cell(i32, "pstr_idx");
    const TempCell          bound     = make_temp_cell(i32, "pstr_n");
    const bool              is_q      = t.kind == ir::Type::Kind::Queue;
    const auto              str_const = [&](std::string_view s) { return b.constant(string_value_ref(s), l); };
    const auto              append    = [&](ir::Index piece) {
        const std::array<ir::Index, 2> parts{ b.unary(T::Load, out.lv(b, l), l), piece };
        const ir::Index                dst    = out.lv(b, l);
        const ir::Index                joined = b.concat(parts, l);
        b.binary(T::Store, dst, joined, l);
    };

    const ir::Index out_lv = out.lv(b, l);
    const ir::Index open   = str_const("'{");
    b.binary(T::Store, out_lv, open, l);
    const ir::Index bound_lv = bound.lv(b, l);
    const ir::Index count    = b.convert(is_q ? b.queue_size(lv, ty, l) : b.dyn_array_size(lv, ty, l), i32, true, l);
    b.binary(T::Store, bound_lv, count, l);
    const ir::Index idx_lv   = idx.lv(b, l);
    const ir::Index idx_init = b.constant(module_.pool.put_int(i32, 0), l);
    b.binary(T::Store, idx_lv, idx_init, l);

    b.open_region();
    const ir::Index      cur  = b.unary(T::Load, idx.lv(b, l), l);
    const ir::Index      lim  = b.unary(T::Load, bound.lv(b, l), l);
    const ir::Index      pred = b.binary(T::SLt, cur, lim, l);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    const ir::Index k = b.unary(T::Load, idx.lv(b, l), l);
    b.open_region();
    append(str_const(", "));
    const ir::ExtraSlice sep_r = b.close_region();
    b.open_region();
    const ir::ExtraSlice nosep_r = b.close_region();
    b.cond(b.binary(T::Ne, k, b.constant(module_.pool.put_int(i32, 0), l), l), sep_r, nosep_r, l);
    std::string            efrag;
    std::vector<ir::FmtArg> eargs;
    expand_p_lvalue(b.elem_select(lv, k, ty, l), t.element, est, efrag, eargs, b, l);
    append(b.sformatf(efrag, eargs, l));
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index slot = idx.lv(b, l);
    const ir::Index sc   = b.unary(T::Load, idx.lv(b, l), l);
    const ir::Index one  = b.constant(module_.pool.put_int(i32, 1), l);
    const ir::Index next = b.binary(T::Add, sc, one, l);
    b.binary(T::Store, slot, next, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
    append(str_const("}"));
    return b.unary(T::Load, out.lv(b, l), l);
}

// `%p` of an associative array, as a string value: `'{k0:v0, k1:v1, ...}` over
// the live entries in ascending key order (LRM 7.9.3's traversal order, which
// 7.8.5 defines for integral and string keys and leaves implementation-defined
// for the rest -- see docs/design/assoc-keys.md).
//
// Structurally build_container_p_string with the index space replaced by a key
// walk: assoc_traverse(First) seeds, assoc_traverse(Next) advances, assoc_select
// reads the entry -- exactly the shape lower_foreach_assoc_dim already uses for
// `foreach`. That key walk is also why this cannot be a runtime formatter the way
// a queue of flat elements is: a stored key is an order-preserving byte span and
// has to be DENORMALIZED into a typed cell before it can be printed, which lives
// in the backend's AssocFirst / AssocNext lowering, and a struct value or nested
// container then renders through the ordinary aggregate expansion.
ir::Index Lowerer::build_assoc_p_string(ir::Index               lv,
                                        ir::Ref                 ty,
                                        const ir::Type&         t,
                                        const slang::ast::Type* sty,
                                        ir::Builder&            b,
                                        ir::SourceLoc           l) {
    using T    = ir::Inst::Tag;
    using Step = ir::Builder::AssocStep;
    const slang::ast::AssociativeArrayType* ast =
      sty != nullptr ? &sty->getCanonicalType().as<slang::ast::AssociativeArrayType>() : nullptr;
    const slang::ast::Type* est = ast != nullptr ? &ast->elementType : nullptr;
    // A wildcard `[*]` index has no slang index type, and its lowered key is a
    // synthetic u64 placeholder, so it renders as a plain integral key.
    const slang::ast::Type* kst = ast != nullptr ? ast->indexType : nullptr;
    const ir::Ref           kr        = t.right != 0 ? make_int_type(64, false) : static_cast<ir::Ref>(t.fields_id);
    const ir::Ref           bit1      = make_int_type(1, false);
    const TempCell out  = make_temp_cell(module_.pool.string_type, "pstr");
    const TempCell key  = make_temp_cell(kr, "pkey");
    const TempCell ok   = make_temp_cell(bit1, "pstr_ok");
    const TempCell more = make_temp_cell(bit1, "pstr_more");
    const auto     str_const = [&](std::string_view s) { return b.constant(string_value_ref(s), l); };
    const auto     append    = [&](ir::Index piece) {
        const std::array<ir::Index, 2> parts{ b.unary(T::Load, out.lv(b, l), l), piece };
        const ir::Index                dst    = out.lv(b, l);
        const ir::Index                joined = b.concat(parts, l);
        b.binary(T::Store, dst, joined, l);
    };
    // An aggregate key has no plane value, so it travels as an lvalue -- the same
    // split lower_assoc_key makes for `a[k]`.
    const auto key_operand = [&] {
        return is_agg_abi_ref(kr) ? key.lv(b, l) : b.unary(T::Load, key.lv(b, l), l);
    };

    const ir::Index out_lv = out.lv(b, l);
    const ir::Index open   = str_const("'{");
    b.binary(T::Store, out_lv, open, l);
    // `more` is "an entry has already been emitted", i.e. the separator flag. A
    // key walk has no index to compare against zero the way the queue loop does.
    const ir::Index more_lv   = more.lv(b, l);
    const ir::Index more_zero = b.constant(module_.pool.put_int(bit1, 0), l);
    b.binary(T::Store, more_lv, more_zero, l);
    const ir::Index ok_lv    = ok.lv(b, l);
    const ir::Index ok_first = b.convert(b.assoc_traverse(lv, key.lv(b, l), Step::First, ty, l), bit1, false, l);
    b.binary(T::Store, ok_lv, ok_first, l);

    b.open_region();
    const ir::Index      pred = b.unary(T::Load, ok.lv(b, l), l);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    b.open_region();
    append(str_const(", "));
    const ir::ExtraSlice sep_r = b.close_region();
    b.open_region();
    const ir::ExtraSlice nosep_r = b.close_region();
    b.cond(b.unary(T::Load, more.lv(b, l), l), sep_r, nosep_r, l);
    const ir::Index more_set_lv = more.lv(b, l);
    const ir::Index more_one    = b.constant(module_.pool.put_int(bit1, 1), l);
    b.binary(T::Store, more_set_lv, more_one, l);
    std::string            efrag;
    std::vector<ir::FmtArg> eargs;
    // The key prints by the same leaf rules as any other %p element, so an enum
    // key prints its name and a string key prints quoted. An unpacked struct /
    // fixed array key has no plane value to Load -- same split key_operand()
    // makes for a[k] itself -- so it renders through the ordinary aggregate %p
    // expansion instead: expand_p_scalar assumes a packed value and crashes (a
    // slang assertion, casting an unpacked struct's type as packed) on
    // anything else. A bare queue / dynamic-array / associative-array key (P2/
    // P3) is left on the scalar path, which refuses it the same way a
    // container-typed VALUE element already is -- expand_p_lvalue's container
    // renderer works from a flat element descriptor a nested container has no
    // image for (existing TODO, expand_p_scalar), a limitation orthogonal to
    // which types are legal KEYS.
    if (kst == nullptr) {
        efrag += "%0d";
        // A wildcard index (`[*]`) has no slang type; its keys are unsigned.
        eargs.push_back({ b.unary(T::Load, key.lv(b, l), l), false });
    } else if (is_agg_abi_ref(kr)) {
        expand_p_lvalue(key.lv(b, l), kr, kst, efrag, eargs, b, l);
    } else {
        expand_p_scalar(b.unary(T::Load, key.lv(b, l), l), *kst, efrag, eargs, b, l);
    }
    efrag += ':';
    expand_p_lvalue(b.assoc_select(lv, key_operand(), ty, l), t.element, est, efrag, eargs, b, l);
    append(b.sformatf(efrag, eargs, l));
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index ok_step_lv = ok.lv(b, l);
    const ir::Index ok_next    = b.convert(b.assoc_traverse(lv, key.lv(b, l), Step::Next, ty, l), bit1, false, l);
    b.binary(T::Store, ok_step_lv, ok_next, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
    append(str_const("}"));
    return b.unary(T::Load, out.lv(b, l), l);
}

// Offset AND length of the first `%p` / `%0p` in `fmt` that is a real conversion,
// skipping any `%%` escape. `%%p` is a literal percent followed by the letter p, so
// its `%p` substring must not be spliced over — doing so both corrupts the escape
// and leaves the actual specifier behind for the runtime formatter to reject.
// Fragments already spliced for earlier `%p` args never contain a bare `%p`, so
// scanning from the start always lands on the pending one.
//
// The length is returned rather than assumed because the token is 2 characters for
// `%p` and 3 for the `%0p` shorter form; splicing a fixed 2 over the latter would
// leave a stray `p` in the output.
std::pair<size_t, size_t> Lowerer::find_p_spec(std::string_view fmt) {
    for (size_t i = 0; i + 1 < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            continue;
        }
        if (fmt[i + 1] == '%') {
            ++i; // consume the escape, so its second % cannot start a match
            continue;
        }
        const clotho::FormatSpec spec = clotho::parse_format_spec(fmt.substr(i + 1));
        if (spec.ok && spec.conv == 'p') {
            return { i, 1 + spec.len };
        }
    }
    return { std::string::npos, 0 };
}

// `lead_text` is format text that is not one of `format_args`: the constant a
// $sformat / $sformatf format expression folded to (see const_format_text).
// It is consumed exactly as a string-literal argument would be, so a folded
// format gets the same specifier validation, %m / %l splicing and %p
// expansion, and queues its specifiers for the value args that follow.
void Lowerer::build_format(std::span<const Expression* const> format_args,
                           std::string&                       fmt,
                           std::vector<ir::FmtArg>&           args,
                           ir::Builder&                       b,
                           char                               default_conv,
                           std::string_view                   lead_text) {
    // Conversion chars (lowercase) of specifiers seen but not yet matched to
    // a value arg, in order (FIFO). Needed so a `%t` argument can be scaled
    // to ticks and a string object checked against %s.
    std::vector<char> pending;
    size_t            next = 0; // front of `pending`

    if (!lead_text.empty()) {
        append_format_literal(fmt, lead_text, pending);
    }
    for (const Expression* arg : format_args) {
        if (arg->kind == ExpressionKind::EmptyArgument) {
            // An empty argument (adjacent commas) displays one space
            // (LRM §21.2.1).
            fmt += ' ';
            continue;
        }
        // A string-literal argument (SV-style) contributes format text, even
        // when width-converted; unwrap conversions to see the literal. A
        // literal that an earlier specifier is still waiting on is a value
        // argument instead ($display("%s", "hi") prints hi).
        const Expression* lit = arg;
        while (lit->kind == ExpressionKind::Conversion) {
            lit = &lit->as<ConversionExpression>().operand();
        }
        if (lit->kind == ExpressionKind::StringLiteral && next >= pending.size()) {
            append_format_literal(fmt, lit->as<StringLiteral>().getValue(), pending);
        } else {
            const bool str  = is_string_expr(*arg);
            const bool fl   = arg->type->isFloating();
            char       conv = str ? 's' : (fl ? 'g' : default_conv);
            if (next < pending.size()) {
                conv = pending[next++]; // consumed by an earlier specifier
            } else if (fl) {
                // Bare real: the "default decimal format" (LRM §21.2.1.1)
                // is unpinned for reals; %#g (6 significant digits,
                // trailing zeros kept) matches iverilog byte for byte.
                fmt += "%#g";
            } else {
                // Bare value default: the task's radix, auto-sized
                // (LRM §21.2.1.1); a string object formats as a string.
                fmt += '%';
                fmt += str ? 's' : default_conv;
            }
            if (conv == 'p') {
                // Expand the aggregate operand into `'{...}` text plus a per-leaf
                // value spec (LRM §21.2.1.6), splicing it in place of the `%p`
                // token. The leaf args are pushed here, in the same order the
                // spliced specs appear, so positional matching stays correct.
                std::string frag;
                expand_p_arg(*arg, frag, args, b);
                const auto [pos, len] = find_p_spec(fmt);
                if (pos != std::string::npos) {
                    fmt.replace(pos, len, frag);
                }
                continue;
            }
            if (str && conv != 's') {
                // TODO: %d/%h/... of a string object (would need the packed
                // reinterpretation of its characters).
                fail("a string argument to $display/$write requires %s");
            }
            const ir::SourceLoc al = loc(*arg);
            const bool          real_conv =
              conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G';
            ir::Index v = lower_expr(*arg, b);
            // Whether the decimal conversions print `v` as a signed number: the
            // argument's own signedness, unless it is converted below.
            bool v_signed = arg->type->isSigned();
            if (fl && !real_conv) {
                switch (conv) {
                    case 'd':
                    case 'h':
                    case 'x': // check_specifier keeps %x/%X as a hex alias
                    case 'o':
                    case 'b':
                        // A real under %d/%h/%x/%o/%b prints its rounded
                        // integer value in the requested base: Table 21-1
                        // gives these specifiers a common integral operand,
                        // and LRM 21.2.1.2 converts a real argument to that
                        // operand by the LRM 6.12.1 real-to-integer rule
                        // (round half away from zero). Only the base
                        // differs, so width/zero-padding/sign stay on the
                        // shared integer path below. Signed 64-bit keeps
                        // negatives printing as two's complement (matches
                        // iverilog: -3.7 under %h is fffffffffffffffc).
                        v        = b.float_cvt(ir::Inst::Tag::FtoI, v, make_int_type(64, false), al);
                        v_signed = true;
                        break;
                    case 't':
                        break; // scale_to_ticks below has the real path
                    default:
                        // TODO: %c of a real (would need the rounded integer
                        // truncated to a character code), and %u / %z of a
                        // real (unformatted raw bits, LRM 21.2.1.3).
                        fail(std::string("%") + conv + " with a real argument is not supported");
                }
            } else if (!fl && real_conv) {
                // An integral argument under %e/%f/%g converts to real
                // (LRM 6.12.1) so the runtime formats one representation.
                v = b.float_cvt(
                  arg->type->isSigned() ? ir::Inst::Tag::SItoF : ir::Inst::Tag::UItoF, v, module_.pool.real_type, al);
            }
            if (conv == 't') {
                // The scaled tick count is a 64-bit value with the operand's own sign
                // (scale_to_ticks); a real operand's count is signed.
                v        = scale_to_ticks(*arg, v, b);
                v_signed = fl || arg->type->isSigned();
            }
            args.push_back({ v, v_signed });
        }
    }
    if (next < pending.size()) {
        fail("display format has more format specifiers than arguments (LRM 21.2.1.1)");
    }
}

// Match a display-family task name against `base` plus an optional radix
// suffix ($displayb / $writeh / ...); returns the bare-argument default
// conversion ('d' for the plain name, the suffix's radix otherwise) or 0
// when the name does not match (LRM §21.2.1.1).
char Lowerer::display_radix(std::string_view name, std::string_view base) {
    if (!name.starts_with(base)) {
        return 0;
    }
    const std::string_view rest = name.substr(base.size());
    if (rest.empty()) {
        return 'd';
    }
    if (rest == "b" || rest == "o" || rest == "h") {
        return rest[0];
    }
    return 0;
}

// Lower $display / $write: format the arguments and write to standard output.
// Build the $display-style message from `msg_args` and emit a severity report
// (LRM 20.10 / 16.3) at `level`. Non-terminating; $fatal adds a finish after.
void Lowerer::emit_severity(ir::Severity                       level,
                            const std::string&                 banner,
                            std::span<const Expression* const> msg_args,
                            const CallExpression&              call,
                            ir::Builder&                       b) {
    std::string            fmt;
    std::vector<ir::FmtArg> args;
    build_format(msg_args, fmt, args, b);
    b.severity(static_cast<uint32_t>(level), banner, fmt, args, loc(call));
}

// Lower a severity system task: $info / $warning / $error / $fatal (LRM
// 20.10). Each renders an optional $display-style message tail. $fatal's first
// argument is the $finish verbosity level (a constant 0/1/2), the rest the
// message; it reports, then terminates via $finish(level).
void Lowerer::lower_severity_task(const CallExpression& call, std::string_view name, ir::Builder& b) {
    const std::string banner = severity_banner(call.sourceRange.start());
    const auto        args   = call.arguments();
    if (name == "$fatal") {
        uint32_t                           level = 1;
        std::span<const Expression* const> msg   = args;
        if (!args.empty()) {
            const slang::ConstantValue cv = fold_const(*args[0], Fold::Required);
            if (cv.isInteger()) {
                level = cv.integer().as<uint32_t>().value_or(1);
            }
            msg = args.subspan(1);
        }
        emit_severity(ir::Severity::Fatal, banner, msg, call, b);
        b.finish(finish_banner(call, "$fatal"), level, loc(call));
        return;
    }
    const ir::Severity sev = name == "$info"      ? ir::Severity::Info
                             : name == "$warning" ? ir::Severity::Warning
                                                  : ir::Severity::Error;
    emit_severity(sev, banner, args, call, b);
}

void Lowerer::lower_display(const CallExpression& call, bool newline, ir::Builder& b, char default_conv) {
    std::string            fmt;
    std::vector<ir::FmtArg> args;
    build_format(call.arguments(), fmt, args, b, default_conv);
    b.display(fmt, newline, args, loc(call));
}

// Lower $strobe / $strobeb / $strobeo / $strobeh (LRM 21.2.2): unlike
// $display, the argument list is not evaluated here — it is compiled into
// a synthesized task (build_strobe_body) whose body evaluates it, run once
// at the Postponed region of the current time step (LRM: "That time is the
// end of the current simulation time, when all the simulation events have
// occurred for that simulation time, just before simulation time is
// advanced"). The call site just pends that task.
void Lowerer::lower_strobe(const CallExpression& call, ir::Builder& b, char default_conv, bool fd_arg) {
    if (fn_ctx_ != nullptr && !fn_ctx_->is_task) {
        // The deferred body is ramped through the caller's task-call record
        // slot, and record_slot() anchors on coro_begin_ -- a plain function
        // is not a coroutine and has neither. (The clock itself is reachable
        // here: the wheel is a global, so $time in a function works.)
        // TODO: a record slot for a non-coroutine body would lift this.
        fail("$strobe inside a function is not supported (no record slot to ramp "
             "the deferred body)");
    }
    const ir::FunctionId fid =
      build_deferred_body(call.arguments(), default_conv, loc(call), fd_arg, /*monitor=*/false);
    b.strobe_pend(fid, loc(call));
}

// Lower $monitor / $monitorb/o/h and $fmonitor (LRM 21.2.2): register a single
// persistent monitor. Unlike $strobe (fire-once), the monitor body re-runs at
// the Postponed region of EVERY subsequent time step and prints only when its
// rendered output changed (runtime change-detection, rt_monitor_emit). A new
// $monitor replaces the previous; $monitoron/$monitoroff enable/disable.
void Lowerer::lower_monitor(const CallExpression& call, ir::Builder& b, char default_conv, bool fd_arg) {
    if (fn_ctx_ != nullptr && !fn_ctx_->is_task) {
        fail("$monitor inside a function is not supported (no record slot to ramp "
             "the deferred body)");
    }
    const ir::FunctionId fid =
      build_deferred_body(call.arguments(), default_conv, loc(call), fd_arg, /*monitor=*/true);
    b.monitor_set(fid, loc(call));
}

// Lower $fdisplay / $fwrite (LRM §21.3.3): the first argument is the
// destination descriptor (an MCD or FD); the rest form the message.
void Lowerer::lower_fdisplay(const CallExpression& call, bool newline, ir::Builder& b, char default_conv) {
    const auto args_in = call.arguments();
    if (args_in.empty()) {
        fail("$fdisplay / $fwrite requires a file-descriptor argument");
    }
    const ir::Index        fd = lower_expr(*args_in[0], b);
    std::string            fmt;
    std::vector<ir::FmtArg> args;
    build_format(args_in.subspan(1), fmt, args, b, default_conv);
    b.fdisplay(fd, fmt, newline, args, loc(call));
}

// The constant text of a $sformat / $sformatf format argument, when it has
// one. LRM §21.3.3 makes these two the exception to the literal-format rule
// ("The only exception is $sformat, whose second argument can be an
// expression of string data type"), but most formats that are not literals
// are still CONSTANT: the shape that motivated this is a concatenation of
// string literals (Accellera UVM uvm_phase.svh:1963 builds its jump-failure
// message that way), and a `localparam string` or a stringified macro is the
// same situation. Folding one recovers the whole compile-time path --
// specifier validation, %m / %l splicing, %p expansion, a baked format global
// -- and leaves nothing to parse at run time.
//
// A concatenation of string literals has type bit[N] (LRM 5.9), not string, so
// the folded value arrives as an integral whose bytes are the text.
// convertToStr() is slang's LRM 6.16 conversion: 8-bit chunks most
// significant first, a zero chunk dropped, and a short leading chunk when N is
// not a multiple of 8. That is the same rule rt_string_from_packed applies at
// run time, so a folded format and a computed one that hold the same bits
// agree. It also answers the leading/trailing NUL question outright: a NUL
// byte is not part of the string, so `{"ab", 8'h00, "cd"}` folds to "abcd".
std::optional<std::string> Lowerer::const_format_text(const Expression& e) {
    const slang::ConstantValue cv = fold_const(e, Fold::Runtime).convertToStr();
    if (!cv) {
        return std::nullopt;
    }
    return cv.str();
}

// The noun a refusal names an argument type by. Only the kinds a runtime
// format cannot describe reach here -- rt_arg carries exactly one integral,
// real or string value -- so the list is the complement of that.
std::string_view Lowerer::value_type_noun(const slang::ast::Type& t) {
    if (t.isUnpackedStruct()) {
        return "an unpacked struct";
    }
    if (t.isUnpackedUnion()) {
        return "an unpacked union";
    }
    if (t.isQueue()) {
        return "a queue";
    }
    if (t.isAssociativeArray()) {
        return "an associative array";
    }
    if (t.isDynamicallySizedArray()) {
        return "a dynamic array";
    }
    if (t.isUnpackedArray()) {
        return "a fixed unpacked array";
    }
    if (t.isClass()) {
        return "a class handle";
    }
    if (t.isCHandle()) {
        return "a chandle";
    }
    if (t.isEvent()) {
        return "an event";
    }
    if (t.isVirtualInterface()) {
        return "a virtual interface handle";
    }
    return "neither an integral, a real nor a string";
}

// Lower the format expression and the value arguments of a $sformat /
// $sformatf whose format only exists at run time. Yields the format's string
// value; the value args are appended to `args` raw.
//
// Raw, because which specifier an argument will meet is not knowable here --
// that is the whole difference from build_format, which reads the conversion
// out of the format text and fixes the argument up in the IR (%t scaling, the
// real/integral conversion, %p expansion). All of that moves to the runtime
// renderer, which sees both at once; rt_fmt_dyn carries the one call-site fact
// it cannot derive, the tick multiplier.
//
// `first_pos` is the 1-based call-argument position of value_args[0], so a
// refusal can name the argument the user wrote.
ir::Index Lowerer::lower_dynamic_format(const Expression&                  fmt_expr,
                                        std::span<const Expression* const> value_args,
                                        size_t                             first_pos,
                                        std::vector<ir::FmtArg>&           args,
                                        ir::Builder&                       b,
                                        std::string_view                   who) {
    if (!is_string_expr(fmt_expr) && !fmt_expr.type->isIntegral()) {
        // slang admits any canBeStringLike format operand, which leaves one
        // shape with no conversion to format text here: a byte array (an
        // unpacked array of bytes), whose characters live one per element
        // rather than packed.
        // TODO: a byte-array / byte-queue $sformat format. It would lower as a
        // stream to a string (LRM 11.4.14) before reaching the renderer.
        fail(std::string(who) + " format is " +
             std::string(value_type_noun(fmt_expr.type->getCanonicalType())) +
             "; a runtime format must be a string or a packed value (LRM 21.3.3)");
    }
    // A format that is not a `string` is still string-like: convert it by the
    // LRM 6.16 rule, exactly the conversion const_format_text applies to a
    // constant one, so the same source shape renders the same text whether or
    // not it folds.
    const ir::Index fv =
      is_string_expr(fmt_expr)
        ? lower_expr(fmt_expr, b)
        : b.unary(ir::Inst::Tag::StrFromPacked, lower_expr(fmt_expr, b), loc(fmt_expr));

    for (size_t k = 0; k < value_args.size(); ++k) {
        const Expression& arg = *value_args[k];
        const std::string at  = std::string(who) + " argument " + std::to_string(first_pos + k);
        if (arg.kind == ExpressionKind::EmptyArgument) {
            // An empty argument (adjacent commas) contributes a space to the
            // FORMAT (LRM 21.2.1), which only exists here when the format text
            // is known. With a runtime format there is nothing to contribute
            // to, and the argument consumes no specifier either.
            // TODO: an empty argument against a runtime format string. It
            // would need a per-argument "renders as a space, consumes no
            // specifier" marker in rt_arg for the renderer to honor.
            fail(at + " is empty, which a runtime format string cannot render: an "
                      "empty argument contributes format text, and there is no "
                      "format text here to contribute to");
        }
        const slang::ast::Type& ct = arg.type->getCanonicalType();
        if (!ct.isIntegral() && !ct.isFloating() && !ct.isString()) {
            // TODO: an aggregate or container argument against a runtime format
            // string. Clotho already has runtime container descriptors for %p
            // of a queue / dynamic array (ir::Inst::Tag::ContainerFmtP ->
            // rt_container_fmt_p), so a future version could pass a type
            // descriptor alongside each rt_arg and let the renderer walk the
            // value when it turns out to meet a %p. Refused rather than
            // approximated: rt_arg has no image of an aggregate at all, so the
            // alternative is silently wrong text.
            fail(at + " is " + std::string(value_type_noun(ct)) +
                 ", which a runtime format string cannot render: the runtime argument "
                 "descriptor carries one integral, real or string value, and with a "
                 "format that is not known until run time the frontend cannot tell "
                 "which specifier the argument will meet, so it cannot expand %p "
                 "either (LRM 21.2.1.6)");
        }
        args.push_back({ lower_expr(arg, b), arg.type->isSigned() });
    }
    return fv;
}

// Lower $sformat / $swrite (LRM §21.3.3): render the trailing arguments like
// $write and store the result into the destination string lvalue (the first
// argument). $swrite takes $write-style interleaved literals and values.
//
// $sformat's format is its second argument, and LRM §21.3.3 exempts it from
// the literal-format rule the rest of the format tasks obey. Three cases, in
// decreasing order of what is known: a literal, a constant expression (folded
// by const_format_text onto the same path), and a genuinely runtime string.
void Lowerer::lower_sformat(const CallExpression& call, bool is_sformat, ir::Builder& b, char default_conv) {
    const auto args = call.arguments();
    if (args.empty()) {
        fail("$sformat / $swrite requires a destination string argument");
    }
    std::string            fmt;
    std::vector<ir::FmtArg> values;
    if (!is_sformat) {
        const ir::Index dst = lower_lvalue(*args[0], b);
        build_format(args.subspan(1), fmt, values, b, default_conv);
        b.sformat(dst, fmt, values, loc(call));
        return;
    }
    if (args.size() < 2) {
        fail("$sformat requires a format-string argument");
    }
    const Expression* lit = args[1];
    while (lit->kind == ExpressionKind::Conversion) {
        lit = &lit->as<ConversionExpression>().operand();
    }
    const ir::Index dst = lower_lvalue(*args[0], b);
    if (lit->kind == ExpressionKind::StringLiteral) {
        build_format(args.subspan(1), fmt, values, b, default_conv);
        b.sformat(dst, fmt, values, loc(call));
        return;
    }
    if (const std::optional<std::string> text = const_format_text(*args[1])) {
        build_format(args.subspan(2), fmt, values, b, default_conv, *text);
        b.sformat(dst, fmt, values, loc(call));
        return;
    }
    const ir::Index fv =
      lower_dynamic_format(*args[1], args.subspan(2), 3, values, b, "$sformat");
    b.sformat_dyn(dst, fv, values, time_mul_, loc(call));
}

// Lower $sformatf (LRM §21.3.3): render like $sformat but yield a new string
// value. Its format is the first argument and takes the same three cases.
ir::Index Lowerer::lower_sformatf(const CallExpression& call, ir::Builder& b) {
    const auto args = call.arguments();
    if (args.empty()) {
        fail("$sformatf requires a format-string argument");
    }
    const Expression* lit = args[0];
    while (lit->kind == ExpressionKind::Conversion) {
        lit = &lit->as<ConversionExpression>().operand();
    }
    std::string            fmt;
    std::vector<ir::FmtArg> values;
    if (lit->kind == ExpressionKind::StringLiteral) {
        build_format(args, fmt, values, b);
        return b.sformatf(fmt, values, loc(call));
    }
    if (const std::optional<std::string> text = const_format_text(*args[0])) {
        build_format(args.subspan(1), fmt, values, b, 'd', *text);
        return b.sformatf(fmt, values, loc(call));
    }
    const ir::Index fv =
      lower_dynamic_format(*args[0], args.subspan(1), 2, values, b, "$sformatf");
    return b.sformatf_dyn(fv, values, time_mul_, loc(call));
}

} // namespace clotho::lower
