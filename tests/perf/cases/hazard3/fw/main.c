// Deterministic compute kernel for the Hazard3 perf testbench.
//
// Exercises the integer core plus the M extension (multiply and the sequential
// divider, hazard3_muldiv_seq) over a fixed iteration count, folds the results
// into a checksum, and prints it. Output is byte-identical across runs, so it
// doubles as the correctness oracle: clotho must print the
// same checksum as iverilog.

#define IO ((volatile unsigned int *)0xc0000000u)
#define IO_PRINT_CHAR 0
#define IO_PRINT_U32  1

static void put_char(char c) { IO[IO_PRINT_CHAR] = (unsigned char)c; }

static void put_str(const char *s) {
	while (*s)
		put_char(*s++);
}

static void put_u32(unsigned int x) { IO[IO_PRINT_U32] = x; } // prints %08x\n

#ifndef ITERS
#define ITERS 20000u
#endif

int main(void) {
	put_str("hazard3 perf tb\n");

	unsigned int h = 0x12345678u;
	unsigned int acc = 0u;
	for (unsigned int i = 1u; i <= ITERS; i++) {
		h = h * 1664525u + 1013904223u;   // LCG step (multiply)
		h ^= h >> 15;
		acc += h / i;                     // sequential divide
		acc ^= h % (i + 7u);              // and remainder
		h = (h << 7) | (h >> 25);         // rotate
	}

	put_str("checksum=");
	put_u32(h ^ acc);
	return 0;
}
