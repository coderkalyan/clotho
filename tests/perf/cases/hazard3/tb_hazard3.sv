// Self-contained SystemVerilog testbench for the Hazard3 RISC-V core.
//
// Hazard3's firmware harness is C++ (CXXRTL / Verilator) driving a synthesized
// DUT, with the clock, memory, MMIO and firmware load all living in C++
// (test/sim/tb_common/*.cpp). This file reimplements that harness in
// SystemVerilog so an event-driven simulator (clotho, iverilog) can run firmware
// end to end. The hazard3_cpu_2port instance and its port connections are
// adapted from Hazard3's test/sim/tb_common/hdl/tb.v at
// Wren6991/Hazard3@8af992930f71 (Apache-2.0).
//
// It models the same zero-wait-state AHB-Lite subordinate as
// tb_common/tb_memio.cpp: hready tied high, a one-cycle address->data phase
// pipeline, byte-lane writes, word-aligned reads, and the same MMIO block at
// 0xc0000000 (print_char / print_u32 / exit) plus the mtime timer. The core is
// instantiated directly (hazard3_cpu_2port) with the debug/JTAG subsystem tied
// off, since a firmware run needs no debugger.

`default_nettype none

module tb_hazard3;

	// Testbench IO / memory layout (matches tb_common/include/tb_constants.h).
	localparam [31:0] MEM_BASE   = 32'h8000_0000;
	localparam        MEM_WORDS  = 32'h0004_0000 / 4;   // 256 KiB
	localparam [31:0] IO_BASE    = 32'hc000_0000;
	localparam [31:0] IO_PRINT_CHAR = IO_BASE + 32'h000;
	localparam [31:0] IO_PRINT_U32  = IO_BASE + 32'h004;
	localparam [31:0] IO_EXIT       = IO_BASE + 32'h008;
	localparam [31:0] IO_MTIME      = IO_BASE + 32'h100;
	localparam [31:0] IO_MTIMEH     = IO_BASE + 32'h104;
	localparam [31:0] IO_MTIMECMP   = IO_BASE + 32'h108;
	localparam [31:0] IO_MTIMECMPH  = IO_BASE + 32'h10c;

	// Watchdog: firmware exits via IO_EXIT well before this. Hitting it means a
	// hang, which the check regex treats as a failure (no "EXIT" line printed).
	localparam integer MAX_CYCLES = 20_000_000;

	// Hazard3 configuration (all extensions), pulled in as module-body
	// localparams so the config-dependent widths below (NUM_IRQS) resolve.
`include "config.vh"

	// Bus widths: config.vh (the user-knob header) omits these, so provide the
	// defaults the config-inst include maps through (stock tb.v does the same
	// via its own W_DATA/W_ADDR module params).
	localparam W_ADDR = 32;
	localparam W_DATA = 32;

	// ------------------------------------------------------------------
	// Clock and reset

	reg clk = 1'b0;
	reg rst_n = 1'b0;
	always #5 clk = ~clk;

	initial begin
		rst_n = 1'b0;
		repeat (4) @(posedge clk);
		rst_n <= 1'b1;
	end

	// ------------------------------------------------------------------
	// Core <-> bus wiring

	wire [31:0] i_haddr, i_hwdata, i_hrdata;
	wire        i_hwrite, i_hmastlock;
	wire [1:0]  i_htrans;
	wire [2:0]  i_hsize, i_hburst;
	wire [3:0]  i_hprot;
	wire [7:0]  i_hmaster;

	wire [31:0] d_haddr, d_hwdata, d_hrdata;
	wire        d_hwrite, d_hmastlock, d_hexcl;
	wire [1:0]  d_htrans;
	wire [2:0]  d_hsize, d_hburst;
	wire [3:0]  d_hprot;
	wire [7:0]  d_hmaster;

	wire        i_hready, i_hresp;
	wire        d_hready, d_hresp, d_hexokay;

	wire        pwrup_req, clk_en, unblock_out;
	wire        fence_i_vld, fence_d_vld;

	reg  [NUM_IRQS-1:0] irq = {NUM_IRQS{1'b0}};
	reg                 soft_irq = 1'b0;
	wire                timer_irq;

	// ------------------------------------------------------------------
	// Unified memory + MMIO, one zero-wait-state AHB-Lite slave per port.

	reg [31:0] mem [0:MEM_WORDS-1];

	// mtime free-running counter (increments every cycle, as in tb_memio.cpp).
	reg [63:0] mtime = 64'd0;
	reg [63:0] mtimecmp = 64'd0;
	assign timer_irq = mtime >= mtimecmp;

	// Both ports are always ready; no bus errors are modelled (firmware stays
	// in mapped memory / MMIO).
	assign i_hready = 1'b1;
	assign i_hresp  = 1'b0;
	assign d_hready = 1'b1;
	assign d_hresp  = 1'b0;
	assign d_hexokay = 1'b1;

	// Address-phase captured into data phase (valid when HTRANS is
	// NONSEQ/SEQ, i.e. htrans[1]).
	reg        i_dph_vld;
	reg [31:0] i_dph_addr;
	reg        d_dph_vld, d_dph_write;
	reg [31:0] d_dph_addr;
	reg [2:0]  d_dph_size;

	function [31:0] mem_word (input [31:0] byte_addr);
		mem_word = mem[(byte_addr - MEM_BASE) >> 2];
	endfunction

	function is_mem (input [31:0] a);
		is_mem = a >= MEM_BASE && a < MEM_BASE + MEM_WORDS * 4;
	endfunction

	// Instruction port: read-only. HRDATA is combinational on the latched
	// data-phase address (one-cycle latency).
	assign i_hrdata = i_dph_vld && is_mem(i_dph_addr) ? mem_word(i_dph_addr) : 32'h0;

	always @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			i_dph_vld  <= 1'b0;
			i_dph_addr <= 32'h0;
		end else if (i_hready) begin
			i_dph_vld  <= i_htrans[1];
			i_dph_addr <= i_haddr;
		end
	end

	// Data port: reads from memory or the MMIO block; writes go to memory
	// (byte lanes) or trigger MMIO side effects.
	reg [31:0] d_rdata_r;
	assign d_hrdata = d_rdata_r;

	always @(*) begin
		d_rdata_r = 32'h0;
		if (d_dph_vld && !d_dph_write) begin
			if (is_mem(d_dph_addr))
				d_rdata_r = mem_word(d_dph_addr);
			else case (d_dph_addr)
				IO_MTIME:     d_rdata_r = mtime[31:0];
				IO_MTIMEH:    d_rdata_r = mtime[63:32];
				IO_MTIMECMP:  d_rdata_r = mtimecmp[31:0];
				IO_MTIMECMPH: d_rdata_r = mtimecmp[63:32];
				default:      d_rdata_r = 32'h0;
			endcase
		end
	end

	always @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			d_dph_vld   <= 1'b0;
			d_dph_write <= 1'b0;
			d_dph_addr  <= 32'h0;
			d_dph_size  <= 3'h0;
		end else if (d_hready) begin
			d_dph_vld   <= d_htrans[1];
			d_dph_write <= d_hwrite;
			d_dph_addr  <= d_haddr;
			d_dph_size  <= d_hsize;
		end
	end

	// Data-phase write commit (memory byte lanes + MMIO).
	integer k;
	reg [31:0] nw;
	reg [1:0]  boff;
	reg [2:0]  nbytes;
	always @(posedge clk) begin
		if (rst_n && d_dph_vld && d_dph_write) begin
			if (is_mem(d_dph_addr)) begin
				boff   = d_dph_addr[1:0];
				nbytes = 3'h1 << d_dph_size;
				nw     = mem[(d_dph_addr - MEM_BASE) >> 2];
				for (k = 0; k < 4; k = k + 1)
					if (k >= boff && k < boff + nbytes)
						nw[k*8 +: 8] = d_hwdata[(k - boff)*8 +: 8];
				mem[(d_dph_addr - MEM_BASE) >> 2] <= nw;
			end else case (d_dph_addr)
				IO_PRINT_CHAR: $write("%c", d_hwdata[7:0]);
				IO_PRINT_U32:  $write("%08x\n", d_hwdata);
				IO_EXIT: begin
					$write("\n*** EXIT %0d ***\n", d_hwdata);
					$finish;
				end
				IO_MTIME:      mtime[31:0]     <= d_hwdata;
				IO_MTIMEH:     mtime[63:32]    <= d_hwdata;
				IO_MTIMECMP:   mtimecmp[31:0]  <= d_hwdata;
				IO_MTIMECMPH:  mtimecmp[63:32] <= d_hwdata;
				default: ;
			endcase
		end
	end

	// mtime advances one tick per cycle (matches tb_memio.cpp step()). A write
	// in the same cycle wins via the block above (NBA ordering).
	always @(posedge clk) begin
		if (rst_n)
			mtime <= mtime + 64'd1;
	end

	// Watchdog.
	reg [31:0] cycle_ctr = 32'h0;
	always @(posedge clk) begin
		if (rst_n) begin
			cycle_ctr <= cycle_ctr + 1;
			if (cycle_ctr >= MAX_CYCLES) begin
				$write("\n*** TIMEOUT ***\n");
				$finish;
			end
		end
	end

	// ------------------------------------------------------------------
	// Firmware load

	initial $readmemh("firmware.hex", mem);

	// ------------------------------------------------------------------
	// Core instance (debug/JTAG tied off).

	hazard3_cpu_2port #(
`include "hazard3_config_inst.vh"
	) cpu (
		.clk                        (clk),
		.clk_always_on              (clk),
		.rst_n                      (rst_n),

		.pwrup_req                  (pwrup_req),
		.pwrup_ack                  (1'b1),
		.clk_en                     (clk_en),
		.unblock_out                (unblock_out),
		.unblock_in                 (unblock_out),

		.i_haddr                    (i_haddr),
		.i_hwrite                   (i_hwrite),
		.i_htrans                   (i_htrans),
		.i_hsize                    (i_hsize),
		.i_hburst                   (i_hburst),
		.i_hprot                    (i_hprot),
		.i_hmastlock                (i_hmastlock),
		.i_hmaster                  (i_hmaster),
		.i_hready                   (i_hready),
		.i_hresp                    (i_hresp),
		.i_hwdata                   (i_hwdata),
		.i_hrdata                   (i_hrdata),

		.d_haddr                    (d_haddr),
		.d_hwrite                   (d_hwrite),
		.d_htrans                   (d_htrans),
		.d_hsize                    (d_hsize),
		.d_hburst                   (d_hburst),
		.d_hprot                    (d_hprot),
		.d_hmastlock                (d_hmastlock),
		.d_hmaster                  (d_hmaster),
		.d_hexcl                    (d_hexcl),
		.d_hready                   (d_hready),
		.d_hresp                    (d_hresp),
		.d_hexokay                  (d_hexokay),
		.d_hwdata                   (d_hwdata),
		.d_hrdata                   (d_hrdata),

		.fence_i_vld                (fence_i_vld),
		.fence_d_vld                (fence_d_vld),
		.fence_rdy                  (1'b1),

		.dbg_req_halt               (1'b0),
		.dbg_req_halt_on_reset      (1'b0),
		.dbg_req_resume             (1'b0),
		.dbg_halted                 (),
		.dbg_running                (),
		.dbg_data0_rdata            (32'h0),
		.dbg_data0_wdata            (),
		.dbg_data0_wen              (),
		.dbg_instr_data             (32'h0),
		.dbg_instr_data_vld         (1'b0),
		.dbg_instr_data_rdy         (),
		.dbg_instr_caught_exception (),
		.dbg_instr_caught_ebreak    (),
		.dbg_sbus_addr              (32'h0),
		.dbg_sbus_write             (1'b0),
		.dbg_sbus_size              (2'h0),
		.dbg_sbus_vld               (1'b0),
		.dbg_sbus_rdy               (),
		.dbg_sbus_err               (),
		.dbg_sbus_wdata             (32'h0),
		.dbg_sbus_rdata             (),

		.mhartid_val                (32'h0),
		.eco_version                (4'ha),

		.irq                        (irq),
		.soft_irq                   (soft_irq),
		.timer_irq                  (timer_irq)
	);

endmodule

`default_nettype wire
