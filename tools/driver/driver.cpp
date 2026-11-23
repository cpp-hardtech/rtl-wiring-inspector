#include "slang/driver/Driver.h"

#include "common/Utilities.hpp"
#include "common/Wildcard.hpp"
#include "netlist/BuilderOptions.hpp"
#include "netlist/CombLoops.hpp"
#include "netlist/Debug.hpp"
#include "netlist/NetlistDiagnostics.hpp"
#include "netlist/NetlistDot.hpp"
#include "netlist/NetlistGraph.hpp"
#include "netlist/NetlistSerializer.hpp"
#include "netlist/PathFinder.hpp"
#include "netlist/VisitAll.hpp"

#include "common/FormatBuffer.hpp"
#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/Compilation.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/text/Json.h"
#include "slang/util/Util.h"
#include "slang/util/VersionInfo.h"

#include "fmt/color.h"
#include "fmt/format.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace slang;
using namespace slang::ast;
using namespace slang::driver;
using namespace slang::netlist;

namespace {

/// Human-readable description of an operator node: its symbol and kind
/// followed by the signedness and width of its result. The symbol alone
/// is ambiguous, as `&` is both bitwise and reduction AND.
auto describeOperation(Operation const &op) -> std::string {
  return fmt::format("{} ({}, {} {}-bit)", toSymbol(op.op), toString(op.op),
                     op.isSigned ? "signed" : "unsigned", op.width);
}

/// Get the TextLocation for a node, if it has one.
auto getNodeLocation(NetlistNode const &node) -> std::optional<TextLocation> {
  switch (node.kind) {
  case NodeKind::Port:
    return node.as<Port>().location;
  case NodeKind::Assignment:
    return node.as<Assignment>().location;
  case NodeKind::Conditional:
    return node.as<Conditional>().location;
  case NodeKind::Case:
    return node.as<Case>().location;
  case NodeKind::Operation:
    return node.as<Operation>().location;
  default:
    return std::nullopt;
  }
}

void reportNodeDiag(NetlistDiagnostics &diagnostics, NetlistNode const &node) {
  switch (node.kind) {
  case NodeKind::Port: {
    auto const &port = node.as<Port>();
    auto srcLoc = port.location.sourceLocation;
    if (port.isInput()) {
      Diagnostic diagnostic(diag::InputPort, srcLoc);
      diagnostic << port.name;
      diagnostics.issue(diagnostic);
    } else if (port.isOutput()) {
      Diagnostic diagnostic(diag::OutputPort, srcLoc);
      diagnostic << port.name;
      diagnostics.issue(diagnostic);
    } else if (port.direction == ast::ArgumentDirection::InOut) {
      Diagnostic diagnostic(diag::InOutPort, srcLoc);
      diagnostic << port.name;
      diagnostics.issue(diagnostic);
    } else {
      SLANG_UNREACHABLE;
    }
    break;
  }
  case NodeKind::Assignment: {
    auto const &assignment = node.as<Assignment>();
    Diagnostic diagnostic(diag::Assignment, assignment.location.sourceLocation);
    diagnostics.issue(diagnostic);
    break;
  }
  case NodeKind::Conditional: {
    auto const &conditional = node.as<Conditional>();
    Diagnostic diagnostic(diag::Conditional,
                          conditional.location.sourceLocation);
    diagnostics.issue(diagnostic);
    break;
  }
  case NodeKind::Case: {
    auto const &caseNode = node.as<Case>();
    Diagnostic diagnostic(diag::Case, caseNode.location.sourceLocation);
    diagnostics.issue(diagnostic);
    break;
  }
  case NodeKind::Operation: {
    auto const &op = node.as<Operation>();
    Diagnostic diagnostic(diag::Operation, op.location.sourceLocation);
    diagnostic << describeOperation(op);
    diagnostics.issue(diagnostic);
    break;
  }
  case NodeKind::Merge:
    break;
  default:
    break;
  }
}

void reportEdgeDiag(NetlistDiagnostics &diagnostics, NetlistEdge &edge) {
  if (edge.symbol != nullptr && !edge.symbol->empty()) {
    Diagnostic diagnostic(diag::Value, edge.symbol->location.sourceLocation);
    diagnostic << fmt::format("{}{}", edge.symbol->hierarchicalPath,
                              toString(edge.bounds));
    diagnostics.issue(diagnostic);
  }
}

void reportNodeText(netlist::FormatBuffer &buffer, FileTable const &fileTable,
                    NetlistNode const &node) {
  switch (node.kind) {
  case NodeKind::Port: {
    auto const &port = node.as<Port>();
    auto loc = port.location.toString(fileTable);
    if (port.isInput()) {
      buffer.format("{}: note: input port {}\n", loc, port.name);
    } else if (port.isOutput()) {
      buffer.format("{}: note: output port {}\n", loc, port.name);
    } else if (port.direction == ast::ArgumentDirection::InOut) {
      buffer.format("{}: note: inout port {}\n", loc, port.name);
    } else {
      SLANG_UNREACHABLE;
    }
    break;
  }
  case NodeKind::Assignment: {
    auto const &assignment = node.as<Assignment>();
    buffer.format("{}: note: assignment\n",
                  assignment.location.toString(fileTable));
    break;
  }
  case NodeKind::Conditional: {
    auto const &conditional = node.as<Conditional>();
    buffer.format("{}: note: conditional statement\n",
                  conditional.location.toString(fileTable));
    break;
  }
  case NodeKind::Case: {
    auto const &caseNode = node.as<Case>();
    buffer.format("{}: note: case statement\n",
                  caseNode.location.toString(fileTable));
    break;
  }
  case NodeKind::Operation: {
    auto const &op = node.as<Operation>();
    buffer.format("{}: note: operation {}\n", op.location.toString(fileTable),
                  describeOperation(op));
    break;
  }
  case NodeKind::Merge:
    break;
  default:
    break;
  }
}

void reportEdgeText(netlist::FormatBuffer &buffer, FileTable const &fileTable,
                    NetlistEdge &edge) {
  if (edge.symbol != nullptr && !edge.symbol->empty()) {
    buffer.format("{}: note: value {}{}\n",
                  edge.symbol->location.toString(fileTable),
                  edge.symbol->hierarchicalPath, toString(edge.bounds));
  }
}

/// Report a path using diagnostics (with source lines and carets)
/// when source locations are available, otherwise fall back to
/// plain text output.
auto reportPath(FileTable const &fileTable, NetlistDiagnostics *diagnostics,
                const NetlistPath &path) -> std::string {

  // Check if the first located node has a source location to
  // decide which reporting mode to use.
  bool useDiag = false;
  if (diagnostics) {
    for (auto const *node : path) {
      if (auto loc = getNodeLocation(*node)) {
        useDiag = loc->hasSourceLocation();
        break;
      }
    }
  }

  if (useDiag) {
    for (size_t i = 0; i < path.size() - 1; ++i) {
      auto const *nodeA = path[i];
      auto const *nodeB = path[i + 1];
      auto edgeIt = nodeA->findEdgeTo(*nodeB);
      SLANG_ASSERT(edgeIt != nodeA->end() &&
                   "edge between nodes not found in path");
      reportNodeDiag(*diagnostics, *nodeA);
      reportEdgeDiag(*diagnostics, **edgeIt);
    }
    reportNodeDiag(*diagnostics, *path.back());
    auto result = diagnostics->getString();
    diagnostics->clear();
    return std::string(result);
  }

  netlist::FormatBuffer buffer;
  for (size_t i = 0; i < path.size() - 1; ++i) {
    auto const *nodeA = path[i];
    auto const *nodeB = path[i + 1];
    auto edgeIt = nodeA->findEdgeTo(*nodeB);
    SLANG_ASSERT(edgeIt != nodeA->end() &&
                 "edge between nodes not found in path");
    reportNodeText(buffer, fileTable, *nodeA);
    reportEdgeText(buffer, fileTable, **edgeIt);
  }
  reportNodeText(buffer, fileTable, *path.back());
  return buffer.str();
}

}; // namespace

auto main(int argc, char **argv) -> int {
  OS::setupConsole();

  Driver driver;
  driver.addStandardArgs();

  std::optional<bool> showHelp;
  driver.cmdLine.add("-h,--help", showHelp, "Display available options");

  std::optional<bool> showVersion;
  driver.cmdLine.add("--version", showVersion,
                     "Display version information and exit");

  std::optional<bool> noColours;
  driver.cmdLine.add("--no-colours", noColours,
                     "Disable colored output (default is enabled only when "
                     "stdout is a terminal that supports it)");

  std::optional<bool> colour;
  driver.cmdLine.add("--colour", colour,
                     "Force colored output even when stdout is not a "
                     "terminal (--no-colours still takes precedence)");

  std::optional<bool> quiet;
  driver.cmdLine.add("-q,--quiet", quiet, "Suppress non-essential output");

  std::optional<bool> stats;
  driver.cmdLine.add("--stats", stats,
                     "Print execution statistics (phase timings and peak "
                     "memory) to stderr");

  std::optional<bool> statsJson;
  driver.cmdLine.add("--stats-json", statsJson,
                     "Print execution statistics as JSON to stderr");

  std::optional<bool> debug;
  driver.cmdLine.add("-d,--debug", debug, "Output debugging information");

  std::optional<bool> reportRegisters;
  driver.cmdLine.add("--report-registers", reportRegisters,
                     "Report all registers in the design to stdout");

  std::optional<bool> combLoops;
  driver.cmdLine.add("--comb-loops", combLoops,
                     "Report any combinational loops in the design to stdout");

  std::optional<bool> noResolveAssignBits;
  driver.cmdLine.add(
      "--no-resolve-assign-bits", noResolveAssignBits,
      "Disable bit-aligned dependency resolution of concatenations, "
      "replications, conversions, and equal-width conditional operators "
      "in assignments and port connections. When set, each LSP on one "
      "side of an assignment fans into every LSP on the other side.");

  std::optional<bool> noPropCutsAcrossPorts;
  driver.cmdLine.add(
      "--no-prop-cuts-across-ports", noPropCutsAcrossPorts,
      "Disable propagation of concat-induced cut points across module "
      "port boundaries. When set, port nodes and module-internal "
      "assignments stay whole-word at port boundaries; "
      "scalar->concat->port->concat->scalar paths are bit-imprecise.");

  std::optional<bool> expandOperations;
  driver.cmdLine.add(
      "--expand-operations", expandOperations,
      "Expand binary, unary and conditional operators on the right-hand "
      "side of an assignment into Operation nodes, so that traced paths "
      "name the operators they pass through. Off by default; the graph "
      "is otherwise unchanged.");

  std::vector<std::string> blackBoxes;
  driver.cmdLine.add(
      "--black-box", blackBoxes,
      "Glob pattern matched against each instance's definition name and "
      "hierarchical path; matched instances record only port-boundary "
      "connectivity. May be repeated. Supports `*` (within a path "
      "segment), `**` or `...` (recursive across `.`), and `?` (single "
      "char within a segment).",
      "<pattern>");

  std::optional<std::string> netlistDotFile;
  driver.cmdLine.add("--netlist-dot", netlistDotFile,
                     "Dump the netlist in DOT format to the specified file, "
                     "or '-' for stdout. Combine with --fan-out, --fan-in or "
                     "--from/--to to render only that cone or path.",
                     "<file>", CommandLineFlags::FilePath);

  std::optional<std::string> fromPointName;
  driver.cmdLine.add("--from", fromPointName,
                     "Specify a start point from which to trace a path. Used "
                     "alone (without --to), reports the combinational fan-out "
                     "cone from the node.",
                     "<name>");

  std::optional<std::string> toPointName;
  driver.cmdLine.add("--to", toPointName,
                     "Specify a finish point to trace a path to. Used alone "
                     "(without --from), reports the combinational fan-in cone "
                     "to the node.",
                     "<name>");

  std::optional<std::string> fanOutName;
  driver.cmdLine.add("--fan-out", fanOutName,
                     "Report the combinational fan-out cone from a named node",
                     "<name>");

  std::optional<std::string> fanInName;
  driver.cmdLine.add("--fan-in", fanInName,
                     "Report the combinational fan-in cone to a named node",
                     "<name>");

  std::optional<std::string> sensitivityName;
  driver.cmdLine.add(
      "--sensitivity", sensitivityName,
      "Report the clocks/resets gating a named node. For a register the "
      "node's own clocking edges are listed; otherwise the union over every "
      "register in its combinational fan-out.",
      "<name>");

  std::optional<std::string> constantDriversName;
  driver.cmdLine.add(
      "--constant-drivers", constantDriversName,
      "Report the constant values driving a named node when its "
      "combinational fan-in bottoms out only at literals (i.e. the node is "
      "tied off). Prints nothing driving it if any register or external "
      "input reaches the node.",
      "<name>");

  std::optional<std::string> driversName;
  driver.cmdLine.add(
      "--drivers", driversName,
      "Report, per bit, which node drives a named signal. An optional bit "
      "range narrows the query, e.g. `m.sig[3:0]` or `m.sig[2]`; without one "
      "the whole signal is reported.",
      "<name[bit-range]>");

  std::optional<std::string> findPattern;
  driver.cmdLine.add(
      "--find", findPattern,
      "Find named nodes matching a glob pattern. Supports `*` (within a "
      "path segment), `**` or `...` (recursive across `.`), and `?` "
      "(single char within a segment).",
      "<pattern>");

  std::optional<std::string> findRegexPattern;
  driver.cmdLine.add("--find-regex", findRegexPattern,
                     "Find named nodes matching a regex pattern", "<pattern>");

  std::optional<std::string> format;
  driver.cmdLine.add(
      "--format", format,
      "Output format for the tabular query commands (--report-registers, "
      "--find, --find-regex, --fan-out, --fan-in, --sensitivity, "
      "--constant-drivers, --drivers): 'table' (default) or 'json'",
      "<table|json>");

  std::vector<std::string> scopeFilters;
  driver.cmdLine.add(
      "--scope", scopeFilters,
      "Restrict the node-listing query commands (--report-registers, --find, "
      "--find-regex, --fan-out, --fan-in, --sensitivity) to a hierarchical "
      "subtree. A node passes if its path is the scope or a descendant of it "
      "(so `top.cpu` matches `top.cpu.alu.x` but not `top.cpu2`). Literal "
      "paths only; use --name for globs. May be repeated.",
      "<path>");

  std::vector<std::string> nameFilters;
  driver.cmdLine.add(
      "--name", nameFilters,
      "Restrict the same node-listing query commands to nodes whose "
      "hierarchical path matches a glob pattern (same syntax as --find: "
      "`*`/`?` stay within a segment, `**`/`...` cross boundaries). May be "
      "repeated.",
      "<pattern>");

  std::optional<std::string> outputFile;
  driver.cmdLine.add("-o,--output", outputFile,
                     "Write tabular query output to the given file instead of "
                     "stdout ('-' for stdout)",
                     "<file>", CommandLineFlags::FilePath);

  std::optional<std::string> saveNetlistFile;
  driver.cmdLine.add("--save-netlist", saveNetlistFile,
                     "Save the netlist to a JSON file", "<file>",
                     CommandLineFlags::FilePath);

  std::optional<std::string> loadNetlistFile;
  driver.cmdLine.add("--load-netlist", loadNetlistFile,
                     "Load a netlist from a JSON file (skips compilation)",
                     "<file>", CommandLineFlags::FilePath);

  if (!driver.parseCommandLine(argc, argv)) {
    return 1;
  }

  if (showHelp == true) {
    std::cout << fmt::format(
        "{}\n",
        driver.cmdLine.getHelpText("slang SystemVerilog netlist tool").c_str());
    return 0;
  }

  if (showVersion == true) {
    printf("rtl-wiring-inspector version %d.%d.%d+%s\n", VersionInfo::getMajor(),
           VersionInfo::getMinor(), VersionInfo::getPatch(),
           std::string(VersionInfo::getHash()).c_str());
    return 0;
  }

  if (debug) {
    Config::getInstance().debugEnabled = true;
  }

  if (quiet) {
    Config::getInstance().quietEnabled = true;
  }

  // Output format for the tabular query commands.
  enum class Format { Table, Json };
  auto outputFormat = Format::Table;
  if (format) {
    if (*format == "json") {
      outputFormat = Format::Json;
    } else if (*format == "table") {
      outputFormat = Format::Table;
    } else {
      fmt::print(stderr,
                 "error: unknown --format value '{}'; expected 'table' or "
                 "'json'\n",
                 *format);
      return 1;
    }
  }

  // Colours are enabled only when stdout is an interactive terminal unless
  // an explicit flag says otherwise.
  bool useColours = false;
  if (noColours == true) {
    useColours = false;
  } else if (colour == true) {
    useColours = true;
  } else {
    useColours = OS::fileSupportsColors(stdout);
  }

  // Write file contents, turning a stream failure into an error that names
  // the target instead of leaking an iostream failure message.
  auto writeFile = [](std::string const &path, std::string_view content) {
    if (path == "-") {
      OS::print(content);
      return;
    }
    errno = 0;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      auto error = std::error_code(errno, std::generic_category());
      SLANG_THROW(std::runtime_error(
          fmt::format("could not write file '{}': {}", path,
                      error.message())));
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    if (file.fail()) {
      SLANG_THROW(std::runtime_error(
          fmt::format("could not write file '{}': write failed", path)));
    }
  };

  auto writeOutput = [&](std::string_view content) {
    if (outputFile && *outputFile != "-") {
      writeFile(*outputFile, content);
    } else {
      OS::print(content);
    }
  };

  auto nodeKindName = [](NodeKind kind) -> std::string_view {
    switch (kind) {
    case NodeKind::Port:
      return "Port";
    case NodeKind::Variable:
      return "Variable";
    case NodeKind::Assignment:
      return "Assignment";
    case NodeKind::Conditional:
      return "Conditional";
    case NodeKind::Case:
      return "Case";
    case NodeKind::Merge:
      return "Merge";
    case NodeKind::State:
      return "State";
    case NodeKind::Constant:
      return "Constant";
    case NodeKind::Operation:
      return "Operation";
    default:
      return "None";
    }
  };

  auto edgeKindName = [](ast::EdgeKind kind) -> std::string_view {
    switch (kind) {
    case ast::EdgeKind::PosEdge:
      return "PosEdge";
    case ast::EdgeKind::NegEdge:
      return "NegEdge";
    case ast::EdgeKind::BothEdges:
      return "BothEdges";
    default:
      return "None";
    }
  };

  // Append one node step of the path/loop JSON representation.
  auto writeNodeStep = [&](JsonWriter &writer, NetlistNode const &node,
                           FileTable const &fileTable) {
    writer.startObject();
    writer.writeProperty("type");
    writer.writeValue(std::string_view("node"));
    writer.writeProperty("node_kind");
    writer.writeValue(nodeKindName(node.kind));
    writer.writeProperty("name");
    switch (node.kind) {
    case NodeKind::Port:
      writer.writeValue(node.as<Port>().name);
      break;
    case NodeKind::State:
      writer.writeValue(node.as<State>().name);
      break;
    case NodeKind::Variable:
      writer.writeValue(node.as<Variable>().name);
      break;
    default:
      writer.writeValue(std::string_view{});
      break;
    }
    writer.writeProperty("path");
    auto path = node.getHierarchicalPath();
    writer.writeValue(path ? *path : std::string_view{});
    writer.writeProperty("location");
    auto location = node.getLocation();
    auto locationText =
        location ? location->toString(fileTable) : std::string();
    writer.writeValue(locationText);
    writer.endObject();
  };

  // Append one edge step of the path/loop JSON representation.
  auto writeEdgeStep = [&](JsonWriter &writer, NetlistEdge const &edge) {
    writer.startObject();
    writer.writeProperty("type");
    writer.writeValue(std::string_view("edge"));
    writer.writeProperty("symbol_path");
    writer.writeValue(edge.symbol
                          ? std::string_view(edge.symbol->hierarchicalPath)
                          : std::string_view{});
    writer.writeProperty("bits");
    writer.writeValue(toString(edge.bounds));
    writer.writeProperty("edge_kind");
    writer.writeValue(edgeKindName(edge.edgeKind));
    writer.endObject();
  };

  // Serialize a path as an alternating node/edge step array. The number of
  // edge steps is one less than the number of node steps.
  auto pathToJson = [&](NetlistPath const &path,
                        FileTable const &fileTable) -> std::string {
    JsonWriter writer;
    writer.setPrettyPrint(true);
    writer.startArray();
    for (size_t i = 0; i < path.size(); ++i) {
      writeNodeStep(writer, *path[i], fileTable);
      if (i + 1 < path.size()) {
        auto edgeIt = path[i]->findEdgeTo(*path[i + 1]);
        SLANG_ASSERT(edgeIt != path[i]->end() &&
                     "edge between nodes not found in path");
        writeEdgeStep(writer, **edgeIt);
      }
    }
    writer.endArray();
    return fmt::format("{}\n", writer.view());
  };

  // Serialize detected loops. Each entry lists the cycle's nodes and the
  // edges connecting consecutive nodes, including the closing edge.
  auto loopsToJson = [&](
      std::vector<std::vector<NetlistNode const *>> const &cycles,
      FileTable const &fileTable) -> std::string {
    JsonWriter writer;
    writer.setPrettyPrint(true);
    writer.startArray();
    for (auto const &cycle : cycles) {
      writer.startObject();
      writer.writeProperty("length");
      writer.writeValue(static_cast<int64_t>(cycle.size()));
      writer.writeProperty("nodes");
      writer.startArray();
      for (auto const *node : cycle) {
        writeNodeStep(writer, *node, fileTable);
      }
      writer.endArray();
      writer.writeProperty("edges");
      writer.startArray();
      for (size_t i = 0; i < cycle.size(); ++i) {
        auto const *from = cycle[i];
        auto const *to = cycle[(i + 1) % cycle.size()];
        auto edgeIt = from->findEdgeTo(*to);
        SLANG_ASSERT(edgeIt != from->end() &&
                     "edge between cycle nodes not found");
        writeEdgeStep(writer, **edgeIt);
      }
      writer.endArray();
      writer.endObject();
    }
    writer.endArray();
    return fmt::format("{}\n", writer.view());
  };

  // Emit a table either as a formatted text table or as a JSON array of
  // objects keyed by the lower-cased column headers.
  auto emitTable = [&](Utilities::Row const &header,
                       Utilities::Table const &table) {
    if (outputFormat == Format::Json) {
      JsonWriter writer;
      writer.setPrettyPrint(true);
      writer.startArray();
      for (auto const &row : table) {
        writer.startObject();
        for (size_t i = 0; i < header.size() && i < row.size(); ++i) {
          std::string key(header[i].size(), '\0');
          std::transform(header[i].begin(), header[i].end(), key.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          writer.writeProperty(key);
          writer.writeValue(row[i]);
        }
        writer.endObject();
      }
      writer.endArray();
      writeOutput(fmt::format("{}\n", writer.view()));
    } else {
      netlist::FormatBuffer buffer;
      Utilities::formatTable(buffer, header, table);
      writeOutput(buffer.str());
    }
  };

  // Whether a node's hierarchical path passes the --scope/--name filters. A
  // node must fall within some --scope subtree (if any given) and match some
  // --name glob (if any given). With no filters set, everything passes.
  auto passesFilters = [&](std::string_view path) -> bool {
    if (!scopeFilters.empty()) {
      auto inScope = std::any_of(
          scopeFilters.begin(), scopeFilters.end(),
          [&](auto const &scope) { return pathInScope(path, scope); });
      if (!inScope) {
        return false;
      }
    }
    if (!nameFilters.empty()) {
      std::string subject(path);
      auto named = std::any_of(
          nameFilters.begin(), nameFilters.end(), [&](auto const &p) {
            return wildcardMatch(subject.c_str(), p.c_str());
          });
      if (!named) {
        return false;
      }
    }
    return true;
  };

  // Split a query of the form "path[hi:lo]" or "path[bit]" into a base path
  // and an optional bit range. A trailing bracketed expression that does not
  // parse as a range is treated as part of the name.
  auto parseNameAndRange = [](std::string_view spec)
      -> std::pair<std::string, std::optional<DriverBitRange>> {
    auto parseInt = [](std::string_view s) -> std::optional<int32_t> {
      int32_t value = 0;
      auto *end = s.data() + s.size();
      auto [ptr, ec] = std::from_chars(s.data(), end, value);
      if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
      }
      return value;
    };
    auto whole = std::string(spec);
    if (spec.empty() || spec.back() != ']') {
      return {whole, std::nullopt};
    }
    auto open = spec.rfind('[');
    if (open == std::string_view::npos) {
      return {whole, std::nullopt};
    }
    auto inner = spec.substr(open + 1, spec.size() - open - 2);
    auto path = std::string(spec.substr(0, open));
    auto colon = inner.find(':');
    if (colon == std::string_view::npos) {
      auto bit = parseInt(inner);
      if (!bit) {
        return {whole, std::nullopt};
      }
      return {path, DriverBitRange{*bit, *bit}};
    }
    auto hi = parseInt(inner.substr(0, colon));
    auto lo = parseInt(inner.substr(colon + 1));
    if (!hi || !lo) {
      return {whole, std::nullopt};
    }
    return {path, DriverBitRange{*hi, *lo}};
  };

  // Human-readable description of a driver node for the --drivers report.
  auto describeDriver = [](NetlistNode &node) -> std::string {
    switch (node.kind) {
    case NodeKind::Port: {
      auto const &port = node.as<Port>();
      auto const *dir =
          port.isInput() ? "input" : (port.isOutput() ? "output" : "inout");
      return fmt::format("{} port {}", dir, port.hierarchicalPath);
    }
    case NodeKind::Variable:
      return fmt::format("variable {}", node.as<Variable>().hierarchicalPath);
    case NodeKind::State:
      return fmt::format("register {}", node.as<State>().hierarchicalPath);
    case NodeKind::Constant:
      return fmt::format("constant {}", node.as<Constant>().value.toString());
    case NodeKind::Assignment:
      return "assignment";
    case NodeKind::Conditional:
      return "conditional";
    case NodeKind::Case:
      return "case";
    case NodeKind::Operation:
      return fmt::format("operation {}",
                         describeOperation(node.as<Operation>()));
    case NodeKind::Merge:
      return "merge";
    default:
      return "unknown";
    }
  };

  using Clock = std::chrono::steady_clock;
  std::vector<std::pair<std::string, double>> phaseTimes;

  auto timePhase = [&](const std::string &name, auto &&fn) {
    auto start = Clock::now();
    fn();
    std::chrono::duration<double> elapsed = Clock::now() - start;
    phaseTimes.emplace_back(name, elapsed.count());
  };

  // Pointer to the graph, set once it's constructed, for printStats access.
  NetlistGraph *graphPtr = nullptr;
  bool graphLoaded = false;

  auto printStatsJson = [&] {
    auto peakRSS = OS::getPeakMemoryBytes();
    JsonWriter writer;
    writer.startObject();
    writer.writeProperty("time_seconds");
    writer.startObject();
    for (auto &[name, seconds] : phaseTimes) {
      writer.writeProperty(name);
      writer.writeValue(seconds);
    }
    writer.endObject();
    writer.writeProperty("peak_rss_bytes");
    writer.writeValue(peakRSS);

    if (graphPtr) {
      writer.writeProperty("graph_source");
      writer.writeValue(graphLoaded ? std::string_view("loaded")
                                     : std::string_view("built"));
      writer.writeProperty("graph_nodes");
      writer.writeValue(static_cast<int64_t>(graphPtr->numNodes()));
      writer.writeProperty("graph_edges");
      writer.writeValue(static_cast<int64_t>(graphPtr->numEdges()));

      // Build phasing is only meaningful for a freshly built graph.
      if (!graphLoaded) {
        auto const &bp = graphPtr->getBuildProfile();
        writer.writeProperty("netlist_profile");
        writer.startObject();

        writer.writeProperty("phase1_collect_seconds");
        writer.writeValue(bp.phase1_collectSeconds);
        writer.writeProperty("phase2_parallel_seconds");
        writer.writeValue(bp.phase2_parallelSeconds);
        writer.writeProperty("phase3_drain_seconds");
        writer.writeValue(bp.phase3_drainSeconds);
        writer.writeProperty("phase4_rvalue_seconds");
        writer.writeValue(bp.phase4_rvalueSeconds);
        writer.writeProperty("phase5_merge_edges_seconds");
        writer.writeValue(bp.phase5_mergeEdgesSeconds);

        writer.writeProperty("drain_pending_rvalues_seconds");
        writer.writeValue(bp.drain_pendingRValuesSeconds);
        writer.writeProperty("drain_merges_seconds");
        writer.writeValue(bp.drain_mergesSeconds);

        writer.writeProperty("deferred_block_count");
        writer.writeValue(static_cast<int64_t>(bp.deferredBlockCount));
        writer.writeProperty("deferred_pending_rvalue_count");
        writer.writeValue(static_cast<int64_t>(bp.deferredPendingRValueCount));

        writer.writeProperty("task_min_seconds");
        writer.writeValue(bp.taskMinSeconds);
        writer.writeProperty("task_max_seconds");
        writer.writeValue(bp.taskMaxSeconds);
        writer.writeProperty("task_mean_seconds");
        writer.writeValue(bp.taskMeanSeconds);
        writer.writeProperty("task_median_seconds");
        writer.writeValue(bp.taskMedianSeconds);
        writer.writeProperty("task_total_seconds");
        writer.writeValue(bp.taskTotalSeconds);
        writer.writeProperty("task_cpu_total_seconds");
        writer.writeValue(bp.taskCpuTotalSeconds);
        writer.writeProperty("num_threads");
        writer.writeValue(static_cast<int64_t>(bp.numThreads));

        writer.endObject();
      }

      writer.writeProperty("black_boxes");
      writer.startObject();
      writer.writeProperty("patterns");
      writer.startArray();
      for (auto const &pattern : graphPtr->getBlackBoxPatterns()) {
        writer.writeValue(pattern);
      }
      writer.endArray();
      writer.writeProperty("matched_paths");
      writer.startArray();
      for (auto const &path : graphPtr->getBlackBoxPaths()) {
        writer.writeValue(path);
      }
      writer.endArray();
      writer.writeProperty("unmatched_patterns");
      writer.startArray();
      for (auto const &pattern : graphPtr->getUnmatchedBlackBoxPatterns()) {
        writer.writeValue(pattern);
      }
      writer.endArray();
      writer.endObject();
    }

    writer.endObject();
    OS::printE(fmt::format("{}\n", writer.view()));
  };

  auto printStatsHuman = [&] {
    auto peakRSS = OS::getPeakMemoryBytes();
    double total = 0;
    for (auto &[name, seconds] : phaseTimes) {
      total += seconds;
    }

    auto fmtTime = [](double s) { return fmt::format("{:.3f}s", s); };

    netlist::FormatBuffer buf;

    buf.format("\nPhase Timing\n");
    Utilities::Table phaseRows;
    for (auto &[name, seconds] : phaseTimes) {
      phaseRows.push_back({name, fmtTime(seconds)});
    }
    phaseRows.push_back({"total", fmtTime(total)});
    Utilities::formatTable(buf, {"Phase", "Time"}, phaseRows);

    if (graphPtr && !graphLoaded) {
      auto const &bp = graphPtr->getBuildProfile();

      buf.format("\nNetlist Build ({} thread{})\n", bp.numThreads,
                 bp.numThreads == 1 ? "" : "s");
      Utilities::formatTable(
          buf, {"Phase", "Time"},
          {{"collect", fmtTime(bp.phase1_collectSeconds)},
           {"parallel DFA", fmtTime(bp.phase2_parallelSeconds)},
           {"drain", fmtTime(bp.phase3_drainSeconds)},
           {"resolve R-values", fmtTime(bp.phase4_rvalueSeconds)},
           {"merge edges", fmtTime(bp.phase5_mergeEdgesSeconds)},
           {"total", fmtTime(bp.totalSeconds())}});

      if (bp.deferredBlockCount > 0) {
        buf.format("\nDFA Tasks ({} blocks, {} pending R-values)\n",
                   bp.deferredBlockCount, bp.deferredPendingRValueCount);
        Utilities::formatTable(buf, {"Statistic", "Time"},
                               {{"min", fmtTime(bp.taskMinSeconds)},
                                {"max", fmtTime(bp.taskMaxSeconds)},
                                {"mean", fmtTime(bp.taskMeanSeconds)},
                                {"median", fmtTime(bp.taskMedianSeconds)},
                                {"wall sum", fmtTime(bp.taskTotalSeconds)},
                                {"CPU sum", fmtTime(bp.taskCpuTotalSeconds)}});
      }
    }

    if (graphPtr) {
      buf.format("\nNetlist size: {} nodes, {} edges\n", graphPtr->numNodes(),
                 graphPtr->numEdges());

      auto const &blackBoxPaths = graphPtr->getBlackBoxPaths();
      auto const unmatched = graphPtr->getUnmatchedBlackBoxPatterns();
      if (!blackBoxPaths.empty() || !unmatched.empty()) {
        buf.format("\nBlack boxes: {} matched path{}\n",
                   blackBoxPaths.size(),
                   blackBoxPaths.size() == 1 ? "" : "s");
        for (auto const &path : blackBoxPaths) {
          buf.format("  {}\n", path);
        }
        for (auto const &pattern : unmatched) {
          buf.format("  unmatched pattern: {}\n", pattern);
        }
      }
    }

    buf.format("\nPeak RSS: {:.1f} MB\n",
               static_cast<double>(peakRSS) / (1024.0 * 1024.0));
    fmt::print(stderr, "{}", buf.str());
  };

  auto printStats = [&] {
    if (stats) {
      printStatsHuman();
    }
    if (statsJson) {
      printStatsJson();
    }
  };

  SLANG_TRY {

    NetlistGraph graph;
    graphPtr = &graph;
    std::unique_ptr<Compilation> compilation;
    std::unique_ptr<NetlistDiagnostics> diagnostics;

    if (loadNetlistFile) {
      // Load a previously-saved netlist (skips compilation).
      graphLoaded = true;
      SmallVector<char> fileContent;
      auto ec = OS::readFile(*loadNetlistFile, fileContent);
      if (ec) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not read file: {}", *loadNetlistFile)));
      }
      timePhase("load", [&] {
        NetlistSerializer::deserialize(
            std::string_view(fileContent.data(), fileContent.size()), graph);
      });

      DEBUG_PRINT("Loaded netlist has {} nodes and {} edges\n",
                  graph.numNodes(), graph.numEdges());
    } else {
      // Build a netlist from source files.
      if (!driver.processOptions()) {
        return 2;
      }

      bool parseOk = false;
      timePhase("parsing", [&] { parseOk = driver.parseAllSources(); });
      if (!parseOk) {
        return 1;
      }

      timePhase("elaboration", [&] {
        compilation = driver.createCompilation();
        driver.reportCompilation(*compilation, true);

        // Force construction of the whole AST.
        VisitAll va;
        compilation->getRoot().visit(va);

        // Freeze the compilation for subsequent multithreaded analysis.
        compilation->freeze();
      });

      if (!driver.reportDiagnostics(true)) {
        return 1;
      }

      std::unique_ptr<analysis::AnalysisManager> analysisManager;
      timePhase("analysis",
                [&] { analysisManager = driver.runAnalysis(*compilation); });
      if (!driver.reportDiagnostics(true)) {
        return 1;
      }

      timePhase("netlist", [&] {
        BuilderOptions const opts{
            .resolveAssignBits = !noResolveAssignBits.value_or(false),
            .propCutsAcrossPorts = !noPropCutsAcrossPorts.value_or(false),
            .expandOperations = expandOperations.value_or(false),
            .numThreads = driver.options.numThreads.value_or(0),
            .blackBoxes = blackBoxes};
        graph.build(*compilation, *analysisManager, opts);
      });

      DEBUG_PRINT("Netlist has {} nodes and {} edges\n", graph.numNodes(),
                  graph.numEdges());

      // A pattern that matched nothing is not an error, but silently doing
      // nothing hides typos in the pattern.
      for (auto const &pattern : graph.getUnmatchedBlackBoxPatterns()) {
        fmt::print(stderr,
                   "warning: --black-box pattern '{}' matched no instances\n",
                   pattern);
      }

      diagnostics =
          std::make_unique<NetlistDiagnostics>(*compilation, useColours);
    }

    // --- Commands that work on both built and loaded netlists ---

    // Write the netlist out.
    if (saveNetlistFile) {
      auto json = NetlistSerializer::serialize(graph);
      writeFile(*saveNetlistFile, json);
      printStats();
      return 0;
    }

    // A lone --from/--to endpoint means "the reachable cone", which is exactly
    // the combinational fan-out/fan-in from that node. Alias it onto the
    // corresponding cone selector so every downstream handler (tabular output
    // and scoped --netlist-dot) treats them uniformly. When both endpoints are
    // given, path-finding takes over instead; an explicit --fan-out/--fan-in
    // always wins.
    if (fromPointName && !toPointName && !fanOutName && !fanInName) {
      fanOutName = fromPointName;
    } else if (toPointName && !fromPointName && !fanInName && !fanOutName) {
      fanInName = toPointName;
    }

    // Collect the combinational fan-out over every node with the given name.
    auto collectFanOut = [&](std::string const &name)
        -> std::vector<NetlistNode *> {
      auto startNodes = graph.lookupAll(name);
      if (startNodes.empty()) {
        SLANG_THROW(
            std::runtime_error(fmt::format("could not find node: {}", name)));
      }
      std::unordered_set<NetlistNode *> seen;
      std::vector<NetlistNode *> result;
      for (auto *node : startNodes) {
        if (seen.insert(node).second) {
          result.push_back(node);
        }
        for (auto *n : graph.getCombFanOut(*node)) {
          if (seen.insert(n).second) {
            result.push_back(n);
          }
        }
      }
      return result;
    };

    // Collect the combinational fan-in over every node with the given name.
    auto collectFanIn = [&](std::string const &name)
        -> std::vector<NetlistNode *> {
      auto endNodes = graph.lookupAll(name);
      if (endNodes.empty()) {
        SLANG_THROW(
            std::runtime_error(fmt::format("could not find node: {}", name)));
      }
      std::unordered_set<NetlistNode *> seen;
      std::vector<NetlistNode *> result;
      for (auto *node : endNodes) {
        if (seen.insert(node).second) {
          result.push_back(node);
        }
        for (auto *n : graph.getCombFanIn(*node)) {
          if (seen.insert(n).second) {
            result.push_back(n);
          }
        }
      }
      return result;
    };

    // Find up to one path for every start/end node pair with the given names.
    auto collectPaths = [&](std::string const &fromName,
                            std::string const &toName)
        -> std::vector<NetlistPath> {
      auto fromNodes = graph.lookupAll(fromName);
      if (fromNodes.empty()) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not find start point: {}", fromName)));
      }
      auto toNodes = graph.lookupAll(toName);
      if (toNodes.empty()) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not find finish point: {}", toName)));
      }
      std::vector<NetlistPath> paths;
      PathFinder pathFinder;
      for (auto *from : fromNodes) {
        for (auto *to : toNodes) {
          auto path = pathFinder.find(*from, *to);
          if (!path.empty()) {
            paths.push_back(std::move(path));
          }
        }
      }
      return paths;
    };

    // Emit a node-list table for a fan cone, listing each signal once even
    // when it is split across several nodes.
    auto emitConeTable = [&](std::vector<NetlistNode *> const &nodes) {
      auto header = Utilities::Row{"Name", "Location"};
      auto table = Utilities::Table{};
      std::unordered_set<std::string> reported;
      for (auto const *n : nodes) {
        auto path = n->getHierarchicalPath();
        if (path.has_value() && passesFilters(*path)) {
          auto loc = n->getLocation();
          auto location = loc ? loc->toString(graph.fileTable) : std::string();
          if (!reported.insert(std::string(*path) + "\n" + location).second) {
            continue;
          }
          table.push_back(Utilities::Row{std::string(*path), location});
        }
      }
      emitTable(header, table);
    };

    if (reportRegisters) {
      auto header = Utilities::Row{"Name", "Location"};
      auto table = Utilities::Table{};
      std::unordered_set<std::string> reported;

      for (auto const &node : graph.filterNodes(NodeKind::State)) {
        auto const &stateNode = node->as<State>();
        if (!passesFilters(stateNode.hierarchicalPath)) {
          continue;
        }
        auto loc = stateNode.location.toString(graph.fileTable);
        // Bit-aligned resolution can split one register into several State
        // nodes; report the register once.
        if (!reported.insert(stateNode.hierarchicalPath + "\n" + loc).second) {
          continue;
        }
        table.push_back(Utilities::Row{stateNode.hierarchicalPath, loc});
      }

      emitTable(header, table);
      printStats();
      return 0;
    }

    // Report combinational loops, optionally filtered and machine-readable.
    if (combLoops) {
      CombLoops combLoopsAnalysis(graph);
      auto cycles = combLoopsAnalysis.getAllLoops();

      // Keep a loop when any of its nodes passes --scope/--name.
      auto cyclePasses = [&](std::vector<NetlistNode const *> const &cycle) {
        if (scopeFilters.empty() && nameFilters.empty()) {
          return true;
        }
        return std::any_of(cycle.begin(), cycle.end(), [&](auto *node) {
          auto path = node->getHierarchicalPath();
          return path.has_value() && passesFilters(*path);
        });
      };
      std::vector<std::vector<NetlistNode const *>> filtered;
      for (auto const &cycle : cycles) {
        if (cyclePasses(cycle)) {
          filtered.push_back(cycle);
        }
      }

      if (outputFormat == Format::Json) {
        writeOutput(loopsToJson(filtered, graph.fileTable));
        printStats();
        return 0;
      }

      netlist::FormatBuffer buffer;
      if (filtered.empty()) {
        buffer.format("No combinational loops detected in the design.\n");
      } else {
        for (auto const &cycle : filtered) {
          buffer.format("Combinational loop detected:\n\n");
          buffer.format("{}\n",
                        reportPath(graph.fileTable, diagnostics.get(), cycle));
        }
      }
      writeOutput(buffer.str());
      printStats();
      return 0;
    }

    // Output a DOT file of the netlist. When combined with a fan-out, fan-in
    // or path selector, render only that induced subgraph instead of the
    // whole netlist. A signal name can resolve to several nodes, so a cone
    // covers every one of them and a path scope is the union of every path
    // found between same-named endpoints.
    if (netlistDotFile) {
      netlist::FormatBuffer buffer;
      if (fanOutName || fanInName || (fromPointName && toPointName)) {
        std::unordered_set<NetlistNode const *> scope;
        if (fanOutName) {
          for (auto *node : collectFanOut(*fanOutName)) {
            scope.insert(node);
          }
        } else if (fanInName) {
          for (auto *node : collectFanIn(*fanInName)) {
            scope.insert(node);
          }
        } else {
          auto paths = collectPaths(*fromPointName, *toPointName);
          if (paths.empty()) {
            SLANG_THROW(std::runtime_error(fmt::format(
                "no path between {} and {}", *fromPointName, *toPointName)));
          }
          for (auto const &path : paths) {
            for (auto const *node : path) {
              scope.insert(node);
            }
          }
        }
        NetlistDot::render(graph, buffer, scope);
      } else {
        NetlistDot::render(graph, buffer);
      }
      writeFile(*netlistDotFile, buffer.str());
      printStats();
      return 0;
    }

    // Find named nodes by wildcard or regex pattern.
    if (findPattern.has_value() || findRegexPattern.has_value()) {
      auto nodes = findPattern.has_value()
                       ? graph.findNodes(*findPattern)
                       : graph.findNodesRegex(*findRegexPattern);
      auto header = Utilities::Row{"Name", "Location"};
      auto table = Utilities::Table{};
      // A signal split across several nodes by bit-aligned resolution would
      // otherwise be listed once per node with the same name and location.
      std::unordered_set<std::string> reported;
      for (auto const *node : nodes) {
        auto path = node->getHierarchicalPath();
        if (!passesFilters(path.value_or(""))) {
          continue;
        }
        auto name = std::string(path.value_or("(unnamed)"));
        if (!reported.insert(name).second) {
          continue;
        }
        auto loc = node->getLocation();
        auto location = loc ? loc->toString(graph.fileTable) : std::string();
        table.push_back(Utilities::Row{name, location});
      }
      emitTable(header, table);
      printStats();
      return 0;
    }

    // Report combinational fan-out from a named node. Every node sharing the
    // name contributes its cone; the union is reported once per node.
    if (fanOutName.has_value()) {
      emitConeTable(collectFanOut(*fanOutName));
      printStats();
      return 0;
    }

    // Report combinational fan-in to a named node.
    if (fanInName.has_value()) {
      emitConeTable(collectFanIn(*fanInName));
      printStats();
      return 0;
    }

    // Report the clocks/resets gating a named node. A single hierarchical
    // name can resolve to several nodes (e.g. a register's State node and
    // its same-named output Port), so aggregate sensitivity per symbol. When
    // a symbol has a State node, only its own clocking edges count; the
    // same-named Port would otherwise pick up the clocks of the registers it
    // feeds. Symbols without a State node fall back to the fan-out union.
    if (sensitivityName.has_value()) {
      auto nodes = graph.findNodes(*sensitivityName);
      if (nodes.empty()) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not find node: {}", *sensitivityName)));
      }
      std::ranges::sort(nodes, {}, [](NetlistNode *node) { return node->ID; });

      std::map<std::string, std::vector<NetlistNode *>> nodesByPath;
      for (auto *node : nodes) {
        nodesByPath[std::string(node->getHierarchicalPath().value_or(""))]
            .push_back(node);
      }

      std::vector<NetlistGraph::SensitivitySource> sensitivity;
      for (auto &[path, group] : nodesByPath) {
        bool hasState =
            std::any_of(group.begin(), group.end(), [](NetlistNode *node) {
              return node->kind == NodeKind::State;
            });
        for (auto *node : group) {
          if (hasState && node->kind != NodeKind::State) {
            continue;
          }
          for (auto const &src : graph.getSensitivity(*node)) {
            if (std::find(sensitivity.begin(), sensitivity.end(), src) ==
                sensitivity.end()) {
              sensitivity.push_back(src);
            }
          }
        }
      }
      auto header = Utilities::Row{"Name", "Edge", "Location"};
      auto table = Utilities::Table{};
      for (auto const &src : sensitivity) {
        auto path = src.source->getHierarchicalPath();
        if (!passesFilters(path.value_or(""))) {
          continue;
        }
        auto loc = src.source->getLocation();
        table.push_back(Utilities::Row{std::string(path.value_or("(unnamed)")),
                                       std::string(ast::toString(src.edgeKind)),
                                       loc ? loc->toString(graph.fileTable)
                                           : std::string()});
      }
      emitTable(header, table);
      printStats();
      return 0;
    }

    // Report the constant values driving a named node.
    if (constantDriversName.has_value()) {
      auto *node = graph.lookup(*constantDriversName);
      if (node == nullptr) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not find node: {}", *constantDriversName)));
      }
      auto constants = graph.getConstantDrivers(*node);
      auto header = Utilities::Row{"Value", "Location"};
      auto table = Utilities::Table{};
      for (auto const *n : constants) {
        auto const &constant = n->as<Constant>();
        auto loc = constant.getLocation();
        table.push_back(Utilities::Row{constant.value.toString(),
                                       loc ? loc->toString(graph.fileTable)
                                           : std::string()});
      }
      emitTable(header, table);
      printStats();
      return 0;
    }

    // Report, per bit, the nodes driving a named signal.
    if (driversName.has_value()) {
      auto [path, range] = parseNameAndRange(*driversName);
      // A signal may exist only as an edge annotation, with no node of its
      // own (an internal wire or reg); report its drivers, which is the empty
      // table when it has none. Only a name that is absent is an error.
      if (!graph.hasSymbol(path)) {
        SLANG_THROW(
            std::runtime_error(fmt::format("could not find node: {}", path)));
      }
      // Without an explicit bit range, report the whole signal.
      auto drivers =
          range ? graph.getBitDrivers(path, *range) : graph.getBitDrivers(path);
      auto header = Utilities::Row{"Bits", "Driver", "Location"};
      auto table = Utilities::Table{};
      for (auto const &bd : drivers) {
        auto loc = bd.driver->getLocation();
        table.push_back(Utilities::Row{
            toString(bd.bounds), describeDriver(*bd.driver),
            loc ? loc->toString(graph.fileTable) : std::string()});
      }
      emitTable(header, table);
      printStats();
      return 0;
    }

    // Find a point-to-point path in the netlist. A single name can resolve
    // to several split nodes, so search every start/end pair and report the
    // first path found.
    if (fromPointName.has_value() && toPointName.has_value()) {
      DEBUG_PRINT("Searching for path between: {} and {}\n", *fromPointName,
                  *toPointName);

      auto paths = collectPaths(*fromPointName, *toPointName);
      if (paths.empty()) {
        SLANG_THROW(std::runtime_error(fmt::format(
            "no path between {} and {}", *fromPointName, *toPointName)));
      }

      if (outputFormat == Format::Json) {
        writeOutput(pathToJson(paths.front(), graph.fileTable));
        printStats();
        return 0;
      }

      auto result =
          reportPath(graph.fileTable, diagnostics.get(), paths.front());
      writeOutput(fmt::format("{}\n", result));
      printStats();
      return 0;
    }

    // A bare --stats/--stats-json is a complete action: build (or load) the
    // graph and report its statistics.
    if (stats || statsJson) {
      printStats();
      return 0;
    }

    // If we reach here, no action was specified.
    SLANG_THROW(std::runtime_error("no action specified"));
  }
  SLANG_CATCH(const std::exception &e) {
    SLANG_REPORT_EXCEPTION(e, "{}\n");
    return 1;
  }

  return 0;
}
