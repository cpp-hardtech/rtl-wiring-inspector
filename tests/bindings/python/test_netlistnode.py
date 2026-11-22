import unittest

import pyslang
import rtl_wiring_inspector


class TestNetlistNode(unittest.TestCase):

    def test_import(self):
        self.assertTrue(hasattr(rtl_wiring_inspector, "NetlistNode"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "Port"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "Variable"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "State"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "Assignment"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "Conditional"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "Case"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "Merge"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "Operation"))
        self.assertTrue(hasattr(rtl_wiring_inspector, "NodeKind"))

    def test_operation_node_kind(self):
        self.assertTrue(hasattr(rtl_wiring_inspector.NodeKind, "Operation"))

    def test_operation_properties(self):
        for name in ("op", "op_kind", "width", "is_signed"):
            self.assertTrue(
                hasattr(rtl_wiring_inspector.Operation, name),
                f"Operation is missing {name}",
            )

    def test_get_location(self):
        tree = pyslang.syntax.SyntaxTree.fromText(
            "module m(output logic a); assign a = 1; endmodule"
        )
        compilation = pyslang.ast.Compilation()
        compilation.addSyntaxTree(tree)
        self.assertEqual(len(compilation.getAllDiagnostics()), 0)
        compilation.freeze()
        analysis_manager = pyslang.analysis.AnalysisManager()
        analysis_manager.analyze(compilation)
        graph = rtl_wiring_inspector.NetlistGraph()
        graph.build(compilation, analysis_manager)

        port = graph.lookup("m.a")
        location = port.get_location()
        self.assertIsInstance(location, tuple)
        self.assertEqual(len(location), 3)
        self.assertGreater(location[1], 0)

        for node in graph:
            self.assertIsInstance(node.get_location(), tuple)


if __name__ == "__main__":
    unittest.main()
