<p align="center">
  <img src=".github/branding/logo.png" width="160" alt="RTL Wiring Inspector logo">
</p>

<h1 align="center">RTL Wiring Inspector</h1>

<p align="center">Trace signal dependencies through elaborated SystemVerilog RTL.</p>

RTL Wiring Inspector elaborates a SystemVerilog design and builds a bit-level dependency
graph of the result. Use it to answer concrete questions about a design: what drives this
signal, where does it feed, what sits on the path between two ports, and which registers or
combinational loops exist in the cone.

The analysis is static. A C++20 library provides the graph; two command-line tools and a
Python extension expose it. The frontend is the slang SystemVerilog compiler.

## Requirements

- A C++20 compiler, CMake 3.20 or newer.
- Python 3 for the build scripts and the optional bindings. The Python extension targets
  Python 3.12 or newer.
- Optional: Doxygen for the documentation build (`BUILD_DOCS=OFF` skips it).

## Build

```sh
git clone https://github.com/cpp-hardtech/rtl-wiring-inspector.git
cd rtl-wiring-inspector
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build --parallel --target install
ctest --test-dir build --output-on-failure
```

The two tools land in `install/bin/`. Set `ENABLE_PY_BINDINGS=ON` to build the Python
extension as part of the same configuration.

## Quick start

The repository ships a ripple-carry adder for smoke testing. Trace the path from one input
port to one output port:

```sh
./install/bin/rtl-wiring-inspector tests/driver/rca.sv \
  --from rca.i_op1 --to rca.o_sum
```

The report lists the input, the assignments along the path, and the output port, with source
locations. Passing only one side narrows the query:

```sh
# Combinational fan-out of a single signal
./install/bin/rtl-wiring-inspector tests/driver/rca.sv --from rca.i_op1

# Combinational fan-in of a single signal, plus a DOT export of the graph
./install/bin/rtl-wiring-inspector tests/driver/rca.sv \
  --to rca.o_sum --netlist-dot rca-fan-in.dot

# Declarations rather than connectivity
./install/bin/rtl-report tests/driver/rca.sv --ports --scope rca
```

## What it analyses

- **Signal dependencies.** Bit ranges are followed through continuous assignments,
  procedural blocks, and nonblocking assignments. Constant conditions are evaluated and
  static loops are unrolled.
- **Paths and cones.** Query a path between two signals, or the fan-in and fan-out of one
  signal.
- **Graph structure.** Detect combinational loops, report registers, and dump the graph in
  DOT format. Construction can use several threads on large designs.
- **Declarations and drivers.** Inspect ports, typed variables, nets, and drivers, or export
  the elaborated AST as JSON.

Path queries report static dependencies. Some expressions are approximated conservatively,
so a reported path can include a dependency that a Boolean analysis would rule out. See the
[developer guide](docs/developer-guide.dox) for how the graph is built and where precision is
traded for soundness.

## `rtl-wiring-inspector` options

| Group | Option | Purpose |
| --- | --- | --- |
| Query | `--from <signal>` | Start of the dependency query (JSON with `--format json`). |
| Query | `--to <signal>` | End of the dependency query (JSON with `--format json`). |
| Query | `--fan-in <signal>` | Report the combinational fan-in. |
| Query | `--fan-out <signal>` | Report the combinational fan-out. |
| Search | `--find <name>` | Locate signals by name. |
| Search | `--find-regex <pattern>` | Locate signals by regular expression. |
| Report | `--drivers` | Report, per bit, the drivers of a signal. |
| Report | `--constant-drivers` | Report drivers whose value is constant. |
| Report | `--sensitivity` | Report sensitivity of the selected signals. |
| Report | `--comb-loops` | Detect combinational loops (JSON with `--format json`). |
| Report | `--report-registers` | Report registers in the design. |
| Graph | `--netlist-dot <file>` | Write the graph in DOT format. |
| Graph | `--save-netlist <file>` | Persist the built netlist. |
| Graph | `--load-netlist <file>` | Reuse a persisted netlist instead of rebuilding. |
| Precision | `--expand-operations` | Expand operations instead of treating them as opaque. |
| Precision | `--no-resolve-assign-bits` | Disable bit-level resolution of assignments. |
| Precision | `--no-prop-cuts-across-ports` | Stop propagation from crossing port boundaries. |
| Scope | `--black-box <module>` | Treat a module as a black box. |
| Scope | `--scope <instance>` | Restrict analysis to an instance. |
| Output | `--name <name>` | Restrict output to a named signal. |
| Output | `--format <format>` | Select the output format. |
| Output | `--no-colours` | Disable ANSI colours. |
| Output | `--colour` | Force ANSI colours even when stdout is not a terminal. |
| Output | `--stats` | Print construction statistics to stderr. |
| Output | `--stats-json` | Print construction statistics as JSON to stderr. |
| Info | `--version` | Print the version and exit. |

## `rtl-report` options

| Option | Purpose |
| --- | --- |
| `--ports` | Report port declarations. |
| `--direction <input\|output\|inout\|ref>` | Filter `--ports` by direction. Repeat to include multiple directions. |
| `--variables` | Report typed variables and nets. |
| `--drivers` | Report driver declarations. |
| `--ast-json <file>` | Export the elaborated AST as JSON. |
| `--scope <instance>` | Restrict the report to an instance. |
| `--name <name>` | Restrict the report to a named declaration. |
| `--format <format>` | Select the output format. |
| `--version` | Print the version and exit. |

## Python bindings

Build and install the extension from the checkout, then import both the compiler bindings
and this package:

```sh
python3 -m pip install .
python3 -c "import pyslang, rtl_wiring_inspector"
```

Both extensions share the same nanobind runtime, so slang objects can be passed between them.
Use a `pyslang` build that targets the same slang version, otherwise the stable ABI floor will
reject the combination. The [connectivity example](examples/connectivity_check.py) builds a
graph from SystemVerilog and checks whether a path exists between two named ports.

## Documentation

- [User guide](docs/user-guide.dox)
- [Developer guide](docs/developer-guide.dox)
- [Python examples](examples/)
- [Contributing guide](CONTRIBUTING.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Distributed under the MIT License. The original copyright notice and the full license text
are retained in [LICENSE](LICENSE).
