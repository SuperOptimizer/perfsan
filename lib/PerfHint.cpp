//===- PerfHint.cpp - Hint collection, dedup, scoring, emission -----------===//

#include "PerfHint.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <set>

using namespace perfsanitizer;

const char *PerfHint::categoryName() const {
  switch (Category) {
  case HintCategory::ConstexprPromotion:
    return "constexpr-promotion";
  case HintCategory::ConstevalPromotion:
    return "consteval-promotion";
  case HintCategory::RestrictAnnotation:
    return "restrict-annotation";
  case HintCategory::AlignmentHint:
    return "alignment";
  case HintCategory::BranchPrediction:
    return "branch-prediction";
  case HintCategory::LoopBound:
    return "loop-bound";
  case HintCategory::LoopInvariant:
    return "loop-invariant";
  case HintCategory::Vectorization:
    return "vectorization";
  case HintCategory::AliasBarrier:
    return "alias-barrier";
  case HintCategory::InliningCandidate:
    return "inlining";
  case HintCategory::TailCall:
    return "tail-call";
  case HintCategory::HeapToStack:
    return "heap-to-stack";
  case HintCategory::FMAContraction:
    return "fma-contraction";
  case HintCategory::PureConst:
    return "pure-const";
  case HintCategory::VirtualDevirt:
    return "virtual-devirt";
  case HintCategory::HotColdSplit:
    return "hot-cold-split";
  case HintCategory::RedundantLoad:
    return "redundant-load";
  case HintCategory::SROAEscape:
    return "sroa-escape";
  case HintCategory::NoExcept:
    return "noexcept";
  case HintCategory::MoveSemantics:
    return "move-semantics";
  case HintCategory::DataLayout:
    return "data-layout";
  case HintCategory::BranchlessSelect:
    return "branchless-select";
  case HintCategory::SoAvsAoS:
    return "soa-vs-aos";
  case HintCategory::ColdPathOutlining:
    return "cold-path-outlining";
  case HintCategory::LoopUnswitching:
    return "loop-unswitching";
  case HintCategory::SIMDWidth:
    return "simd-width";
  case HintCategory::StrengthReduction:
    return "strength-reduction";
  case HintCategory::MissingNodiscard:
    return "missing-nodiscard";
  case HintCategory::SignedLoopCounter:
    return "signed-loop-counter";
  case HintCategory::ExceptionCost:
    return "exception-cost";
  case HintCategory::FalseSharing:
    return "false-sharing";
  case HintCategory::StringByValue:
    return "string-by-value";
  case HintCategory::ContainerReserve:
    return "container-reserve";
  case HintCategory::RangeForConversion:
    return "range-for-conversion";
  case HintCategory::ConstexprIf:
    return "constexpr-if";
  case HintCategory::LambdaCaptureOpt:
    return "lambda-capture-opt";
  case HintCategory::OutputParamToReturn:
    return "output-param-to-return";
  case HintCategory::UnusedInclude:
    return "unused-include";
  case HintCategory::SmallFunctionNotInline:
    return "small-function-not-inline";
  case HintCategory::UnnecessaryCopy:
    return "unnecessary-copy";
  case HintCategory::RedundantComputation:
    return "redundant-computation";
  case HintCategory::TightLoopAllocation:
    return "tight-loop-allocation";
  case HintCategory::BoolBranching:
    return "bool-branching";
  case HintCategory::SortAlgorithm:
    return "sort-algorithm";
  case HintCategory::PowerOfTwo:
    return "power-of-two";
  case HintCategory::ExceptionInDestructor:
    return "exception-in-destructor";
  case HintCategory::VectorBoolAvoid:
    return "vector-bool-avoid";
  case HintCategory::MutexInLoop:
    return "mutex-in-loop";
  case HintCategory::StdFunctionOverhead:
    return "std-function-overhead";
  case HintCategory::SharedPtrOverhead:
    return "shared-ptr-overhead";
  case HintCategory::BitManipulation:
    return "bit-manipulation";
  case HintCategory::RedundantAtomic:
    return "redundant-atomic";
  case HintCategory::CacheLineSplit:
    return "cache-line-split";
  case HintCategory::CrossTUInlining:
    return "cross-tu-inlining";
  case HintCategory::HotColdFunction:
    return "hot-cold-function";
  case HintCategory::EmptyLoopBody:
    return "empty-loop-body";
  case HintCategory::DuplicateCondition:
    return "duplicate-condition";
  case HintCategory::StringConcatInLoop:
    return "string-concat-in-loop";
  case HintCategory::RegexInLoop:
    return "regex-in-loop";
  case HintCategory::DynamicCastInLoop:
    return "dynamic-cast-in-loop";
  case HintCategory::VirtualDtorMissing:
    return "virtual-dtor-missing";
  case HintCategory::CopyInRangeFor:
    return "copy-in-range-for";
  case HintCategory::ThrowInNoexcept:
    return "throw-in-noexcept";
  case HintCategory::GlobalVarInLoop:
    return "global-var-in-loop";
  case HintCategory::VolatileInLoop:
    return "volatile-in-loop";
  case HintCategory::ImplicitConversion:
    return "implicit-conversion";
  case HintCategory::SlicingCopy:
    return "slicing-copy";
  case HintCategory::DivisionChain:
    return "division-chain";
  case HintCategory::SpillPressure:
    return "spill-pressure";
  case HintCategory::UnrollingBlocker:
    return "unrolling-blocker";
  case HintCategory::BranchOnFloat:
    return "branch-on-float";
  case HintCategory::MemoryAccessPattern:
    return "memory-access-pattern";
  case HintCategory::SmallFunctionInline:
    return "small-function-inline";
  }
  return "unknown";
}

void PerfHintCollector::addHint(PerfHint H) {
  Finalized = false;
  Hints.push_back(std::move(H));
}

void PerfHintCollector::finalize() {
  if (Finalized)
    return;

  // Deduplicate: same file+line+category keeps the higher score.
  std::set<std::tuple<std::string, unsigned, HintCategory>> Seen;
  std::vector<PerfHint> Deduped;
  Deduped.reserve(Hints.size());

  // Sort by score descending first so higher-scored duplicates come first.
  std::sort(Hints.begin(), Hints.end(),
            [](const PerfHint &A, const PerfHint &B) {
              return A.Score > B.Score;
            });

  for (auto &H : Hints) {
    auto Key = std::make_tuple(H.File, H.Line, H.Category);
    if (Seen.insert(Key).second)
      Deduped.push_back(std::move(H));
  }

  Hints = std::move(Deduped);
  // Already sorted by score descending from above.
  Finalized = true;
}

void PerfHintCollector::emitToStream(llvm::raw_ostream &OS) const {
  if (Hints.empty()) {
    OS << "perfsanitizer: no optimization hints found.\n";
    return;
  }

  OS << "=== PerfSanitizer Report (" << Hints.size() << " hints) ===\n\n";

  for (const auto &H : Hints) {
    OS << "[" << H.severityTag() << " score:" << H.Score << "] "
       << "[" << H.categoryName() << "] ";
    if (!H.File.empty())
      OS << H.File << ":" << H.Line << ":" << H.Col << " ";
    if (!H.FunctionName.empty())
      OS << "in " << H.FunctionName << " ";
    OS << "\n";
    OS << "  " << H.Message << "\n";
    if (!H.Suggestion.empty())
      OS << "  -> " << H.Suggestion << "\n";
    OS << "\n";
  }

  // Summary by category.
  OS << "--- Summary by category ---\n";
  std::map<HintCategory, unsigned> CatCount;
  for (const auto &H : Hints)
    CatCount[H.Category]++;
  for (const auto &[Cat, Count] : CatCount) {
    PerfHint Dummy;
    Dummy.Category = Cat;
    OS << "  " << Dummy.categoryName() << ": " << Count << "\n";
  }
  OS << "\n";
}

PerfHintCollector &PerfHintCollector::instance() {
  static PerfHintCollector Inst;
  return Inst;
}
