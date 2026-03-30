//===- PerfASTVisitor.h - Clang AST performance analysis --------*- C++ -*-===//
//
// Walks the Clang AST to find source-level optimization opportunities.
//
//===----------------------------------------------------------------------===//

#ifndef PERFSANITIZER_PERFASTVISITOR_H
#define PERFSANITIZER_PERFASTVISITOR_H

#include "PerfHint.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"

namespace perfsanitizer {

class PerfASTVisitor : public clang::RecursiveASTVisitor<PerfASTVisitor> {
public:
  explicit PerfASTVisitor(clang::ASTContext &Ctx, PerfHintCollector &Collector)
      : Ctx(Ctx), SM(Ctx.getSourceManager()), Collector(Collector) {}

  // Declaration visitors
  bool VisitFunctionDecl(clang::FunctionDecl *FD);
  bool VisitVarDecl(clang::VarDecl *VD);
  bool VisitCXXRecordDecl(clang::CXXRecordDecl *RD);
  bool VisitCXXMethodDecl(clang::CXXMethodDecl *MD);
  bool VisitFieldDecl(clang::FieldDecl *FD);

  // Statement/expression visitors
  bool VisitForStmt(clang::ForStmt *S);
  bool VisitWhileStmt(clang::WhileStmt *S);
  bool VisitIfStmt(clang::IfStmt *S);
  bool VisitCallExpr(clang::CallExpr *CE);
  bool VisitCXXNewExpr(clang::CXXNewExpr *NE);
  bool VisitCXXThrowExpr(clang::CXXThrowExpr *TE);
  bool VisitBinaryOperator(clang::BinaryOperator *BO);
  bool VisitDeclRefExpr(clang::DeclRefExpr *DRE);
  bool VisitCXXTryStmt(clang::CXXTryStmt *S);
  bool VisitLambdaExpr(clang::LambdaExpr *LE);

private:
  clang::ASTContext &Ctx;
  clang::SourceManager &SM;
  PerfHintCollector &Collector;
  unsigned CurrentLoopDepth = 0;

  /// Helper: make a PerfHint with source location from a SourceLocation.
  PerfHint makeHint(HintCategory Cat, Impact Base, clang::SourceLocation Loc,
                    llvm::StringRef Msg, llvm::StringRef Suggestion);

  /// Check if a function body is simple enough for constexpr.
  bool bodyCouldBeConstexpr(const clang::FunctionDecl *FD);

  /// Check if an expression is a compile-time constant.
  bool isCompileTimeConstant(const clang::Expr *E);

  /// Get the enclosing function name.
  std::string getEnclosingFunctionName(clang::SourceLocation Loc);

  /// Check for cold path outlining opportunities in an if-statement.
  void checkColdPathOutlining(clang::IfStmt *S);

  /// Check for loop unswitching opportunities (loop-invariant conditions).
  void checkLoopUnswitching(clang::Stmt *LoopBody, clang::SourceLocation Loc);

  /// Check for missing [[nodiscard]] on function declarations.
  void checkMissingNodiscard(clang::FunctionDecl *FD);

  /// Check for discarded return values at call sites.
  void checkDiscardedReturnValue(clang::CallExpr *CE);

  /// Check for signed loop counter vs unsigned bound.
  void checkSignedLoopCounter(clang::ForStmt *S);

  /// Check if a given expression references any of the given local variables.
  bool referencesAnyVar(const clang::Expr *E,
                        const llvm::SmallPtrSetImpl<const clang::VarDecl *> &Vars);

  /// Collect variables declared within a statement subtree.
  void collectLocalVars(const clang::Stmt *S,
                        llvm::SmallPtrSetImpl<const clang::VarDecl *> &Vars);

  /// Count statements in a compound statement (non-recursive top level).
  unsigned countStatements(const clang::Stmt *S);

  /// Check for false sharing in adjacent atomic/volatile fields.
  void checkFalseSharing(clang::CXXRecordDecl *RD);

  /// Check for std::string parameters passed by value.
  void checkStringByValue(clang::FunctionDecl *FD);

  /// Check for push_back/emplace_back without prior reserve() in loops.
  void checkContainerReserve(clang::Stmt *LoopBody, clang::SourceLocation Loc);

  /// Check for index-based for loops convertible to range-for.
  void checkRangeForConversion(clang::ForStmt *S);

  /// Check for if-conditions that could be if constexpr.
  void checkConstexprIf(clang::IfStmt *S);

  /// Check for large lambda captures by value.
  void checkLambdaCaptureOpt(clang::LambdaExpr *LE);

  /// Check for output parameters that could be return values.
  void checkOutputParamToReturn(clang::FunctionDecl *FD);

  /// Helper: check if a type name contains a substring.
  bool typeNameContains(clang::QualType T, llvm::StringRef Substr);

  /// Helper: check if a statement contains a try/catch.
  bool containsTryCatch(const clang::Stmt *S);

  /// Helper: check if a function body calls .reserve() on a given variable.
  bool hasReserveCallFor(const clang::Stmt *Body, const clang::VarDecl *VD);

  /// Helper: find push_back/emplace_back calls in a loop body.
  void findPushBackCalls(
      const clang::Stmt *S,
      llvm::SmallVectorImpl<std::pair<const clang::CXXMemberCallExpr *,
                                      const clang::VarDecl *>> &Results);
};

class PerfASTConsumer : public clang::ASTConsumer {
public:
  explicit PerfASTConsumer(clang::CompilerInstance &CI,
                           PerfHintCollector &Collector)
      : CI(CI), Collector(Collector) {}

  void HandleTranslationUnit(clang::ASTContext &Ctx) override;

private:
  clang::CompilerInstance &CI;
  PerfHintCollector &Collector;
};

} // namespace perfsanitizer

#endif // PERFSANITIZER_PERFASTVISITOR_H
