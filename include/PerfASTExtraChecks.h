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
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"

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

/// Detect when a function parameter is a pointer/reference to a class type
/// whose full definition may not be needed — a forward declaration would
/// suffice, avoiding an #include.
void checkUnusedInclude(const clang::FunctionDecl *FD, clang::ASTContext &Ctx,
                        PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect small functions (<=3 statements) in .cpp files that are not marked
/// inline. These incur call overhead that could be avoided.
void checkSmallFunctionNotInline(const clang::FunctionDecl *FD,
                                 clang::ASTContext &Ctx,
                                 PerfHintCollector &Collector);

/// Detect non-const pass-by-value parameters of types >64 bytes or types with
/// non-trivial copy constructors (string, vector, map).
void checkUnnecessaryCopy(const clang::FunctionDecl *FD, clang::ASTContext &Ctx,
                          PerfHintCollector &Collector);

/// In for loops, detect expressions that compute the same value every iteration
/// but aren't hoisted (e.g., strlen(), string::size(), index computations not
/// using the loop variable).
void checkRedundantComputation(const clang::ForStmt *FS, clang::ASTContext &Ctx,
                               PerfHintCollector &Collector,
                               unsigned LoopDepth);

/// Detect new/malloc/make_unique/make_shared inside for/while loops.
/// Allocation in hot loops is almost always a performance bug.
void checkTightLoopAllocation(const clang::Stmt *LoopBody,
                              clang::ASTContext &Ctx,
                              PerfHintCollector &Collector,
                              unsigned LoopDepth);

/// Detect `if (flag) return true; else return false;` or
/// `if (x) return true; return false;` patterns that could be simplified.
void checkBoolBranching(const clang::IfStmt *IS, clang::ASTContext &Ctx,
                        PerfHintCollector &Collector);

/// Detect manual bubble sort / selection sort patterns (nested for loops with
/// swap). Suggest std::sort.
void checkSortAlgorithm(const clang::ForStmt *FS, clang::ASTContext &Ctx,
                        PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect `x % N` where N is a power of 2 (could be `x & (N-1)`) and
/// `x / N` where N is a power of 2 (could be `x >> log2(N)`).
void checkPowerOfTwo(const clang::BinaryOperator *BO, clang::ASTContext &Ctx,
                     PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect throw expressions inside destructors.
void checkExceptionInDestructor(const clang::CXXDestructorDecl *DD,
                                clang::ASTContext &Ctx,
                                PerfHintCollector &Collector);

/// Detect std::vector<bool> which is a notoriously slow specialization.
void checkVectorBoolAvoid(const clang::VarDecl *VD, clang::ASTContext &Ctx,
                          PerfHintCollector &Collector);

/// Detect lock_guard/unique_lock/mutex.lock() inside loops.
void checkMutexInLoop(const clang::Stmt *LoopBody, clang::ASTContext &Ctx,
                      PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect std::function used as parameters or in variable declarations.
/// Overload for FunctionDecl (checks parameters).
void checkStdFunctionOverhead(const clang::FunctionDecl *FD,
                              clang::ASTContext &Ctx,
                              PerfHintCollector &Collector,
                              unsigned LoopDepth);

/// Detect std::function used in variable declarations.
/// Overload for VarDecl.
void checkStdFunctionOverhead(const clang::VarDecl *VD,
                              clang::ASTContext &Ctx,
                              PerfHintCollector &Collector,
                              unsigned LoopDepth);

/// Detect for/while loops with empty body (null body or empty CompoundStmt).
void checkEmptyLoopBody(const clang::ForStmt *FS, clang::ASTContext &Ctx,
                        PerfHintCollector &Collector, unsigned LoopDepth);

/// Detect if(a) ... else if(a) where both conditions are textually identical.
void checkDuplicateCondition(const clang::IfStmt *IS, clang::ASTContext &Ctx,
                             PerfHintCollector &Collector, unsigned LoopDepth);

/// Find operator+= on std::string inside loops.
void checkStringConcatInLoop(const clang::Stmt *LoopBody,
                             clang::ASTContext &Ctx,
                             PerfHintCollector &Collector, unsigned LoopDepth);

/// Find CXXConstructExpr of std::regex / std::basic_regex inside a loop.
void checkRegexInLoop(const clang::Stmt *LoopBody, clang::ASTContext &Ctx,
                      PerfHintCollector &Collector, unsigned LoopDepth);

/// Find CXXDynamicCastExpr inside loops.
void checkDynamicCastInLoop(const clang::Stmt *LoopBody,
                            clang::ASTContext &Ctx,
                            PerfHintCollector &Collector, unsigned LoopDepth);

/// Base class with virtual methods but non-virtual destructor.
void checkVirtualDtorMissing(const clang::CXXRecordDecl *RD,
                             clang::ASTContext &Ctx,
                             PerfHintCollector &Collector, unsigned LoopDepth);

/// Range-for where the loop variable is by value and element type is
/// non-trivial or >64 bytes.
void checkCopyInRangeFor(const clang::CXXForRangeStmt *S,
                         clang::ASTContext &Ctx, PerfHintCollector &Collector,
                         unsigned LoopDepth);

/// throw expression inside a noexcept function.
void checkThrowInNoexcept(const clang::CXXThrowExpr *TE,
                          const clang::FunctionDecl *EnclosingFD,
                          clang::ASTContext &Ctx,
                          PerfHintCollector &Collector);

/// DeclRefExpr referencing a global/namespace-scope variable inside a loop.
void checkGlobalVarInLoop(const clang::Stmt *LoopBody, clang::ASTContext &Ctx,
                          PerfHintCollector &Collector, unsigned LoopDepth);

/// Load/store of volatile variable inside a loop.
void checkVolatileInLoop(const clang::Stmt *LoopBody, clang::ASTContext &Ctx,
                         PerfHintCollector &Collector, unsigned LoopDepth);

/// Narrowing implicit conversion (double->float, long->int, etc.) in loops.
void checkImplicitConversion(const clang::ImplicitCastExpr *ICE,
                             clang::ASTContext &Ctx,
                             PerfHintCollector &Collector, unsigned LoopDepth);

/// Passing a derived class object by value to a function taking a base class
/// by value (object slicing).
void checkSlicingCopy(const clang::CallExpr *CE, clang::ASTContext &Ctx,
                      PerfHintCollector &Collector, unsigned LoopDepth);

/// Multiple divisions by the same variable in a block — suggest computing
/// reciprocal once.
void checkDivisionChain(const clang::BinaryOperator *BO, clang::ASTContext &Ctx,
                        PerfHintCollector &Collector, unsigned LoopDepth);

} // namespace perfsanitizer

#endif // PERFSANITIZER_PERFASTEXTRACHECKS_H
