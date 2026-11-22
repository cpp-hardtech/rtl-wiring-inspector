module leaf(input logic i, output logic o);
  assign o = i;
endmodule

module top(input logic [3:0] a, output logic [3:0] y);
  leaf u_first (.i(a[0]), .o(y[0]));
  leaf u_second (.i(a[1]), .o(y[1]));

  for (genvar g = 0; g < 2; g++) begin : gen
    leaf u_loop (.i(a[g]), .o());
  end

  if (1) begin : gen_if
    leaf u_if (.i(a[2]), .o(y[2]));
  end

  leaf u_arr [1:0] (.i(a[1:0]), .o());

  leaf u_direct_a (.i(a[3]), .o());
  leaf u_direct_b (.i(a[3]), .o());
endmodule
