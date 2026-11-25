import json
import os
import subprocess
import sys
import tempfile
import unittest

from utilities import fuzzy_compare_strings


class ReportTests(unittest.TestCase):
    executable = ...

    def run_report(self, *args, check=True):
        result = subprocess.run(
            [self.executable, *args],
            capture_output=True,
            text=True,
        )
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_help(self):
        result = subprocess.run(
            [self.executable, "--help"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("USAGE:", result.stdout)

    def test_version(self):
        result = subprocess.run(
            [self.executable, "--version"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("rtl-report version", result.stdout)

    def test_ast_json(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            outfile = f.name
        try:
            result = subprocess.run(
                [self.executable, "rca.sv", "--ast-json", outfile],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0)
            with open(outfile) as f:
                data = json.load(f)
            self.assertEqual(data["name"], "$root")
            self.assertEqual(data["kind"], "Root")
            names = [m.get("name") for m in data.get("members", [])]
            self.assertIn("rca", names)
        finally:
            os.unlink(outfile)

    def test_ast_json_scope_literal(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            outfile = f.name
        try:
            result = subprocess.run(
                [
                    self.executable,
                    "rca.sv",
                    "--ast-json",
                    outfile,
                    "--scope",
                    "rca.sum_q",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0)
            with open(outfile) as f:
                data = json.load(f)
            self.assertEqual(data["name"], "sum_q")
            self.assertEqual(data["kind"], "Variable")
        finally:
            os.unlink(outfile)

    def test_ast_json_scope_not_found(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            outfile = f.name
        try:
            result = subprocess.run(
                [
                    self.executable,
                    "rca.sv",
                    "--ast-json",
                    outfile,
                    "--scope",
                    "rca.bogus",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("scope 'rca.bogus' not found", result.stderr)
        finally:
            os.unlink(outfile)

    def test_ast_json_scope_removed_flag(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--ast-json",
                "-",
                "--ast-json-scope",
                "rca",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--ast-json-scope", result.stderr)

    def test_no_action(self):
        result = subprocess.run(
            [self.executable, "rca.sv"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no action specified", result.stderr)

    def test_ports(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--ports"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertTrue(
            fuzzy_compare_strings(
                """
Direction  Name       Width  Net Type  Location
In         rca.i_clk  1      wire      rca.sv:3:31
In         rca.i_rst  1      wire      rca.sv:4:31
In         rca.i_op0  8      wire      rca.sv:5:31
In         rca.i_op1  8      wire      rca.sv:6:31
Out        rca.o_sum  8      var       rca.sv:7:31
Out        rca.o_co   1      var       rca.sv:8:31
""",
                result.stdout,
            )
        )

    def test_variables(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--variables"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertTrue(
            fuzzy_compare_strings(
                """
Name       Type        Width  Kind  Drivers  Location
rca.i_clk  logic       1      wire  1        rca.sv:3:31
rca.i_rst  logic       1      wire  1        rca.sv:4:31
rca.i_op0  logic[7:0]  8      wire  1        rca.sv:5:31
rca.i_op1  logic[7:0]  8      wire  1        rca.sv:6:31
rca.o_sum  logic[7:0]  8      var   1        rca.sv:7:31
rca.o_co   logic       1      var   1        rca.sv:8:31
rca.carry  logic[7:0]  8      var   8        rca.sv:10:23
rca.sum    logic[7:0]  8      var   7        rca.sv:11:23
rca.sum_q  logic[7:0]  8      var   1        rca.sv:12:23
rca.co_q   logic       1      var   1        rca.sv:13:23
""",
                result.stdout,
            )
        )

    def test_drivers(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--drivers"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertTrue(
            fuzzy_compare_strings(
                """
Value             Range  Driver    Type  Location
rca.p_width                              rca.sv:2:15
rca.i_clk                                rca.sv:3:31
↳                 [0]    i_clk     cont  rca.sv:3:31
rca.i_rst                                rca.sv:4:31
↳                 [0]    i_rst     cont  rca.sv:4:31
rca.i_op0                                rca.sv:5:31
↳                 [7:0]  i_op0     cont  rca.sv:5:31
rca.i_op1                                rca.sv:6:31
↳                 [7:0]  i_op1     cont  rca.sv:6:31
rca.o_sum                                rca.sv:7:31
↳                 [7:0]  o_sum     cont  rca.sv:16:17
rca.o_co                                 rca.sv:8:31
↳                 [0]    o_co      cont  rca.sv:16:11
rca.carry                                rca.sv:10:23
↳                 [0]    carry[0]  cont  rca.sv:15:10
↳                 [1]    carry[1]  cont  rca.sv:19:13
↳                 [2]    carry[2]  cont  rca.sv:19:13
↳                 [3]    carry[3]  cont  rca.sv:19:13
↳                 [4]    carry[4]  cont  rca.sv:19:13
↳                 [5]    carry[5]  cont  rca.sv:19:13
↳                 [6]    carry[6]  cont  rca.sv:19:13
↳                 [7]    carry[7]  cont  rca.sv:19:13
rca.sum                                  rca.sv:11:23
↳                 [0]    sum[0]    cont  rca.sv:19:25
↳                 [1]    sum[1]    cont  rca.sv:19:25
↳                 [2]    sum[2]    cont  rca.sv:19:25
↳                 [3]    sum[3]    cont  rca.sv:19:25
↳                 [4]    sum[4]    cont  rca.sv:19:25
↳                 [5]    sum[5]    cont  rca.sv:19:25
↳                 [6]    sum[6]    cont  rca.sv:19:25
rca.sum_q                                rca.sv:12:23
↳                 [7:0]  sum_q     proc  rca.sv:24:7
rca.co_q                                 rca.sv:13:23
↳                 [0]    co_q      proc  rca.sv:25:7
rca.genblk1[0].i                         rca.sv:18:15
rca.genblk1[1].i                         rca.sv:18:15
rca.genblk1[2].i                         rca.sv:18:15
rca.genblk1[3].i                         rca.sv:18:15
rca.genblk1[4].i                         rca.sv:18:15
rca.genblk1[5].i                         rca.sv:18:15
rca.genblk1[6].i                         rca.sv:18:15
""",
                result.stdout,
            )
        )

    def test_ports_json(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--ports", "--format", "json"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertEqual(len(data), 6)
        first = data[0]
        self.assertEqual(first["name"], "rca.i_clk")
        self.assertEqual(first["direction"], "In")
        self.assertEqual(first["width"], 1)
        self.assertEqual(first["netType"], "wire")
        self.assertEqual(first["location"], "rca.sv:3:31")

        op0 = next(p for p in data if p["name"] == "rca.i_op0")
        self.assertEqual(op0["width"], 8)
        self.assertEqual(data[-1]["name"], "rca.o_co")
        self.assertEqual(data[-1]["direction"], "Out")

    def test_nonansi_concatenated_ports(self):
        result = self.run_report("nonansi_ports.sv", "--ports", "--format", "json")
        ports = json.loads(result.stdout)
        self.assertEqual(
            [(p["name"], p["direction"], p["width"], p["netType"]) for p in ports],
            [
                ("legacy_ports.data_in", "In", 5, "wire"),
                ("legacy_ports.control", "In", 1, "wire"),
                ("legacy_ports.data_out", "Out", 4, "var"),
                ("legacy_ports.mixed_out", "Out", 3, "mixed"),
            ],
        )
        self.assertTrue(ports[0]["location"].startswith("nonansi_ports.sv:2:"))

        filtered = self.run_report(
            "nonansi_ports.sv",
            "--ports",
            "--format",
            "json",
            "--scope",
            "legacy_ports",
            "--scope",
            "legacy_*",
            "--name",
            "legacy_ports.data_*",
        )
        self.assertEqual(json.loads(filtered.stdout), [ports[0], ports[2]])

    def test_variables_json(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--variables", "--format", "json"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        by_name = {v["name"]: v for v in data}

        carry = by_name["rca.carry"]
        self.assertEqual(carry["type"], "logic[7:0]")
        self.assertEqual(carry["width"], 8)
        self.assertEqual(carry["kind"], "var")
        self.assertEqual(carry["drivers"], 8)
        self.assertEqual(carry["location"], "rca.sv:10:23")

        clk = by_name["rca.i_clk"]
        self.assertEqual(clk["type"], "logic")
        self.assertEqual(clk["width"], 1)
        self.assertEqual(clk["drivers"], 1)

        sum_var = by_name["rca.sum"]
        self.assertEqual(sum_var["drivers"], 7)

    def test_fixed_size_widths(self):
        ports = self.run_report("widths.sv", "--ports", "--format", "json")
        port_widths = {port["name"]: port["width"] for port in json.loads(ports.stdout)}
        self.assertEqual(
            port_widths,
            {"widths.packed_input": 8, "widths.samples": 24, "widths.result": 8},
        )

        variables = self.run_report("widths.sv", "--variables", "--format", "json")
        widths = {var["name"]: var["width"] for var in json.loads(variables.stdout)}
        for name, expected in {
            "packed_input": 8,
            "samples": 24,
            "history": 40,
            "fixed_record": 6,
            "dynamic_samples": 0,
            "dynamic_record": 0,
            "period": 64,
        }.items():
            with self.subTest(variable=name):
                self.assertEqual(widths[f"widths.{name}"], expected)

    def test_drivers_json(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--drivers", "--format", "json"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        by_value = {v["value"]: v for v in data}

        carry = by_value["rca.carry"]
        self.assertEqual(carry["location"], "rca.sv:10:23")
        self.assertEqual(len(carry["drivers"]), 8)
        self.assertEqual(carry["drivers"][0]["range"], "[0]")
        self.assertEqual(carry["drivers"][0]["driver"], "carry[0]")
        self.assertEqual(carry["drivers"][0]["kind"], "cont")

        sum_q = by_value["rca.sum_q"]
        self.assertEqual(len(sum_q["drivers"]), 1)
        self.assertEqual(sum_q["drivers"][0]["kind"], "proc")
        self.assertEqual(sum_q["drivers"][0]["range"], "[7:0]")

        self.assertEqual(by_value["rca.p_width"]["drivers"], [])

    def test_unknown_format(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--ports", "--format", "bogus"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown --format value", result.stderr)

    def test_scope_variables(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--variables",
                "--scope",
                "rca.sum_q",
                "--scope",
                "rca.co_q",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        names = [v["name"] for v in data]
        self.assertEqual(names, ["rca.sum_q", "rca.co_q"])

    def test_scope_drivers(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--drivers",
                "--scope",
                "rca.sum_q",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertEqual(len(data), 1)
        self.assertEqual(data[0]["value"], "rca.sum_q")
        self.assertEqual(len(data[0]["drivers"]), 1)
        self.assertEqual(data[0]["drivers"][0]["kind"], "proc")

    def test_scope_ports_subscope(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--ports",
                "--scope",
                "rca",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertEqual(len(data), 6)

    def test_scope_not_found(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--ports", "--scope", "rca.bogus"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("scope 'rca.bogus' not found", result.stderr)

    def test_scope_glob_single_segment(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--variables",
                "--scope",
                "rca.sum_*",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        names = [v["name"] for v in json.loads(result.stdout)]
        self.assertEqual(names, ["rca.sum_q"])

    def test_scope_glob_recursive(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--drivers",
                "--scope",
                "rca.**.i",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        values = [v["value"] for v in json.loads(result.stdout)]
        self.assertEqual(len(values), 7)
        for v in values:
            self.assertTrue(v.startswith("rca.genblk1["))
            self.assertTrue(v.endswith("].i"))

    def test_scope_glob_question_mark(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--drivers",
                "--scope",
                "rca.genblk1[?].i",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        values = [v["value"] for v in json.loads(result.stdout)]
        self.assertEqual(len(values), 7)

    def test_scope_glob_dedupe(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--variables",
                "--scope",
                "rca.sum_*",
                "--scope",
                "rca.*_q",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        names = [v["name"] for v in json.loads(result.stdout)]
        self.assertEqual(names, ["rca.sum_q", "rca.co_q"])

    def test_scope_glob_no_match(self):
        result = subprocess.run(
            [self.executable, "rca.sv", "--ports", "--scope", "rca.bogus_*"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("scope 'rca.bogus_*' matched no symbols", result.stderr)

    def test_name_filter_ports(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--ports",
                "--name",
                "rca.i_*",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        names = [p["name"] for p in json.loads(result.stdout)]
        self.assertEqual(names, ["rca.i_clk", "rca.i_rst", "rca.i_op0", "rca.i_op1"])

    def test_name_filter_variables(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--variables",
                "--name",
                "rca.*_q",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        names = [v["name"] for v in json.loads(result.stdout)]
        self.assertEqual(sorted(names), ["rca.co_q", "rca.sum_q"])

    def test_name_filter_drivers(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--drivers",
                "--name",
                "rca.carry",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertEqual(len(data), 1)
        self.assertEqual(data[0]["value"], "rca.carry")
        self.assertEqual(len(data[0]["drivers"]), 8)

    def test_name_filter_recursive_glob(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--variables",
                "--name",
                "rca...",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        names = [v["name"] for v in json.loads(result.stdout)]
        # rca... should match every variable under rca.
        self.assertIn("rca.carry", names)
        self.assertIn("rca.sum_q", names)
        self.assertIn("rca.i_clk", names)

    def test_name_filter_multiple_or(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--variables",
                "--name",
                "rca.sum_*",
                "--name",
                "rca.*_q",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        names = [v["name"] for v in json.loads(result.stdout)]
        # sum_q matches both patterns but should appear once.
        self.assertEqual(sorted(names), ["rca.co_q", "rca.sum_q"])

    def test_name_filter_with_scope(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--ports",
                "--scope",
                "rca",
                "--name",
                "rca.o_*",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        names = [p["name"] for p in json.loads(result.stdout)]
        self.assertEqual(sorted(names), ["rca.o_co", "rca.o_sum"])

    def test_name_filter_no_match_empty(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--ports",
                "--name",
                "rca.bogus_*",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), [])

    def test_output_to_file_table(self):
        with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as f:
            outfile = f.name
        try:
            result = subprocess.run(
                [self.executable, "rca.sv", "--ports", "--output", outfile],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            with open(outfile) as f:
                content = f.read()
            self.assertIn("Direction", content)
            self.assertIn("rca.i_clk", content)
        finally:
            os.unlink(outfile)

    def test_output_to_file_json(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            outfile = f.name
        try:
            result = subprocess.run(
                [
                    self.executable,
                    "rca.sv",
                    "--variables",
                    "--format",
                    "json",
                    "--output",
                    outfile,
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            with open(outfile) as f:
                data = json.load(f)
            names = [v["name"] for v in data]
            self.assertIn("rca.carry", names)
        finally:
            os.unlink(outfile)

    def test_output_short_alias(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            outfile = f.name
        try:
            result = subprocess.run(
                [
                    self.executable,
                    "rca.sv",
                    "--drivers",
                    "--format",
                    "json",
                    "-o",
                    outfile,
                    "--name",
                    "rca.carry",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0)
            with open(outfile) as f:
                data = json.load(f)
            self.assertEqual(len(data), 1)
            self.assertEqual(data[0]["value"], "rca.carry")
        finally:
            os.unlink(outfile)

    def test_output_dash_is_stdout(self):
        result = subprocess.run(
            [
                self.executable,
                "rca.sv",
                "--ports",
                "--output",
                "-",
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertEqual(len(data), 6)

    def test_disabled_actions_do_not_select_a_report(self):
        for action in ("ports", "variables", "drivers"):
            with self.subTest(action=action):
                result = self.run_report("rca.sv", f"--{action}=false", check=False)
                self.assertEqual(result.returncode, 1)
                self.assertIn("no action specified", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_disabled_actions_do_not_override_an_enabled_report(self):
        expected = self.run_report("rca.sv", "--drivers", "--format", "json")
        result = self.run_report(
            "rca.sv",
            "--ports=false",
            "--variables=false",
            "--drivers=true",
            "--format",
            "json",
        )
        self.assertEqual(json.loads(result.stdout), json.loads(expected.stdout))

    def test_multiple_actions_rejected(self):
        for args in (
            ("--variables", "--ports"),
            ("--ports", "--variables"),
            ("--drivers", "--variables"),
            ("--ports", "--ast-json", "-"),
            ("--ports", "--variables", "--drivers"),
        ):
            with self.subTest(args=args):
                result = self.run_report("rca.sv", *args, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("multiple actions specified", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_multiple_actions_rejected_before_compilation(self):
        result = self.run_report(
            "does-not-exist.sv", "--ports", "--variables", check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("multiple actions specified", result.stderr)

    def test_ast_json_invalid_format_rejected(self):
        result = self.run_report(
            "rca.sv", "--ast-json", "-", "--format", "yaml", check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown --format value 'yaml'", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_ast_json_ignores_valid_format(self):
        for fmt in ("table", "json"):
            with self.subTest(fmt=fmt):
                result = self.run_report(
                    "rca.sv",
                    "--ast-json",
                    "-",
                    "--format",
                    fmt,
                    "--scope",
                    "rca.sum_q",
                )
                data = json.loads(result.stdout)
                self.assertEqual(data["name"], "sum_q")

    def test_ast_json_multiple_scopes_is_array(self):
        result = self.run_report(
            "rca.sv",
            "--ast-json",
            "-",
            "--scope",
            "rca.sum_q",
            "--scope",
            "rca.co_q",
        )
        data = json.loads(result.stdout)
        self.assertIsInstance(data, list)
        self.assertEqual([item["name"] for item in data], ["sum_q", "co_q"])

    def test_ast_json_glob_scope_is_array(self):
        result = self.run_report("rca.sv", "--ast-json", "-", "--scope", "rca.*_q")
        data = json.loads(result.stdout)
        self.assertIsInstance(data, list)
        self.assertEqual([item["name"] for item in data], ["sum_q", "co_q"])

    def test_ast_json_duplicate_scope_stays_single_object(self):
        result = self.run_report(
            "rca.sv",
            "--ast-json",
            "-",
            "--scope",
            "rca.sum_q",
            "--scope",
            "rca.sum_q",
        )
        data = json.loads(result.stdout)
        self.assertIsInstance(data, dict)
        self.assertEqual(data["name"], "sum_q")

    def test_ast_json_recursive_glob_stays_single_object(self):
        result = self.run_report("rca.sv", "--ast-json", "-", "--scope", "rca.**")
        data = json.loads(result.stdout)
        self.assertIsInstance(data, dict)
        self.assertEqual(data["name"], "rca")
        self.assertEqual(data["kind"], "Instance")

    # Repeated module instances, generate constructs, instance arrays.
    def test_scope_repeated_instance_ports(self):
        result = self.run_report("hierarchy.sv", "--ports", "--scope", "top.u_second")
        self.assertTrue(
            fuzzy_compare_strings(
                """
Direction  Name            Width  Net Type  Location
In         top.u_second.i  1      wire      hierarchy.sv:1:25
Out        top.u_second.o  1      var       hierarchy.sv:1:41
""",
                result.stdout,
            )
        )

    def test_scope_repeated_instance_variables(self):
        result = self.run_report(
            "hierarchy.sv",
            "--variables",
            "--scope",
            "top.u_second",
            "--format",
            "json",
        )
        data = json.loads(result.stdout)
        self.assertEqual(
            [v["name"] for v in data], ["top.u_second.i", "top.u_second.o"]
        )
        self.assertEqual([v["drivers"] for v in data], [1, 1])

    def test_scope_repeated_instance_drivers(self):
        result = self.run_report(
            "hierarchy.sv",
            "--drivers",
            "--scope",
            "top.u_second",
            "--format",
            "json",
        )
        data = json.loads(result.stdout)
        self.assertEqual(
            [v["value"] for v in data], ["top.u_second.i", "top.u_second.o"]
        )
        for value in data:
            self.assertEqual(len(value["drivers"]), 1)
            self.assertEqual(value["drivers"][0]["range"], "[0]")
            self.assertEqual(value["drivers"][0]["kind"], "cont")

    def test_scope_generate_loop_instances(self):
        result = self.run_report(
            "hierarchy.sv",
            "--ports",
            "--scope",
            "top.gen[*].u_loop",
            "--format",
            "json",
        )
        self.assertEqual(
            [p["name"] for p in json.loads(result.stdout)],
            [
                "top.gen[0].u_loop.i",
                "top.gen[0].u_loop.o",
                "top.gen[1].u_loop.i",
                "top.gen[1].u_loop.o",
            ],
        )

    def test_scope_generate_if_instance(self):
        result = self.run_report(
            "hierarchy.sv",
            "--ports",
            "--scope",
            "top.gen_if.u_if",
            "--format",
            "json",
        )
        self.assertEqual(
            [p["name"] for p in json.loads(result.stdout)],
            ["top.gen_if.u_if.i", "top.gen_if.u_if.o"],
        )

    def test_scope_instance_array(self):
        result = self.run_report(
            "hierarchy.sv",
            "--ports",
            "--scope",
            "top.u_arr[*]",
            "--format",
            "json",
        )
        self.assertEqual(
            [p["name"] for p in json.loads(result.stdout)],
            [
                "top.u_arr[0].i",
                "top.u_arr[0].o",
                "top.u_arr[1].i",
                "top.u_arr[1].o",
            ],
        )

    def test_all_instances_reported_once(self):
        result = self.run_report("hierarchy.sv", "--ports", "--format", "json")
        names = [p["name"] for p in json.loads(result.stdout)]
        self.assertEqual(len(names), 20)
        self.assertEqual(len(set(names)), 20)
        for expected in (
            "top.u_second.i",
            "top.gen[1].u_loop.o",
            "top.gen_if.u_if.i",
            "top.u_arr[1].o",
            "top.u_direct_b.o",
        ):
            self.assertIn(expected, names)

    # Overlapping and recursive scopes report each symbol once.
    def test_scope_recursive_glob_matches_literal_scope(self):
        for action in ("--ports", "--variables", "--drivers"):
            with self.subTest(action=action):
                glob = self.run_report(
                    "rca.sv", action, "--scope", "rca.**", "--format", "json"
                )
                literal = self.run_report(
                    "rca.sv", action, "--scope", "rca", "--format", "json"
                )
                self.assertEqual(json.loads(glob.stdout), json.loads(literal.stdout))

    def test_scope_overlapping_scopes_dedupe(self):
        overlap = self.run_report(
            "hierarchy.sv",
            "--ports",
            "--scope",
            "top",
            "--scope",
            "top.u_second",
            "--format",
            "json",
        )
        single = self.run_report(
            "hierarchy.sv", "--ports", "--scope", "top", "--format", "json"
        )
        self.assertEqual(json.loads(overlap.stdout), json.loads(single.stdout))

    def test_scope_overlapping_scopes_reverse_order_dedupe(self):
        overlap = self.run_report(
            "hierarchy.sv",
            "--ports",
            "--scope",
            "top.u_second",
            "--scope",
            "top",
            "--format",
            "json",
        )
        single = self.run_report(
            "hierarchy.sv", "--ports", "--scope", "top", "--format", "json"
        )
        overlap_names = [p["name"] for p in json.loads(overlap.stdout)]
        single_names = [p["name"] for p in json.loads(single.stdout)]
        self.assertEqual(sorted(overlap_names), sorted(single_names))
        self.assertEqual(len(overlap_names), len(set(overlap_names)))

    def test_scope_recursive_glob_nested_dedupe(self):
        glob = self.run_report(
            "hierarchy.sv", "--ports", "--scope", "top.**", "--format", "json"
        )
        literal = self.run_report(
            "hierarchy.sv", "--ports", "--scope", "top", "--format", "json"
        )
        self.assertEqual(
            [p["name"] for p in json.loads(glob.stdout)],
            [p["name"] for p in json.loads(literal.stdout)],
        )


if __name__ == "__main__":
    if len(sys.argv) > 1:
        ReportTests.executable = sys.argv.pop(1)
    unittest.main()
