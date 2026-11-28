#include "slang/driver/Driver.h"

#include "report/ReportDrivers.hpp"
#include "report/ReportPorts.hpp"
#include "report/ReportVariables.hpp"

#include "common/Wildcard.hpp"
#include "netlist/VisitAll.hpp"

#include "common/FormatBuffer.hpp"
#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/ASTSerializer.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Scope.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/text/Json.h"
#include "slang/util/OS.h"
#include "slang/util/Util.h"
#include "slang/util/VersionInfo.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

using namespace slang;
using namespace slang::ast;
using namespace slang::driver;
using namespace slang::report;

namespace {

void writeReport(std::string const &path, std::string_view content) {
  if (path != "-") {
    OS::writeFile(path, content);
    return;
  }

  std::cout.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!std::cout.flush()) {
    SLANG_THROW(
        std::runtime_error("could not write report to standard output"));
  }
}

constexpr std::array<std::pair<std::string_view, ArgumentDirection>, 4>
    directionNames{{
    {"input", ArgumentDirection::In},
    {"output", ArgumentDirection::Out},
    {"inout", ArgumentDirection::InOut},
    {"ref", ArgumentDirection::Ref},
    }};

auto generateJson(Compilation &compilation, JsonWriter &writer,
                  const std::vector<const ast::Symbol *> &scopeSymbols) {
  writer.setPrettyPrint(true);
  ASTSerializer serializer(compilation, writer);
  if (scopeSymbols.empty()) {
    serializer.serialize(compilation.getRoot());
    return;
  }
  if (scopeSymbols.size() == 1) {
    serializer.serialize(*scopeSymbols.front());
    return;
  }
  // Multiple resolved scopes are emitted as one JSON array so that the
  // output is always a single valid JSON value.
  writer.startArray();
  for (auto const *sym : scopeSymbols) {
    serializer.serialize(*sym);
  }
  writer.endArray();
}

/// True if @p s contains any wildcard metacharacter recognised by
/// netlist::wildcardMatch.
auto hasGlobChar(std::string_view s) -> bool {
  return s.find_first_of("*?") != std::string_view::npos ||
         s.find("...") != std::string_view::npos;
}

/// Recursively walk @p scope, appending every member whose hierarchical
/// path matches @p pattern (deduped by symbol identity, preserving AST
/// traversal order) and returning the number of matches found. A matched
/// member's subtree is not walked: visiting the member reports the whole
/// subtree, so walking it again would only produce duplicate entries.
auto collectGlobMatches(const ast::Scope &scope, std::string const &pattern,
                        std::unordered_set<const ast::Symbol *> &seen,
                        std::vector<const ast::Symbol *> &out) -> size_t {
  size_t matches = 0;
  for (auto const &member : scope.members()) {
    auto path = member.getHierarchicalPath();
    if (!path.empty() &&
        netlist::wildcardMatch(path.c_str(), pattern.c_str())) {
      ++matches;
      if (seen.insert(&member).second) {
        out.push_back(&member);
      }
      continue;
    }
    if (auto const *inst = member.as_if<ast::InstanceSymbol>()) {
      if (!inst->body.flags.has(ast::InstanceFlags::Uninstantiated)) {
        matches += collectGlobMatches(inst->body, pattern, seen, out);
      }
    } else if (auto const *childScope = member.as_if<ast::Scope>()) {
      matches += collectGlobMatches(*childScope, pattern, seen, out);
    }
  }
  return matches;
}

} // namespace

auto main(int argc, char **argv) -> int {
  OS::setupConsole();

  Driver driver;
  driver.addStandardArgs();

  std::optional<bool> showHelp;
  driver.cmdLine.add("-h,--help", showHelp, "Display available options");

  std::optional<bool> showVersion;
  driver.cmdLine.add("--version", showVersion,
                     "Display version information and exit");

  std::optional<bool> reportPorts;
  driver.cmdLine.add("--ports", reportPorts,
                     "Report all ports in the design to stdout");

  std::vector<std::string> directions;
  driver.cmdLine.add("--direction", directions,
                     "Restrict --ports to the selected direction(s). "
                     "May be repeated.",
                     "<input|output|inout|ref>");

  std::optional<bool> reportVariables;
  driver.cmdLine.add("--variables", reportVariables,
                     "Report all variables in the design to stdout");

  std::optional<bool> reportDrivers;
  driver.cmdLine.add("--drivers", reportDrivers,
                     "Report all drivers in the design to stdout");

  std::optional<std::string> astJsonFile;
  driver.cmdLine.add("--ast-json", astJsonFile,
                     "Dump the compiled AST in JSON format to the specified "
                     "file, or '-' for stdout",
                     "<file>", CommandLineFlags::FilePath);

  std::optional<std::string> format;
  driver.cmdLine.add("--format", format,
                     "Output format for --ports, --variables, and --drivers: "
                     "'table' (default) or 'json'",
                     "<table|json>");

  std::vector<std::string> scopes;
  driver.cmdLine.add("--scope", scopes,
                     "Restrict --ports, --variables, --drivers, and "
                     "--ast-json to the given hierarchical scope(s). Accepts "
                     "literal paths or glob patterns (*, **, ?, ...). May be "
                     "repeated.",
                     "<path>");

  std::vector<std::string> nameFilters;
  driver.cmdLine.add("--name", nameFilters,
                     "Restrict --ports, --variables, and --drivers to symbols "
                     "whose hierarchical path matches one of the given glob "
                     "patterns (same syntax as --scope: `*`/`?` stay within a "
                     "single segment, `**`/`...` cross boundaries). May be "
                     "repeated.",
                     "<pattern>");

  std::optional<std::string> outputFile;
  driver.cmdLine.add("-o,--output", outputFile,
                     "Write --ports, --variables, or --drivers output to the "
                     "given file instead of stdout. Use '-' for stdout. "
                     "(For --ast-json, pass the file as the flag's value.)",
                     "<file>", CommandLineFlags::FilePath);

  if (!driver.parseCommandLine(argc, argv)) {
    return 1;
  }

  if (showHelp == true) {
    std::cout << fmt::format(
        "{}\n",
        driver.cmdLine.getHelpText("slang SystemVerilog AST reporting tool")
            .c_str());
    return 0;
  }

  if (showVersion == true) {
    printf("rtl-report version %d.%d.%d+%s\n", VersionInfo::getMajor(),
           VersionInfo::getMinor(), VersionInfo::getPatch(),
           std::string(VersionInfo::getHash()).c_str());
    return 0;
  }

  SLANG_TRY {
    const auto actionCount =
        int(reportPorts == true) + int(reportVariables == true) +
        int(reportDrivers == true) + int(astJsonFile.has_value());
    if (actionCount > 1) {
      SLANG_THROW(std::runtime_error(
          "multiple actions specified; pass exactly one of --ports, "
          "--variables, --drivers, or --ast-json"));
    }

    if (!directions.empty() && reportPorts != true) {
      SLANG_THROW(std::runtime_error("--direction requires --ports"));
    }
    std::vector<ArgumentDirection> directionFilters;
    for (const auto &value : directions) {
      const auto match =
          std::ranges::find_if(directionNames, [&](const auto &entry) {
            return entry.first == value;
          });
      if (match == directionNames.end()) {
        SLANG_THROW(std::runtime_error(
            fmt::format("unknown --direction value '{}'; expected input, "
                        "output, inout, or ref",
                        value)));
      }
      directionFilters.push_back(match->second);
    }

    enum class Format { Table, Json };
    auto outputFormat = Format::Table;
    if (format) {
      if (*format == "json") {
        outputFormat = Format::Json;
      } else if (*format == "table") {
        outputFormat = Format::Table;
      } else {
        SLANG_THROW(std::runtime_error(
            fmt::format("unknown --format value '{}'; expected 'table' or "
                        "'json'",
                        *format)));
      }
    }

    if (!driver.processOptions()) {
      return 2;
    }

    if (!driver.parseAllSources()) {
      return 1;
    }

    auto compilation = driver.createCompilation();
    driver.reportCompilation(*compilation, true);

    if (!driver.reportDiagnostics(true)) {
      return 1;
    }

    // Scope names must be resolved before the compilation is frozen,
    // since slang's name lookup can allocate diagnostics on miss.
    // Glob patterns are expanded by walking the symbol tree; literal
    // names take the faster lookupName path.
    std::vector<const ast::Symbol *> scopeSymbols;
    std::unordered_set<const ast::Symbol *> seenSymbols;
    scopeSymbols.reserve(scopes.size());
    for (auto const &scopeName : scopes) {
      if (hasGlobChar(scopeName)) {
        auto const matches = collectGlobMatches(
            compilation->getRoot(), scopeName, seenSymbols, scopeSymbols);
        if (matches == 0) {
          SLANG_THROW(std::runtime_error(
              fmt::format("scope '{}' matched no symbols", scopeName)));
        }
      } else {
        auto const *sym = compilation->getRoot().lookupName(scopeName);
        if (sym == nullptr) {
          SLANG_THROW(std::runtime_error(
              fmt::format("scope '{}' not found", scopeName)));
        }
        if (seenSymbols.insert(sym).second) {
          scopeSymbols.push_back(sym);
        }
      }
    }

    // AST JSON serialisation needs to allocate constants during traversal,
    // so it must run before the compilation is frozen.
    if (astJsonFile) {
      JsonWriter writer;
      generateJson(*compilation, writer, scopeSymbols);
      writeReport(*astJsonFile, writer.view());
      return 0;
    }

    netlist::VisitAll va;
    compilation->getRoot().visit(va);

    compilation->freeze();

    const auto reportPath = outputFile.value_or("-");

    auto emit = [&](auto &visitor) {
      visitor.setNameFilters(nameFilters);
      if (scopeSymbols.empty()) {
        compilation->getRoot().visit(visitor);
      } else {
        for (auto const *sym : scopeSymbols) {
          sym->visit(visitor);
        }
      }
      if (outputFormat == Format::Json) {
        JsonWriter writer;
        writer.setPrettyPrint(true);
        visitor.report(writer);
        writeReport(reportPath, fmt::format("{}\n", writer.view()));
      } else {
        netlist::FormatBuffer buf;
        visitor.report(buf);
        writeReport(reportPath, buf.str());
      }
    };

    if (reportPorts == true) {
      ReportPorts visitor(*compilation);
      visitor.setDirectionFilters(std::move(directionFilters));
      emit(visitor);
      return 0;
    }

    // Both --variables and --drivers need driver counts from analysis.
    if (reportVariables == true || reportDrivers == true) {
      auto analysisManager = driver.runAnalysis(*compilation);
      if (!driver.reportDiagnostics(true)) {
        return 1;
      }
      if (reportVariables == true) {
        ReportVariables visitor(*compilation, *analysisManager);
        emit(visitor);
      } else {
        ReportDrivers visitor(*compilation, *analysisManager);
        emit(visitor);
      }
      return 0;
    }

    SLANG_THROW(std::runtime_error(
        "no action specified; pass --ports, --variables, --drivers, or "
        "--ast-json"));
  }
  SLANG_CATCH(const std::exception &e) {
    SLANG_REPORT_EXCEPTION(e, "{}\n");
    return 1;
  }

  return 0;
}
