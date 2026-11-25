module widths(
    input logic [7:0] packed_input,
    input logic [7:0] samples [3],
    output logic [7:0] result
);
    typedef logic [3:0] nibble;
    nibble history [2][5];
    struct { logic [4:0] value; logic valid; } fixed_record;
    logic [7:0] dynamic_samples [];
    struct { logic [3:0] id; string label; } dynamic_record;
    real period;

    assign result = packed_input ^ samples[0];
endmodule
