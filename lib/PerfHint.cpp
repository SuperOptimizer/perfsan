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
