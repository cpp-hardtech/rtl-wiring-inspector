module legacy_ports(
    .data_in({upper[2:1], lower}),
    .control(control),
    .data_out({count, ready}),
    .mixed_out({status, valid})
);
    input wire [3:0] upper;
    input wire [2:0] lower;
    input wire control;
    output reg [2:0] count;
    output reg ready;
    output wire [1:0] status;
    output reg valid;
endmodule
