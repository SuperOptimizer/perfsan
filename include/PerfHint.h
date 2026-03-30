//===- PerfHint.h - Performance hint infrastructure -------------*- C++ -*-===//
//
// Part of the PerfSanitizer project.
// A diagnostic tool that suggests source-level changes to improve codegen.
//
//===----------------------------------------------------------------------===//

#ifndef PERFSANITIZER_PERFHINT_H
#define PERFSANITIZER_PERFHINT_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Basic/SourceLocation.h"
#include <string>
#include <vector>

namespace perfsanitizer {

/// Impact score buckets — used for sorting and diagnostic severity.
enum class Impact : unsigned {
  Critical = 100, // Vectorization blocker in hot loop, major alias issue
  High = 75,      // restrict annotation, consteval on hot path
  Medium = 50,    // constexpr promotion, branch hints
  Low = 25,       // Minor inlining candidate, alignment
  Info = 10,      // Informational only
};

/// Which layer detected this hint.
enum class HintLayer : uint8_t {
  AST, // Clang AST visitor
  IR,  // LLVM IR pass
};

/// Category tag for grouping/filtering.
enum class HintCategory : uint8_t {
  ConstexprPromotion,
  ConstevalPromotion,
  RestrictAnnotation,
  AlignmentHint,
  BranchPrediction,
  LoopBound,
  LoopInvariant,
  Vectorization,
  AliasBarrier,
  InliningCandidate,
  TailCall,
  HeapToStack,
  FMAContraction,
  PureConst,
  VirtualDevirt,
  HotColdSplit,
  RedundantLoad,
  SROAEscape,
  NoExcept,
  MoveSemantics,
  DataLayout,
  BranchlessSelect,
  SoAvsAoS,
  ColdPathOutlining,
  LoopUnswitching,
  SIMDWidth,
  StrengthReduction,
  MissingNodiscard,
  SignedLoopCounter,
};

/// A single performance hint with source location and impact score.
struct PerfHint {
  HintCategory Category;
  HintLayer Layer;
  unsigned Score; // 0-200, scaled by loop depth
  std::string Message;
  std::string Suggestion;
  std::string FunctionName;

  // Source location — for AST hints these are real SourceLocations.
  // For IR hints, we store file:line:col as strings from debug info.
  std::string File;
  unsigned Line = 0;
  unsigned Col = 0;

  // For Clang AST hints, optionally carry the real SourceLocation.
  clang::SourceLocation SrcLoc;

  /// Compute the severity string for display.
  llvm::StringRef severityTag() const {
    if (Score >= 90)
      return "CRITICAL";
    if (Score >= 65)
      return "HIGH";
    if (Score >= 40)
      return "MEDIUM";
    if (Score >= 20)
      return "LOW";
    return "INFO";
  }

  /// Category name for display.
  const char *categoryName() const;
};

/// Collects, deduplicates, and sorts hints from both layers.
class PerfHintCollector {
public:
  void addHint(PerfHint H);

  /// Sort hints by score (descending), deduplicate same-location same-category.
  void finalize();

  /// Emit all hints as compiler diagnostics or to a stream.
  void emitToStream(llvm::raw_ostream &OS) const;

  const std::vector<PerfHint> &getHints() const { return Hints; }
  bool empty() const { return Hints.empty(); }
  size_t size() const { return Hints.size(); }

  /// Singleton access for cross-layer collection.
  static PerfHintCollector &instance();

private:
  std::vector<PerfHint> Hints;
  bool Finalized = false;
};

/// Scale a base impact score by loop nesting depth.
/// Each level multiplies by 1.5, capped at 200.
inline unsigned scaleByLoopDepth(Impact Base, unsigned Depth) {
  double S = static_cast<double>(static_cast<unsigned>(Base));
  for (unsigned I = 0; I < Depth; ++I)
    S *= 1.5;
  return std::min(static_cast<unsigned>(S), 200u);
}

} // namespace perfsanitizer

#endif // PERFSANITIZER_PERFHINT_H
