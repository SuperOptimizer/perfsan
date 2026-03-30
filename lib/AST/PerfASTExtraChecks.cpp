//===- PerfASTExtraChecks.cpp - Extra AST perf checks ---------------------===//
//
// Standalone free functions for detecting additional performance anti-patterns.
// These are decoupled from PerfASTVisitor so they can be developed in parallel
// and called from the visitor at integration time.
//
//===----------------------------------------------------------------------===//

#include "PerfASTExtraChecks.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtIterator.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Basic/ExceptionSpecificationType.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include <cmath>

using namespace perfsanitizer;
using namespace clang;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Build a PerfHint with source-location info extracted from the SourceManager.
PerfHint makeHint(HintCategory Cat, Impact Base, unsigned LoopDepth,
                  SourceLocation Loc, ASTContext &Ctx, llvm::StringRef Msg,
                  llvm::StringRef Suggestion) {
  PerfHint H;
  H.Category = Cat;
  H.Layer = HintLayer::AST;
  H.Score = scaleByLoopDepth(Base, LoopDepth);
  H.Message = Msg.str();
  H.Suggestion = Suggestion.str();
  H.SrcLoc = Loc;

  const SourceManager &SM = Ctx.getSourceManager();
  if (Loc.isValid() && !SM.isInSystemHeader(Loc)) {
    auto PLoc = SM.getPresumedLoc(Loc);
    if (PLoc.isValid()) {
      H.File = PLoc.getFilename();
      H.Line = PLoc.getLine();
      H.Col = PLoc.getColumn();
    }
  }
  return H;
}

/// Return true if the type name (as a string) contains \p Substr.
bool typeNameContains(QualType T, llvm::StringRef Substr) {
  if (T.isNull())
    return false;
  std::string Name = T.getUnqualifiedType().getAsString();
  return llvm::StringRef(Name).contains(Substr);
}

/// Return true if \p T is a non-trivially-copyable class/struct type.
bool isNonTrivialType(QualType T) {
  T = T.getNonReferenceType().getCanonicalType();
  if (const auto *RT = T->getAs<RecordType>()) {
    if (const auto *RD = dyn_cast<CXXRecordDecl>(RT->getDecl())) {
      if (RD->hasDefinition())
        return !RD->isTriviallyCopyable();
    }
  }
  return false;
}

/// Walk a statement tree looking for a CXXMemberCallExpr on \p VD whose
/// method name matches any of the given names.  Return true if found.
bool hasMethodCallOnVar(const Stmt *S, const VarDecl *VD,
                        llvm::ArrayRef<llvm::StringRef> MethodNames) {
  if (!S)
    return false;
  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(S)) {
    if (const auto *ME = dyn_cast<MemberExpr>(MCE->getCallee())) {
      if (const auto *DRE = dyn_cast<DeclRefExpr>(ME->getBase()->IgnoreImpCasts())) {
        if (DRE->getDecl() == VD) {
          if (const auto *MD = MCE->getMethodDecl()) {
            llvm::StringRef Called = MD->getName();
            for (llvm::StringRef Name : MethodNames) {
              if (Called == Name)
                return true;
            }
          }
        }
      }
    }
  }
  for (const Stmt *Child : S->children()) {
    if (hasMethodCallOnVar(Child, VD, MethodNames))
      return true;
  }
  return false;
}

/// Simplified: we don't use parent traversal for these checks.
/// The checks that need enclosing function context will receive it
/// from the caller instead.

/// Return true if \p Body is a single assignment of the form dst[i] = src[i].
bool isByteCopyBody(const Stmt *Body) {
  const Stmt *Inner = Body;
  // Peel off CompoundStmt with a single child.
  if (const auto *CS = dyn_cast<CompoundStmt>(Body)) {
    if (CS->size() != 1)
      return false;
    Inner = *CS->body_begin();
  }
  const auto *BO = dyn_cast<BinaryOperator>(Inner);
  if (!BO || BO->getOpcode() != BO_Assign)
    return false;

  // LHS and RHS should be ArraySubscriptExpr.
  const auto *LHS = dyn_cast<ArraySubscriptExpr>(BO->getLHS()->IgnoreImpCasts());
  const auto *RHS = dyn_cast<ArraySubscriptExpr>(BO->getRHS()->IgnoreImpCasts());
  if (!LHS || !RHS)
    return false;

  // Check that the element type is char-sized (char, uint8_t, unsigned char).
  QualType ElemTy = LHS->getType().getCanonicalType();
  if (!ElemTy->isCharType() && !ElemTy->isSpecificBuiltinType(BuiltinType::UChar) &&
      !ElemTy->isSpecificBuiltinType(BuiltinType::SChar))
    return false;

  return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 1. SmallVectorSize
// ---------------------------------------------------------------------------

void perfsanitizer::checkSmallVectorSize(const CallExpr *CE, ASTContext &Ctx,
                                         PerfHintCollector &Collector,
                                         unsigned LoopDepth) {
  // Look for calls to .reserve() on a std::vector with a small constant arg.
  const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE);
  if (!MCE)
    return;

  const auto *ME = dyn_cast<MemberExpr>(MCE->getCallee());
  if (!ME)
    return;

  const CXXMethodDecl *MD = MCE->getMethodDecl();
  if (!MD || MD->getName() != "reserve")
    return;

  // Check that the object is a std::vector.
  QualType ObjTy = ME->getBase()->IgnoreImpCasts()->getType();
  if (!typeNameContains(ObjTy, "vector"))
    return;
  // Exclude SmallVector itself.
  if (typeNameContains(ObjTy, "SmallVector"))
    return;

  // Check if the argument is a small constant (<= 32).
  if (MCE->getNumArgs() < 1)
    return;
  const Expr *Arg = MCE->getArg(0)->IgnoreImpCasts();
  Expr::EvalResult Result;
  if (!Arg->EvaluateAsInt(Result, Ctx))
    return;

  llvm::APSInt Val = Result.Val.getInt();
  if (Val.getExtValue() > 32 || Val.getExtValue() <= 0)
    return;

  std::string Msg =
      "std::vector::reserve() called with small constant (" +
      std::to_string(Val.getExtValue()) +
      "); consider SmallVector<T, " + std::to_string(Val.getExtValue()) +
      "> or std::array to avoid heap allocation";

  Collector.addHint(makeHint(HintCategory::ContainerReserve, Impact::Low,
                             LoopDepth, CE->getBeginLoc(), Ctx, Msg,
                             "Use llvm::SmallVector<T, N> or std::array<T, N> "
                             "when the maximum size is small and known"));
}

// ---------------------------------------------------------------------------
// 2. VirtualInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkVirtualInLoop(const CallExpr *CE, ASTContext &Ctx,
                                       PerfHintCollector &Collector,
                                       unsigned LoopDepth) {
  if (LoopDepth == 0)
    return;

  const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE);
  if (!MCE)
    return;

  const CXXMethodDecl *MD = MCE->getMethodDecl();
  if (!MD || !MD->isVirtual())
    return;

  // Check that the call is on a pointer or reference (i.e., cannot be
  // devirtualized trivially by the compiler).
  const Expr *Obj = MCE->getImplicitObjectArgument();
  if (!Obj)
    return;

  QualType ObjTy = Obj->getType();
  // If the object is accessed through a pointer or reference, the compiler
  // generally cannot devirtualize.
  bool IsIndirect = ObjTy->isPointerType() || ObjTy->isReferenceType();
  // Also consider: the base expression itself might be a dereference.
  if (!IsIndirect) {
    if (const auto *UO = dyn_cast<UnaryOperator>(Obj->IgnoreImpCasts())) {
      if (UO->getOpcode() == UO_Deref)
        IsIndirect = true;
    }
  }
  if (!IsIndirect)
    return;

  std::string MethodName = MD->getQualifiedNameAsString();
  std::string Msg = "Virtual call to '" + MethodName +
                    "' inside loop (depth " + std::to_string(LoopDepth) +
                    "); indirect dispatch prevents inlining and "
                    "branch prediction is poor";

  Collector.addHint(makeHint(HintCategory::VirtualDevirt, Impact::High,
                             LoopDepth, CE->getBeginLoc(), Ctx, Msg,
                             "Consider devirtualization via CRTP pattern, "
                             "marking the class/method 'final', or using a "
                             "template parameter instead of runtime polymorphism"));
}

// ---------------------------------------------------------------------------
// 3. SharedPtrOverhead
// ---------------------------------------------------------------------------

void perfsanitizer::checkSharedPtrOverhead(const ParmVarDecl *PVD,
                                           ASTContext &Ctx,
                                           PerfHintCollector &Collector,
                                           unsigned LoopDepth) {
  if (!PVD)
    return;

  QualType T = PVD->getType();

  // We only care about pass-by-value shared_ptr (not reference or pointer).
  if (T->isReferenceType() || T->isPointerType())
    return;

  if (!typeNameContains(T, "shared_ptr"))
    return;

  std::string Msg =
      "std::shared_ptr parameter '" + PVD->getNameAsString() +
      "' is passed by value, causing atomic reference count "
      "increment/decrement on every call";

  Collector.addHint(makeHint(HintCategory::MoveSemantics, Impact::Medium,
                             LoopDepth, PVD->getLocation(), Ctx, Msg,
                             "Pass by const reference (const "
                             "std::shared_ptr<T>&) if ownership is not "
                             "transferred, or use std::unique_ptr<T> if "
                             "sole ownership is sufficient"));
}

// ---------------------------------------------------------------------------
// 4. MapToUnorderedMap
// ---------------------------------------------------------------------------

void perfsanitizer::checkMapToUnorderedMap(const VarDecl *VD, ASTContext &Ctx,
                                           PerfHintCollector &Collector,
                                           unsigned LoopDepth) {
  if (!VD)
    return;

  QualType T = VD->getType();
  if (!typeNameContains(T, "std::map"))
    return;
  // Avoid matching std::unordered_map or std::multimap.
  if (typeNameContains(T, "unordered_map") || typeNameContains(T, "multimap"))
    return;

  // Walk the enclosing function body looking for order-dependent operations
  // on this variable.  If we find any, do not suggest the change.
  // TODO: Walk the enclosing function body to check for order-dependent
  // operations. For now, we skip this check to avoid parent map API issues.
  // This means we may false-positive on maps that use lower_bound/upper_bound.

  std::string Msg =
      "std::map variable '" + VD->getNameAsString() +
      "' does not use order-dependent operations; O(log n) lookup "
      "on each access";

  Collector.addHint(makeHint(HintCategory::DataLayout, Impact::Medium,
                             LoopDepth, VD->getLocation(), Ctx, Msg,
                             "Consider std::unordered_map for O(1) average "
                             "lookup if iteration order is not required"));
}

// ---------------------------------------------------------------------------
// 5. EmptyCheck
// ---------------------------------------------------------------------------

void perfsanitizer::checkEmptyVsSize(const BinaryOperator *BO,
                                     ASTContext &Ctx,
                                     PerfHintCollector &Collector,
                                     unsigned LoopDepth) {
  if (!BO)
    return;

  // Match: .size() == 0, .size() != 0, 0 == .size(), 0 != .size()
  BinaryOperatorKind Op = BO->getOpcode();
  if (Op != BO_EQ && Op != BO_NE)
    return;

  const Expr *LHS = BO->getLHS()->IgnoreImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreImpCasts();

  // Figure out which side is the .size() call and which is the zero literal.
  const CXXMemberCallExpr *SizeCall = nullptr;
  const Expr *OtherSide = nullptr;

  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(LHS)) {
    SizeCall = MCE;
    OtherSide = RHS;
  } else if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(RHS)) {
    SizeCall = MCE;
    OtherSide = LHS;
  }

  if (!SizeCall)
    return;

  // Check that the method is named "size".
  const CXXMethodDecl *MD = SizeCall->getMethodDecl();
  if (!MD || MD->getName() != "size")
    return;

  // Check that the other side is integer literal 0.
  Expr::EvalResult Result;
  if (!OtherSide->EvaluateAsInt(Result, Ctx))
    return;
  if (Result.Val.getInt() != 0)
    return;

  std::string OpStr = (Op == BO_EQ) ? "== 0" : "!= 0";
  std::string Replacement = (Op == BO_EQ) ? ".empty()" : "!.empty()";

  std::string Msg = "Container .size() " + OpStr +
                    " comparison; .empty() is preferred and may be O(1) "
                    "for containers where .size() is O(n)";

  Collector.addHint(makeHint(HintCategory::StrengthReduction, Impact::Low,
                             LoopDepth, BO->getOperatorLoc(), Ctx, Msg,
                             "Replace .size() " + OpStr + " with " +
                                 Replacement));
}

// ---------------------------------------------------------------------------
// 6. PreIncrementPreference
// ---------------------------------------------------------------------------

void perfsanitizer::checkPreIncrement(const UnaryOperator *UO, ASTContext &Ctx,
                                      PerfHintCollector &Collector,
                                      unsigned LoopDepth) {
  if (LoopDepth == 0)
    return;

  if (!UO || UO->getOpcode() != UO_PostInc)
    return;

  // Only flag non-trivial types (iterators, complex objects).
  QualType T = UO->getSubExpr()->getType();
  if (T->isBuiltinType() || T->isPointerType())
    return; // primitive or raw pointer: compiler will optimise.

  if (!isNonTrivialType(T))
    return;

  std::string TypeStr = T.getAsString();
  std::string Msg =
      "Post-increment (i++) on non-trivial type '" + TypeStr +
      "' inside loop creates an unnecessary temporary copy";

  Collector.addHint(makeHint(HintCategory::StrengthReduction, Impact::Low,
                             LoopDepth, UO->getOperatorLoc(), Ctx, Msg,
                             "Use pre-increment (++i) to avoid the temporary "
                             "copy; the result is unused here"));
}

// ---------------------------------------------------------------------------
// 7. BranchFreePredicate
// ---------------------------------------------------------------------------

void perfsanitizer::checkBranchFreePredicate(const IfStmt *IS,
                                             ASTContext &Ctx,
                                             PerfHintCollector &Collector,
                                             unsigned LoopDepth) {
  if (LoopDepth == 0)
    return;

  if (!IS)
    return;

  // Must have both then and else branches.
  const Stmt *Then = IS->getThen();
  const Stmt *Else = IS->getElse();
  if (!Then || !Else)
    return;

  // The condition should be a simple comparison (binary relational/equality).
  const Expr *Cond = IS->getCond();
  if (!Cond)
    return;

  const auto *CondBO = dyn_cast<BinaryOperator>(Cond->IgnoreImpCasts());
  if (!CondBO)
    return;

  BinaryOperatorKind CondOp = CondBO->getOpcode();
  if (CondOp != BO_LT && CondOp != BO_GT && CondOp != BO_LE &&
      CondOp != BO_GE && CondOp != BO_EQ && CondOp != BO_NE)
    return;

  // Both branches should be "simple" — a single statement each.
  auto countStmts = [](const Stmt *S) -> unsigned {
    if (const auto *CS = dyn_cast<CompoundStmt>(S))
      return CS->size();
    return 1;
  };
  if (countStmts(Then) != 1 || countStmts(Else) != 1)
    return;

  std::string Msg =
      "Simple if/else with comparison predicate inside loop (depth " +
      std::to_string(LoopDepth) +
      "); branch misprediction may be costly on tight loops";

  Collector.addHint(
      makeHint(HintCategory::BranchlessSelect, Impact::Medium, LoopDepth,
               IS->getIfLoc(), Ctx, Msg,
               "Consider branchless alternative using arithmetic or "
               "conditional move, e.g. result += (cond) * value"));
}

// ---------------------------------------------------------------------------
// 8. MemcpyOpportunity
// ---------------------------------------------------------------------------

void perfsanitizer::checkMemcpyOpportunity(const ForStmt *FS, ASTContext &Ctx,
                                           PerfHintCollector &Collector,
                                           unsigned LoopDepth) {
  if (!FS || !FS->getBody())
    return;

  // Check if the loop body is a single byte-copy assignment.
  if (!isByteCopyBody(FS->getBody()))
    return;

  // Verify this looks like a counted loop: init, cond, inc all present.
  if (!FS->getInit() || !FS->getCond() || !FS->getInc())
    return;

  std::string Msg =
      "Byte-by-byte copy loop detected; compilers may not always "
      "recognise this as a memcpy idiom";

  Collector.addHint(makeHint(HintCategory::StrengthReduction, Impact::High,
                             LoopDepth, FS->getForLoc(), Ctx, Msg,
                             "Replace with std::memcpy, std::copy, or "
                             "std::memmove for optimised bulk copy"));
}

// ---------------------------------------------------------------------------
// 9. StaticLocalInit
// ---------------------------------------------------------------------------

void perfsanitizer::checkStaticLocalInit(const VarDecl *VD, ASTContext &Ctx,
                                         PerfHintCollector &Collector,
                                         unsigned LoopDepth) {
  if (!VD)
    return;

  if (!VD->isStaticLocal())
    return;

  // Only flag non-trivial initialisation (not constexpr, not POD-zero-init).
  if (VD->isConstexpr())
    return;

  const Expr *Init = VD->getInit();
  if (!Init)
    return;

  // If the initialiser is a constant expression the compiler can avoid the
  // guard variable.
  if (Init->isConstantInitializer(Ctx, /*ForRef=*/false))
    return;

  // Non-trivial init: the compiler inserts a thread-safe guard check on
  // every invocation of the enclosing function.
  std::string Msg =
      "Static local variable '" + VD->getNameAsString() +
      "' has non-trivial initialisation; a hidden guard variable "
      "check is executed on every function call";

  Collector.addHint(
      makeHint(HintCategory::LoopInvariant, Impact::Medium, LoopDepth,
               VD->getLocation(), Ctx, Msg,
               "Move to file/namespace scope, make the initialiser "
               "constexpr, or use a lazy-init pattern to avoid the "
               "per-call guard check"));
}

// ---------------------------------------------------------------------------
// 10. ExcessiveCopyInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkExcessiveCopy(const VarDecl *VD, ASTContext &Ctx,
                                       PerfHintCollector &Collector,
                                       unsigned LoopDepth) {
  if (LoopDepth == 0)
    return;

  if (!VD)
    return;

  // Only local, non-static variables.
  if (VD->isStaticLocal() || !VD->isLocalVarDecl())
    return;

  QualType T = VD->getType();

  // Check for well-known expensive types by name.
  bool IsExpensive = typeNameContains(T, "std::string") ||
                     typeNameContains(T, "basic_string") ||
                     typeNameContains(T, "std::vector") ||
                     typeNameContains(T, "std::map") ||
                     typeNameContains(T, "std::unordered_map") ||
                     typeNameContains(T, "std::set") ||
                     typeNameContains(T, "std::list") ||
                     typeNameContains(T, "std::deque");

  // Fallback: any non-trivially-copyable class is suspect.
  if (!IsExpensive)
    IsExpensive = isNonTrivialType(T);

  // But filter out trivially-destructible types (they're probably cheap).
  if (!IsExpensive)
    return;

  // Must have an initialiser or be default-constructed (i.e., it's being
  // constructed fresh each iteration).
  if (!VD->getInit() && !T->getAsCXXRecordDecl())
    return;

  std::string TypeStr = T.getAsString();
  std::string Msg =
      "Non-trivial variable '" + VD->getNameAsString() + "' of type '" +
      TypeStr +
      "' is constructed every loop iteration (depth " +
      std::to_string(LoopDepth) + "); this may cause repeated allocation";

  Collector.addHint(
      makeHint(HintCategory::LoopInvariant, Impact::High, LoopDepth,
               VD->getLocation(), Ctx, Msg,
               "Hoist the declaration outside the loop and call "
               ".clear() at the start of each iteration to reuse "
               "the existing allocation"));
}

// ---------------------------------------------------------------------------
// 11. UnusedInclude (forward-declaration opportunity)
// ---------------------------------------------------------------------------

void perfsanitizer::checkUnusedInclude(const FunctionDecl *FD, ASTContext &Ctx,
                                       PerfHintCollector &Collector,
                                       unsigned LoopDepth) {
  if (!FD || !FD->hasBody())
    return;

  for (const ParmVarDecl *PVD : FD->parameters()) {
    QualType T = PVD->getType();

    // Only consider pointer or reference parameters.
    if (!T->isPointerType() && !T->isReferenceType())
      continue;

    QualType Pointee = T->getPointeeType();
    if (Pointee.isNull())
      continue;

    // Must be a record (class/struct) type.
    const CXXRecordDecl *RD = Pointee->getAsCXXRecordDecl();
    if (!RD || !RD->hasDefinition())
      continue;

    // If the function body never accesses members of this type beyond passing
    // the pointer/reference through, a forward declaration would suffice.
    // Heuristic: check if the parameter is only used in call expressions
    // (passed to other functions) or simple comparisons, not member access.
    bool NeedsFullDef = false;
    const Stmt *Body = FD->getBody();

    // Walk the body looking for MemberExpr or arrow expressions on this param.
    std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
      if (!S || NeedsFullDef)
        return;
      if (const auto *ME = dyn_cast<MemberExpr>(S)) {
        if (const auto *DRE =
                dyn_cast<DeclRefExpr>(ME->getBase()->IgnoreImpCasts())) {
          if (DRE->getDecl() == PVD)
            NeedsFullDef = true;
        }
      }
      // CXXMemberCallExpr on the param also needs full def.
      if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(S)) {
        if (const auto *IOA = MCE->getImplicitObjectArgument()) {
          if (const auto *DRE =
                  dyn_cast<DeclRefExpr>(IOA->IgnoreImpCasts())) {
            if (DRE->getDecl() == PVD)
              NeedsFullDef = true;
          }
        }
      }
      // sizeof, alignof on the type needs full def.
      if (const auto *UETTE = dyn_cast<UnaryExprOrTypeTraitExpr>(S)) {
        if (UETTE->getTypeOfArgument()->getAsCXXRecordDecl() == RD)
          NeedsFullDef = true;
      }
      for (const Stmt *Child : S->children())
        Walk(Child);
    };
    Walk(Body);

    if (NeedsFullDef)
      continue;

    std::string Msg =
        "Parameter '" + PVD->getNameAsString() + "' of type '" +
        T.getAsString() +
        "' only uses the type opaquely; the full class definition "
        "may not be needed in this translation unit";

    Collector.addHint(makeHint(HintCategory::UnusedInclude, Impact::Low,
                               LoopDepth, PVD->getLocation(), Ctx, Msg,
                               "Consider using a forward declaration instead "
                               "of #including the full header to reduce "
                               "compilation time"));
  }
}

// ---------------------------------------------------------------------------
// 12. SmallFunctionNotInline
// ---------------------------------------------------------------------------

void perfsanitizer::checkSmallFunctionNotInline(const FunctionDecl *FD,
                                                ASTContext &Ctx,
                                                PerfHintCollector &Collector) {
  if (!FD || !FD->hasBody())
    return;

  // Only flag functions in .cpp files (not headers).
  const SourceManager &SM = Ctx.getSourceManager();
  SourceLocation Loc = FD->getLocation();
  if (Loc.isInvalid())
    return;

  llvm::StringRef Filename = SM.getFilename(SM.getSpellingLoc(Loc));
  if (Filename.empty())
    return;

  // Consider it a header if it ends with .h, .hpp, .hxx, or .inc.
  if (Filename.ends_with(".h") || Filename.ends_with(".hpp") ||
      Filename.ends_with(".hxx") || Filename.ends_with(".inc"))
    return;

  // Skip if already inline, constexpr, or a template.
  if (FD->isInlined() || FD->isConstexpr() || FD->isTemplateInstantiation())
    return;
  if (FD->getTemplatedKind() != FunctionDecl::TK_NonTemplate)
    return;

  // Skip main().
  if (FD->isMain())
    return;

  // Count statements in the body.
  const auto *Body = dyn_cast<CompoundStmt>(FD->getBody());
  if (!Body)
    return;

  if (Body->size() > 3)
    return;

  std::string Msg =
      "Function '" + FD->getNameAsString() + "' has only " +
      std::to_string(Body->size()) +
      " statement(s) but is not marked inline; call overhead may "
      "exceed the function body cost";

  Collector.addHint(
      makeHint(HintCategory::SmallFunctionNotInline, Impact::Low,
               /*LoopDepth=*/0, FD->getLocation(), Ctx, Msg,
               "Consider marking this function 'inline' or moving it "
               "to the header so the compiler can inline it at call sites"));
}

// ---------------------------------------------------------------------------
// 13. UnnecessaryCopy
// ---------------------------------------------------------------------------

void perfsanitizer::checkUnnecessaryCopy(const FunctionDecl *FD,
                                         ASTContext &Ctx,
                                         PerfHintCollector &Collector) {
  if (!FD)
    return;

  for (const ParmVarDecl *PVD : FD->parameters()) {
    QualType T = PVD->getType();

    // Only flag pass-by-value (not reference, not pointer).
    if (T->isReferenceType() || T->isPointerType())
      continue;

    // Skip fundamental/builtin types.
    if (T->isBuiltinType() || T->isEnumeralType())
      continue;

    // Check if const-qualified value param — still a copy.
    bool IsConst = T.isConstQualified();

    // Check for well-known expensive types.
    bool IsExpensiveName = typeNameContains(T, "basic_string") ||
                           typeNameContains(T, "std::string") ||
                           typeNameContains(T, "std::vector") ||
                           typeNameContains(T, "std::map") ||
                           typeNameContains(T, "std::unordered_map") ||
                           typeNameContains(T, "std::set") ||
                           typeNameContains(T, "std::list") ||
                           typeNameContains(T, "std::deque");

    // Check size > 64 bytes if we can determine it.
    bool IsLargeType = false;
    if (const RecordType *RT = T->getAs<RecordType>()) {
      const RecordDecl *RD = RT->getDecl();
      if (RD->isCompleteDefinition()) {
        const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
        if (Layout.getSize().getQuantity() > 64)
          IsLargeType = true;
      }
    }

    // Check for non-trivial copy constructor.
    bool HasNonTrivialCopy = false;
    if (const auto *RD = T->getAsCXXRecordDecl()) {
      if (RD->hasDefinition() && !RD->hasTrivialCopyConstructor())
        HasNonTrivialCopy = true;
    }

    if (!IsExpensiveName && !IsLargeType && !HasNonTrivialCopy)
      continue;

    std::string Reason;
    if (IsExpensiveName)
      Reason = "has a non-trivial copy constructor";
    else if (IsLargeType)
      Reason = "is larger than 64 bytes";
    else
      Reason = "has a non-trivial copy constructor";

    std::string Msg =
        "Parameter '" + PVD->getNameAsString() + "' of type '" +
        T.getAsString() + "' is passed by value but " + Reason +
        "; this creates an expensive copy on every call";

    std::string Sug = IsConst
        ? "Change to 'const " + T.getUnqualifiedType().getAsString() + " &'"
        : "Change to 'const " + T.getAsString() + " &' if the parameter "
          "is not modified, or use move semantics if ownership is transferred";

    Collector.addHint(makeHint(HintCategory::UnnecessaryCopy, Impact::Medium,
                               /*LoopDepth=*/0, PVD->getLocation(), Ctx, Msg,
                               Sug));
  }
}

// ---------------------------------------------------------------------------
// 14. RedundantComputation
// ---------------------------------------------------------------------------

namespace {

/// Check if an expression references the given loop induction variable.
bool referencesVar(const Stmt *S, const VarDecl *Var) {
  if (!S)
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S)) {
    if (DRE->getDecl() == Var)
      return true;
  }
  for (const Stmt *Child : S->children()) {
    if (referencesVar(Child, Var))
      return true;
  }
  return false;
}

/// Try to extract the loop induction variable from a ForStmt's init.
const VarDecl *getInductionVar(const ForStmt *FS) {
  if (!FS->getInit())
    return nullptr;
  // int i = 0;
  if (const auto *DS = dyn_cast<DeclStmt>(FS->getInit())) {
    if (DS->isSingleDecl()) {
      if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl()))
        return VD;
    }
  }
  // i = 0;
  if (const auto *BO = dyn_cast<BinaryOperator>(FS->getInit())) {
    if (BO->getOpcode() == BO_Assign) {
      if (const auto *DRE = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreImpCasts()))
        return dyn_cast<VarDecl>(DRE->getDecl());
    }
  }
  return nullptr;
}

/// Check if a CallExpr is a call to strlen() or string::size()/string::length()
/// that does NOT reference the given induction variable.
bool isLoopInvariantSizeCall(const CallExpr *CE, const VarDecl *IndVar) {
  if (!CE)
    return false;

  // Check for strlen(arg).
  if (const FunctionDecl *Callee = CE->getDirectCallee()) {
    if (Callee->getName() == "strlen" || Callee->getName() == "wcslen") {
      // Check that args don't reference induction var.
      for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
        if (IndVar && referencesVar(CE->getArg(I), IndVar))
          return false;
      }
      return true;
    }
  }

  // Check for .size() or .length() on a string/container.
  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE)) {
    if (const CXXMethodDecl *MD = MCE->getMethodDecl()) {
      llvm::StringRef Name = MD->getName();
      if (Name == "size" || Name == "length") {
        // The object should not reference the induction variable.
        if (const Expr *Obj = MCE->getImplicitObjectArgument()) {
          if (IndVar && referencesVar(Obj, IndVar))
            return false;
          return true;
        }
      }
    }
  }

  return false;
}

} // anonymous namespace

void perfsanitizer::checkRedundantComputation(const ForStmt *FS,
                                              ASTContext &Ctx,
                                              PerfHintCollector &Collector,
                                              unsigned LoopDepth) {
  if (!FS || !FS->getBody())
    return;

  const VarDecl *IndVar = getInductionVar(FS);

  // Also check the condition for redundant calls like i < strlen(s).
  auto CheckExpr = [&](const Stmt *S) {
    if (!S)
      return;
    // Walk looking for call expressions.
    std::function<void(const Stmt *)> Walk = [&](const Stmt *Node) {
      if (!Node)
        return;
      if (const auto *CE = dyn_cast<CallExpr>(Node)) {
        if (isLoopInvariantSizeCall(CE, IndVar)) {
          std::string CallName;
          if (const FunctionDecl *FCallee = CE->getDirectCallee())
            CallName = FCallee->getNameAsString();
          else if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE))
            if (const CXXMethodDecl *MD = MCE->getMethodDecl())
              CallName = MD->getQualifiedNameAsString();

          std::string Msg =
              "Call to '" + CallName +
              "' is loop-invariant but recomputed every iteration "
              "(loop depth " +
              std::to_string(LoopDepth) + ")";

          Collector.addHint(
              makeHint(HintCategory::RedundantComputation, Impact::High,
                       LoopDepth, CE->getBeginLoc(), Ctx, Msg,
                       "Hoist the result into a variable before the loop"));
        }
      }
      for (const Stmt *Child : Node->children())
        Walk(Child);
    };
    Walk(S);
  };

  // Check the loop condition.
  CheckExpr(FS->getCond());
  // Check the loop body.
  CheckExpr(FS->getBody());
}

// ---------------------------------------------------------------------------
// 15. TightLoopAllocation
// ---------------------------------------------------------------------------

void perfsanitizer::checkTightLoopAllocation(const Stmt *LoopBody,
                                             ASTContext &Ctx,
                                             PerfHintCollector &Collector,
                                             unsigned LoopDepth) {
  if (!LoopBody || LoopDepth == 0)
    return;

  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;

    // Detect C++ new expressions.
    if (const auto *NE = dyn_cast<CXXNewExpr>(S)) {
      Collector.addHint(makeHint(
          HintCategory::TightLoopAllocation, Impact::Critical, LoopDepth,
          NE->getBeginLoc(), Ctx,
          "Heap allocation via 'new' inside loop (depth " +
              std::to_string(LoopDepth) +
              "); each iteration triggers a malloc",
          "Pre-allocate outside the loop, use an object pool, or "
          "use stack allocation if the size is bounded"));
      return; // Don't recurse into children of this new-expr.
    }

    // Detect function calls: malloc, calloc, make_unique, make_shared.
    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      const FunctionDecl *Callee = CE->getDirectCallee();
      if (Callee) {
        llvm::StringRef Name = Callee->getName();
        bool IsAlloc = (Name == "malloc" || Name == "calloc" ||
                        Name == "realloc" || Name == "aligned_alloc");
        if (!IsAlloc) {
          // Check qualified name for std::make_unique, std::make_shared.
          std::string QName = Callee->getQualifiedNameAsString();
          IsAlloc = (llvm::StringRef(QName).contains("make_unique") ||
                     llvm::StringRef(QName).contains("make_shared"));
        }
        if (IsAlloc) {
          Collector.addHint(makeHint(
              HintCategory::TightLoopAllocation, Impact::Critical, LoopDepth,
              CE->getBeginLoc(), Ctx,
              "Heap allocation via '" + Callee->getNameAsString() +
                  "' inside loop (depth " + std::to_string(LoopDepth) +
                  "); allocation in hot loops is a major performance issue",
              "Pre-allocate outside the loop, reuse buffers, or use "
              "stack-based allocation"));
        }
      }
    }

    for (const Stmt *Child : S->children())
      Walk(Child);
  };

  Walk(LoopBody);
}

// ---------------------------------------------------------------------------
// 16. BoolBranching
// ---------------------------------------------------------------------------

namespace {

/// Return true if \p S is `return true;` or `return false;`, and set \p Value.
bool isReturnBool(const Stmt *S, bool &Value) {
  const Stmt *Inner = S;
  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    if (CS->size() != 1)
      return false;
    Inner = *CS->body_begin();
  }
  const auto *RS = dyn_cast<ReturnStmt>(Inner);
  if (!RS || !RS->getRetValue())
    return false;
  const Expr *Ret = RS->getRetValue()->IgnoreImpCasts();
  if (const auto *BL = dyn_cast<CXXBoolLiteralExpr>(Ret)) {
    Value = BL->getValue();
    return true;
  }
  // Also handle integer literal 0 or 1 in C-style code.
  if (const auto *IL = dyn_cast<IntegerLiteral>(Ret)) {
    uint64_t V = IL->getValue().getZExtValue();
    if (V == 0 || V == 1) {
      Value = (V == 1);
      return true;
    }
  }
  return false;
}

} // anonymous namespace

void perfsanitizer::checkBoolBranching(const IfStmt *IS, ASTContext &Ctx,
                                       PerfHintCollector &Collector) {
  if (!IS || !IS->getThen())
    return;

  bool ThenVal = false;
  if (!isReturnBool(IS->getThen(), ThenVal))
    return;

  // Pattern 1: if (x) return true; else return false;
  if (IS->getElse()) {
    bool ElseVal = false;
    if (!isReturnBool(IS->getElse(), ElseVal))
      return;
    if (ThenVal == ElseVal)
      return; // Both return the same value — weird but not our check.

    std::string Replacement =
        ThenVal ? "return <condition>;" : "return !<condition>;";

    Collector.addHint(
        makeHint(HintCategory::BoolBranching, Impact::Low, /*LoopDepth=*/0,
                 IS->getIfLoc(), Ctx,
                 "Redundant bool branching: 'if (x) return true; else "
                 "return false;' can be simplified",
                 "Replace with '" + Replacement + "'"));
    return;
  }

  // Pattern 2: if (x) return true; return false; (no else, next stmt is
  // return false). We check if there's no else and the then-branch returns
  // true — the caller would need to verify the next statement, but we flag
  // the pattern conservatively.
  if (ThenVal) {
    Collector.addHint(
        makeHint(HintCategory::BoolBranching, Impact::Low, /*LoopDepth=*/0,
                 IS->getIfLoc(), Ctx,
                 "Possible redundant bool branching: 'if (x) return true;' "
                 "followed by 'return false;' can be simplified to "
                 "'return x;'",
                 "Replace with 'return <condition>;'"));
  }
}

// ---------------------------------------------------------------------------
// 17. SortAlgorithm
// ---------------------------------------------------------------------------

namespace {

/// Check if a statement contains a swap-like pattern (std::swap, manual
/// temp-based swap, or direct member swap).
bool containsSwap(const Stmt *S) {
  if (!S)
    return false;
  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
      llvm::StringRef Name = Callee->getName();
      if (Name == "swap" || Name == "iter_swap")
        return true;
      std::string QName = Callee->getQualifiedNameAsString();
      if (llvm::StringRef(QName).contains("swap"))
        return true;
    }
  }
  // Also detect manual swap pattern: temp = a; a = b; b = temp;
  // We simplify this to: look for 3 consecutive assignments in a compound.
  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    unsigned AssignCount = 0;
    for (const Stmt *Child : CS->body()) {
      if (const auto *BO = dyn_cast<BinaryOperator>(Child)) {
        if (BO->getOpcode() == BO_Assign)
          AssignCount++;
        else
          AssignCount = 0;
      } else {
        AssignCount = 0;
      }
      if (AssignCount >= 3)
        return true;
    }
  }
  for (const Stmt *Child : S->children()) {
    if (containsSwap(Child))
      return true;
  }
  return false;
}

/// Check if a ForStmt's body contains a nested ForStmt.
const ForStmt *findNestedFor(const Stmt *S) {
  if (!S)
    return nullptr;
  for (const Stmt *Child : S->children()) {
    if (const auto *Inner = dyn_cast<ForStmt>(Child))
      return Inner;
    if (const ForStmt *Found = findNestedFor(Child))
      return Found;
  }
  return nullptr;
}

} // anonymous namespace

void perfsanitizer::checkSortAlgorithm(const ForStmt *FS, ASTContext &Ctx,
                                       PerfHintCollector &Collector,
                                       unsigned LoopDepth) {
  if (!FS || !FS->getBody())
    return;

  // Look for a nested for loop.
  const ForStmt *Inner = findNestedFor(FS->getBody());
  if (!Inner || !Inner->getBody())
    return;

  // The inner loop body (or the space between the two loops) should contain
  // a swap or swap-like pattern.
  if (!containsSwap(Inner->getBody()) && !containsSwap(FS->getBody()))
    return;

  std::string Msg =
      "Nested loop with swap detected — this resembles a manual O(n^2) "
      "sort (bubble sort or selection sort)";

  Collector.addHint(
      makeHint(HintCategory::SortAlgorithm, Impact::High, LoopDepth,
               FS->getForLoc(), Ctx, Msg,
               "Replace with std::sort (O(n log n)) or std::partial_sort "
               "if only a subset is needed"));
}

// ---------------------------------------------------------------------------
// 18. PowerOfTwo
// ---------------------------------------------------------------------------

namespace {

/// Return true if the value is a positive power of 2.
bool isPowerOfTwo(uint64_t V) { return V > 0 && (V & (V - 1)) == 0; }

/// Compute floor(log2(V)) for a power of 2.
unsigned log2OfPow2(uint64_t V) {
  unsigned Log = 0;
  while (V > 1) {
    V >>= 1;
    ++Log;
  }
  return Log;
}

} // anonymous namespace

void perfsanitizer::checkPowerOfTwo(const BinaryOperator *BO, ASTContext &Ctx,
                                    PerfHintCollector &Collector,
                                    unsigned LoopDepth) {
  if (!BO)
    return;

  BinaryOperatorKind Op = BO->getOpcode();
  if (Op != BO_Rem && Op != BO_Div)
    return;

  // RHS must be a positive integer literal that is a power of 2 (and > 1).
  const Expr *RHS = BO->getRHS()->IgnoreImpCasts();
  Expr::EvalResult Result;
  if (!RHS->EvaluateAsInt(Result, Ctx))
    return;

  llvm::APSInt Val = Result.Val.getInt();
  if (Val.isNegative() || Val.getExtValue() <= 1)
    return;

  uint64_t UVal = Val.getZExtValue();
  if (!isPowerOfTwo(UVal))
    return;

  // LHS should be an unsigned or integer type (we still flag signed, but
  // note the caveat).
  QualType LHSTy = BO->getLHS()->getType();
  bool IsSigned = LHSTy->isSignedIntegerType();

  if (Op == BO_Rem) {
    std::string Msg =
        "Modulo by power-of-two constant (" + std::to_string(UVal) +
        ") can be replaced with bitwise AND";
    std::string Sug = "Replace 'x % " + std::to_string(UVal) + "' with 'x & " +
                      std::to_string(UVal - 1) + "'";
    if (IsSigned)
      Sug += " (ensure x is non-negative or use unsigned type)";
    Collector.addHint(makeHint(HintCategory::PowerOfTwo, Impact::Medium,
                               LoopDepth, BO->getOperatorLoc(), Ctx, Msg, Sug));
  } else {
    // BO_Div
    unsigned Shift = log2OfPow2(UVal);
    std::string Msg =
        "Division by power-of-two constant (" + std::to_string(UVal) +
        ") can be replaced with right shift";
    std::string Sug = "Replace 'x / " + std::to_string(UVal) + "' with 'x >> " +
                      std::to_string(Shift) + "'";
    if (IsSigned)
      Sug += " (ensure x is non-negative or use unsigned type)";
    Collector.addHint(makeHint(HintCategory::PowerOfTwo, Impact::Medium,
                               LoopDepth, BO->getOperatorLoc(), Ctx, Msg, Sug));
  }
}

// ---------------------------------------------------------------------------
// 19. ExceptionInDestructor
// ---------------------------------------------------------------------------

namespace {

/// Recursively search for CXXThrowExpr within a statement tree.
bool containsThrow(const Stmt *S) {
  if (!S)
    return false;
  if (isa<CXXThrowExpr>(S))
    return true;
  for (const Stmt *Child : S->children()) {
    if (containsThrow(Child))
      return true;
  }
  return false;
}

} // anonymous namespace

void perfsanitizer::checkExceptionInDestructor(const CXXDestructorDecl *DD,
                                               ASTContext &Ctx,
                                               PerfHintCollector &Collector) {
  if (!DD || !DD->hasBody())
    return;

  const Stmt *Body = DD->getBody();

  // Walk the body looking for throw expressions.
  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;
    if (const auto *TE = dyn_cast<CXXThrowExpr>(S)) {
      std::string Msg =
          "Throw expression inside destructor '" +
          DD->getQualifiedNameAsString() +
          "'; this can cause std::terminate if another exception is "
          "already active, and prevents noexcept optimizations";

      Collector.addHint(makeHint(
          HintCategory::ExceptionInDestructor, Impact::High,
          /*LoopDepth=*/0, TE->getThrowLoc(), Ctx, Msg,
          "Remove the throw from the destructor; use error codes, "
          "logging, or a separate cleanup method instead"));
      return;
    }
    // Don't recurse into nested try/catch — throw inside catch in a
    // destructor is still dangerous.
    for (const Stmt *Child : S->children())
      Walk(Child);
  };

  Walk(Body);
}

// ---------------------------------------------------------------------------
// 20. VectorBoolAvoid
// ---------------------------------------------------------------------------

void perfsanitizer::checkVectorBoolAvoid(const VarDecl *VD, ASTContext &Ctx,
                                         PerfHintCollector &Collector) {
  if (!VD)
    return;

  QualType T = VD->getType();
  if (!typeNameContains(T, "vector<bool>"))
    return;

  std::string Msg =
      "std::vector<bool> variable '" + VD->getNameAsString() +
      "' uses a specialization that stores bits packed; this causes "
      "poor performance due to bit manipulation overhead and inability "
      "to take addresses of elements";

  Collector.addHint(
      makeHint(HintCategory::VectorBoolAvoid, Impact::Medium,
               /*LoopDepth=*/0, VD->getLocation(), Ctx, Msg,
               "Use std::vector<char>, std::vector<uint8_t>, or "
               "std::bitset<N> (if size is fixed) instead"));
}

// ---------------------------------------------------------------------------
// 21. MutexInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkMutexInLoop(const Stmt *LoopBody, ASTContext &Ctx,
                                     PerfHintCollector &Collector,
                                     unsigned LoopDepth) {
  if (!LoopBody || LoopDepth == 0)
    return;

  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;

    // Check variable declarations for lock_guard / unique_lock / scoped_lock.
    if (const auto *DS = dyn_cast<DeclStmt>(S)) {
      for (const Decl *D : DS->decls()) {
        if (const auto *VD = dyn_cast<VarDecl>(D)) {
          QualType T = VD->getType();
          if (typeNameContains(T, "lock_guard") ||
              typeNameContains(T, "unique_lock") ||
              typeNameContains(T, "scoped_lock")) {
            Collector.addHint(makeHint(
                HintCategory::MutexInLoop, Impact::High, LoopDepth,
                VD->getLocation(), Ctx,
                "Lock guard '" + VD->getNameAsString() +
                    "' acquired inside loop (depth " +
                    std::to_string(LoopDepth) +
                    "); lock/unlock overhead on every iteration",
                "Move the lock outside the loop, use lock-free "
                "data structures, or batch operations to reduce "
                "lock contention"));
          }
        }
      }
    }

    // Check for explicit .lock() calls on mutex-like types.
    if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(S)) {
      if (const CXXMethodDecl *MD = MCE->getMethodDecl()) {
        if (MD->getName() == "lock") {
          QualType ObjTy;
          if (const Expr *Obj = MCE->getImplicitObjectArgument())
            ObjTy = Obj->getType();
          if (typeNameContains(ObjTy, "mutex")) {
            Collector.addHint(makeHint(
                HintCategory::MutexInLoop, Impact::High, LoopDepth,
                MCE->getBeginLoc(), Ctx,
                "Explicit mutex.lock() call inside loop (depth " +
                    std::to_string(LoopDepth) +
                    "); lock acquisition on every iteration is expensive",
                "Move the lock outside the loop, or use lock-free "
                "algorithms / batching to reduce synchronization cost"));
          }
        }
      }
    }

    for (const Stmt *Child : S->children())
      Walk(Child);
  };

  Walk(LoopBody);
}

// ---------------------------------------------------------------------------
// 22. StdFunctionOverhead (FunctionDecl overload — checks parameters)
// ---------------------------------------------------------------------------

void perfsanitizer::checkStdFunctionOverhead(const FunctionDecl *FD,
                                             ASTContext &Ctx,
                                             PerfHintCollector &Collector,
                                             unsigned LoopDepth) {
  if (!FD)
    return;

  for (const ParmVarDecl *PVD : FD->parameters()) {
    QualType T = PVD->getType().getNonReferenceType();
    if (!typeNameContains(T, "function<"))
      continue;

    std::string Msg =
        "Parameter '" + PVD->getNameAsString() +
        "' uses std::function which has type-erasure overhead "
        "(heap allocation for large callables, virtual dispatch)";

    Collector.addHint(
        makeHint(HintCategory::StdFunctionOverhead, Impact::Medium, LoopDepth,
                 PVD->getLocation(), Ctx, Msg,
                 "Consider using a template parameter (auto/Callable) "
                 "or a function pointer if the callable type is known "
                 "at compile time"));
  }
}

// ---------------------------------------------------------------------------
// 23. StdFunctionOverhead (VarDecl overload)
// ---------------------------------------------------------------------------

void perfsanitizer::checkStdFunctionOverhead(const VarDecl *VD,
                                             ASTContext &Ctx,
                                             PerfHintCollector &Collector,
                                             unsigned LoopDepth) {
  if (!VD)
    return;

  QualType T = VD->getType();
  if (!typeNameContains(T, "function<"))
    return;

  std::string Msg =
      "Variable '" + VD->getNameAsString() +
      "' uses std::function which incurs type-erasure overhead "
      "(potential heap allocation, indirect call)";

  Collector.addHint(
      makeHint(HintCategory::StdFunctionOverhead, Impact::Medium, LoopDepth,
               VD->getLocation(), Ctx, Msg,
               "Use auto with a lambda, a function pointer, or a "
               "template parameter to avoid std::function overhead"));
}

// ---------------------------------------------------------------------------
// 24. EmptyLoopBody
// ---------------------------------------------------------------------------

void perfsanitizer::checkEmptyLoopBody(const ForStmt *FS, ASTContext &Ctx,
                                       PerfHintCollector &Collector,
                                       unsigned LoopDepth) {
  if (!FS)
    return;

  const Stmt *Body = FS->getBody();
  bool IsEmpty = false;
  if (!Body) {
    IsEmpty = true;
  } else if (const auto *CS = dyn_cast<CompoundStmt>(Body)) {
    if (CS->body_empty())
      IsEmpty = true;
  } else if (isa<NullStmt>(Body)) {
    IsEmpty = true;
  }

  if (!IsEmpty)
    return;

  Collector.addHint(
      makeHint(HintCategory::EmptyLoopBody, Impact::Medium, LoopDepth,
               FS->getForLoc(), Ctx,
               "For-loop has an empty body — likely a bug or busy-wait",
               "If this is intentional, add a comment. Otherwise, add the "
               "intended loop body or remove the loop"));
}

// ---------------------------------------------------------------------------
// 25. DuplicateCondition
// ---------------------------------------------------------------------------

void perfsanitizer::checkDuplicateCondition(const IfStmt *IS, ASTContext &Ctx,
                                            PerfHintCollector &Collector,
                                            unsigned LoopDepth) {
  if (!IS)
    return;

  // Look for else-if chain: if (a) ... else if (a) ...
  const Stmt *ElseBranch = IS->getElse();
  if (!ElseBranch)
    return;
  const auto *ElseIf = dyn_cast<IfStmt>(ElseBranch);
  if (!ElseIf)
    return;

  const Expr *Cond1 = IS->getCond();
  const Expr *Cond2 = ElseIf->getCond();
  if (!Cond1 || !Cond2)
    return;

  const SourceManager &SM = Ctx.getSourceManager();
  const LangOptions &LO = Ctx.getLangOpts();

  CharSourceRange R1 = CharSourceRange::getTokenRange(Cond1->getSourceRange());
  CharSourceRange R2 = CharSourceRange::getTokenRange(Cond2->getSourceRange());

  StringRef Text1 = Lexer::getSourceText(R1, SM, LO);
  StringRef Text2 = Lexer::getSourceText(R2, SM, LO);

  if (Text1.empty() || Text2.empty() || Text1 != Text2)
    return;

  Collector.addHint(makeHint(
      HintCategory::DuplicateCondition, Impact::Medium, LoopDepth,
      ElseIf->getIfLoc(), Ctx,
      "Duplicate condition '" + Text1.str() +
          "' in if/else-if chain — the else-if branch is unreachable",
      "Remove the duplicate branch or fix the condition"));
}

// ---------------------------------------------------------------------------
// 26. StringConcatInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkStringConcatInLoop(const Stmt *LoopBody,
                                            ASTContext &Ctx,
                                            PerfHintCollector &Collector,
                                            unsigned LoopDepth) {
  if (!LoopBody || LoopDepth == 0)
    return;

  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;
    if (const auto *OpCall = dyn_cast<CXXOperatorCallExpr>(S)) {
      if (OpCall->getOperator() == OO_PlusEqual && OpCall->getNumArgs() >= 1) {
        QualType T = OpCall->getArg(0)->getType();
        if (typeNameContains(T, "basic_string")) {
          Collector.addHint(makeHint(
              HintCategory::StringConcatInLoop, Impact::High, LoopDepth,
              OpCall->getOperatorLoc(), Ctx,
              "operator+= on std::string inside loop (depth " +
                  std::to_string(LoopDepth) +
                  "); may cause repeated reallocations",
              "Use std::string::reserve() before the loop, or build with "
              "std::ostringstream / fmt::format for large concatenations"));
        }
      }
    }
    for (const Stmt *Child : S->children())
      Walk(Child);
  };
  Walk(LoopBody);
}

// ---------------------------------------------------------------------------
// 27. RegexInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkRegexInLoop(const Stmt *LoopBody, ASTContext &Ctx,
                                     PerfHintCollector &Collector,
                                     unsigned LoopDepth) {
  if (!LoopBody || LoopDepth == 0)
    return;

  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;
    if (const auto *CE = dyn_cast<CXXConstructExpr>(S)) {
      QualType T = CE->getType();
      if (typeNameContains(T, "basic_regex") || typeNameContains(T, "std::regex")) {
        Collector.addHint(makeHint(
            HintCategory::RegexInLoop, Impact::Critical, LoopDepth,
            CE->getBeginLoc(), Ctx,
            "std::regex constructed inside loop (depth " +
                std::to_string(LoopDepth) +
                "); regex compilation is extremely expensive",
            "Hoist the std::regex construction outside the loop — compile "
            "the pattern once and reuse it"));
      }
    }
    for (const Stmt *Child : S->children())
      Walk(Child);
  };
  Walk(LoopBody);
}

// ---------------------------------------------------------------------------
// 28. DynamicCastInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkDynamicCastInLoop(const Stmt *LoopBody,
                                           ASTContext &Ctx,
                                           PerfHintCollector &Collector,
                                           unsigned LoopDepth) {
  if (!LoopBody || LoopDepth == 0)
    return;

  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;
    if (const auto *DC = dyn_cast<CXXDynamicCastExpr>(S)) {
      Collector.addHint(makeHint(
          HintCategory::DynamicCastInLoop, Impact::High, LoopDepth,
          DC->getBeginLoc(), Ctx,
          "dynamic_cast inside loop (depth " + std::to_string(LoopDepth) +
              "); RTTI lookup is expensive in tight loops",
          "Consider using static_cast if the type is known, or redesign "
          "with virtual methods / std::variant to avoid RTTI"));
    }
    for (const Stmt *Child : S->children())
      Walk(Child);
  };
  Walk(LoopBody);
}

// ---------------------------------------------------------------------------
// 29. VirtualDtorMissing
// ---------------------------------------------------------------------------

void perfsanitizer::checkVirtualDtorMissing(const CXXRecordDecl *RD,
                                            ASTContext &Ctx,
                                            PerfHintCollector &Collector,
                                            unsigned LoopDepth) {
  if (!RD || !RD->isCompleteDefinition() || !RD->isPolymorphic())
    return;

  // Check if any method is virtual.
  bool HasVirtualMethod = false;
  for (const auto *M : RD->methods()) {
    if (M->isVirtual() && !isa<CXXDestructorDecl>(M)) {
      HasVirtualMethod = true;
      break;
    }
  }
  if (!HasVirtualMethod)
    return;

  // Check if destructor is virtual.
  if (const auto *Dtor = RD->getDestructor()) {
    if (Dtor->isVirtual())
      return;
  }

  Collector.addHint(makeHint(
      HintCategory::VirtualDtorMissing, Impact::High, LoopDepth,
      RD->getLocation(), Ctx,
      "Class '" + RD->getNameAsString() +
          "' has virtual methods but a non-virtual destructor — "
          "deleting through a base pointer is undefined behavior",
      "Declare the destructor virtual: 'virtual ~" +
          RD->getNameAsString() + "() = default;'"));
}

// ---------------------------------------------------------------------------
// 30. CopyInRangeFor
// ---------------------------------------------------------------------------

void perfsanitizer::checkCopyInRangeFor(const CXXForRangeStmt *S,
                                        ASTContext &Ctx,
                                        PerfHintCollector &Collector,
                                        unsigned LoopDepth) {
  if (!S)
    return;

  const VarDecl *LoopVar = S->getLoopVariable();
  if (!LoopVar)
    return;

  QualType T = LoopVar->getType();
  // If it's already a reference, no copy.
  if (T->isReferenceType())
    return;

  // Check if the element type is non-trivial or large.
  bool ShouldFlag = false;
  std::string Reason;

  if (isNonTrivialType(T)) {
    ShouldFlag = true;
    Reason = "has a non-trivial copy constructor";
  } else if (typeNameContains(T, "basic_string") ||
             typeNameContains(T, "vector") ||
             typeNameContains(T, "map") ||
             typeNameContains(T, "set")) {
    ShouldFlag = true;
    Reason = "is a standard container type";
  } else if (T->isRecordType()) {
    if (const RecordType *RT = T->getAs<RecordType>()) {
      const RecordDecl *RecD = RT->getDecl();
      if (RecD->isCompleteDefinition()) {
        uint64_t Size = Ctx.getTypeSize(T) / 8;
        if (Size > 64) {
          ShouldFlag = true;
          Reason = "is " + std::to_string(Size) + " bytes";
        }
      }
    }
  }

  if (!ShouldFlag)
    return;

  Collector.addHint(makeHint(
      HintCategory::CopyInRangeFor, Impact::Medium, LoopDepth,
      LoopVar->getLocation(), Ctx,
      "Range-for loop variable '" + LoopVar->getNameAsString() +
          "' copies each element (type " + Reason +
          "); this creates an expensive copy every iteration",
      "Use 'const auto&' or 'auto&' to avoid copying: "
      "'for (const auto& " +
          LoopVar->getNameAsString() + " : ...)'"));
}

// ---------------------------------------------------------------------------
// 31. ThrowInNoexcept
// ---------------------------------------------------------------------------

void perfsanitizer::checkThrowInNoexcept(const CXXThrowExpr *TE,
                                         const FunctionDecl *EnclosingFD,
                                         ASTContext &Ctx,
                                         PerfHintCollector &Collector) {
  if (!TE || !EnclosingFD)
    return;

  auto *FPT = EnclosingFD->getType()->getAs<FunctionProtoType>();
  if (!FPT)
    return;

  if (!isNoexceptExceptionSpec(FPT->getExceptionSpecType()))
    return;

  Collector.addHint(makeHint(
      HintCategory::ThrowInNoexcept, Impact::High, /*LoopDepth=*/0,
      TE->getThrowLoc(), Ctx,
      "throw expression inside noexcept function '" +
          EnclosingFD->getNameAsString() +
          "' — this will call std::terminate at runtime",
      "Remove the throw, use a different error reporting mechanism, "
      "or remove noexcept from the function if it can legitimately throw"));
}

// ---------------------------------------------------------------------------
// 32. GlobalVarInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkGlobalVarInLoop(const Stmt *LoopBody,
                                         ASTContext &Ctx,
                                         PerfHintCollector &Collector,
                                         unsigned LoopDepth) {
  if (!LoopBody || LoopDepth == 0)
    return;

  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(S)) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (VD->hasGlobalStorage() && !VD->isStaticLocal() &&
            !VD->getType().isConstQualified()) {
          Collector.addHint(makeHint(
              HintCategory::GlobalVarInLoop, Impact::Medium, LoopDepth,
              DRE->getBeginLoc(), Ctx,
              "Reference to global/namespace-scope variable '" +
                  VD->getNameAsString() +
                  "' inside loop (depth " + std::to_string(LoopDepth) +
                  "); may inhibit optimization due to aliasing",
              "Cache the value in a local variable before the loop if "
              "the global is not modified by other threads"));
        }
      }
    }
    for (const Stmt *Child : S->children())
      Walk(Child);
  };
  Walk(LoopBody);
}

// ---------------------------------------------------------------------------
// 33. VolatileInLoop
// ---------------------------------------------------------------------------

void perfsanitizer::checkVolatileInLoop(const Stmt *LoopBody, ASTContext &Ctx,
                                        PerfHintCollector &Collector,
                                        unsigned LoopDepth) {
  if (!LoopBody || LoopDepth == 0)
    return;

  std::function<void(const Stmt *)> Walk = [&](const Stmt *S) {
    if (!S)
      return;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(S)) {
      QualType T = DRE->getType();
      if (T.isVolatileQualified()) {
        Collector.addHint(makeHint(
            HintCategory::VolatileInLoop, Impact::Medium, LoopDepth,
            DRE->getBeginLoc(), Ctx,
            "Volatile access inside loop (depth " +
                std::to_string(LoopDepth) +
                "); prevents optimization and vectorization",
            "If volatile is not required for hardware/signal correctness, "
            "remove it. If needed, consider reading into a local variable "
            "once outside the loop"));
      }
    }
    for (const Stmt *Child : S->children())
      Walk(Child);
  };
  Walk(LoopBody);
}

// ---------------------------------------------------------------------------
// 34. ImplicitConversion (narrowing)
// ---------------------------------------------------------------------------

void perfsanitizer::checkImplicitConversion(const ImplicitCastExpr *ICE,
                                            ASTContext &Ctx,
                                            PerfHintCollector &Collector,
                                            unsigned LoopDepth) {
  if (!ICE || LoopDepth == 0)
    return;

  if (ICE->getCastKind() != CK_FloatingCast &&
      ICE->getCastKind() != CK_IntegralCast)
    return;

  QualType SrcTy = ICE->getSubExpr()->getType();
  QualType DstTy = ICE->getType();

  uint64_t SrcSize = Ctx.getTypeSize(SrcTy);
  uint64_t DstSize = Ctx.getTypeSize(DstTy);

  // Only flag narrowing: larger source to smaller dest.
  if (SrcSize <= DstSize)
    return;

  std::string SrcName = SrcTy.getAsString();
  std::string DstName = DstTy.getAsString();

  Collector.addHint(makeHint(
      HintCategory::ImplicitConversion, Impact::Low, LoopDepth,
      ICE->getBeginLoc(), Ctx,
      "Implicit narrowing conversion from '" + SrcName + "' to '" + DstName +
          "' inside loop (depth " + std::to_string(LoopDepth) +
          "); may lose data and adds conversion overhead",
      "Use an explicit cast to document intent, or change the "
      "variable type to avoid the conversion"));
}

// ---------------------------------------------------------------------------
// 35. SlicingCopy
// ---------------------------------------------------------------------------

void perfsanitizer::checkSlicingCopy(const CallExpr *CE, ASTContext &Ctx,
                                     PerfHintCollector &Collector,
                                     unsigned LoopDepth) {
  if (!CE)
    return;

  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee)
    return;

  for (unsigned I = 0; I < CE->getNumArgs() && I < Callee->getNumParams();
       ++I) {
    const ParmVarDecl *Param = Callee->getParamDecl(I);
    QualType ParamTy = Param->getType();

    // Only check pass-by-value of class types.
    if (ParamTy->isReferenceType() || ParamTy->isPointerType())
      continue;
    const CXXRecordDecl *ParamRD = ParamTy->getAsCXXRecordDecl();
    if (!ParamRD || !ParamRD->hasDefinition())
      continue;

    // Check if the argument type is a derived class.
    const Expr *Arg = CE->getArg(I)->IgnoreImpCasts();
    QualType ArgTy = Arg->getType();
    if (ArgTy->isReferenceType())
      ArgTy = ArgTy.getNonReferenceType();
    if (ArgTy->isPointerType())
      continue;

    const CXXRecordDecl *ArgRD = ArgTy->getAsCXXRecordDecl();
    if (!ArgRD || !ArgRD->hasDefinition())
      continue;

    // Check if ArgRD derives from ParamRD (and they are different).
    if (ArgRD == ParamRD)
      continue;
    if (!ArgRD->isDerivedFrom(ParamRD))
      continue;

    Collector.addHint(makeHint(
        HintCategory::SlicingCopy, Impact::Medium, LoopDepth,
        Arg->getBeginLoc(), Ctx,
        "Passing derived class '" + ArgRD->getNameAsString() +
            "' by value to parameter taking base class '" +
            ParamRD->getNameAsString() +
            "' — object slicing will discard derived-class data",
        "Pass by reference or pointer to preserve the full object: "
        "'const " +
            ParamRD->getNameAsString() + "&'"));
  }
}

// ---------------------------------------------------------------------------
// 36. DivisionChain
// ---------------------------------------------------------------------------

void perfsanitizer::checkDivisionChain(const BinaryOperator *BO,
                                       ASTContext &Ctx,
                                       PerfHintCollector &Collector,
                                       unsigned LoopDepth) {
  if (!BO || BO->getOpcode() != BO_Div)
    return;

  // Only flag floating-point divisions (integer div has different semantics
  // for reciprocal).
  if (!BO->getType()->isFloatingType())
    return;

  const Expr *Divisor = BO->getRHS()->IgnoreParenImpCasts();

  // Get the divisor as a DeclRefExpr.
  const auto *DivRef = dyn_cast<DeclRefExpr>(Divisor);
  if (!DivRef)
    return;

  const VarDecl *DivVar = dyn_cast<VarDecl>(DivRef->getDecl());
  if (!DivVar)
    return;

  // Walk up to the enclosing compound statement via parent map to find
  // sibling divisions by the same variable.
  auto Parents = Ctx.getParents(*BO);
  const CompoundStmt *Enclosing = nullptr;
  for (int Depth = 0; Depth < 10 && !Parents.empty(); ++Depth) {
    if (const auto *CS = Parents[0].get<CompoundStmt>()) {
      Enclosing = CS;
      break;
    }
    Parents = Ctx.getParents(Parents[0]);
  }
  if (!Enclosing)
    return;

  // Count divisions by the same variable in this compound.
  unsigned DivCount = 0;
  std::function<void(const Stmt *)> CountDivs = [&](const Stmt *S) {
    if (!S)
      return;
    if (const auto *InnerBO = dyn_cast<BinaryOperator>(S)) {
      if (InnerBO->getOpcode() == BO_Div &&
          InnerBO->getType()->isFloatingType()) {
        if (const auto *Ref =
                dyn_cast<DeclRefExpr>(InnerBO->getRHS()->IgnoreParenImpCasts())) {
          if (Ref->getDecl() == DivVar)
            ++DivCount;
        }
      }
    }
    for (const Stmt *Child : S->children())
      CountDivs(Child);
  };
  CountDivs(Enclosing);

  if (DivCount < 2)
    return;

  // Only emit once per variable per compound — check if this is the first
  // division in source order.
  bool IsFirst = false;
  std::function<bool(const Stmt *)> FindFirst = [&](const Stmt *S) -> bool {
    if (!S)
      return false;
    if (S == BO) {
      IsFirst = true;
      return true;
    }
    if (const auto *InnerBO = dyn_cast<BinaryOperator>(S)) {
      if (InnerBO->getOpcode() == BO_Div &&
          InnerBO->getType()->isFloatingType()) {
        if (const auto *Ref =
                dyn_cast<DeclRefExpr>(InnerBO->getRHS()->IgnoreParenImpCasts())) {
          if (Ref->getDecl() == DivVar)
            return true; // Found an earlier division by same var.
        }
      }
    }
    for (const Stmt *Child : S->children()) {
      if (FindFirst(Child))
        return true;
    }
    return false;
  };
  FindFirst(Enclosing);

  if (!IsFirst)
    return;

  Collector.addHint(makeHint(
      HintCategory::DivisionChain, Impact::Medium, LoopDepth,
      BO->getOperatorLoc(), Ctx,
      "Multiple divisions by '" + DivVar->getNameAsString() +
          "' (" + std::to_string(DivCount) +
          " times) in the same block; division is expensive",
      "Compute the reciprocal once ('double inv_" +
          DivVar->getNameAsString() + " = 1.0 / " +
          DivVar->getNameAsString() +
          ";') and multiply instead"));
}
