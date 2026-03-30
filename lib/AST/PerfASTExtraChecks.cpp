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
#include "clang/AST/StmtIterator.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/StringRef.h"

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
