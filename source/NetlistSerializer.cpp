#include "netlist/NetlistSerializer.hpp"

#include "slang/numeric/SVInt.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace slang::netlist {

//===----------------------------------------------------------------------===//
// String conversion helpers
//===----------------------------------------------------------------------===//

static auto nodeKindToString(NodeKind kind) -> std::string_view {
  switch (kind) {
  case NodeKind::None:
    return "None";
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
  }
  return "None";
}

/// Return the node kind named by @p str, or nullopt if it names none.
static auto nodeKindFromString(std::string_view str)
    -> std::optional<NodeKind> {
  if (str == "None") {
    return NodeKind::None;
  }
  if (str == "Port") {
    return NodeKind::Port;
  }
  if (str == "Variable") {
    return NodeKind::Variable;
  }
  if (str == "Assignment") {
    return NodeKind::Assignment;
  }
  if (str == "Conditional") {
    return NodeKind::Conditional;
  }
  if (str == "Case") {
    return NodeKind::Case;
  }
  if (str == "Merge") {
    return NodeKind::Merge;
  }
  if (str == "State") {
    return NodeKind::State;
  }
  if (str == "Constant") {
    return NodeKind::Constant;
  }
  if (str == "Operation") {
    return NodeKind::Operation;
  }
  return std::nullopt;
}

static auto edgeKindToString(ast::EdgeKind kind) -> std::string_view {
  switch (kind) {
  case ast::EdgeKind::None:
    return "None";
  case ast::EdgeKind::PosEdge:
    return "PosEdge";
  case ast::EdgeKind::NegEdge:
    return "NegEdge";
  case ast::EdgeKind::BothEdges:
    return "BothEdges";
  }
  return "None";
}

/// Return the edge kind named by @p str, or nullopt if it names none.
static auto edgeKindFromString(std::string_view str)
    -> std::optional<ast::EdgeKind> {
  if (str == "None") {
    return ast::EdgeKind::None;
  }
  if (str == "PosEdge") {
    return ast::EdgeKind::PosEdge;
  }
  if (str == "NegEdge") {
    return ast::EdgeKind::NegEdge;
  }
  if (str == "BothEdges") {
    return ast::EdgeKind::BothEdges;
  }
  return std::nullopt;
}

static auto directionToString(ast::ArgumentDirection dir) -> std::string_view {
  switch (dir) {
  case ast::ArgumentDirection::In:
    return "In";
  case ast::ArgumentDirection::Out:
    return "Out";
  case ast::ArgumentDirection::InOut:
    return "InOut";
  case ast::ArgumentDirection::Ref:
    return "Ref";
  }
  return "In";
}

/// Return the port direction named by @p str, or nullopt if it names none.
static auto directionFromString(std::string_view str)
    -> std::optional<ast::ArgumentDirection> {
  if (str == "In") {
    return ast::ArgumentDirection::In;
  }
  if (str == "Out") {
    return ast::ArgumentDirection::Out;
  }
  if (str == "InOut") {
    return ast::ArgumentDirection::InOut;
  }
  if (str == "Ref") {
    return ast::ArgumentDirection::Ref;
  }
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// JSON helpers
//===----------------------------------------------------------------------===//

static auto locationToJson(TextLocation const &loc) -> json {
  return {
      {"fileIndex", loc.fileIndex}, {"line", loc.line}, {"column", loc.column}};
}

static auto symbolToJson(SymbolReference const &sym) -> json {
  json j;
  j["name"] = sym.name;
  j["path"] = sym.hierarchicalPath;
  j["location"] = locationToJson(sym.location);
  return j;
}

//===----------------------------------------------------------------------===//
// Deserialise-time validation
//===----------------------------------------------------------------------===//

/// Throw a diagnostic carrying the stable "invalid netlist" prefix.
[[noreturn]] static void throwInvalid(std::string message) {
  throw std::runtime_error(fmt::format("invalid netlist: {}", message));
}

static auto jsonTypeName(json const &value) -> std::string_view {
  if (value.is_null()) {
    return "null";
  }
  if (value.is_boolean()) {
    return "boolean";
  }
  if (value.is_number_unsigned()) {
    return "unsigned integer";
  }
  if (value.is_number_integer()) {
    return "integer";
  }
  if (value.is_number_float()) {
    return "number";
  }
  if (value.is_string()) {
    return "string";
  }
  if (value.is_array()) {
    return "array";
  }
  if (value.is_object()) {
    return "object";
  }
  return "value";
}

/// Return the text of @p ex without nlohmann's exception-class prefix, so
/// callers see a stable diagnostic rather than a library type name. JSON parse
/// errors keep their line/column position in the remaining text.
static auto jsonExceptionMessage(json::exception const &ex) -> std::string {
  std::string message = ex.what();
  auto pos = message.find("] ");
  if (pos != std::string::npos) {
    message.erase(0, pos + 2);
  }
  return message;
}

static auto requireObject(json const &value, std::string_view context)
    -> json const & {
  if (!value.is_object()) {
    throwInvalid(fmt::format("{} must be an object, got {}", context,
                             jsonTypeName(value)));
  }
  return value;
}

static auto requireArray(json const &value, std::string_view context)
    -> json const & {
  if (!value.is_array()) {
    throwInvalid(fmt::format("{} must be an array, got {}", context,
                             jsonTypeName(value)));
  }
  return value;
}

static auto requireField(json const &object, std::string_view key,
                         std::string_view context) -> json const & {
  auto it = object.find(std::string(key));
  if (it == object.end()) {
    // The root document has no enclosing path worth naming.
    auto prefix =
        context == "netlist" ? std::string{} : fmt::format("{}: ", context);
    throwInvalid(fmt::format("{}missing required field '{}'", prefix, key));
  }
  return *it;
}

static auto requireString(json const &object, std::string_view key,
                          std::string_view context) -> std::string {
  auto const &value = requireField(object, key, context);
  if (!value.is_string()) {
    throwInvalid(fmt::format("{}.{} must be a string, got {}", context, key,
                             jsonTypeName(value)));
  }
  return value.get<std::string>();
}

static auto requireBool(json const &object, std::string_view key,
                        std::string_view context) -> bool {
  auto const &value = requireField(object, key, context);
  if (!value.is_boolean()) {
    throwInvalid(fmt::format("{}.{} must be a boolean, got {}", context, key,
                             jsonTypeName(value)));
  }
  return value.get<bool>();
}

/// Return @p value as a signed 64-bit integer.
static auto asInt64(json const &value, std::string_view context) -> int64_t {
  if (value.is_number_unsigned()) {
    auto raw = value.get<uint64_t>();
    if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throwInvalid(fmt::format("{} is out of range", context));
    }
    return static_cast<int64_t>(raw);
  }
  if (value.is_number_integer()) {
    return value.get<int64_t>();
  }
  throwInvalid(fmt::format("{} must be an integer, got {}", context,
                           jsonTypeName(value)));
}

static auto requireInt64(json const &object, std::string_view key,
                         std::string_view context) -> int64_t {
  return asInt64(requireField(object, key, context),
                 fmt::format("{}.{}", context, key));
}

static auto requireSize(json const &object, std::string_view key,
                        std::string_view context) -> size_t {
  auto value = requireInt64(object, key, context);
  if (value < 0) {
    throwInvalid(fmt::format("{}.{} must not be negative", context, key));
  }
  return static_cast<size_t>(value);
}

static auto requireUInt64(json const &object, std::string_view key,
                          std::string_view context) -> uint64_t {
  auto value = requireInt64(object, key, context);
  if (value < 0) {
    throwInvalid(fmt::format("{}.{} must not be negative", context, key));
  }
  return static_cast<uint64_t>(value);
}

static auto asInt32(json const &value, std::string_view context) -> int32_t {
  auto raw = asInt64(value, context);
  if (raw < std::numeric_limits<int32_t>::min() ||
      raw > std::numeric_limits<int32_t>::max()) {
    throwInvalid(fmt::format("{} is out of range", context));
  }
  return static_cast<int32_t>(raw);
}

static auto parseLocation(json const &value, std::string_view context,
                          std::vector<uint32_t> const &fileIndexMap)
    -> TextLocation {
  requireObject(value, context);
  auto fileIndex = requireSize(value, "fileIndex", context);
  auto line = requireSize(value, "line", context);
  auto column = requireSize(value, "column", context);

  uint32_t mappedIndex = FileTable::NoFile;
  if (fileIndex != FileTable::NoFile) {
    if (fileIndex >= fileIndexMap.size()) {
      throwInvalid(fmt::format(
          "{}.fileIndex {} is out of range (fileTable has {} entries)", context,
          fileIndex, fileIndexMap.size()));
    }
    mappedIndex = fileIndexMap[fileIndex];
  }
  return TextLocation{mappedIndex, line, column};
}

static auto parseBounds(json const &value, std::string_view context)
    -> DriverBitRange {
  requireArray(value, context);
  if (value.size() != 2) {
    throwInvalid(fmt::format("{} must contain exactly two integers", context));
  }
  auto lower = asInt32(value[0], fmt::format("{}[0]", context));
  auto upper = asInt32(value[1], fmt::format("{}[1]", context));
  if (lower > upper) {
    throwInvalid(
        fmt::format("{} is invalid: lower bound {} exceeds upper bound {}",
                    context, lower, upper));
  }
  return DriverBitRange{lower, upper};
}

struct ParsedFileTable {
  /// Unique filenames in first-occurrence order, ready to be added to a
  /// FileTable.
  std::vector<std::string> uniqueNames;
  /// Maps an index in the serialised array to its index in @c uniqueNames,
  /// which is what a TextLocation needs after deduplication.
  std::vector<uint32_t> indexMap;
};

static auto parseFileTable(json const &root) -> ParsedFileTable {
  auto const &files =
      requireArray(requireField(root, "fileTable", "netlist"), "fileTable");

  ParsedFileTable result;
  result.indexMap.reserve(files.size());
  std::unordered_map<std::string, uint32_t> seen;
  seen.reserve(files.size());
  for (size_t i = 0; i < files.size(); ++i) {
    auto const &entry = files[i];
    if (!entry.is_string()) {
      throwInvalid(fmt::format("fileTable[{}] must be a string, got {}", i,
                               jsonTypeName(entry)));
    }
    auto name = entry.get<std::string>();
    auto [it, inserted] = seen.try_emplace(
        name, static_cast<uint32_t>(result.uniqueNames.size()));
    if (inserted) {
      result.uniqueNames.push_back(name);
    }
    result.indexMap.push_back(it->second);
  }
  return result;
}

static auto parseBlackBoxes(json const &root) -> std::vector<std::string> {
  std::vector<std::string> result;
  auto it = root.find("blackBoxes");
  if (it == root.end()) {
    return result;
  }
  auto const &blackBoxes = requireArray(*it, "blackBoxes");
  result.reserve(blackBoxes.size());
  for (size_t i = 0; i < blackBoxes.size(); ++i) {
    auto const &entry = blackBoxes[i];
    if (!entry.is_string()) {
      throwInvalid(fmt::format("blackBoxes[{}] must be a string, got {}", i,
                               jsonTypeName(entry)));
    }
    result.push_back(entry.get<std::string>());
  }
  return result;
}

struct ParsedNode {
  size_t id;
  std::unique_ptr<NetlistNode> node;
};

static auto parseNode(json const &nodeJson, size_t index,
                      std::vector<uint32_t> const &fileIndexMap) -> ParsedNode {
  auto context = fmt::format("nodes[{}]", index);
  requireObject(nodeJson, context);

  auto id = requireSize(nodeJson, "id", context);
  auto kindName = requireString(nodeJson, "kind", context);
  auto kind = nodeKindFromString(kindName);
  if (!kind.has_value()) {
    throwInvalid(fmt::format("{}: unknown node kind '{}'", context, kindName));
  }

  std::unique_ptr<NetlistNode> node;
  switch (*kind) {
  case NodeKind::Port: {
    auto name = requireString(nodeJson, "name", context);
    auto path = requireString(nodeJson, "path", context);
    auto directionName = requireString(nodeJson, "direction", context);
    auto direction = directionFromString(directionName);
    if (!direction.has_value()) {
      throwInvalid(fmt::format("{}: unknown port direction '{}'", context,
                               directionName));
    }
    auto bounds = parseBounds(requireField(nodeJson, "bounds", context),
                              fmt::format("{}.bounds", context));
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    node = std::make_unique<Port>(std::move(name), std::move(path), location,
                                  *direction, bounds);
    break;
  }
  case NodeKind::Variable: {
    auto name = requireString(nodeJson, "name", context);
    auto path = requireString(nodeJson, "path", context);
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    auto bounds = parseBounds(requireField(nodeJson, "bounds", context),
                              fmt::format("{}.bounds", context));
    node = std::make_unique<Variable>(std::move(name), std::move(path),
                                      location, bounds);
    break;
  }
  case NodeKind::State: {
    auto name = requireString(nodeJson, "name", context);
    auto path = requireString(nodeJson, "path", context);
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    auto bounds = parseBounds(requireField(nodeJson, "bounds", context),
                              fmt::format("{}.bounds", context));
    node = std::make_unique<State>(std::move(name), std::move(path), location,
                                   bounds);
    break;
  }
  case NodeKind::Assignment: {
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    node = std::make_unique<Assignment>(location);
    break;
  }
  case NodeKind::Conditional: {
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    node = std::make_unique<Conditional>(location);
    break;
  }
  case NodeKind::Case: {
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    node = std::make_unique<Case>(location);
    break;
  }
  case NodeKind::Constant: {
    auto width = requireUInt64(nodeJson, "width", context);
    auto valueStr = requireString(nodeJson, "value", context);
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    ConstantValue value;
    if (!valueStr.empty()) {
      // Best-effort round-trip: SVInt::fromString recovers integer
      // literals serialized via ConstantValue::toString. Non-integer
      // constants (real, string, aggregate) are not round-tripped and
      // are restored as a default-constructed (bad) ConstantValue.
      try {
        value = ConstantValue(SVInt::fromString(valueStr));
      } catch (...) {
        value = ConstantValue();
      }
    }
    node = std::make_unique<Constant>(std::move(value), width, location);
    break;
  }
  case NodeKind::Operation: {
    auto opName = requireString(nodeJson, "op", context);
    auto opKind = operationKindFromString(opName);
    if (!opKind.has_value()) {
      throwInvalid(
          fmt::format("{}: unknown operation kind '{}'", context, opName));
    }
    auto width = requireUInt64(nodeJson, "width", context);
    auto isSigned = requireBool(nodeJson, "signed", context);
    auto location =
        parseLocation(requireField(nodeJson, "location", context),
                      fmt::format("{}.location", context), fileIndexMap);
    node = std::make_unique<Operation>(*opKind, width, isSigned, location);
    break;
  }
  case NodeKind::Merge: {
    node = std::make_unique<Merge>();
    break;
  }
  case NodeKind::None: {
    node = std::make_unique<NetlistNode>(NodeKind::None);
    break;
  }
  }

  return ParsedNode{id, std::move(node)};
}

struct ParsedEdge {
  size_t sourceId;
  size_t targetId;
  ast::EdgeKind edgeKind;
  std::optional<SymbolReference> symbol;
  DriverBitRange bounds;
  bool disabled;
};

static auto parseEdge(json const &edgeJson, size_t index,
                      std::unordered_map<size_t, size_t> const &nodeIndices,
                      std::vector<uint32_t> const &fileIndexMap) -> ParsedEdge {
  auto context = fmt::format("edges[{}]", index);
  requireObject(edgeJson, context);

  auto sourceId = requireSize(edgeJson, "source", context);
  auto targetId = requireSize(edgeJson, "target", context);
  if (!nodeIndices.contains(sourceId)) {
    throwInvalid(fmt::format("{}: source references unknown node id {}",
                             context, sourceId));
  }
  if (!nodeIndices.contains(targetId)) {
    throwInvalid(fmt::format("{}: target references unknown node id {}",
                             context, targetId));
  }

  auto edgeKindName = requireString(edgeJson, "edgeKind", context);
  auto edgeKind = edgeKindFromString(edgeKindName);
  if (!edgeKind.has_value()) {
    throwInvalid(
        fmt::format("{}: unknown edge kind '{}'", context, edgeKindName));
  }

  auto symbolContext = fmt::format("{}.symbol", context);
  auto const &symbolJson = requireField(edgeJson, "symbol", context);
  requireObject(symbolJson, symbolContext);
  std::optional<SymbolReference> symbol;
  if (!symbolJson.empty()) {
    auto name = requireString(symbolJson, "name", symbolContext);
    auto path = requireString(symbolJson, "path", symbolContext);
    auto location =
        parseLocation(requireField(symbolJson, "location", symbolContext),
                      fmt::format("{}.location", symbolContext), fileIndexMap);
    symbol.emplace(std::move(name), std::move(path), location);
  }

  auto bounds = parseBounds(requireField(edgeJson, "bounds", context),
                            fmt::format("{}.bounds", context));
  auto disabled = requireBool(edgeJson, "disabled", context);

  return ParsedEdge{sourceId,          targetId, *edgeKind,
                    std::move(symbol), bounds,   disabled};
}

static void deserializeImpl(std::string_view jsonStr, NetlistGraph &graph) {
  if (graph.numNodes() != 0 || graph.numEdges() != 0 ||
      graph.fileTable.size() != 0 || graph.symbolTable.size() != 0) {
    throwInvalid(fmt::format(
        "target graph must be empty (has {} nodes, {} edges, {} file table "
        "entries)",
        graph.numNodes(), graph.numEdges(), graph.fileTable.size()));
  }

  auto root = json::parse(jsonStr);
  requireObject(root, "netlist");

  auto version = asInt64(requireField(root, "version", "netlist"), "version");
  if (version != NetlistSerializer::formatVersion) {
    throwInvalid(fmt::format("unsupported format version {} (expected {})",
                             version, NetlistSerializer::formatVersion));
  }

  auto fileTable = parseFileTable(root);
  auto blackBoxes = parseBlackBoxes(root);

  // Parse and validate every node and edge before touching the graph, so a
  // malformed file cannot leave a partially populated graph behind.
  auto const &nodesJson =
      requireArray(requireField(root, "nodes", "netlist"), "nodes");
  std::vector<ParsedNode> nodes;
  nodes.reserve(nodesJson.size());
  std::unordered_map<size_t, size_t> nodeIndices;
  nodeIndices.reserve(nodesJson.size());
  for (size_t i = 0; i < nodesJson.size(); ++i) {
    auto parsed = parseNode(nodesJson[i], i, fileTable.indexMap);
    auto [it, inserted] = nodeIndices.try_emplace(parsed.id, i);
    if (!inserted) {
      throwInvalid(fmt::format(
          "nodes[{}]: duplicate node id {} (first used at nodes[{}])", i,
          parsed.id, it->second));
    }
    nodes.push_back(std::move(parsed));
  }

  auto const &edgesJson =
      requireArray(requireField(root, "edges", "netlist"), "edges");
  std::vector<ParsedEdge> edges;
  edges.reserve(edgesJson.size());
  for (size_t i = 0; i < edgesJson.size(); ++i) {
    edges.push_back(
        parseEdge(edgesJson[i], i, nodeIndices, fileTable.indexMap));
  }

  // Everything is validated: commit the parsed contents to the graph.
  for (auto const &name : fileTable.uniqueNames) {
    graph.fileTable.addFile(name);
  }
  for (auto const &path : blackBoxes) {
    graph.addBlackBoxPath(path);
  }

  std::unordered_map<size_t, NetlistNode *> idMap;
  idMap.reserve(nodes.size());
  for (auto &parsed : nodes) {
    auto &addedNode = graph.addNode(std::move(parsed.node));
    idMap.emplace(parsed.id, &addedNode);
  }

  for (auto const &parsedEdge : edges) {
    // Replay the saved edge set verbatim: the JSON already records the
    // outcome of the builder's edge-merging policy, so parallel edges must
    // not be collapsed back together here.
    auto &edge =
        idMap.at(parsedEdge.sourceId)->addEdge(*idMap.at(parsedEdge.targetId));
    edge.edgeKind = parsedEdge.edgeKind;
    if (parsedEdge.symbol.has_value()) {
      edge.symbol = graph.symbolTable.intern(*parsedEdge.symbol);
    } else {
      edge.symbol = nullptr;
    }
    edge.bounds = parsedEdge.bounds;
    edge.disabled = parsedEdge.disabled;
  }
}

//===----------------------------------------------------------------------===//
// Serialize
//===----------------------------------------------------------------------===//

auto NetlistSerializer::serialize(NetlistGraph const &graph) -> std::string {
  json root;
  root["version"] = formatVersion;

  // Serialize file table.
  json fileTableJson = json::array();
  for (size_t i = 0; i < graph.fileTable.size(); ++i) {
    fileTableJson.push_back(
        std::string(graph.fileTable.getFilename(static_cast<uint32_t>(i))));
  }
  root["fileTable"] = fileTableJson;

  // Serialize black-box instance paths.
  json blackBoxesJson = json::array();
  for (auto const &path : graph.getBlackBoxPaths()) {
    blackBoxesJson.push_back(path);
  }
  root["blackBoxes"] = blackBoxesJson;

  // Serialize nodes.
  json nodesJson = json::array();
  for (auto const &nodePtr : graph) {
    auto const &node = *nodePtr;
    json nodeJson;
    nodeJson["id"] = node.ID;
    nodeJson["kind"] = nodeKindToString(node.kind);

    switch (node.kind) {
    case NodeKind::Port: {
      auto const &port = node.as<Port>();
      nodeJson["path"] = port.hierarchicalPath;
      nodeJson["name"] = port.name;
      nodeJson["bounds"] = {port.bounds.lower(), port.bounds.upper()};
      nodeJson["direction"] = directionToString(port.direction);
      nodeJson["location"] = locationToJson(port.location);
      break;
    }
    case NodeKind::Variable: {
      auto const &var = node.as<Variable>();
      nodeJson["path"] = var.hierarchicalPath;
      nodeJson["name"] = var.name;
      nodeJson["bounds"] = {var.bounds.lower(), var.bounds.upper()};
      nodeJson["location"] = locationToJson(var.location);
      break;
    }
    case NodeKind::State: {
      auto const &state = node.as<State>();
      nodeJson["path"] = state.hierarchicalPath;
      nodeJson["name"] = state.name;
      nodeJson["bounds"] = {state.bounds.lower(), state.bounds.upper()};
      nodeJson["location"] = locationToJson(state.location);
      break;
    }
    case NodeKind::Assignment: {
      auto const &assign = node.as<Assignment>();
      nodeJson["location"] = locationToJson(assign.location);
      break;
    }
    case NodeKind::Conditional: {
      auto const &cond = node.as<Conditional>();
      nodeJson["location"] = locationToJson(cond.location);
      break;
    }
    case NodeKind::Case: {
      auto const &caseNode = node.as<Case>();
      nodeJson["location"] = locationToJson(caseNode.location);
      break;
    }
    case NodeKind::Constant: {
      auto const &constNode = node.as<Constant>();
      nodeJson["location"] = locationToJson(constNode.location);
      nodeJson["width"] = constNode.width;
      nodeJson["value"] = constNode.value.toString();
      break;
    }
    case NodeKind::Operation: {
      auto const &opNode = node.as<Operation>();
      nodeJson["op"] = toString(opNode.op);
      nodeJson["width"] = opNode.width;
      nodeJson["signed"] = opNode.isSigned;
      nodeJson["location"] = locationToJson(opNode.location);
      break;
    }
    case NodeKind::Merge:
    case NodeKind::None:
      break;
    }

    nodesJson.push_back(std::move(nodeJson));
  }
  root["nodes"] = nodesJson;

  // Serialize edges.
  json edgesJson = json::array();
  for (auto const &nodePtr : graph) {
    for (auto const &edgePtr : nodePtr->getOutEdges()) {
      auto const &edge = *edgePtr;
      json edgeJson;
      edgeJson["source"] = edge.getSourceNode().ID;
      edgeJson["target"] = edge.getTargetNode().ID;
      edgeJson["edgeKind"] = edgeKindToString(edge.edgeKind);
      edgeJson["symbol"] =
          edge.symbol != nullptr ? symbolToJson(*edge.symbol) : json::object();
      edgeJson["bounds"] = {edge.bounds.lower(), edge.bounds.upper()};
      edgeJson["disabled"] = edge.disabled;
      edgesJson.push_back(std::move(edgeJson));
    }
  }
  root["edges"] = edgesJson;

  return root.dump(2);
}

//===----------------------------------------------------------------------===//
// Deserialize
//===----------------------------------------------------------------------===//

void NetlistSerializer::deserialize(std::string_view jsonStr,
                                    NetlistGraph &graph) {
  try {
    deserializeImpl(jsonStr, graph);
  } catch (json::exception const &ex) {
    throw std::runtime_error(
        fmt::format("invalid netlist: {}", jsonExceptionMessage(ex)));
  }
}

} // namespace slang::netlist
