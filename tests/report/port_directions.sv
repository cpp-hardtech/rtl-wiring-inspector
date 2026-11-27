module io_ports(input logic data_in,
                output logic data_out,
                inout wire bus,
                ref logic state);
    assign data_out = data_in;
endmodule

module top;
    logic data_in, data_out, state;
    wire bus;
    io_ports dut(.data_in, .data_out, .bus, .state);
endmodule
