module driver_kinds(
  input logic a, b,
  output logic [1:0] mixed,
  output wire continuous_only
);
  assign mixed[0] = a;
  always_comb mixed[1] = b;
  assign continuous_only = a;
endmodule
