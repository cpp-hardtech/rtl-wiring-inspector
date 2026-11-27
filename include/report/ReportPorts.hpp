#pragma once

#include "report/ReportVisitorBase.hpp"

#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/NetType.h"

#include <algorithm>
#include <vector>

namespace slang::report {

struct PortInfo {
  std::string name;
  ast::ArgumentDirection direction;
  uint64_t width;
  std::string netType;
  SourceLocation location;
};

/// Visitor for printing port information.
class ReportPorts : public ReportVisitorBase<ReportPorts, PortInfo> {
  std::vector<ast::ArgumentDirection> directionFilters;

  auto directionMatches(ast::ArgumentDirection direction) const -> bool {
    return directionFilters.empty() ||
           std::ranges::find(directionFilters, direction) !=
               directionFilters.end();
  }

  /// Return the net type name ("wire", "wand", ...) if the port connects
  /// to a NetSymbol, otherwise "var".
  static auto portNetTypeName(ast::PortSymbol const &symbol) -> std::string {
    if (auto const *net = symbol.internalSymbol
                              ? symbol.internalSymbol->as_if<ast::NetSymbol>()
                              : nullptr) {
      return std::string(net->netType.name);
    }
    return "var";
  }

  static auto portNetTypeName(ast::MultiPortSymbol const &symbol)
      -> std::string {
    if (symbol.ports.empty()) {
      return "var";
    }
    auto netType = portNetTypeName(*symbol.ports.front());
    for (auto const *port : symbol.ports.subspan(1)) {
      if (portNetTypeName(*port) != netType) {
        return "mixed";
      }
    }
    return netType;
  }

public:
  using ReportVisitorBase::ReportVisitorBase;

  /// Report ports with any of the given directions. An empty list keeps all.
  void setDirectionFilters(std::vector<ast::ArgumentDirection> filters) {
    directionFilters = std::move(filters);
  }

  auto tableHeader() const -> netlist::Utilities::Row {
    return {"Direction", "Name", "Width", "Net Type", "Location"};
  }

  void appendItemRows(netlist::Utilities::Table &table,
                      PortInfo const &port) const {
    table.push_back(netlist::Utilities::Row{
        std::string(toString(port.direction)), port.name,
        std::to_string(port.width), port.netType, locationStr(port.location)});
  }

  void emitJsonItem(JsonWriter &writer, PortInfo const &port) const {
    writer.startObject();
    writer.writeProperty("name");
    writer.writeValue(port.name);
    writer.writeProperty("direction");
    writer.writeValue(std::string_view(toString(port.direction)));
    writer.writeProperty("width");
    writer.writeValue(port.width);
    writer.writeProperty("netType");
    writer.writeValue(port.netType);
    writer.writeProperty("location");
    writer.writeValue(locationStr(port.location));
    writer.endObject();
  }

  void handle(const ast::MultiPortSymbol &symbol) {
    auto path = symbol.getHierarchicalPath();
    if (!directionMatches(symbol.direction) || !acceptSymbol(symbol, path)) {
      return;
    }
    items.push_back(PortInfo{
        .name = std::move(path),
        .direction = symbol.direction,
        .width = symbol.getType().getBitWidth(),
        .netType = portNetTypeName(symbol),
        .location = symbol.location,
    });
  }

  void handle(const ast::PortSymbol &symbol) {
    auto path = symbol.getHierarchicalPath();
    if (!directionMatches(symbol.direction) || !acceptSymbol(symbol, path)) {
      return;
    }
    items.push_back(PortInfo{
        .name = std::move(path),
        .direction = symbol.direction,
        .width = reportWidth(symbol.getType()),
        .netType = portNetTypeName(symbol),
        .location = symbol.location,
    });
  }
};

} // namespace slang::report
