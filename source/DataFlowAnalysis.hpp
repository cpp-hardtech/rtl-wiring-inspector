#pragma once

#include "DriverMap.hpp"
#include "IntervalMapUtils.hpp"
#include "ValueTracker.hpp"

#include "netlist/Debug.hpp"

#include "slang/analysis/AbstractFlowAnalysis.h"
#include "slang/analysis/AnalysisManager.h"
#include "slang/analysis/DataFlowAnalysis.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/ValuePath.h"
#include "slang/ast/symbols/ClassSymbols.h"
#include "slang/util/BumpAllocator.h"
#include "slang/util/IntervalMap.h"
#include "slang/util/ScopeGuard.h"

namespace slang::netlist {

class NetlistBuilder;
struct BitSliceSource;
struct Segment;

struct AnalysisState {

  // Each tracked variable has its definitions intervals stored here.
  ValueDrivers valueDrivers;

  // Definitions contributed by non-blocking assignments along the paths
  // leading to this point. They are kept separate from `valueDrivers`
  // until the block finishes, because a non-blocking assignment does not
  // update the value seen by later reads in the same block. Carrying them
  // in the state lets mutually exclusive branches all contribute while a
  // later write on the same path replaces an earlier one.
  ValueDrivers pendingNBAs;

  // The current control flow node in the graph.
  NetlistNode *node{nullptr};

  // The previous branching condition node in the graph.
  NetlistNode *condition{nullptr};

  // Whether the control flow that arrived at this point is reachable.
  bool reachable = true;

  AnalysisState() = default;
  AnalysisState(AnalysisState &&other) = default;
  auto operator=(AnalysisState &&other) -> AnalysisState & = default;
};

/// A data flow analysis used as part of the netlist graph construction.
struct DataFlowAnalysis
    : public analysis::AbstractFlowAnalysis<DataFlowAnalysis, AnalysisState> {

  using ParentAnalysis =
      analysis::AbstractFlowAnalysis<DataFlowAnalysis, AnalysisState>;

  friend class AbstractFlowAnalysis;
  friend class OperationLowering;

  analysis::AnalysisManager &analysisManager;

  // ValueSymbol to bit ranges mapping to the netlist node(s) that are driving
  // them.
  ValueTracker valueTracker{SlotGrowth::Exact};

  // Track attributes of the current assignment expression.
  bool isLValue = false;
  bool isBlocking = false;
  bool prohibitLValue = false;

  // A reference to the netlist graph under construction.
  NetlistBuilder &builder;

  // An external node that is used as a root for the the DFA. For example, a
  // port node that is created by the DFA caller to reference port connection
  // lvalues against.
  NetlistNode *externalNode;

  // Allocator reused across every handleRvalue() invocation within the DFA.
  BumpAllocator rvalueAllocator;
  DriverMap::AllocatorType rvalueMapAllocator{rvalueAllocator};

  // Allocator reused across every BitSliceList::build() invocation within
  // the DFA, for the bit-aligned AssignmentExpression path.
  BumpAllocator sliceAllocator;

  DataFlowAnalysis(analysis::AnalysisManager &analysisManager,
                   ast::Symbol const &symbol, NetlistBuilder &builder,
                   NetlistNode *externalNode = nullptr)
      : AbstractFlowAnalysis(symbol, {}), analysisManager(analysisManager),
        builder(builder), externalNode(externalNode) {}

  auto getState() -> AnalysisState & { return ParentAnalysis::getState(); }
  auto getState() const -> AnalysisState const & {
    return ParentAnalysis::getState();
  }

  [[nodiscard]] auto saveLValueFlag() {
    auto guard =
        ScopeGuard([this, savedLVal = isLValue] { isLValue = savedLVal; });
    isLValue = false;
    return guard;
  }

  //===---------------------------------------------------------===//
  // L- and R-value handling.
  //===---------------------------------------------------------===//

  void handleRvalue(ast::ValueSymbol const &symbol, ast::Expression const &lsp,
                    DriverBitRange bounds);

  void handleLvalue(ast::ValueSymbol const &symbol, ast::Expression const &lsp,
                    DriverBitRange bounds);

  /// As per DataFlowAnalysis in upstream slang, but with custom handling of
  /// L- and R-values.
  void noteReference(ast::ValuePath const &path);

  /// Finalize the analysis by processing any pending non-blocking L-values.
  /// This should be called after the main analysis has completed.
  void finalize();

  //===---------------------------------------------------------===//
  // AST handlers
  //===---------------------------------------------------------===//

  template <typename T>
    requires(analysis::detail::IsSelectExpr<T>)
  void handle(const T &expr) {
    auto clearLValFlag = [this]() {
      auto guard =
          ScopeGuard([this, savedLVal = isLValue] { isLValue = savedLVal; });
      isLValue = false;
      return guard;
    };

    ast::ValuePath path(expr, this->getEvalContext());
    for (auto &elem : path) {
      switch (elem.kind) {
      case ast::ExpressionKind::NamedValue:
      case ast::ExpressionKind::HierarchicalValue:
        break;
      case ast::ExpressionKind::ElementSelect: {
        auto guard = clearLValFlag();
        this->visit(elem.as<ast::ElementSelectExpression>().selector());
        break;
      }
      case ast::ExpressionKind::RangeSelect: {
        auto guard = clearLValFlag();
        auto &rs = elem.as<ast::RangeSelectExpression>();
        this->visit(rs.left());
        this->visit(rs.right());
        break;
      }
      case ast::ExpressionKind::MemberAccess: {
        auto &mae = elem.as<ast::MemberAccessExpression>();
        if (auto prop = mae.member.as_if<ast::ClassPropertySymbol>();
            prop && prop->lifetime == ast::VariableLifetime::Static) {
          auto guard = clearLValFlag();
          this->visit(mae.value());
        }
        break;
      }
      default: {
        auto guard = clearLValFlag();
        this->visit(elem);
        break;
      }
      }
    }

    noteReference(path);
  }

  void updateNode(NetlistNode *node, bool conditional);

  void handle(const ast::ProceduralAssignStatement &stmt);

  void handle(const ast::AssignmentExpression &expr);

  /// Legacy whole-expression assignment handler. Used both when
  /// `--resolve-assign-bits` is off and as a fallback when the bit-aligned
  /// path can't align the two sides' widths (e.g. string assignments where
  /// the concatenation is wider than the named value).
  void handleAssignmentLegacy(const ast::AssignmentExpression &expr);

  /// Visit @p expr in R-value position, expanding its operators into
  /// Operation nodes when that is enabled.
  void visitRvalue(ast::Expression const &expr);

  void handle(ast::ConditionalStatement const &stmt);

  void handle(ast::ConditionalExpression const &expr);

  void handle(ast::CaseStatement const &stmt);

  /// For loops whose iteration variable is declared outside the header
  /// (e.g. a module-level integer) are not unrolled by the base analysis,
  /// which drops the body's definitions when the exit condition can't be
  /// evaluated. Unroll them here when the iteration count is provable, and
  /// otherwise analyse the body once conservatively.
  void handle(ast::ForLoopStatement const &stmt);

  /// Repeat loops: unroll a constant count; otherwise analyse the body
  /// once while keeping the zero-iteration path.
  void handle(ast::RepeatLoopStatement const &stmt);

  /// While loops: analyse the body at least once when the condition is not
  /// statically decidable, so the body's definitions are not lost.
  void handle(ast::WhileLoopStatement const &stmt);

  /// Walk a conditional whose condition carries a pattern match, taking
  /// over the branch bookkeeping the base class would otherwise do. The
  /// base splits the flow state into branches before visiting the
  /// pattern, and a pattern visit rejoins a split state, which discards
  /// both branches along with the current node and every definition made
  /// earlier in the block. Reading the condition and its pattern while
  /// the state is whole and only then forking the two arms avoids that.
  /// The match itself stays opaque, so both arms contribute.
  template <typename TConditions, typename TrueFn, typename FalseFn>
  void visitPatternConditional(TConditions const &conditions, TrueFn visitTrue,
                               FalseFn visitFalse);

  //===---------------------------------------------------------===//
  // State management
  //===---------------------------------------------------------===//

  auto mergeStates(AnalysisState &result, AnalysisState const &other);

  void joinState(AnalysisState &result, AnalysisState const &other);

  void meetState(AnalysisState &result, AnalysisState const &other);

  auto copyState(AnalysisState const &source) -> AnalysisState;

  static auto unreachableState() -> AnalysisState;
  static auto topState() -> AnalysisState;

private:
  void addNonBlockingLvalue(ast::ValueSymbol const &symbol,
                            ast::Expression const &lsp, DriverBitRange bounds,
                            NetlistNode *node);

  void processNonBlockingLvalues();

  void driveLhsLspSegment(const BitSliceSource &src, const Segment &seg);
  void driveRhsLspSegment(const BitSliceSource &src, const Segment &seg);

  /// Try to unroll @p stmt when its loop variables are declared outside the
  /// header. Returns true when the whole loop was visited. Sets @p
  /// hasExternalStaticVar when at least one header initializer targets a
  /// variable with static lifetime, which selects the conservative
  /// at-least-once handling on failure.
  auto tryUnrollExternalForLoop(ast::ForLoopStatement const &stmt,
                                bool &hasExternalStaticVar) -> bool;
};

} // namespace slang::netlist
