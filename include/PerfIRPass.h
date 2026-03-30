//===- PerfIRPass.h - LLVM IR performance analysis pass ---------*- C++ -*-===//
//
// NewPM module pass that analyzes LLVM IR for missed optimization opportunities
// that could be unlocked by source-level changes.
//
//===----------------------------------------------------------------------===//

#ifndef PERFSANITIZER_PERFIRPASS_H
#define PERFSANITIZER_PERFIRPASS_H

#include "PerfHint.h"
#include "llvm/IR/PassManager.h"

namespace perfsanitizer {

class PerfIRPass : public llvm::PassInfoMixin<PerfIRPass> {
public:
  explicit PerfIRPass(PerfHintCollector &Collector) : Collector(Collector) {}

  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  static bool isRequired() { return true; }

private:
  PerfHintCollector &Collector;

  void analyzeFunction(llvm::Function &F,
                       llvm::FunctionAnalysisManager &FAM);

  // Individual IR checks
  void checkVectorizationBlockers(llvm::Function &F,
                                  llvm::FunctionAnalysisManager &FAM);
  void checkAliasBarriers(llvm::Function &F,
                          llvm::FunctionAnalysisManager &FAM);
  void checkLoopTripCounts(llvm::Function &F,
                           llvm::FunctionAnalysisManager &FAM);
  void checkMissedTailCalls(llvm::Function &F);
  void checkRedundantLoads(llvm::Function &F);
  void checkSROAEscapes(llvm::Function &F);
  void checkInliningCandidates(llvm::Function &F,
                               llvm::FunctionAnalysisManager &FAM);
  void checkBranchPatterns(llvm::Function &F,
                           llvm::FunctionAnalysisManager &FAM);
  void checkDataLayoutIssues(llvm::Function &F);
  void checkSoAvsAoS(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
  void checkSIMDWidth(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
  void checkStrengthReduction(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};

} // namespace perfsanitizer

#endif // PERFSANITIZER_PERFIRPASS_H
