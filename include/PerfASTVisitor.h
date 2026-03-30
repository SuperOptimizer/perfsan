//===- PerfASTVisitor.h - Clang AST performance analysis --------*- C++ -*-===//
//
// Walks the Clang AST to find source-level optimization opportunities.
//
//===----------------------------------------------------------------------===//

#ifndef PERFSANITIZER_PERFASTVISITOR_H
#define PERFSANITIZER_PERFASTVISITOR_H

#include "PerfHint.h"
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
