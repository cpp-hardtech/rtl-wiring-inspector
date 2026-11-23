"""Integration tests for --load-netlist input validation diagnostics."""

import json
import os
import subprocess
import sys
import tempfile
import unittest

VALID_BASIC = {
    "version": 4,
    "fileTable": [],
    "nodes": [{"id": 1, "kind": "None"}],
    "edges": [],
}


class LoaderValidationTests(unittest.TestCase):
    executable = "rtl-wiring-inspector"

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)

    def write_text(self, name, text):
        path = os.path.join(self.tempdir.name, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def write_json(self, name, data):
        return self.write_text(name, json.dumps(data))

    def run_tool(self, *args):
        return subprocess.run(
            [self.executable, *args], capture_output=True, text=True
        )

    def load(self, netlist, *args):
        return self.run_tool("--load-netlist", netlist, *args)

    def assert_rejected(self, result, *expected):
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertIn("invalid netlist:", result.stderr)
        self.assertNotIn("json.exception", result.stderr)
        for text in expected:
            self.assertIn(text, result.stderr)

    def test_unknown_node_kind_is_rejected(self):
        data = {
            "version": 4,
            "fileTable": [],
            "nodes": [{"id": 1, "kind": "NotARealKind"}],
            "edges": [],
        }
        path = self.write_json("badkind.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "NotARealKind", "unknown node kind")

    def test_unknown_direction_is_rejected(self):
        data = {
            "version": 4,
            "fileTable": ["m.sv"],
            "nodes": [
                {
                    "id": 1,
                    "kind": "Port",
                    "path": "m.a",
                    "name": "a",
                    "bounds": [0, 0],
                    "direction": "Sideways",
                    "location": {"fileIndex": 0, "line": 1, "column": 1},
                }
            ],
            "edges": [],
        }
        path = self.write_json("baddir.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "Sideways", "unknown port direction")

    def test_unknown_edge_kind_is_rejected(self):
        data = {
            "version": 4,
            "fileTable": [],
            "nodes": [{"id": 1, "kind": "None"}, {"id": 2, "kind": "None"}],
            "edges": [
                {
                    "source": 1,
                    "target": 2,
                    "edgeKind": "Sideways",
                    "symbol": {},
                    "bounds": [0, 0],
                    "disabled": False,
                }
            ],
        }
        path = self.write_json("badedgekind.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "Sideways", "unknown edge kind")

    def test_unknown_operation_kind_is_rejected(self):
        data = {
            "version": 4,
            "fileTable": ["m.sv"],
            "nodes": [
                {
                    "id": 1,
                    "kind": "Operation",
                    "op": "NotAnOperator",
                    "width": 8,
                    "signed": False,
                    "location": {"fileIndex": 0, "line": 1, "column": 1},
                }
            ],
            "edges": [],
        }
        path = self.write_json("badop.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "NotAnOperator", "unknown operation kind")

    def test_missing_version_is_reported(self):
        data = {"fileTable": [], "nodes": [], "edges": []}
        path = self.write_json("noversion.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "missing required field 'version'")

    def test_truncated_json_is_reported(self):
        path = self.write_text("truncated.json", '{"version": 4, "nodes": [')
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "parse error")

    def test_duplicate_node_id_is_rejected(self):
        data = {
            "version": 4,
            "fileTable": [],
            "nodes": [{"id": 7, "kind": "None"}, {"id": 7, "kind": "None"}],
            "edges": [],
        }
        path = self.write_json("dupid.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "duplicate node id 7")

    def test_unknown_edge_endpoint_is_rejected(self):
        data = {
            "version": 4,
            "fileTable": [],
            "nodes": [{"id": 1, "kind": "None"}],
            "edges": [
                {
                    "source": 1,
                    "target": 42,
                    "edgeKind": "None",
                    "symbol": {},
                    "bounds": [0, 0],
                    "disabled": False,
                }
            ],
        }
        path = self.write_json("badendpoint.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "unknown node id 42")

    def test_invalid_bounds_are_rejected(self):
        data = {
            "version": 4,
            "fileTable": ["m.sv"],
            "nodes": [
                {
                    "id": 1,
                    "kind": "Port",
                    "path": "m.a",
                    "name": "a",
                    "bounds": [5, 2],
                    "direction": "In",
                    "location": {"fileIndex": 0, "line": 1, "column": 1},
                }
            ],
            "edges": [],
        }
        path = self.write_json("badbounds.json", data)
        result = self.load(path, "--report-registers")
        self.assert_rejected(result, "lower bound 5 exceeds upper bound 2")

    def test_none_kinds_still_load(self):
        data = {
            "version": 4,
            "fileTable": [],
            "nodes": [{"id": 1, "kind": "None"}, {"id": 2, "kind": "None"}],
            "edges": [
                {
                    "source": 1,
                    "target": 2,
                    "edgeKind": "None",
                    "symbol": {},
                    "bounds": [0, 0],
                    "disabled": False,
                }
            ],
        }
        path = self.write_json("none.json", data)
        result = self.load(path, "--report-registers")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_saved_netlist_still_loads(self):
        sv = self.write_text(
            "m.sv",
            """module m(input logic a, output logic b);
  assign b = a;
endmodule
""",
        )
        netlist = os.path.join(self.tempdir.name, "m.json")
        saved = self.run_tool(sv, "--save-netlist", netlist)
        self.assertEqual(saved.returncode, 0, saved.stderr)

        # The strict loader must still accept everything the serialiser emits.
        reloaded = self.run_tool(
            "--load-netlist", netlist, "--from", "m.a", "--to", "m.b"
        )
        self.assertEqual(reloaded.returncode, 0, reloaded.stderr)
        self.assertIn("input port a", reloaded.stdout)
        self.assertIn("output port b", reloaded.stdout)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        LoaderValidationTests.executable = sys.argv.pop(1)
    unittest.main()
