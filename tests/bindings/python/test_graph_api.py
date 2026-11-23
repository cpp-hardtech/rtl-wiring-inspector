import gc
import json
import unittest

import pyslang
import rtl_wiring_inspector


class BuiltNetlist:
    """
    Build a netlist graph from SystemVerilog and hold the references that must
    outlive it.
    """

    def __init__(self, code: str, **build_options):
        self.tree = pyslang.syntax.SyntaxTree.fromText(code)
        self.compilation = pyslang.ast.Compilation()
        self.compilation.addSyntaxTree(self.tree)
        diagnostics = self.compilation.getAllDiagnostics()
        assert not any(d.isError() for d in diagnostics), diagnostics

        rtl_wiring_inspector.VisitAll().run(self.compilation)
        self.compilation.freeze()
        self.analysis_manager = pyslang.analysis.AnalysisManager()
        self.analysis_manager.analyze(self.compilation)
        self.compilation.unfreeze()

        self.graph = rtl_wiring_inspector.NetlistGraph()
        self.graph.build(self.compilation, self.analysis_manager, **build_options)


BIT_SLICE_CODE = """
module m(input logic a, input logic b, output logic [1:0] y);
    assign y[0] = a;
    assign y[1] = b;
endmodule
"""

FANOUT_CODE = """
module m(input logic a, output logic x, output logic y);
    assign x = a;
    assign y = a;
endmodule
"""

CLOCKED_CODE = """
module m(input logic clk, input logic a, output logic b, output logic c);
    always_ff @(posedge clk) b <= a;
    assign c = a;
endmodule
"""

BLACK_BOX_CODE = """
module foo(input logic x, input logic y, output logic z);
    assign z = x | y;
endmodule

module top(input logic a, input logic b, output logic c);
    foo u_mux(.x(a), .y(b), .z(c));
endmodule
"""

NESTED_CODE = """
module leaf(input logic i, output logic o);
    assign o = i;
endmodule

module mid(input logic clk, input logic i, output logic o);
    logic q;
    always_ff @(posedge clk) q <= i;
    leaf u_leaf(.i(q), .o(o));
endmodule

module top(input logic clk, input logic a, output logic c);
    mid u_mid(.clk(clk), .i(a), .o(c));
endmodule
"""


class TestBitDrivers(unittest.TestCase):
    def test_bit_drivers_cover_each_bit_range(self):
        built = BuiltNetlist(BIT_SLICE_CODE)
        drivers = built.graph.get_bit_drivers("m.y")

        self.assertEqual(len(drivers), 2)
        self.assertEqual(
            sorted((d.bounds.lower, d.bounds.upper) for d in drivers),
            [(0, 0), (1, 1)],
        )
        for driver in drivers:
            self.assertIsInstance(driver, rtl_wiring_inspector.BitDriver)
            self.assertEqual(driver.bounds.width, 1)
            self.assertEqual(
                driver.driver.kind, rtl_wiring_inspector.NodeKind.Assignment
            )

    def test_bit_driver_bounds_unpack(self):
        built = BuiltNetlist(BIT_SLICE_CODE)
        drivers = built.graph.get_bit_drivers("m.y")
        self.assertEqual([tuple(driver.bounds) for driver in drivers], [(0, 0), (1, 1)])

    def test_bit_drivers_resolve_to_source_ports(self):
        built = BuiltNetlist(BIT_SLICE_CODE)
        expected = {(0, 0): "m.a", (1, 1): "m.b"}
        for driver in built.graph.get_bit_drivers("m.y"):
            key = (driver.bounds.lower, driver.bounds.upper)
            ports = list(built.graph.get_drivers(driver.driver))
            self.assertEqual(len(ports), 1)
            self.assertIsInstance(ports[0], rtl_wiring_inspector.Port)
            self.assertEqual(ports[0].path, expected[key])

    def test_bit_drivers_range_filter(self):
        built = BuiltNetlist(BIT_SLICE_CODE)
        graph = built.graph

        selected = graph.get_bit_drivers("m.y", 1, 1)
        self.assertEqual([(d.bounds.lower, d.bounds.upper) for d in selected], [(1, 1)])
        self.assertEqual(graph.get_bit_drivers("m.y", 2, 2), [])
        self.assertEqual(graph.get_bit_drivers("m.missing"), [])

    def test_bit_drivers_agree_with_drivers(self):
        built = BuiltNetlist(BIT_SLICE_CODE)
        graph = built.graph
        bit_drivers = graph.get_bit_drivers("m.y")
        drivers = graph.get_drivers("m.y", 0, 1)
        self.assertEqual(
            sorted(bit_driver.driver.ID for bit_driver in bit_drivers),
            sorted(node.ID for node in drivers),
        )


class TestEdgeTraversal(unittest.TestCase):
    def test_out_and_in_edges_are_bidirectional(self):
        built = BuiltNetlist(CLOCKED_CODE)
        graph = built.graph

        self.assertEqual(sum(len(node.out_edges) for node in graph), graph.num_edges())
        self.assertEqual(sum(len(node.in_edges) for node in graph), graph.num_edges())
        for node in graph:
            for edge in node.out_edges:
                self.assertIsInstance(edge, rtl_wiring_inspector.NetlistEdge)
                self.assertIs(edge.source, node)
                self.assertIn(edge, edge.target.in_edges)
            for edge in node.in_edges:
                self.assertIn(edge, edge.source.out_edges)

    def test_edge_properties(self):
        built = BuiltNetlist(CLOCKED_CODE)
        graph = built.graph
        edges = [edge for node in graph for edge in node.out_edges]

        posedge = [
            edge for edge in edges if edge.edge_kind == pyslang.ast.EdgeKind.PosEdge
        ]
        self.assertEqual(len(posedge), 1)
        clock = posedge[0]
        self.assertIsInstance(clock.edge_kind, pyslang.ast.EdgeKind)
        self.assertEqual(clock.symbol_name, "clk")
        self.assertEqual(clock.symbol_path, "m.clk")
        self.assertEqual((clock.bounds.lower, clock.bounds.upper), (0, 0))
        self.assertFalse(clock.disabled)

        data = next(
            edge
            for edge in edges
            if edge.edge_kind == pyslang.ast.EdgeKind.None_
            and edge.symbol_path == "m.a"
        )
        self.assertEqual(data.source.path, "m.a")
        self.assertEqual(data.target.kind, rtl_wiring_inspector.NodeKind.Assignment)

    def test_out_edges_fan_out(self):
        built = BuiltNetlist(FANOUT_CODE)
        node = built.graph.lookup("m.a")
        edges = node.out_edges
        self.assertEqual(len(edges), 2)
        self.assertTrue(all(edge.source is node for edge in edges))


class TestSerializer(unittest.TestCase):
    @staticmethod
    def edge_signatures(graph):
        return sorted(
            (
                edge.source.kind.name,
                getattr(edge.source, "path", None),
                edge.target.kind.name,
                getattr(edge.target, "path", None),
                edge.edge_kind.name,
                edge.symbol_path,
                (edge.bounds.lower, edge.bounds.upper),
                edge.disabled,
            )
            for node in graph
            for edge in node.out_edges
        )

    def test_round_trip_preserves_graph(self):
        built = BuiltNetlist(BIT_SLICE_CODE)
        original = built.graph
        serialized = rtl_wiring_inspector.NetlistSerializer.serialize(original)
        self.assertIsInstance(serialized, str)
        self.assertEqual(json.loads(serialized)["version"], 4)

        loaded = rtl_wiring_inspector.NetlistGraph()
        result = rtl_wiring_inspector.NetlistSerializer.deserialize(serialized, loaded)
        self.assertIsNone(result)
        self.assertEqual(loaded.num_nodes(), original.num_nodes())
        self.assertEqual(loaded.num_edges(), original.num_edges())

        for name in ("m.a", "m.b", "m.y"):
            before = original.lookup(name)
            after = loaded.lookup(name)
            self.assertIsNotNone(after)
            self.assertEqual(after.kind, before.kind)
            self.assertEqual(
                (after.bounds.lower, after.bounds.upper),
                (before.bounds.lower, before.bounds.upper),
            )

        self.assertEqual(
            [(d.bounds.lower, d.bounds.upper) for d in loaded.get_bit_drivers("m.y")],
            [(d.bounds.lower, d.bounds.upper) for d in original.get_bit_drivers("m.y")],
        )
        self.assertEqual(self.edge_signatures(loaded), self.edge_signatures(original))

    def test_round_trip_reaches_same_queries(self):
        built = BuiltNetlist(FANOUT_CODE)
        loaded = rtl_wiring_inspector.NetlistGraph()
        rtl_wiring_inspector.NetlistSerializer.deserialize(
            rtl_wiring_inspector.NetlistSerializer.serialize(built.graph), loaded
        )

        path = rtl_wiring_inspector.PathFinder().find(
            loaded.lookup("m.a"), loaded.lookup("m.x")
        )
        self.assertFalse(path.empty())
        self.assertEqual(path.front().path, "m.a")
        self.assertEqual(path.back().path, "m.x")

    def test_invalid_json_raises_runtime_error(self):
        with self.assertRaises(RuntimeError):
            rtl_wiring_inspector.NetlistSerializer.deserialize(
                "not json", rtl_wiring_inspector.NetlistGraph()
            )

    def test_unsupported_version_raises_runtime_error(self):
        with self.assertRaises(RuntimeError):
            rtl_wiring_inspector.NetlistSerializer.deserialize(
                '{"version": 99, "fileTable": [], "nodes": [], "edges": []}',
                rtl_wiring_inspector.NetlistGraph(),
            )

    def test_malformed_document_raises_runtime_error(self):
        with self.assertRaises(RuntimeError):
            rtl_wiring_inspector.NetlistSerializer.deserialize(
                '{"version": 4}', rtl_wiring_inspector.NetlistGraph()
            )


class TestBlackBoxCoverage(unittest.TestCase):
    def test_enum_members(self):
        for name in ("Outside", "Boundary", "Contained"):
            self.assertTrue(hasattr(rtl_wiring_inspector.BlackBoxCoverage, name))

    def test_paths_and_coverage(self):
        built = BuiltNetlist(BLACK_BOX_CODE, black_boxes=["foo"])
        graph = built.graph
        self.assertEqual(graph.get_black_box_paths(), ["top.u_mux"])

        boundary = rtl_wiring_inspector.BlackBoxCoverage.Boundary
        outside = rtl_wiring_inspector.BlackBoxCoverage.Outside
        for name in ("top.u_mux.x", "top.u_mux.y", "top.u_mux.z"):
            self.assertEqual(graph.get_black_box_coverage(graph.lookup(name)), boundary)
        for name in ("top.a", "top.c"):
            self.assertEqual(graph.get_black_box_coverage(graph.lookup(name)), outside)

    def test_paths_round_trip(self):
        built = BuiltNetlist(BLACK_BOX_CODE, black_boxes=["foo"])
        serialized = rtl_wiring_inspector.NetlistSerializer.serialize(built.graph)
        loaded = rtl_wiring_inspector.NetlistGraph()
        rtl_wiring_inspector.NetlistSerializer.deserialize(serialized, loaded)
        self.assertEqual(loaded.get_black_box_paths(), ["top.u_mux"])

    def test_no_black_boxes_by_default(self):
        built = BuiltNetlist(BIT_SLICE_CODE)
        self.assertEqual(built.graph.get_black_box_paths(), [])

    def test_contained_coverage_after_deserialize(self):
        built = BuiltNetlist(NESTED_CODE)
        document = json.loads(
            rtl_wiring_inspector.NetlistSerializer.serialize(built.graph)
        )
        document["blackBoxes"] = ["top.u_mid"]

        loaded = rtl_wiring_inspector.NetlistGraph()
        rtl_wiring_inspector.NetlistSerializer.deserialize(json.dumps(document), loaded)
        self.assertEqual(loaded.get_black_box_paths(), ["top.u_mid"])

        coverage = rtl_wiring_inspector.BlackBoxCoverage
        self.assertEqual(
            loaded.get_black_box_coverage(loaded.lookup("top.u_mid.i")),
            coverage.Boundary,
        )
        self.assertEqual(
            loaded.get_black_box_coverage(loaded.lookup("top.u_mid.q")),
            coverage.Contained,
        )
        self.assertEqual(
            loaded.get_black_box_coverage(loaded.lookup("top.u_mid.u_leaf.i")),
            coverage.Contained,
        )
        self.assertEqual(
            loaded.get_black_box_coverage(loaded.lookup("top.a")),
            coverage.Outside,
        )


class TestReferenceLifetime(unittest.TestCase):
    def test_edges_keep_the_graph_alive(self):
        built = BuiltNetlist(FANOUT_CODE)
        graph = built.graph
        node = graph.lookup("m.a")
        edges = node.out_edges
        self.assertEqual(len(edges), 2)

        del graph
        del node
        del built
        gc.collect()

        self.assertEqual(len(edges), 2)
        self.assertEqual(
            {edge.source.kind for edge in edges},
            {rtl_wiring_inspector.NodeKind.Port},
        )
        self.assertEqual(edges[0].source.path, "m.a")


if __name__ == "__main__":
    unittest.main()
