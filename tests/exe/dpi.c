// The C side of tests/exe/dpi.sv.

// svdpi.h's svLogicVecVal, one bit per aval / bval pair: (0,0) 0, (1,0) 1,
// (0,1) z, (1,1) x.
typedef struct {
    unsigned aval;
    unsigned bval;
} logic_vec;

extern int  sv_twice(int x);
extern void sv_note(const logic_vec* v);

int c_add(int a, int b) {
    return a + b;
}

int c_calls_back(int x) {
    return sv_twice(x) + 1;
}

// Hands the design `8'bzx10_zx10`: an exported subroutine's logic input is the one
// way native code puts z into a design, and nothing in the design shows it.
void c_poke_unknown(void) {
    const logic_vec v = { 0x66, 0xcc };
    sv_note(&v);
}
