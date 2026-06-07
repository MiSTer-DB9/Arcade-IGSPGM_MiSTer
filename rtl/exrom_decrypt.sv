import system_consts::*;

// IGS027A external ARM ROM address-bit XOR ("svg" / MAME pgm_<game>_decrypt).
// Combinational, applied at ROM LOAD time (rtl/rom_loader.sv) to each 16-bit
// word of the external ARM ROM, keyed on the game and the 16-bit word index.
// (The runtime xor_table layer is separate and stays in the ARM read path.)
//
// MUST match the C++ DecryptArmExrom() in sim/games.cpp.
module exrom_decrypt(
    input  game_t       game,
    input  logic [21:0] word_idx,   // 16-bit word index within the external ROM
    input  logic [15:0] word_in,
    output logic [15:0] word_out
);

    // IGS27_CRYPT* primitives (MAME pgmcrypt.cpp): each returns 1 when the
    // corresponding output bit must be XORed for word index i.
    function automatic logic c1   (input logic [21:0] i); c1   = ((i & 22'h040480) != 22'h000080); endfunction
    function automatic logic c1a  (input logic [21:0] i); c1a  = ((i & 22'h040080) != 22'h000080); endfunction
    function automatic logic c1a2 (input logic [21:0] i); c1a2 = ((i & 22'h000480) != 22'h000080); endfunction
    function automatic logic c2   (input logic [21:0] i); c2   = ((i & 22'h104008) == 22'h104008); endfunction
    function automatic logic c2a  (input logic [21:0] i); c2a  = ((i & 22'h004008) == 22'h004008); endfunction
    function automatic logic c3   (input logic [21:0] i); c3   = ((i & 22'h080030) == 22'h080010); endfunction
    function automatic logic c3a2 (input logic [21:0] i); c3a2 = ((i & 22'h000030) == 22'h000010); endfunction
    function automatic logic c4   (input logic [21:0] i); c4   = ((i & 22'h000242) != 22'h000042); endfunction
    function automatic logic c4a  (input logic [21:0] i); c4a  = ((i & 22'h000042) != 22'h000042); endfunction
    function automatic logic c5   (input logic [21:0] i); c5   = ((i & 22'h008100) == 22'h008000); endfunction
    function automatic logic c5a  (input logic [21:0] i); c5a  = ((i & 22'h048100) == 22'h048000); endfunction
    function automatic logic c6   (input logic [21:0] i); c6   = ((i & 22'h002004) != 22'h000004); endfunction
    function automatic logic c6a  (input logic [21:0] i); c6a  = ((i & 22'h022004) != 22'h000004); endfunction
    function automatic logic c7   (input logic [21:0] i); c7   = ((i & 22'h011800) != 22'h010000); endfunction
    function automatic logic c7a  (input logic [21:0] i); c7a  = ((i & 22'h001800) != 22'h000000); endfunction
    function automatic logic c8   (input logic [21:0] i); c8   = ((i & 22'h004820) == 22'h004820); endfunction
    function automatic logic c8a  (input logic [21:0] i); c8a  = ((i & 22'h000820) == 22'h000820); endfunction

    function automatic logic [15:0] addr_xor(input game_t g, input logic [21:0] i);
        logic [15:0] m;
        begin
            m = 16'd0;
            case (g)
                GAME_KOV2:    m = {8'd0, c8a(i),c7a(i),c6a(i),c5a(i),c4a(i),c3(i),  1'b0,   c1a(i)};
                GAME_KOV2P:   m = {8'd0, c8a(i),c7(i), c6(i), c5(i), c4(i), c3(i),  c2a(i), c1a(i)};
                GAME_DDP2:    m = {8'd0, c8a(i),c7a(i),c6(i), c5(i), c4a(i),1'b0,   1'b0,   c1a2(i)};
                // martmast + dw2001 share pgm_mm_decrypt
                GAME_MARTMAST,
                GAME_DW2001:  m = {8'd0, c8a(i),c7(i), c6a(i),c5(i), c4(i), c3a2(i),c2a(i), c1(i)};
                GAME_DWPC:    m = {8'd0, c8(i), c7a(i),c6(i), c5a(i),c4a(i),c3(i),  c2(i),  c1a(i)};
                default:      m = 16'd0;
            endcase
            addr_xor = m;
        end
    endfunction

    assign word_out = word_in ^ addr_xor(game, word_idx);

endmodule
