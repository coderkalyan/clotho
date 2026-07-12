// Copyright 2018 ETH Zurich and University of Bologna.
// Copyright and related rights are licensed under the Solderpad Hardware
// License, Version 0.51 (the "License"); you may not use this file except in
// compliance with the License.  You may obtain a copy of the License at
// http://solderpad.org/licenses/SHL-0.51. Unless required by applicable law
// or agreed to in writing, software, hardware and materials distributed under
// this License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//                                                                                                          //
// Author:                              Francesco Minervini - minervif@student.ethz.ch                      //
//                                                                                                          //
// Additional contributions by:         Davide Schiavone - pschiavo@iis.ee.ethz.ch                          //
//                                      Andrea Bettati - andrea.bettati@studenti.unipr.it                   //
// Design Name:                         Interrupt generator                                                 //
// Project Name:                        RI5CY, Zeroriscy                                                    //
// Language:                            SystemVerilog                                                       //
//                                                                                                          //
// Description:                         Send interrupt request to core choosing one option between:         //
//                                      - Random;                                                           //
//                                      - Standard;                                                         //
//                                      - PC value-triggering                                               //
//                                                                                                          //
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Modified by Kalyan Sriram for Clotho, 2026: the generator is replaced by a stub
// that keeps only the port list and ties every output inactive; CoreMark needs no
// interrupts. Original: openhwgroup/cv32e40p@6033d2b1be32,
// example_tb/core/cv32e40p_random_interrupt_generator.sv

module cv32e40p_random_interrupt_generator (
    input  logic        rst_ni,
    input  logic        clk_i,
    input  logic        irq_i,
    input  logic        irq_ack_i,
    output logic [18:0] irq_rnd_lines_o,
    output logic        irq_ack_o,
    input  logic [31:0] irq_mode_i,
    input  logic [31:0] irq_min_cycles_i,
    input  logic [31:0] irq_max_cycles_i,
    input  logic [31:0] irq_min_id_i,
    input  logic [31:0] irq_max_id_i,
    output logic [31:0] irq_act_id_o,
    output logic        irq_id_we_o,
    input  logic [31:0] irq_pc_id_i,
    input  logic [31:0] irq_pc_trig_i,
    input  logic [31:0] irq_lines_i
);
  assign irq_rnd_lines_o = 19'b0;
  assign irq_ack_o       = 1'b0;
  assign irq_act_id_o    = 32'b0;
  assign irq_id_we_o     = 1'b0;
endmodule
