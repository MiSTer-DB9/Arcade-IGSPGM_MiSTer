module IGS023_Buffer(
    input clk,

    input ce_pixel,
    input scan_active,
    input frame_reset,
    input next_line,

    output logic [11:0] scan_color,

    input wr,
    input [8:0] column,
    input [10:0] color,

    input [7:0] line,
    output line_writable
);

reg [8:0] scan_column;
reg [1:0] scan_buffer;

always_ff @(posedge clk) begin
    if (frame_reset) begin
        scan_buffer <= 2'b11;
    end else if (next_line) begin
        scan_buffer <= scan_buffer + 1;
        scan_column <= 0;
    end

    if (scan_active & ce_pixel) begin
        scan_column <= scan_column + 1;
    end
end

wire [1:0] erase_buffer = scan_buffer - 2'b01;
assign line_writable = (line[1:0] != scan_buffer) && (line[1:0] != erase_buffer);

logic [3:0] buf_wr;
logic [8:0] buf_addr[4];
logic [11:0] buf_data[4];
logic [11:0] buf_q[4];

singleport_ram #(.WIDTH(12), .WIDTHAD(9)) buf0( .clock(clk), .wren(buf_wr[0]), .address(buf_addr[0]), .data(buf_data[0]), .q(buf_q[0]));
singleport_ram #(.WIDTH(12), .WIDTHAD(9)) buf1( .clock(clk), .wren(buf_wr[1]), .address(buf_addr[1]), .data(buf_data[1]), .q(buf_q[1]));
singleport_ram #(.WIDTH(12), .WIDTHAD(9)) buf2( .clock(clk), .wren(buf_wr[2]), .address(buf_addr[2]), .data(buf_data[2]), .q(buf_q[2]));
singleport_ram #(.WIDTH(12), .WIDTHAD(9)) buf3( .clock(clk), .wren(buf_wr[3]), .address(buf_addr[3]), .data(buf_data[3]), .q(buf_q[3]));

always_comb begin
    buf_wr[0] = 0; buf_wr[1] = 0; buf_wr[2] = 0; buf_wr[3] = 0;
    buf_addr[0] = column; buf_addr[1] = column; buf_addr[2] = column; buf_addr[3] = column;

    buf_wr[erase_buffer] = 1;
    buf_data[erase_buffer] = 0;
    buf_addr[erase_buffer] = scan_column;

    buf_addr[scan_buffer] = scan_column;
    scan_color = buf_q[scan_buffer];

    if (wr) begin
        buf_data[line[1:0]] = { 1'b1, color };
        buf_wr[line[1:0]] = 1;
    end
end


endmodule




