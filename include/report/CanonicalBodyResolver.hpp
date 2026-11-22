#pragma once

#include "slang/ast/Scope.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"

#include <unordered_map>

namespace slang::report {

/// Resolves AST symbols to their canonical counterparts.
///
/// Slang's AnalysisManager stores drivers only against canonical instance
/// bodies. A report that visits every instantiated body therefore has to
/// redirect driver queries to the matching symbol in the canonical body,
/// while still reporting the symbol's own hierarchical path.
///
/// Slang sets a canonical pointer on the outermost non-canonical instance
/// only; nested instances are paired structurally by walking up to an anchor
/// whose canonical is known and lockstep-traversing it with its canonical to
/// register every paired body and value symbol below it. Results are memoized.
class CanonicalBodyResolver {
public:
  /// Return the value symbol that the AnalysisManager stores drivers
  /// against for @p symbol. For a symbol inside a non-canonical instance
  /// body that is the corresponding member of the canonical body;
  /// otherwise it is @p symbol itself.
  auto getCanonicalValueSymbol(ast::ValueSymbol const &symbol)
      -> ast::ValueSymbol const & {
    if (auto it = valueCache.find(&symbol); it != valueCache.end()) {
      return *it->second;
    }
    if (auto const *scope = symbol.getParentScope()) {
      if (auto const *body = scope->getContainingInstance()) {
        getCanonicalBody(*body);
        if (auto it = valueCache.find(&symbol); it != valueCache.end()) {
          return *it->second;
        }
      }
    }
    valueCache.emplace(&symbol, &symbol);
    return symbol;
  }

  /// Return the canonical instance body for @p body. An entry mapping a
  /// body to itself means it is canonical (no redirect).
  auto getCanonicalBody(ast::InstanceBodySymbol const &body)
      -> ast::InstanceBodySymbol const & {
    if (auto it = bodyCache.find(&body); it != bodyCache.end()) {
      return *it->second;
    }

    // Walk up looking for an anchor: an enclosing body whose canonical
    // is already known, either because slang set it via setCanonicalBody
    // (the outermost non-canonical instance) or because it was cached on
    // a previous query.
    ast::InstanceBodySymbol const *cur = &body;
    ast::InstanceBodySymbol const *anchor = nullptr;
    ast::InstanceBodySymbol const *anchorCanonical = nullptr;
    while (cur != nullptr) {
      if (auto it = bodyCache.find(cur); it != bodyCache.end()) {
        anchor = cur;
        anchorCanonical = it->second;
        break;
      }
      if (cur->parentInstance != nullptr) {
        if (auto const *direct = cur->parentInstance->getCanonicalBody();
            direct != nullptr && direct != cur) {
          anchor = cur;
          anchorCanonical = direct;
          break;
        }
      }
      auto const *parentScope = cur->parentInstance != nullptr
                                    ? cur->parentInstance->getParentScope()
                                    : nullptr;
      if (parentScope == nullptr) {
        break;
      }
      cur = parentScope->getContainingInstance();
    }

    if (anchor == nullptr || anchorCanonical == anchor) {
      // No redirect along the chain; the body is canonical.
      bodyCache.emplace(&body, &body);
      return body;
    }

    // Pair the anchor with its canonical, then traverse the subtree to
    // register every nested body and value pair.
    bodyCache.emplace(anchor, anchorCanonical);
    populatePairedBodies(*anchor, *anchorCanonical);

    if (auto it = bodyCache.find(&body); it != bodyCache.end()) {
      return *it->second;
    }
    // Defensive: fall back to identity rather than asserting.
    bodyCache.emplace(&body, &body);
    return body;
  }

private:
  /// Walk @p local and @p canonical in lockstep, registering paired value
  /// symbols and instance bodies in the caches. Recurses through generate
  /// blocks and child instance bodies. Positional matching is sound because
  /// slang's instance-cache key requires identical content before linking a
  /// canonical body.
  void populatePairedBodies(ast::Scope const &local,
                            ast::Scope const &canonical) {
    auto localIt = local.members().begin();
    auto localEnd = local.members().end();
    auto canonIt = canonical.members().begin();
    auto canonEnd = canonical.members().end();
    for (; localIt != localEnd && canonIt != canonEnd; ++localIt, ++canonIt) {
      if (localIt->isValue() && canonIt->isValue()) {
        valueCache.emplace(&localIt->as<ast::ValueSymbol>(),
                           &canonIt->as<ast::ValueSymbol>());
        continue;
      }
      if (localIt->kind == ast::SymbolKind::Instance &&
          canonIt->kind == ast::SymbolKind::Instance) {
        auto const &localInst = localIt->as<ast::InstanceSymbol>();
        auto const &canonInst = canonIt->as<ast::InstanceSymbol>();
        auto const *canon = canonInst.getCanonicalBody();
        auto const &canonBody = canon != nullptr ? *canon : canonInst.body;
        if (&localInst.body != &canonBody) {
          bodyCache.emplace(&localInst.body, &canonBody);
          populatePairedBodies(localInst.body, canonBody);
        } else {
          bodyCache.emplace(&localInst.body, &localInst.body);
        }
        continue;
      }
      if (localIt->kind == canonIt->kind) {
        if (localIt->kind == ast::SymbolKind::GenerateBlock) {
          populatePairedBodies(localIt->as<ast::GenerateBlockSymbol>(),
                               canonIt->as<ast::GenerateBlockSymbol>());
        } else if (localIt->kind == ast::SymbolKind::GenerateBlockArray) {
          populatePairedBodies(localIt->as<ast::GenerateBlockArraySymbol>(),
                               canonIt->as<ast::GenerateBlockArraySymbol>());
        } else if (localIt->kind == ast::SymbolKind::InstanceArray) {
          // Array of instances (e.g. `sub u[4](...)`); recurse so each
          // element's instance body is paired with its canonical.
          populatePairedBodies(localIt->as<ast::InstanceArraySymbol>(),
                               canonIt->as<ast::InstanceArraySymbol>());
        }
      }
    }
  }

  /// A symbol from a non-canonical instance body maps to the corresponding
  /// symbol in the canonical body; every other symbol maps to itself.
  std::unordered_map<ast::ValueSymbol const *, ast::ValueSymbol const *>
      valueCache;

  /// Maps each instance body to its canonical counterpart.
  std::unordered_map<ast::InstanceBodySymbol const *,
                     ast::InstanceBodySymbol const *>
      bodyCache;
};

} // namespace slang::report
