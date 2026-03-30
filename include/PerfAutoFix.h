//===- PerfAutoFix.h - Auto-fix generation for perf hints -------*- C++ -*-===//
//
// Part of the PerfSanitizer project.
// Generates source-level fixes for performance hints using clang::Rewriter.
//
//===----------------------------------------------------------------------===//

#ifndef PERFSANITIZER_PERFAUTOFIX_H
#define PERFSANITIZER_PERFAUTOFIX_H

#pragma once
#include "PerfHint.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include <string>
#include <vector>

namespace perfsanitizer {

struct AutoFix {
  clang::SourceLocation Loc;
  enum Kind {
    Insert,      // Insert text at location
    Replace,     // Replace range with new text
    InsertAfter, // Insert text after location
  } FixKind;
  std::string OldText;  // For Replace: text to find and replace
  std::string NewText;  // Text to insert or replacement text
  std::string Description;
};

class PerfAutoFixer {
public:
  PerfAutoFixer(clang::SourceManager &SM, const clang::LangOptions &LangOpts);

  /// Generate fixes for a hint. Returns empty vector if no auto-fix available.
  std::vector<AutoFix> generateFixes(const PerfHint &Hint,
                                      clang::ASTContext &Ctx);

  /// Apply all fixes using Rewriter.
  bool applyFixes(const std::vector<AutoFix> &Fixes);

  /// Write modified buffers to disk.
  bool writeFixedFiles();

  /// Get a diff-style output of changes.
  std::string getDiff();

  clang::Rewriter &getRewriter() { return Rewrite; }

private:
  clang::SourceManager &SM;
  clang::Rewriter Rewrite;

  // Category-specific fix generators.
  std::vector<AutoFix> fixConstexprPromotion(const PerfHint &H,
                                              clang::ASTContext &Ctx);
  std::vector<AutoFix> fixNoexcept(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixNodiscard(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixBranchPrediction(const PerfHint &H,
                                            clang::ASTContext &Ctx);
  std::vector<AutoFix> fixSignedLoopCounter(const PerfHint &H,
                                             clang::ASTContext &Ctx);
  std::vector<AutoFix> fixPreIncrement(const PerfHint &H,
                                        clang::ASTContext &Ctx);
  std::vector<AutoFix> fixEmptyCheck(const PerfHint &H,
                                      clang::ASTContext &Ctx);
  std::vector<AutoFix> fixRestrict(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixConstVariable(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixPureConst(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixHeapToStack(const PerfHint &H,
                                       clang::ASTContext &Ctx);
  std::vector<AutoFix> fixFMA(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixVirtualDevirt(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixAlignment(const PerfHint &H,
                                     clang::ASTContext &Ctx);
  std::vector<AutoFix> fixDataLayout(const PerfHint &H,
                                      clang::ASTContext &Ctx);
  std::vector<AutoFix> fixLoopInvariant(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixInlining(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixColdPath(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixContainerReserve(const PerfHint &H,
                                            clang::ASTContext &Ctx);
  std::vector<AutoFix> fixStringByValue(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixRangeForConversion(const PerfHint &H,
                                              clang::ASTContext &Ctx);
  std::vector<AutoFix> fixTailCall(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixExceptionInDestructor(const PerfHint &H,
                                                 clang::ASTContext &Ctx);
  std::vector<AutoFix> fixVectorBool(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixSharedPtr(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixUnnecessaryCopy(const PerfHint &H,
                                           clang::ASTContext &Ctx);
  std::vector<AutoFix> fixBoolBranching(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixPowerOfTwo(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixSoAComment(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixVectorizePragma(const PerfHint &H,
                                           clang::ASTContext &Ctx);
  std::vector<AutoFix> fixExceptionComment(const PerfHint &H,
                                            clang::ASTContext &Ctx);
  std::vector<AutoFix> fixMutexComment(const PerfHint &H,
                                        clang::ASTContext &Ctx);
  std::vector<AutoFix> fixStdFunctionComment(const PerfHint &H,
                                              clang::ASTContext &Ctx);
  std::vector<AutoFix> fixLoopBound(const PerfHint &H, clang::ASTContext &Ctx);
  std::vector<AutoFix> fixLambdaCapture(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixTightAllocComment(const PerfHint &H,
                                             clang::ASTContext &Ctx);
  std::vector<AutoFix> fixRedundantComputComment(const PerfHint &H,
                                                  clang::ASTContext &Ctx);
  std::vector<AutoFix> fixAliasBarrier(const PerfHint &H,
                                        clang::ASTContext &Ctx);
  std::vector<AutoFix> fixHotColdSplit(const PerfHint &H,
                                        clang::ASTContext &Ctx);
  std::vector<AutoFix> fixRedundantLoad(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixSROAEscape(const PerfHint &H,
                                      clang::ASTContext &Ctx);
  std::vector<AutoFix> fixMoveSemantics(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixBranchlessSelect(const PerfHint &H,
                                            clang::ASTContext &Ctx);
  std::vector<AutoFix> fixLoopUnswitching(const PerfHint &H,
                                           clang::ASTContext &Ctx);
  std::vector<AutoFix> fixSIMDWidth(const PerfHint &H,
                                     clang::ASTContext &Ctx);
  std::vector<AutoFix> fixFalseSharing(const PerfHint &H,
                                        clang::ASTContext &Ctx);
  std::vector<AutoFix> fixConstexprIf(const PerfHint &H,
                                       clang::ASTContext &Ctx);
  std::vector<AutoFix> fixOutputParam(const PerfHint &H,
                                       clang::ASTContext &Ctx);
  std::vector<AutoFix> fixUnusedInclude(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixSmallFunctionInline(const PerfHint &H,
                                               clang::ASTContext &Ctx);
  std::vector<AutoFix> fixSortAlgorithm(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixBitManip(const PerfHint &H,
                                    clang::ASTContext &Ctx);
  std::vector<AutoFix> fixRedundantAtomic(const PerfHint &H,
                                           clang::ASTContext &Ctx);
  std::vector<AutoFix> fixCacheLineSplit(const PerfHint &H,
                                          clang::ASTContext &Ctx);
  std::vector<AutoFix> fixCrossTUInline(const PerfHint &H,
                                         clang::ASTContext &Ctx);
  std::vector<AutoFix> fixHotColdFunc(const PerfHint &H,
                                       clang::ASTContext &Ctx);
};

} // namespace perfsanitizer

#endif // PERFSANITIZER_PERFAUTOFIX_H
