#include "Test.hpp"

TEST_CASE("Chain of assignments through a procedural loop", "[Loop]") {
  auto const &tree = (R"(
module m(input logic a, output logic b);
  localparam N=4;
  logic [N-1:0] p;
  assign b = p[N-1];
  always_comb begin
    p[0] = a;
    for (int i=0; i<N-1; i++)
      p[i+1] = p[i];
  end
endmodule
  )");
  const NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.b"));
  CHECK(test.renderDot() == R"(digraph {
  node [shape=record];
  N1 [label="In port a"]
  N2 [label="Out port b"]
  N3 [label="Assignment"]
  N4 [label="Assignment"]
  N5 [label="Assignment"]
  N6 [label="Assignment"]
  N7 [label="Assignment"]
  N8 [label="Merge"]
  N1 -> N4 [label="a[0]"]
  N3 -> N2 [label="b[0]"]
  N4 -> N5 [label="p[0]"]
  N4 -> N8
  N5 -> N6 [label="p[1]"]
  N6 -> N7 [label="p[2]"]
  N7 -> N8
  N7 -> N3 [label="p[3]"]
}
)");
}

TEST_CASE("Nested for loops", "[Loop]") {
  auto const &tree = R"(
module m(input logic [3:0] a, output logic [3:0] b);
  logic [3:0] t;
  always_comb begin
    for (int i = 0; i < 2; i++)
      for (int j = 0; j < 2; j++)
        t[i*2+j] = a[i*2+j];
  end
  assign b = t;
endmodule
)";
  const NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.b"));
}

TEST_CASE("Loop body depends on external signal", "[Loop]") {
  auto const &tree = R"(
module m(input logic [3:0] a, input logic sel, output logic [3:0] b);
  logic [3:0] t;
  always_comb begin
    for (int i = 0; i < 4; i++) begin
      if (sel)
        t[i] = a[i];
      else
        t[i] = 1'b0;
    end
  end
  assign b = t;
endmodule
)";
  const NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.b"));
  CHECK(test.pathExists("m.sel", "m.b"));
}

TEST_CASE("While loop with assignments", "[Loop]") {
  // The loop variable is automatic and not tracked, so the body is
  // analysed once conservatively and the unknown index falls back to the
  // whole symbol.
  auto const &tree = R"(
module m(input logic [7:0] a, output logic [7:0] b);
  logic [7:0] t;
  always_comb begin
    int i;
    i = 0;
    t = '0;
    while (i < 8) begin
      t[i] = a[i];
      i = i + 1;
    end
  end
  assign b = t;
endmodule
)";
  NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.b"));
}

TEST_CASE("For loop with a module-level integer variable", "[Loop]") {
  auto const &tree = R"(
module m(input logic [3:0] a, output logic [3:0] y);
  integer i;
  always_comb begin
    for (i = 0; i < 4; i = i + 1)
      y[i] = a[i];
  end
endmodule
)";
  NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.y"));
  for (int bit = 0; bit < 4; bit++) {
    CHECK(!test.getDrivers("m.y", {bit, bit}).empty());
  }
}

TEST_CASE("For loop with a module-level integer assigns a whole vector",
          "[Loop]") {
  auto const &tree = R"(
module m(input logic [3:0] a, output logic [3:0] y);
  integer i;
  always_comb begin
    for (i = 0; i < 4; i = i + 1)
      y = a;
  end
endmodule
)";
  NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.y"));
  CHECK(!test.getDrivers("m.y", {0, 3}).empty());
}

TEST_CASE("Nested for loops with module-level integers", "[Loop]") {
  auto const &tree = R"(
module m(input logic [3:0] a, output logic [3:0] b);
  integer i;
  integer j;
  logic [3:0] t;
  always_comb begin
    for (i = 0; i < 2; i = i + 1)
      for (j = 0; j < 2; j = j + 1)
        t[i*2+j] = a[i*2+j];
  end
  assign b = t;
endmodule
)";
  NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.b"));
  for (int bit = 0; bit < 4; bit++) {
    CHECK(!test.getDrivers("m.b", {bit, bit}).empty());
  }
}

TEST_CASE("Repeat loop with a constant count", "[Loop]") {
  auto const &tree = R"(
module m(input logic [3:0] a, output logic [3:0] y);
  always_comb begin
    int i;
    i = 0;
    repeat (4) begin
      y[i] = a[i];
      i = i + 1;
    end
  end
endmodule
)";
  NetlistTest test(tree);
  CHECK(test.pathExists("m.a", "m.y"));
  // The unknown index falls back to the whole symbol, so every bit has a
  // driver rather than only bit 0.
  for (int bit = 0; bit < 4; bit++) {
    CHECK(!test.getDrivers("m.y", {bit, bit}).empty());
  }
}

TEST_CASE("Repeat loop with non-blocking assignments", "[Loop]") {
  auto const &tree = R"(
module m(input logic clk, input logic [3:0] d, output logic [3:0] q);
  always_ff @(posedge clk) begin
    int i;
    i = 0;
    repeat (4) begin
      q[i] <= d[i];
      i = i + 1;
    end
  end
endmodule
)";
  NetlistTest test(tree);
  CHECK(test.pathExists("m.d", "m.q"));
  for (int bit = 0; bit < 4; bit++) {
    CHECK(!test.getDrivers("m.q", {bit, bit}).empty());
  }
}
