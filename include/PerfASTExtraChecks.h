//===- PerfASTExtraChecks.h - Extra AST perf checks -------------*- C++ -*-===//
//
// Standalone free functions that detect additional performance anti-patterns
// in the Clang AST. These are intended to be called from PerfASTVisitor but
// are decoupled into their own translation unit to allow independent development.
//
//===----------------------------------------------------------------------===//

#ifndef PERFSANITIZER_PERFASTEXTRACHECKS_H
#define PERFSANITIZER_PERFASTEXTRACHECKS_H

#include "PerfHint.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"

namespace perfsanitizer {

/// Detect std::vector where .reserve() is called with a small constant (<= 32).
/// Suggests SmallVector<T, N> or std::array instead.
void checkSmallVectorSize(const clang::CallExpr *CE, clang::ASTContext &Ctx,
                          PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect virtual function calls inside loops.
/// Suggests devirtualization via CRTP, final, or template parameter.
void checkVirtualInLoop(const clang::CallExpr *CE, clang::ASTContext &Ctx,
                        PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect std::shared_ptr passed by value (copies reference count).
/// Suggests const ref or unique_ptr if sole ownership.
void checkSharedPtrOverhead(const clang::ParmVarDecl *PVD,
                            clang::ASTContext &Ctx,
                            PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect std::map usage where std::unordered_map may be better.
/// Checks that no order-dependent operations (lower_bound, upper_bound,
/// rbegin) are called on the variable.
void checkMapToUnorderedMap(const clang::VarDecl *VD, clang::ASTContext &Ctx,
                            PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect .size() == 0 or .size() != 0 patterns.
/// Suggests .empty() which may be O(1) vs O(n) for some containers.
void checkEmptyVsSize(const clang::BinaryOperator *BO, clang::ASTContext &Ctx,
                      PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect post-increment (i++) on iterators or complex types in loops.
/// Suggests pre-increment (++i) to avoid temporary copy.
void checkPreIncrement(const clang::UnaryOperator *UO, clang::ASTContext &Ctx,
                       PerfHintCollector &Collector, unsigned LoopDepth);

/// Inside loops, detect simple if/else predicates that could be replaced
/// with branchless arithmetic (e.g., result += (x > 0) * val).
void checkBranchFreePredicate(const clang::IfStmt *IS, clang::ASTContext &Ctx,
                              PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect byte-by-byte copy loops on char/uint8_t arrays.
/// Suggests std::memcpy or std::copy.
void checkMemcpyOpportunity(const clang::ForStmt *FS, clang::ASTContext &Ctx,
                            PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect non-trivial static local variable initialization inside loops.
/// The hidden guard variable check happens every call.
/// Suggests moving to file scope or making constexpr.
void checkStaticLocalInit(const clang::VarDecl *VD, clang::ASTContext &Ctx,
                          PerfHintCollector &Collector, unsigned LoopDepth);

/// Inside loops, detect local variables of non-trivial type (string, vector,
/// map) being constructed each iteration. Suggests hoisting outside loop
/// and using .clear().
void checkExcessiveCopy(const clang::VarDecl *VD, clang::ASTContext &Ctx,
                        PerfHintCollector &Collector, unsigned LoopDepth);

} // namespace perfsanitizer

#endif // PERFSANITIZER_PERFASTEXTRACHECKS_H
