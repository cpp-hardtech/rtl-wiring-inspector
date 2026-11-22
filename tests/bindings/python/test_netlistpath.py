import unittest

import pyslang
import rtl_wiring_inspector


def build_graph(code):
    """
    Build a netlist graph from SystemVerilog and return it together with the
    objects that must outlive it.
    """
    tree = pyslang.syntax.SyntaxTree.fromText(code)
    compilation = pyslang.ast.Compilation()
    compilation.addSyntaxTree(tree)
    diagnostics = compilation.getAllDiagnostics()
    assert not any(d.isError() for d in diagnostics), diagnostics

    rtl_wiring_inspector.VisitAll().run(compilation)
    compilation.freeze()
    analysis_manager = pyslang.analysis.AnalysisManager()
    analysis_manager.analyze(compilation)
    compilation.unfreeze()

    graph = rtl_wiring_inspector.NetlistGraph()
    graph.build(compilation, analysis_manager)
    return tree, compilation, analysis_manager, graph


FANOUT_CODE = """
module m(input logic a, output logic x, output logic y);
    assign x = a;
    assign y = a;
endmodule
"""


class TestNetlistPathGuards(unittest.TestCase):
    def test_empty_path_front_and_back_raise_index_error(self):
        path = rtl_wiring_inspector.NetlistPath()
        self.assertTrue(path.empty())
        with self.assertRaises(IndexError):
            path.front()
        with self.assertRaises(IndexError):
            path.back()

    def test_empty_path_index_raises_index_error(self):
        path = rtl_wiring_inspector.NetlistPath()
        for index in (0, 10, 2**40, -1):
            with self.assertRaises(IndexError):
                path[index]

    def test_empty_path_from_finder_raises_index_error(self):
        tree, compilation, analysis_manager, graph = build_graph(FANOUT_CODE)
        path = rtl_wiring_inspector.PathFinder().find(
            graph.lookup("m.x"), graph.lookup("m.a")
        )
        self.assertTrue(path.empty())
        with self.assertRaises(IndexError):
            path.front()
        with self.assertRaises(IndexError):
            path.back()

    def test_path_indexing(self):
        tree, compilation, analysis_manager, graph = build_graph(FANOUT_CODE)
        path = rtl_wiring_inspector.PathFinder().find(
            graph.lookup("m.a"), graph.lookup("m.x")
        )
        self.assertEqual(path.size(), 3)
        self.assertEqual(path.front().path, "m.a")
        self.assertEqual(path.back().path, "m.x")
        self.assertEqual(path[0].path, "m.a")
        self.assertEqual(path[path.size() - 1].path, "m.x")
        for index in (path.size(), 10, 2**40, -1):
            with self.assertRaises(IndexError):
                path[index]


if __name__ == "__main__":
    unittest.main()
