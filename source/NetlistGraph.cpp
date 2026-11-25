#include "netlist/NetlistGraph.hpp"

#include "DepthFirstSearch.hpp"
#include "NetlistBuilder.hpp"
#include "common/Wildcard.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>

using namespace slang::netlist;

void NetlistGraph::build(ast::Compilation &compilation,
                         analysis::AnalysisManager &analysisManager,
                         BuilderOptions options) {
  NetlistBuilder builder(compilation, analysisManager, *this, options);
  builder.build(compilation.getRoot());
  builder.finalize();
  setBuildProfile(builder.getBuildProfile());
}

namespace {

/// Attributes an edge must share with another to be mergeable with it.
auto edgeGroupKey(NetlistEdge const *edge) {
  return std::tuple(&edge->getTargetNode(), edge->symbol, edge->edgeKind);
}

/// Orders edges so that mergeable ones form a contiguous ascending run.
auto edgeMergeKey(NetlistEdge const *edge) {
  return std::tuple_cat(edgeGroupKey(edge), edge->bounds.toPair());
}

} // namespace

void NetlistGraph::mergeParallelEdges() {
  std::vector<NetlistEdge *> candidates;
  std::vector<NetlistEdge const *> absorbed;

  for (auto const &node : nodes) {
    if (!node->mayHaveParallelOutEdges()) {
      continue;
    }

    // Unannotated edges carry no range, so they are never merged.
    candidates.clear();
    for (auto const &edge : node->getOutEdges()) {
      if (edge->hasSymbol()) {
        candidates.push_back(edge.get());
      }
    }
    std::ranges::sort(candidates, {}, edgeMergeKey);

    // Sorting leaves mergeable edges in an ascending run, so each edge need
    // only be offered to the one kept before it.
    absorbed.clear();
    NetlistEdge *keep = nullptr;
    for (auto *edge : candidates) {
      if (keep != nullptr && edgeGroupKey(keep) == edgeGroupKey(edge) &&
          keep->bounds.isContiguousWith(edge->bounds)) {
        keep->bounds = keep->bounds.unionWith(edge->bounds);
        absorbed.push_back(edge);
      } else {
        keep = edge;
      }
    }

    if (absorbed.empty()) {
      continue;
    }

    // Ordered by address so the removal predicate stays logarithmic: it is
    // applied to every edge of the node and of each affected target.
    std::ranges::sort(absorbed);
    node->removeOutEdgesIf([&absorbed](NetlistEdge const &edge) {
      return std::ranges::binary_search(absorbed, &edge);
    });
  }
}

void NetlistGraph::buildIndex() const {
  if (indexBuilt)
    return;
  for (auto const &node : nodes) {
    auto path = node->getHierarchicalPath();
    if (path.has_value()) {
      nodeIndex[std::string(*path)].push_back(node.get());
    }
  }
  indexBuilt = true;
}

auto NetlistGraph::lookup(std::string_view name) const -> NetlistNode * {
  buildIndex();
  auto it = nodeIndex.find(std::string(name));
  if (it == nodeIndex.end() || it->second.empty())
    return nullptr;
  return it->second.front();
}

auto NetlistGraph::lookupAll(std::string_view name) const
    -> std::vector<NetlistNode *> {
  buildIndex();
  auto it = nodeIndex.find(std::string(name));
  if (it == nodeIndex.end()) {
    return {};
  }
  auto result = it->second;
  std::ranges::sort(result, {}, [](NetlistNode *node) { return node->ID; });
  return result;
}

auto NetlistGraph::hasSymbol(std::string_view name) const -> bool {
  buildIndex();
  if (nodeIndex.contains(std::string(name))) {
    return true;
  }
  for (auto const &node : nodes) {
    for (auto const &edge : node->getOutEdges()) {
      if (edge->symbol != nullptr &&
          edge->symbol->hierarchicalPath == name) {
        return true;
      }
    }
  }
  return false;
}

auto NetlistGraph::lookup(std::string_view name, DriverBitRange bounds) const
    -> std::vector<NetlistNode *> {
  buildIndex();
  std::vector<NetlistNode *> result;
  auto it = nodeIndex.find(std::string(name));
  if (it == nodeIndex.end())
    return result;
  for (auto *node : it->second) {
    auto nodeBounds = node->getBounds();
    if (nodeBounds.has_value() && nodeBounds->overlaps(bounds)) {
      result.push_back(node);
    }
  }
  return result;
}

namespace {

/// True if @p kind names a declared storage value rather than an operation.
auto isStorageKind(NodeKind kind) -> bool {
  return kind == NodeKind::Port || kind == NodeKind::State ||
         kind == NodeKind::Variable;
}

} // namespace

auto NetlistGraph::getDriverEntries(std::string_view name,
                                    DriverBitRange bounds) const
    -> std::vector<BitDriver> {
  buildIndex();

  auto const it = nodeIndex.find(std::string(name));
  bool hasStorageNode = false;
  if (it != nodeIndex.end()) {
    for (auto *node : it->second) {
      if (isStorageKind(node->kind)) {
        hasStorageNode = true;
        break;
      }
    }
  }

  std::vector<BitDriver> result;

  // Nodes that write the symbol through an edge into a storage node.
  std::unordered_set<NetlistNode *> writeSources;
  if (hasStorageNode) {
    for (auto const &node : nodes) {
      for (auto const &edge : node->getOutEdges()) {
        if (edge->symbol == nullptr || edge->symbol->hierarchicalPath != name) {
          continue;
        }
        if (isStorageKind(edge->getTargetNode().kind)) {
          writeSources.insert(&edge->getSourceNode());
        }
      }
    }
  }

  // Edges annotated with the queried symbol. For an internal symbol without
  // a storage node the writing node is the source of every annotated edge,
  // so those edges are the only driver provenance. For a declared storage
  // value, a read edge from a node that also writes the signal repeats a
  // range its write edge already covers and is dropped.
  for (auto const &node : nodes) {
    for (auto const &edge : node->getOutEdges()) {
      if (edge->symbol == nullptr || edge->symbol->hierarchicalPath != name) {
        continue;
      }
      auto *source = &edge->getSourceNode();
      auto *target = &edge->getTargetNode();
      // A read of the symbol from one of its own storage nodes is not a
      // driver, even when no write edge exists for the queried range.
      if (isStorageKind(source->kind)) {
        auto sourcePath = source->getHierarchicalPath();
        if (sourcePath && *sourcePath == name) {
          continue;
        }
      }
      if (hasStorageNode && !isStorageKind(target->kind) &&
          writeSources.contains(source)) {
        continue;
      }
      auto clipped = edge->bounds.intersection(bounds);
      if (!clipped.has_value()) {
        continue;
      }
      result.push_back(BitDriver{*clipped, source});
    }
  }

  // Port-connection edges into a same-named Port node are annotated with the
  // actual symbol, so the write-edge filter above does not reach them. Report
  // the actual as the driver over the formal port node's own bit range.
  if (hasStorageNode && it != nodeIndex.end()) {
    for (auto *portNode : it->second) {
      if (portNode->kind != NodeKind::Port) {
        continue;
      }
      auto portBounds = portNode->getBounds();
      auto targetBounds =
          portBounds ? portBounds->intersection(bounds)
                     : std::optional<DriverBitRange>(bounds);
      if (!targetBounds.has_value()) {
        continue;
      }
      for (auto const *edge : portNode->getInEdges()) {
        if (edge->symbol != nullptr &&
            edge->symbol->hierarchicalPath == name) {
          continue;
        }
        result.push_back(BitDriver{*targetBounds, &edge->getSourceNode()});
      }
    }
  }

  auto key = [](BitDriver const &b) {
    return std::make_tuple(b.bounds.lower(), b.bounds.upper(), b.driver);
  };
  std::sort(
      result.begin(), result.end(),
      [&](BitDriver const &a, BitDriver const &b) { return key(a) < key(b); });
  result.erase(std::unique(result.begin(), result.end(),
                           [&](BitDriver const &a, BitDriver const &b) {
                             return key(a) == key(b);
                           }),
               result.end());
  return result;
}

auto NetlistGraph::getDrivers(std::string_view name,
                              DriverBitRange bounds) const
    -> std::vector<NetlistNode *> {
  std::unordered_set<NetlistNode *> seen;
  std::vector<NetlistNode *> result;
  for (auto const &entry : getDriverEntries(name, bounds)) {
    if (seen.insert(entry.driver).second) {
      result.push_back(entry.driver);
    }
  }
  return result;
}

auto NetlistGraph::getDrivers(NetlistNode const &node) const
    -> std::vector<NetlistNode *> {
  std::unordered_set<NetlistNode *> seen;
  std::vector<NetlistNode *> result;
  for (auto const *edge : node.getInEdges()) {
    auto *source = &edge->getSourceNode();
    if (seen.insert(source).second) {
      result.push_back(source);
    }
  }
  return result;
}

auto NetlistGraph::getBitDrivers(std::string_view name,
                                 DriverBitRange bounds) const
    -> std::vector<BitDriver> {
  return getDriverEntries(name, bounds);
}

auto NetlistGraph::getBitDrivers(std::string_view name) const
    -> std::vector<BitDriver> {
  // The whole signal: a wide range covers every driving edge, since a signal
  // may be split across several nodes by bit-aligned resolution.
  return getBitDrivers(name,
                       DriverBitRange{0, std::numeric_limits<int32_t>::max()});
}

namespace {

struct CombFanPredicate {
  bool operator()(const NetlistEdge &edge) const {
    return !edge.disabled && edge.getTargetNode().kind != NodeKind::State;
  }
};

struct CombFanBackwardPredicate {
  bool operator()(const NetlistEdge &edge) const {
    return !edge.disabled && edge.getSourceNode().kind != NodeKind::State;
  }
};

class CollectVisitor {
public:
  CollectVisitor(std::vector<NetlistNode *> &result) : result(result) {}
  void visitedNode(NetlistNode &) {}
  void visitNode(NetlistNode &node) { result.push_back(&node); }
  void visitEdge(NetlistEdge &) {}
  void popNode() {}

private:
  std::vector<NetlistNode *> &result;
};

} // namespace

auto NetlistGraph::getCombFanOut(NetlistNode &node) const
    -> std::vector<NetlistNode *> {
  std::vector<NetlistNode *> result;
  CollectVisitor visitor(result);
  DepthFirstSearch<NetlistNode, NetlistEdge, CollectVisitor, CombFanPredicate,
                   Direction::Forward>
      dfs(visitor, node);
  return result;
}

auto NetlistGraph::getCombFanIn(NetlistNode &node) const
    -> std::vector<NetlistNode *> {
  std::vector<NetlistNode *> result;
  CollectVisitor visitor(result);
  DepthFirstSearch<NetlistNode, NetlistEdge, CollectVisitor,
                   CombFanBackwardPredicate, Direction::Backward>
      dfs(visitor, node);
  return result;
}

auto NetlistGraph::getSensitivity(NetlistNode &node) const
    -> std::vector<SensitivitySource> {
  std::vector<SensitivitySource> result;

  auto collectFromState = [&](NetlistNode &state) {
    SLANG_ASSERT(state.kind == NodeKind::State);
    for (auto const &edge : state.getInEdges()) {
      if (edge->disabled || edge->edgeKind == ast::EdgeKind::None) {
        continue;
      }
      auto *source = &edge->getSourceNode();
      auto duplicate =
          std::any_of(result.begin(), result.end(), [&](auto const &existing) {
            return existing.source == source &&
                   existing.edgeKind == edge->edgeKind;
          });
      if (!duplicate) {
        result.push_back({source, edge->edgeKind});
      }
    }
  };

  if (node.kind == NodeKind::State) {
    collectFromState(node);
    return result;
  }

  // Forward walk: collect State targets without traversing into them.
  // getCombFanOut can't be reused — its predicate drops edges-to-State.
  std::unordered_set<NetlistNode *> visited;
  std::vector<NetlistNode *> stack;
  visited.insert(&node);
  stack.push_back(&node);
  while (!stack.empty()) {
    auto *cur = stack.back();
    stack.pop_back();
    for (auto const &edge : cur->getOutEdges()) {
      if (edge->disabled) {
        continue;
      }
      auto &target = edge->getTargetNode();
      if (target.kind == NodeKind::State) {
        collectFromState(target);
        continue;
      }
      if (visited.insert(&target).second) {
        stack.push_back(&target);
      }
    }
  }
  return result;
}

auto NetlistGraph::getConstantDrivers(NetlistNode &node) const
    -> std::vector<NetlistNode *> {
  // Visit register outputs as non-constant sources without following their
  // data and clock inputs.
  std::vector<NetlistNode *> fanIn;
  CollectVisitor visitor(fanIn);
  DepthFirstSearch<NetlistNode, NetlistEdge, CollectVisitor, CombFanPredicate,
                   Direction::Backward>
      dfs(visitor, node);

  std::vector<NetlistNode *> constants;
  for (auto *n : fanIn) {
    if (n == &node) {
      continue;
    }
    switch (n->kind) {
    case NodeKind::Constant:
      constants.push_back(n);
      break;
    case NodeKind::State:
      // Depends on a register: not constant-driven.
      return {};
    case NodeKind::Port:
      // An undriven Port in the fan-in is a top-level input acting as
      // a real external source. A driven Port is just a pass-through.
      if (!n->as<Port>().isDriven()) {
        return {};
      }
      break;
    default:
      // Variable / Assignment / Conditional / Case / Merge / Operation
      // are pass-throughs: connectivity continues through them.
      break;
    }
  }
  if (constants.empty()) {
    return {};
  }
  return constants;
}

auto NetlistGraph::findNodes(std::string_view pattern) const
    -> std::vector<NetlistNode *> {
  buildIndex();
  std::string pat(pattern);
  std::vector<NetlistNode *> result;
  for (auto const &[name, nodeList] : nodeIndex) {
    if (wildcardMatch(name.c_str(), pat.c_str())) {
      result.insert(result.end(), nodeList.begin(), nodeList.end());
    }
  }
  return result;
}

auto NetlistGraph::findNodesRegex(std::string_view pattern) const
    -> std::vector<NetlistNode *> {
  buildIndex();
  std::regex re(pattern.begin(), pattern.end());
  std::vector<NetlistNode *> result;
  for (auto const &[name, nodeList] : nodeIndex) {
    if (std::regex_match(name, re)) {
      result.insert(result.end(), nodeList.begin(), nodeList.end());
    }
  }
  return result;
}

void NetlistGraph::setBlackBoxPatterns(std::vector<std::string> patterns) {
  blackBoxPatterns = std::move(patterns);
  blackBoxPatternMatched.assign(blackBoxPatterns.size(), false);
}

void NetlistGraph::markBlackBoxPatternMatched(size_t index) {
  if (index < blackBoxPatternMatched.size()) {
    blackBoxPatternMatched[index] = true;
  }
}

auto NetlistGraph::getUnmatchedBlackBoxPatterns() const
    -> std::vector<std::string> {
  std::vector<std::string> result;
  for (size_t i = 0; i < blackBoxPatterns.size(); ++i) {
    if (i >= blackBoxPatternMatched.size() ||
        !blackBoxPatternMatched[i]) {
      result.push_back(blackBoxPatterns[i]);
    }
  }
  return result;
}

auto NetlistGraph::getBlackBoxCoverage(NetlistNode const &node) const
    -> BlackBoxCoverage {
  auto path = node.getHierarchicalPath();
  if (!path) {
    return BlackBoxCoverage::Outside;
  }
  auto result = BlackBoxCoverage::Outside;
  for (auto const &bbPath : blackBoxPaths) {
    if (!path->starts_with(bbPath)) {
      continue;
    }
    if (path->size() == bbPath.size()) {
      return BlackBoxCoverage::Contained;
    }
    // The prefix must end on a path segment boundary.
    if ((*path)[bbPath.size()] != '.') {
      continue;
    }
    auto remainder = path->substr(bbPath.size() + 1);
    if (node.kind == NodeKind::Port &&
        remainder.find('.') == std::string_view::npos) {
      result = BlackBoxCoverage::Boundary;
    } else {
      return BlackBoxCoverage::Contained;
    }
  }
  return result;
}
