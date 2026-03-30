//===- PerfASTVisitor.cpp - AST-level performance hint detection ----------===//

#include "PerfASTVisitor.h"
#include "PerfASTExtraChecks.h"
#include "PerfAutoFix.h"
#include "PerfFixItEmitter.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/Type.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/ExceptionSpecificationType.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace perfsanitizer;
using namespace clang;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

PerfHint PerfASTVisitor::makeHint(HintCategory Cat, Impact Base,
                                  SourceLocation Loc, llvm::StringRef Msg,
                                  llvm::StringRef Suggestion) {
  PerfHint H;
  H.Category = Cat;
  H.Layer = HintLayer::AST;
  H.Score = scaleByLoopDepth(Base, CurrentLoopDepth);
  H.Message = Msg.str();
  H.Suggestion = Suggestion.str();
  H.SrcLoc = Loc;

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

bool PerfASTVisitor::bodyCouldBeConstexpr(const FunctionDecl *FD) {
  if (!FD->hasBody())
    return false;
  const Stmt *Body = FD->getBody();
  // Rough heuristic: body has <=20 statements, no calls to non-constexpr
  // functions, no static locals, no asm, no goto.
  unsigned StmtCount = 0;
  std::function<bool(const Stmt *)> Check = [&](const Stmt *S) -> bool {
    if (!S)
      return true;
    ++StmtCount;
    if (StmtCount > 20)
      return false;
    if (isa<GotoStmt>(S) || isa<AsmStmt>(S))
      return false;
    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      if (const auto *Callee = CE->getDirectCallee()) {
        if (!Callee->isConstexpr())
          return false;
      } else {
        return false; // indirect call
      }
    }
    if (const auto *DS = dyn_cast<DeclStmt>(S)) {
      for (const auto *D : DS->decls()) {
        if (const auto *VD = dyn_cast<VarDecl>(D)) {
          if (VD->isStaticLocal())
            return false;
        }
      }
    }
    for (const Stmt *Child : S->children()) {
      if (!Check(Child))
        return false;
    }
    return true;
  };
  return Check(Body);
}

bool PerfASTVisitor::isCompileTimeConstant(const Expr *E) {
  if (!E)
    return false;
  return E->isValueDependent() || E->isIntegerConstantExpr(Ctx) ||
         isa<FloatingLiteral>(E) || isa<IntegerLiteral>(E);
}

std::string PerfASTVisitor::getEnclosingFunctionName(SourceLocation Loc) {
  // This is a best-effort helper; the visitor context usually knows this.
  return "";
}

unsigned PerfASTVisitor::countStatements(const Stmt *S) {
  if (!S)
    return 0;
  if (const auto *CS = dyn_cast<CompoundStmt>(S))
    return CS->size();
  return 1;
}

void PerfASTVisitor::collectLocalVars(
    const Stmt *S, llvm::SmallPtrSetImpl<const VarDecl *> &Vars) {
  if (!S)
    return;
  if (const auto *DS = dyn_cast<DeclStmt>(S)) {
    for (const auto *D : DS->decls()) {
      if (const auto *VD = dyn_cast<VarDecl>(D))
        Vars.insert(VD);
    }
  }
  for (const Stmt *Child : S->children())
    collectLocalVars(Child, Vars);
}

bool PerfASTVisitor::referencesAnyVar(
    const Expr *E, const llvm::SmallPtrSetImpl<const VarDecl *> &Vars) {
  if (!E)
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      if (Vars.count(VD))
        return true;
    }
  }
  for (const Stmt *Child : E->children()) {
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child)) {
      if (referencesAnyVar(ChildExpr, Vars))
        return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Helper: check if a type's qualified name contains a substring
// ---------------------------------------------------------------------------

bool PerfASTVisitor::typeNameContains(QualType T, llvm::StringRef Substr) {
  if (T.isNull())
    return false;
  // Check the canonical type's string representation.
  std::string TypeStr = T.getCanonicalType().getAsString();
  return TypeStr.find(Substr.str()) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Helper: check if a statement subtree contains a CXXTryStmt
// ---------------------------------------------------------------------------

bool PerfASTVisitor::containsTryCatch(const Stmt *S) {
  if (!S)
    return false;
  if (isa<CXXTryStmt>(S))
    return true;
  for (const Stmt *Child : S->children()) {
    if (containsTryCatch(Child))
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Helper: check if a function body calls .reserve() on a given VarDecl
// ---------------------------------------------------------------------------

bool PerfASTVisitor::hasReserveCallFor(const Stmt *Body,
                                       const VarDecl *VD) {
  if (!Body || !VD)
    return false;
  // Walk the entire body looking for calls to .reserve() on VD.
  std::function<bool(const Stmt *)> Search = [&](const Stmt *S) -> bool {
    if (!S)
      return false;
    if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(S)) {
      if (const auto *Callee = MCE->getMethodDecl()) {
        if (Callee->getNameAsString() == "reserve") {
          // Check if the object is our variable.
          const Expr *Obj = MCE->getImplicitObjectArgument();
          if (Obj) {
            Obj = Obj->IgnoreParenImpCasts();
            if (const auto *DRE = dyn_cast<DeclRefExpr>(Obj)) {
              if (DRE->getDecl() == VD)
                return true;
            }
          }
        }
      }
    }
    for (const Stmt *Child : S->children()) {
      if (Search(Child))
        return true;
    }
    return false;
  };
  return Search(Body);
}

// ---------------------------------------------------------------------------
// Helper: find push_back/emplace_back calls on vector variables in a subtree
// ---------------------------------------------------------------------------

void PerfASTVisitor::findPushBackCalls(
    const Stmt *S,
    llvm::SmallVectorImpl<
        std::pair<const CXXMemberCallExpr *, const VarDecl *>> &Results) {
  if (!S)
    return;
  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(S)) {
    if (const auto *Callee = MCE->getMethodDecl()) {
      StringRef Name = Callee->getName();
      if (Name == "push_back" || Name == "emplace_back") {
        // Check if the object type is a vector.
        const Expr *Obj = MCE->getImplicitObjectArgument();
        if (Obj) {
          QualType ObjT = Obj->getType();
          if (typeNameContains(ObjT, "vector")) {
            const Expr *ObjClean = Obj->IgnoreParenImpCasts();
            if (const auto *DRE = dyn_cast<DeclRefExpr>(ObjClean)) {
              if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                Results.push_back({MCE, VD});
              }
            }
          }
        }
      }
    }
  }
  for (const Stmt *Child : S->children())
    findPushBackCalls(Child, Results);
}

// ---------------------------------------------------------------------------
// ExceptionCost — try/catch inside loops
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCXXTryStmt(CXXTryStmt *S) {
  if (SM.isInSystemHeader(S->getBeginLoc()))
    return true;

  if (CurrentLoopDepth > 0) {
    Collector.addHint(makeHint(
        HintCategory::ExceptionCost, Impact::High, S->getBeginLoc(),
        "try/catch block inside a loop (depth " +
            std::to_string(CurrentLoopDepth) +
            ") — exception handling setup adds overhead every iteration.",
        "Move the try/catch outside the loop, use noexcept on hot-path "
        "functions, or switch to error codes for performance-critical loops."));
  }
  return true;
}

// ---------------------------------------------------------------------------
// FalseSharing — adjacent atomic/volatile fields in a record
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkFalseSharing(CXXRecordDecl *RD) {
  // Collect fields that are atomic or volatile.
  struct FieldInfo {
    FieldDecl *Field;
    uint64_t OffsetBits;
    uint64_t SizeBits;
    bool IsAtomicOrVolatile;
  };

  const auto &Layout = Ctx.getASTRecordLayout(RD);
  llvm::SmallVector<FieldInfo, 16> Fields;
  unsigned Idx = 0;
  for (auto *Field : RD->fields()) {
    QualType T = Field->getType();
    bool IsAV = T.isVolatileQualified() || typeNameContains(T, "atomic");
    uint64_t Offset = Layout.getFieldOffset(Idx);
    uint64_t Size = Ctx.getTypeSize(T);
    Fields.push_back({Field, Offset, Size, IsAV});
    ++Idx;
  }

  // Look for pairs of adjacent atomic/volatile fields that could be on
  // different cache lines when accessed by different threads.
  const uint64_t CacheLineBits = 64 * 8; // 64 bytes = 512 bits
  for (size_t I = 0; I + 1 < Fields.size(); ++I) {
    if (!Fields[I].IsAtomicOrVolatile || !Fields[I + 1].IsAtomicOrVolatile)
      continue;

    // Check if they fit together in less than 64 bytes.
    uint64_t CombinedBits =
        (Fields[I + 1].OffsetBits + Fields[I + 1].SizeBits) -
        Fields[I].OffsetBits;
    if (CombinedBits >= CacheLineBits)
      continue; // Already too big, not our concern.

    // They are close together — potential false sharing.
    Collector.addHint(makeHint(
        HintCategory::FalseSharing, Impact::Medium, Fields[I].Field->getLocation(),
        "Adjacent atomic/volatile fields '" +
            Fields[I].Field->getNameAsString() + "' and '" +
            Fields[I + 1].Field->getNameAsString() +
            "' may cause false sharing — they share a cache line but may "
            "be accessed by different threads.",
        "Add alignas(64) padding between these fields, or use "
        "alignas(std::hardware_destructive_interference_size) to ensure "
        "each field occupies its own cache line."));
  }
}

// ---------------------------------------------------------------------------
// StringByValue — detect std::string passed by value
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkStringByValue(FunctionDecl *FD) {
  for (const auto *Param : FD->parameters()) {
    QualType T = Param->getType();
    // Skip references, pointers, and r-value references.
    if (T->isReferenceType() || T->isPointerType())
      continue;
    // Check if it's a std::string (basic_string).
    if (typeNameContains(T, "basic_string")) {
      Collector.addHint(makeHint(
          HintCategory::StringByValue, Impact::Medium, Param->getLocation(),
          "Parameter '" + Param->getNameAsString() +
              "' passes std::string by value — involves a heap allocation "
              "and copy on every call.",
          "Use 'const std::string&' or 'std::string_view' instead to "
          "avoid the copy."));
    }
  }
}

// ---------------------------------------------------------------------------
// ContainerReserve — push_back/emplace_back without reserve() in loops
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkContainerReserve(Stmt *LoopBody,
                                           SourceLocation Loc) {
  if (!LoopBody)
    return;

  llvm::SmallVector<std::pair<const CXXMemberCallExpr *, const VarDecl *>, 8>
      PushCalls;
  findPushBackCalls(LoopBody, PushCalls);

  // For each push_back/emplace_back, check if there's a .reserve() call
  // on the same variable in the enclosing function body.
  // We walk up via the parent map to find the function body.
  for (const auto &[MCE, VD] : PushCalls) {
    // Walk parents to find the enclosing FunctionDecl.
    const Stmt *Current = LoopBody;
    const FunctionDecl *EnclosingFD = nullptr;
    // Use a simpler approach: walk parents of the loop body.
    auto Parents = Ctx.getParents(*MCE);
    const Stmt *Walk = LoopBody;
    // Try to find enclosing function by walking parent map from the MCE.
    auto WalkParents = Ctx.getParents(
        *static_cast<const Stmt *>(LoopBody));
    // Simplified: just check if VD has reserve() anywhere in the
    // translation unit is too broad. Instead check the loop body itself
    // doesn't have reserve and flag it.
    // A practical heuristic: if there's no reserve in the loop body's
    // enclosing compound statement, flag it.

    // Walk up parents to find the enclosing function body.
    const Stmt *FuncBody = nullptr;
    {
      clang::DynTypedNodeList Pars = Ctx.getParents(*LoopBody);
      for (int Depth = 0; Depth < 20 && !Pars.empty(); ++Depth) {
        if (const auto *FDecl = Pars[0].get<FunctionDecl>()) {
          FuncBody = FDecl->getBody();
          break;
        }
        Pars = Ctx.getParents(Pars[0]);
      }
    }

    if (!FuncBody || !hasReserveCallFor(FuncBody, VD)) {
      const CXXMethodDecl *Method = MCE->getMethodDecl();
      Collector.addHint(makeHint(
          HintCategory::ContainerReserve, Impact::Medium, MCE->getBeginLoc(),
          "'" + Method->getNameAsString() +
              "' called on vector '" + VD->getNameAsString() +
              "' inside a loop without a preceding .reserve() call.",
          "Call .reserve() before the loop with the expected number of "
          "elements to avoid repeated reallocations."));
    }
  }
}

// ---------------------------------------------------------------------------
// RangeForConversion — index-based for convertible to range-for
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkRangeForConversion(ForStmt *S) {
  // Pattern: for (int/size_t i = 0; i < container.size(); i++)
  //   ... container[i] ...

  // Check init: must be a single VarDecl initialized to 0.
  const Stmt *Init = S->getInit();
  if (!Init)
    return;
  const VarDecl *IndexVar = nullptr;
  if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
    if (DS->isSingleDecl()) {
      if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
        if (VD->hasInit()) {
          if (const auto *IL = dyn_cast<IntegerLiteral>(
                  VD->getInit()->IgnoreParenImpCasts())) {
            if (IL->getValue() == 0)
              IndexVar = VD;
          }
        }
      }
    }
  }
  if (!IndexVar)
    return;

  // Check cond: i < container.size()
  const Expr *Cond = S->getCond();
  if (!Cond)
    return;
  const BinaryOperator *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO || (BO->getOpcode() != BO_LT && BO->getOpcode() != BO_NE))
    return;

  // LHS should reference IndexVar.
  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const auto *LHSRef = dyn_cast<DeclRefExpr>(LHS);
  if (!LHSRef || LHSRef->getDecl() != IndexVar)
    return;

  // RHS should be a .size() call.
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
  const auto *SizeCall = dyn_cast<CXXMemberCallExpr>(RHS);
  if (!SizeCall)
    return;
  const CXXMethodDecl *SizeMethod = SizeCall->getMethodDecl();
  if (!SizeMethod || SizeMethod->getNameAsString() != "size")
    return;

  // Check inc: i++ or ++i.
  const Stmt *Inc = S->getInc();
  if (!Inc)
    return;
  bool IncOk = false;
  if (const auto *UO = dyn_cast<UnaryOperator>(Inc)) {
    if (UO->isIncrementOp()) {
      if (const auto *DRE =
              dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts())) {
        if (DRE->getDecl() == IndexVar)
          IncOk = true;
      }
    }
  }
  if (!IncOk)
    return;

  // Check body uses container[i].
  // This is a heuristic: look for ArraySubscriptExpr or operator[] calls
  // with IndexVar.
  bool UsesIndexAccess = false;
  std::function<void(const Stmt *)> FindAccess = [&](const Stmt *St) {
    if (!St || UsesIndexAccess)
      return;
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(St)) {
      if (const auto *Idx =
              dyn_cast<DeclRefExpr>(ASE->getIdx()->IgnoreParenImpCasts())) {
        if (Idx->getDecl() == IndexVar)
          UsesIndexAccess = true;
      }
    }
    if (const auto *OpCall = dyn_cast<CXXOperatorCallExpr>(St)) {
      if (OpCall->getOperator() == OO_Subscript && OpCall->getNumArgs() >= 2) {
        if (const auto *Idx = dyn_cast<DeclRefExpr>(
                OpCall->getArg(1)->IgnoreParenImpCasts())) {
          if (Idx->getDecl() == IndexVar)
            UsesIndexAccess = true;
        }
      }
    }
    for (const Stmt *Child : St->children())
      FindAccess(Child);
  };
  if (S->getBody())
    FindAccess(S->getBody());

  if (UsesIndexAccess) {
    Collector.addHint(makeHint(
        HintCategory::RangeForConversion, Impact::Low, S->getBeginLoc(),
        "Index-based for-loop iterates over a container using .size() and "
        "operator[] — could be a range-based for loop.",
        "Use 'for (auto& elem : container)' for clearer intent, fewer "
        "off-by-one risks, and potentially better optimization."));
  }
}

// ---------------------------------------------------------------------------
// ConstexprIf — if conditions that could be if constexpr
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkConstexprIf(IfStmt *S) {
  if (S->isConstexpr())
    return; // Already if constexpr.

  const Expr *Cond = S->getCond();
  if (!Cond)
    return;

  // Check if the condition involves sizeof, type traits (is_same, etc.).
  bool CouldBeConstexpr = false;
  std::string Reason;

  std::function<void(const Expr *)> CheckExpr = [&](const Expr *E) {
    if (!E || CouldBeConstexpr)
      return;
    E = E->IgnoreParenImpCasts();

    // sizeof expressions.
    if (isa<UnaryExprOrTypeTraitExpr>(E)) {
      const auto *UETTE = cast<UnaryExprOrTypeTraitExpr>(E);
      if (UETTE->getKind() == UETT_SizeOf) {
        CouldBeConstexpr = true;
        Reason = "sizeof";
        return;
      }
    }

    // Type trait calls like std::is_same<>::value or std::is_same_v<>.
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      StringRef Name = DRE->getDecl()->getName();
      if (Name.contains("is_same") || Name.contains("is_integral") ||
          Name.contains("is_floating") || Name.contains("is_pointer") ||
          Name.contains("is_arithmetic") || Name.contains("is_enum") ||
          Name.contains("is_class") || Name.contains("is_trivial") ||
          Name.contains("is_base_of")) {
        CouldBeConstexpr = true;
        Reason = "type trait (" + Name.str() + ")";
        return;
      }
    }

    // Member expressions like std::is_same<...>::value.
    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (ME->getMemberDecl()->getName() == "value") {
        // Check if the base type looks like a type trait.
        QualType BaseT = ME->getBase()->getType();
        std::string BaseName = BaseT.getAsString();
        if (BaseName.find("is_same") != std::string::npos ||
            BaseName.find("is_integral") != std::string::npos ||
            BaseName.find("is_base_of") != std::string::npos ||
            BaseName.find("is_trivial") != std::string::npos) {
          CouldBeConstexpr = true;
          Reason = "type trait";
          return;
        }
      }
    }

    for (const Stmt *Child : E->children()) {
      if (const auto *ChildE = dyn_cast_or_null<Expr>(Child))
        CheckExpr(ChildE);
    }
  };

  CheckExpr(Cond);

  if (CouldBeConstexpr) {
    Collector.addHint(makeHint(
        HintCategory::ConstexprIf, Impact::Low, S->getBeginLoc(),
        "If-condition uses " + Reason +
            " which is known at compile time — the branch is always "
            "taken or never taken.",
        "Use 'if constexpr' to eliminate the dead branch at compile time, "
        "reducing code size and enabling better optimization."));
  }
}

// ---------------------------------------------------------------------------
// LambdaCaptureOpt — large captures by value
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkLambdaCaptureOpt(LambdaExpr *LE) {
  for (const auto &Cap : LE->captures()) {
    if (Cap.getCaptureKind() != LCK_ByCopy)
      continue;
    const auto *CapturedVar = dyn_cast_or_null<VarDecl>(Cap.getCapturedVar());
    if (!CapturedVar)
      continue;

    QualType T = CapturedVar->getType();
    bool IsLarge = false;
    std::string TypeDesc;

    if (typeNameContains(T, "basic_string")) {
      IsLarge = true;
      TypeDesc = "std::string";
    } else if (typeNameContains(T, "vector")) {
      IsLarge = true;
      TypeDesc = "std::vector";
    } else if (T->isRecordType()) {
      // Check struct/class size.
      uint64_t Size = Ctx.getTypeSize(T) / 8;
      if (Size > 64) { // More than 64 bytes.
        IsLarge = true;
        TypeDesc = "struct (" + std::to_string(Size) + " bytes)";
      }
    }

    if (IsLarge) {
      Collector.addHint(makeHint(
          HintCategory::LambdaCaptureOpt, Impact::Medium,
          Cap.getLocation(),
          "Lambda captures '" + CapturedVar->getNameAsString() +
              "' (" + TypeDesc + ") by value — this copies the entire object.",
          "Capture by reference [&" + CapturedVar->getNameAsString() +
              "] instead if the lambda does not outlive the captured variable, "
              "or use [&] for all captures."));
    }
  }
}

// ---------------------------------------------------------------------------
// OutputParamToReturn — pointer/ref out-params that could be return values
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkOutputParamToReturn(FunctionDecl *FD) {
  if (!FD->hasBody())
    return;

  // Only check void functions or functions returning simple types.
  // Focus on non-const pointer/reference params.
  for (const auto *Param : FD->parameters()) {
    QualType T = Param->getType();
    bool IsOutParam = false;

    if (T->isPointerType()) {
      QualType Pointee = T->getPointeeType();
      if (!Pointee.isConstQualified())
        IsOutParam = true;
    } else if (T->isLValueReferenceType()) {
      QualType Referent = T.getNonReferenceType();
      if (!Referent.isConstQualified())
        IsOutParam = true;
    }

    if (!IsOutParam)
      continue;

    // Check if the parameter is only written to, never read in the body.
    // Heuristic: look for DeclRefExprs of this param.
    const VarDecl *ParamVD = Param;
    bool IsRead = false;
    bool IsWritten = false;

    std::function<void(const Stmt *)> Analyze = [&](const Stmt *S) {
      if (!S)
        return;
      if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (BO->isAssignmentOp()) {
          // Check if LHS is our param (possibly dereferenced).
          const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
          if (const auto *UO = dyn_cast<UnaryOperator>(LHS)) {
            if (UO->getOpcode() == UO_Deref) {
              if (const auto *DRE = dyn_cast<DeclRefExpr>(
                      UO->getSubExpr()->IgnoreParenImpCasts())) {
                if (DRE->getDecl() == ParamVD) {
                  IsWritten = true;
                  // Don't count this as a read. But check RHS for reads.
                  // Recurse on RHS only.
                  for (const Stmt *Child : BO->getRHS()->children())
                    Analyze(Child);
                  return;
                }
              }
            }
          }
          if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
            if (DRE->getDecl() == ParamVD) {
              IsWritten = true;
              for (const Stmt *Child : BO->getRHS()->children())
                Analyze(Child);
              return;
            }
          }
        }
      }

      // Any other reference to the param is a read.
      if (const auto *DRE = dyn_cast<DeclRefExpr>(S)) {
        if (DRE->getDecl() == ParamVD)
          IsRead = true;
      }

      for (const Stmt *Child : S->children())
        Analyze(Child);
    };

    Analyze(FD->getBody());

    if (IsWritten && !IsRead) {
      Collector.addHint(makeHint(
          HintCategory::OutputParamToReturn, Impact::Low, Param->getLocation(),
          "Parameter '" + Param->getNameAsString() +
              "' is a non-const " +
              (T->isPointerType() ? "pointer" : "reference") +
              " that is only written to, never read — it is a pure output "
              "parameter.",
          "Return the value instead of using an output parameter. Modern "
          "compilers apply NRVO/copy elision, making return-by-value "
          "efficient."));
    }
  }
}

// ---------------------------------------------------------------------------
// Cold path outlining — large blocks in error-handling branches
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkColdPathOutlining(IfStmt *S) {
  auto isErrorBranch = [](const Stmt *Branch) -> bool {
    if (!Branch)
      return false;
    // Check for throw
    if (isa<CXXThrowExpr>(Branch))
      return true;
    if (const auto *CS = dyn_cast<CompoundStmt>(Branch)) {
      for (const auto *Child : CS->body()) {
        if (isa<CXXThrowExpr>(Child))
          return true;
        if (const auto *RS = dyn_cast<ReturnStmt>(Child)) {
          // Return of error code (negative, 0, nullptr, false)
          if (const auto *IL =
                  dyn_cast_or_null<IntegerLiteral>(RS->getRetValue())) {
            if (IL->getValue().isNegative() || IL->getValue() == 0)
              return true;
          }
          if (isa_and_nonnull<CXXNullPtrLiteralExpr>(RS->getRetValue()))
            return true;
        }
        // Check for calls to abort/exit/_Exit
        if (const auto *CE = dyn_cast<CallExpr>(Child)) {
          if (const auto *Callee = CE->getDirectCallee()) {
            StringRef Name = Callee->getName();
            if (Name == "abort" || Name == "exit" || Name == "_Exit" ||
                Name == "terminate" || Name == "quick_exit")
              return true;
          }
        }
      }
    }
    return false;
  };

  const Stmt *Then = S->getThen();
  const Stmt *Else = S->getElse();

  // Check then-branch
  if (isErrorBranch(Then) && countStatements(Then) > 5) {
    Collector.addHint(makeHint(
        HintCategory::ColdPathOutlining, Impact::Medium, S->getBeginLoc(),
        "Error-handling branch has " + std::to_string(countStatements(Then)) +
            " statements — large cold block mixed with hot path.",
        "Move the error-handling code to a separate __attribute__((cold)) "
        "function, or add __attribute__((cold)) to the enclosing path to "
        "improve instruction cache utilization on the hot path."));
  }

  // Check else-branch
  if (isErrorBranch(Else) && countStatements(Else) > 5) {
    Collector.addHint(makeHint(
        HintCategory::ColdPathOutlining, Impact::Medium, S->getElseLoc(),
        "Error-handling else-branch has " +
            std::to_string(countStatements(Else)) +
            " statements — large cold block mixed with hot path.",
        "Move the error-handling code to a separate __attribute__((cold)) "
        "function to improve instruction cache utilization on the hot path."));
  }
}

// ---------------------------------------------------------------------------
// Loop unswitching — loop-invariant conditions inside loops
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkLoopUnswitching(Stmt *LoopStmt,
                                          SourceLocation Loc) {
  if (!LoopStmt)
    return;

  // Collect all variables that are local to the loop.
  llvm::SmallPtrSet<const VarDecl *, 16> LoopLocalVars;

  // Determine the actual loop body and collect vars from the full loop.
  const Stmt *Body = nullptr;
  if (auto *For = dyn_cast<ForStmt>(LoopStmt)) {
    // Collect vars from init, cond, inc, and body.
    if (For->getInit())
      collectLocalVars(For->getInit(), LoopLocalVars);
    if (For->getInc())
      collectLocalVars(For->getInc(), LoopLocalVars);
    Body = For->getBody();
  } else if (auto *While = dyn_cast<WhileStmt>(LoopStmt)) {
    Body = While->getBody();
  } else {
    Body = LoopStmt;
  }

  if (Body)
    collectLocalVars(Body, LoopLocalVars);

  // Walk the top-level statements of the loop body looking for if-statements.
  auto CheckBody = [&](const Stmt *B) {
    if (!B)
      return;
    const CompoundStmt *CS = dyn_cast<CompoundStmt>(B);
    if (!CS)
      return;
    for (const auto *Child : CS->body()) {
      const auto *If = dyn_cast<IfStmt>(Child);
      if (!If)
        continue;
      const Expr *Cond = If->getCond();
      if (!Cond)
        continue;
      // If the condition doesn't reference any loop-local variables,
      // it's a candidate for unswitching.
      if (!referencesAnyVar(Cond, LoopLocalVars)) {
        Collector.addHint(makeHint(
            HintCategory::LoopUnswitching, Impact::High, If->getBeginLoc(),
            "If-condition inside loop does not reference any loop-local "
            "variables — branch is loop-invariant.",
            "Hoist the condition outside the loop (loop unswitching): create "
            "two copies of the loop, one for each branch. This eliminates a "
            "branch from every iteration."));
      }
    }
  };

  CheckBody(Body);
}

// ---------------------------------------------------------------------------
// Missing [[nodiscard]] detection
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkMissingNodiscard(FunctionDecl *FD) {
  // Skip if already has nodiscard/warn_unused_result
  if (FD->hasAttr<WarnUnusedResultAttr>())
    return;
  // Skip void functions
  if (FD->getReturnType()->isVoidType())
    return;
  // Skip constructors/destructors
  if (isa<CXXConstructorDecl>(FD) || isa<CXXDestructorDecl>(FD))
    return;
  // Skip operators (many have conventional usage patterns)
  if (FD->isOverloadedOperator())
    return;

  bool ShouldFlag = false;
  std::string Reason;

  // Check for pure/const attribute — definitely should be nodiscard
  if (FD->hasAttr<PureAttr>() || FD->hasAttr<ConstAttr>()) {
    ShouldFlag = true;
    Reason = "has __attribute__((pure/const)) but no [[nodiscard]] — "
             "discarding the return value means the call is useless";
  }
  // Check for functions returning non-void with scalar or pointer return
  // that look like they should be checked (getters, compute functions)
  else if (FD->getReturnType()->isScalarType() && FD->param_size() > 0 &&
           !FD->hasBody()) {
    // We only flag declarations without bodies to reduce noise — the
    // return-value-discarded check at call sites handles the rest.
  }

  if (ShouldFlag) {
    Collector.addHint(makeHint(
        HintCategory::MissingNodiscard, Impact::Medium, FD->getLocation(),
        "Function '" + FD->getNameAsString() + "' " + Reason + ".",
        "Add [[nodiscard]] to the declaration to warn callers who discard "
        "the return value."));
  }
}

void PerfASTVisitor::checkDiscardedReturnValue(CallExpr *CE) {
  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee)
    return;
  if (Callee->getReturnType()->isVoidType())
    return;

  // Check if this CallExpr's result is used. We do this by checking if the
  // parent is a compound statement (i.e., the call is an expression-statement).
  auto Parents = Ctx.getParents(*CE);
  if (Parents.empty())
    return;

  // If the direct parent is a CompoundStmt or an ExprWithCleanups whose
  // parent is a CompoundStmt, the return value is discarded.
  bool Discarded = false;
  const Stmt *Parent = Parents[0].get<Stmt>();
  if (Parent && isa<CompoundStmt>(Parent)) {
    Discarded = true;
  } else if (Parent && isa<ExprWithCleanups>(Parent)) {
    auto GrandParents = Ctx.getParents(*Parent);
    if (!GrandParents.empty()) {
      const Stmt *GP = GrandParents[0].get<Stmt>();
      if (GP && isa<CompoundStmt>(GP))
        Discarded = true;
    }
  }

  if (!Discarded)
    return;

  // Flag if the callee is pure/const or already has nodiscard
  bool IsPureConst =
      Callee->hasAttr<PureAttr>() || Callee->hasAttr<ConstAttr>();
  bool HasNodiscard = Callee->hasAttr<WarnUnusedResultAttr>();

  if (IsPureConst && !HasNodiscard) {
    Collector.addHint(makeHint(
        HintCategory::MissingNodiscard,
        CurrentLoopDepth > 0 ? Impact::High : Impact::Medium,
        CE->getBeginLoc(),
        "Return value of pure/const function '" +
            Callee->getNameAsString() +
            "' is discarded — the call has no effect.",
        "Use the return value or remove the call. Add [[nodiscard]] to the "
        "function declaration to catch this at compile time."));
  }
}

// ---------------------------------------------------------------------------
// Signed loop counter detection
// ---------------------------------------------------------------------------

void PerfASTVisitor::checkSignedLoopCounter(ForStmt *S) {
  // Look at the init statement for a VarDecl with signed int type.
  const Stmt *Init = S->getInit();
  if (!Init)
    return;

  const VarDecl *CounterVar = nullptr;
  if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
    if (DS->isSingleDecl()) {
      if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
        QualType T = VD->getType();
        if (T->isSignedIntegerType() && !T->isBooleanType())
          CounterVar = VD;
      }
    }
  }

  if (!CounterVar)
    return;

  // Check the condition for comparison against an unsigned type.
  const Expr *Cond = S->getCond();
  if (!Cond)
    return;

  const BinaryOperator *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO)
    return;

  if (!BO->isRelationalOp() && !BO->isEqualityOp())
    return;

  // Check if either side is unsigned/size_t.
  auto isUnsignedExpr = [](const Expr *E) -> bool {
    if (!E)
      return false;
    QualType T = E->getType();
    return T->isUnsignedIntegerType();
  };

  bool HasUnsignedBound = isUnsignedExpr(BO->getLHS()) ||
                          isUnsignedExpr(BO->getRHS());

  if (HasUnsignedBound) {
    Collector.addHint(makeHint(
        HintCategory::SignedLoopCounter, Impact::Medium, S->getBeginLoc(),
        "Loop counter '" + CounterVar->getNameAsString() +
            "' is signed but compared against an unsigned bound — "
            "signed overflow is undefined behavior, limiting optimizations.",
        "Use 'unsigned', 'size_t', or the appropriate unsigned type for the "
        "loop counter to match the bound type. This enables the compiler to "
        "assume wrap-around semantics and apply more aggressive "
        "optimizations."));
  }
}

// ---------------------------------------------------------------------------
// Function declarations — constexpr/consteval/noexcept/pure/const
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitFunctionDecl(FunctionDecl *FD) {
  if (!FD->doesThisDeclarationHaveABody())
    return true;
  if (SM.isInSystemHeader(FD->getLocation()))
    return true;

  // 1. constexpr promotion
  if (!FD->isConstexpr() && !FD->isConsteval()) {
    if (bodyCouldBeConstexpr(FD)) {
      Collector.addHint(makeHint(
          HintCategory::ConstexprPromotion, Impact::Medium, FD->getLocation(),
          "Function '" + FD->getNameAsString() +
              "' could be declared constexpr — its body uses only "
              "constexpr-compatible operations.",
          "Add 'constexpr' to the declaration. If all calls use compile-time "
          "arguments, consider 'consteval'."));
    }
  }

  // 2. constexpr function that could be consteval
  if (FD->isConstexpr() && !FD->isConsteval() && !isa<CXXConstructorDecl>(FD)) {
    // Suggest consteval if the function is small and pure-computational
    if (FD->hasBody()) {
      const Stmt *Body = FD->getBody();
      // Simple heuristic: if it's a single return statement
      if (const auto *CS = dyn_cast<CompoundStmt>(Body)) {
        if (CS->size() <= 3) {
          Collector.addHint(makeHint(
              HintCategory::ConstevalPromotion, Impact::Low,
              FD->getLocation(),
              "constexpr function '" + FD->getNameAsString() +
                  "' has a small body and may be promotable to consteval.",
              "If this function is always called with compile-time arguments, "
              "declare it 'consteval' to guarantee compile-time evaluation."));
        }
      }
    }
  }

  // 3. noexcept suggestion
  auto *FPT = FD->getType()->getAs<clang::FunctionProtoType>();
  if (FPT && !isNoexceptExceptionSpec(FPT->getExceptionSpecType()) &&
      FD->hasBody()) {
    // Check if body contains no throw expressions and no calls to
    // non-noexcept functions (simplified: just check for throw).
    bool HasThrow = false;
    std::function<void(const Stmt *)> FindThrow = [&](const Stmt *S) {
      if (!S || HasThrow)
        return;
      if (isa<CXXThrowExpr>(S)) {
        HasThrow = true;
        return;
      }
      for (const Stmt *Child : S->children())
        FindThrow(Child);
    };
    FindThrow(FD->getBody());

    if (!HasThrow) {
      Collector.addHint(makeHint(
          HintCategory::NoExcept, Impact::Low, FD->getLocation(),
          "Function '" + FD->getNameAsString() +
              "' does not throw but is not declared noexcept.",
          "Add 'noexcept' — enables move semantics in containers and "
          "allows the compiler to omit unwind tables."));
    }
  }

  // 4. Pure/const attribute suggestions
  if (!FD->hasAttr<PureAttr>() && !FD->hasAttr<ConstAttr>() &&
      !FD->isOverloadedOperator() && FD->hasBody() &&
      FD->getReturnType()->isScalarType()) {
    // Check if function only reads its arguments and has no side effects
    bool HasSideEffects = false;
    std::function<void(const Stmt *)> CheckSE = [&](const Stmt *S) {
      if (!S || HasSideEffects)
        return;
      // Writing to pointers/refs, calling unknown functions = side effects
      if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
        if (UO->isIncrementDecrementOp())
          if (auto *Sub = dyn_cast<DeclRefExpr>(UO->getSubExpr()))
            if (Sub->getDecl() != nullptr &&
                !isa<ParmVarDecl>(Sub->getDecl()))
              HasSideEffects = true;
      }
      if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (BO->isAssignmentOp()) {
          if (const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS())) {
            if (!isa<ParmVarDecl>(LHS->getDecl()))
              HasSideEffects = true;
          } else {
            // Assignment to something that isn't a simple local
            HasSideEffects = true;
          }
        }
      }
      if (isa<CXXNewExpr>(S) || isa<CXXDeleteExpr>(S))
        HasSideEffects = true;
      for (const Stmt *Child : S->children())
        CheckSE(Child);
    };
    CheckSE(FD->getBody());

    if (!HasSideEffects && FD->param_size() > 0) {
      Collector.addHint(makeHint(
          HintCategory::PureConst, Impact::Medium, FD->getLocation(),
          "Function '" + FD->getNameAsString() +
              "' appears to have no side effects.",
          "Add __attribute__((pure)) or __attribute__((const)) to enable "
          "CSE and dead call elimination."));
    }
  }

  // 5. Missing [[nodiscard]] check
  checkMissingNodiscard(FD);

  // 6. String by value check
  checkStringByValue(FD);

  // 7. Output parameter to return value check
  checkOutputParamToReturn(FD);

  // Extra checks from PerfASTExtraChecks
  checkSmallFunctionNotInline(FD, Ctx, Collector);
  checkUnnecessaryCopy(FD, Ctx, Collector);
  checkStdFunctionOverhead(FD, Ctx, Collector, 0);
  checkUnusedInclude(FD, Ctx, Collector, 0);

  return true;
}

// ---------------------------------------------------------------------------
// Variable declarations — constexpr, restrict
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitVarDecl(VarDecl *VD) {
  if (SM.isInSystemHeader(VD->getLocation()))
    return true;

  // constexpr variable promotion
  if (!VD->isConstexpr() && VD->hasInit() && VD->getType().isConstQualified()) {
    if (isCompileTimeConstant(VD->getInit())) {
      Collector.addHint(makeHint(
          HintCategory::ConstexprPromotion, Impact::Low, VD->getLocation(),
          "const variable '" + VD->getNameAsString() +
              "' is initialized with a compile-time constant.",
          "Declare as 'constexpr' instead of 'const' — the value will be "
          "folded at compile time in all contexts."));
    }
  }

  // Extra checks from PerfASTExtraChecks
  checkMapToUnorderedMap(VD, Ctx, Collector, CurrentLoopDepth);
  checkVectorBoolAvoid(VD, Ctx, Collector);
  checkStaticLocalInit(VD, Ctx, Collector, CurrentLoopDepth);
  checkExcessiveCopy(VD, Ctx, Collector, CurrentLoopDepth);
  checkStdFunctionOverhead(VD, Ctx, Collector, CurrentLoopDepth);
  if (const auto *PVD = dyn_cast<ParmVarDecl>(VD))
    checkSharedPtrOverhead(PVD, Ctx, Collector, CurrentLoopDepth);

  // restrict suggestion for pointer parameters
  if (VD->getType()->isPointerType() && isa<ParmVarDecl>(VD)) {
    QualType PT = VD->getType();
    if (!PT.isRestrictQualified()) {
      Collector.addHint(makeHint(
          HintCategory::RestrictAnnotation,
          CurrentLoopDepth > 0 ? Impact::High : Impact::Medium,
          VD->getLocation(),
          "Pointer parameter '" + VD->getNameAsString() +
              "' is not __restrict__ qualified.",
          "If this pointer does not alias other pointer arguments, add "
          "'__restrict__' to enable better alias analysis and "
          "vectorization."));
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Record declarations — data layout, struct padding
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCXXRecordDecl(CXXRecordDecl *RD) {
  if (!RD->isCompleteDefinition() || SM.isInSystemHeader(RD->getLocation()))
    return true;

  // Check for potential struct padding issues
  if (RD->field_empty())
    return true;

  const auto &Layout = Ctx.getASTRecordLayout(RD);
  uint64_t TotalFieldSize = 0;
  for (const auto *Field : RD->fields()) {
    TotalFieldSize += Ctx.getTypeSize(Field->getType());
  }
  uint64_t StructSize = Layout.getSize().getQuantity() * 8; // bits

  // If padding is more than 25% of struct size, suggest reordering
  if (StructSize > 0 && TotalFieldSize > 0) {
    uint64_t Padding = StructSize - TotalFieldSize;
    if (Padding * 4 > StructSize && StructSize > 64) { // >25% padding, >8 bytes
      Collector.addHint(makeHint(
          HintCategory::DataLayout, Impact::Medium, RD->getLocation(),
          "Struct '" + RD->getNameAsString() + "' has " +
              std::to_string(Padding / 8) + " bytes of padding (" +
              std::to_string(Padding * 100 / StructSize) + "% of size).",
          "Reorder fields by size (largest first) to minimize padding and "
          "improve cache utilization."));
    }
  }

  // Check for false sharing among atomic/volatile fields.
  checkFalseSharing(RD);

  // Check for missing virtual destructor.
  checkVirtualDtorMissing(RD, Ctx, Collector, 0);

  return true;
}

// ---------------------------------------------------------------------------
// Methods — virtual devirtualization, move semantics
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCXXMethodDecl(CXXMethodDecl *MD) {
  if (SM.isInSystemHeader(MD->getLocation()))
    return true;

  // Virtual devirtualization hint — final classes/methods
  if (MD->isVirtual() && !MD->hasAttr<FinalAttr>()) {
    if (const auto *RD = MD->getParent()) {
      if (!RD->hasAttr<FinalAttr>()) {
        Collector.addHint(makeHint(
            HintCategory::VirtualDevirt, Impact::Medium, MD->getLocation(),
            "Virtual method '" + MD->getNameAsString() +
                "' in class '" + RD->getNameAsString() +
                "' is not marked final.",
            "If not overridden in subclasses, mark as 'final' to enable "
            "devirtualization — eliminates vtable lookup overhead."));
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Field declarations — alignment
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitFieldDecl(FieldDecl *FD) {
  if (SM.isInSystemHeader(FD->getLocation()))
    return true;

  // Suggest explicit alignment for SIMD-friendly types
  QualType T = FD->getType();
  if (T->isArrayType() || T->isVectorType()) {
    if (!FD->hasAttr<AlignedAttr>()) {
      uint64_t Size = Ctx.getTypeSize(T);
      if (Size >= 128) { // 16 bytes or more
        Collector.addHint(makeHint(
            HintCategory::AlignmentHint, Impact::Low, FD->getLocation(),
            "Array/vector field '" + FD->getNameAsString() +
                "' may benefit from explicit alignment.",
            "Add alignas(16) or alignas(32) for SIMD-friendly access "
            "patterns."));
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Loop statements — bounds, invariants, vectorization hints
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitForStmt(ForStmt *S) {
  if (SM.isInSystemHeader(S->getBeginLoc()))
    return true;

  ++CurrentLoopDepth;

  // Check for unknown trip count (no constant bound)
  if (S->getCond()) {
    bool HasConstBound = false;
    if (const auto *BO = dyn_cast<BinaryOperator>(S->getCond())) {
      if (isCompileTimeConstant(BO->getRHS()) ||
          isCompileTimeConstant(BO->getLHS()))
        HasConstBound = true;
    }
    if (!HasConstBound) {
      Collector.addHint(makeHint(
          HintCategory::LoopBound, Impact::High, S->getBeginLoc(),
          "Loop has no compile-time-constant upper bound.",
          "If the trip count is known at compile time, use a constexpr "
          "variable. If bounded at runtime, consider adding "
          "__builtin_assume(n <= MAX) to enable vectorization."));
    }
  }

  // Check for signed loop counter vs unsigned bound
  checkSignedLoopCounter(S);

  // Check for loop unswitching opportunities.
  // Pass the full for-statement so init-declared variables are included.
  checkLoopUnswitching(S, S->getBeginLoc());

  // Check for loop-invariant function calls in the condition
  if (S->getCond()) {
    std::function<void(const Stmt *)> FindCalls = [&](const Stmt *St) {
      if (!St)
        return;
      if (const auto *CE = dyn_cast<CallExpr>(St)) {
        if (const auto *Callee = CE->getDirectCallee()) {
          if (!Callee->isConstexpr() && !Callee->hasAttr<PureAttr>() &&
              !Callee->hasAttr<ConstAttr>()) {
            Collector.addHint(makeHint(
                HintCategory::LoopInvariant, Impact::High, CE->getBeginLoc(),
                "Function call '" + Callee->getNameAsString() +
                    "' in loop condition may be re-evaluated each iteration.",
                "Hoist the call before the loop if the result doesn't change, "
                "or mark the called function as __attribute__((pure))."));
          }
        }
      }
      for (const Stmt *Child : St->children())
        FindCalls(Child);
    };
    FindCalls(S->getCond());
  }

  // Check for range-for conversion opportunity.
  checkRangeForConversion(S);

  // Check for push_back/emplace_back without reserve().
  checkContainerReserve(S->getBody(), S->getBeginLoc());

  // Extra checks from PerfASTExtraChecks
  checkRedundantComputation(S, Ctx, Collector, CurrentLoopDepth);
  checkTightLoopAllocation(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkSortAlgorithm(S, Ctx, Collector, CurrentLoopDepth);
  checkMemcpyOpportunity(S, Ctx, Collector, CurrentLoopDepth);
  checkMutexInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkEmptyLoopBody(S, Ctx, Collector, CurrentLoopDepth);
  checkStringConcatInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkRegexInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkDynamicCastInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkGlobalVarInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkVolatileInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);

  // After visiting children, decrement.
  // Note: RecursiveASTVisitor handles child traversal; we manage depth here.
  // We'll decrement in a post-order fashion via TraverseForStmt override
  // if needed, but for now this is approximate.
  --CurrentLoopDepth;
  return true;
}

bool PerfASTVisitor::VisitWhileStmt(WhileStmt *S) {
  if (SM.isInSystemHeader(S->getBeginLoc()))
    return true;

  ++CurrentLoopDepth;

  Collector.addHint(makeHint(
      HintCategory::LoopBound, Impact::Medium, S->getBeginLoc(),
      "while-loop has no explicit trip count — may inhibit vectorization.",
      "If the iteration count is bounded, consider converting to a for-loop "
      "with a known upper bound, or add __builtin_assume for the bound."));

  // Check for loop unswitching opportunities
  checkLoopUnswitching(S, S->getBeginLoc());

  // Check for push_back/emplace_back without reserve().
  checkContainerReserve(S->getBody(), S->getBeginLoc());

  // Extra checks from PerfASTExtraChecks
  checkTightLoopAllocation(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkMutexInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkStringConcatInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkRegexInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkDynamicCastInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkGlobalVarInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkVolatileInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);

  --CurrentLoopDepth;
  return true;
}

// ---------------------------------------------------------------------------
// If statements — branch prediction hints
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitIfStmt(IfStmt *S) {
  if (SM.isInSystemHeader(S->getBeginLoc()))
    return true;

  // Check for if constexpr opportunities.
  checkConstexprIf(S);

  // Suggest [[likely]]/[[unlikely]] for error-handling patterns
  // Check for cold path outlining opportunities (works with or without else)
  checkColdPathOutlining(S);

  if (S->getElse()) {
    // Check if one branch is a return/throw (error path)
    const Stmt *Then = S->getThen();
    const Stmt *Else = S->getElse();

    auto isErrorPath = [](const Stmt *S) -> bool {
      if (!S)
        return false;
      if (isa<CXXThrowExpr>(S))
        return true;
      if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
        for (const auto *Child : CS->body()) {
          if (isa<CXXThrowExpr>(Child))
            return true;
          if (const auto *RS = dyn_cast<ReturnStmt>(Child)) {
            // Return of error code (negative, nullptr, false, etc.)
            if (const auto *IL =
                    dyn_cast_or_null<IntegerLiteral>(RS->getRetValue())) {
              if (IL->getValue().isNegative() || IL->getValue() == 0)
                return true;
            }
            if (isa_and_nonnull<CXXNullPtrLiteralExpr>(RS->getRetValue()))
              return true;
          }
        }
      }
      return false;
    };

    if (isErrorPath(Then)) {
      Collector.addHint(makeHint(
          HintCategory::BranchPrediction, Impact::Medium, S->getBeginLoc(),
          "If-branch appears to be an error/early-return path.",
          "Add [[unlikely]] to the condition to improve branch prediction: "
          "if ([[unlikely]] (cond)) { ... }"));
    } else if (isErrorPath(Else)) {
      Collector.addHint(makeHint(
          HintCategory::BranchPrediction, Impact::Medium, S->getBeginLoc(),
          "Else-branch appears to be an error/early-return path.",
          "Add [[likely]] to the condition to improve branch prediction: "
          "if ([[likely]] (cond)) { ... }"));
    }
  }

  // Extra checks from PerfASTExtraChecks
  checkBoolBranching(S, Ctx, Collector);
  checkBranchFreePredicate(S, Ctx, Collector, CurrentLoopDepth);
  checkDuplicateCondition(S, Ctx, Collector, CurrentLoopDepth);

  return true;
}

// ---------------------------------------------------------------------------
// Call expressions — inlining, FMA
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCallExpr(CallExpr *CE) {
  if (SM.isInSystemHeader(CE->getBeginLoc()))
    return true;

  // Check for discarded return values
  checkDiscardedReturnValue(CE);

  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee)
    return true;

  // Small function inlining hint
  if (Callee->hasBody() && !Callee->hasAttr<AlwaysInlineAttr>()) {
    const Stmt *Body = Callee->getBody();
    if (const auto *CS = dyn_cast<CompoundStmt>(Body)) {
      if (CS->size() <= 3 && CurrentLoopDepth > 0) {
        Collector.addHint(makeHint(
            HintCategory::InliningCandidate,
            CurrentLoopDepth > 1 ? Impact::High : Impact::Medium,
            CE->getBeginLoc(),
            "Call to small function '" + Callee->getNameAsString() +
                "' (" + std::to_string(CS->size()) +
                " statements) inside a loop.",
            "Mark with __attribute__((always_inline)) or move the body "
            "inline to avoid call overhead in the hot loop."));
      }
    }
  }

  // FMA contraction opportunity: a * b + c pattern
  // (detected at binary operator level, but we check for math function calls)
  StringRef Name = Callee->getName();
  if (Name == "fma" || Name == "fmaf" || Name == "fmal") {
    // Already using FMA — good
  } else if ((Name == "sin" || Name == "cos" || Name == "exp" ||
              Name == "log" || Name == "sqrt") &&
             CurrentLoopDepth > 0) {
    Collector.addHint(makeHint(
        HintCategory::FMAContraction, Impact::Medium, CE->getBeginLoc(),
        "Math function '" + Name.str() + "' called in a loop.",
        "Consider -ffast-math or #pragma STDC FP_CONTRACT ON if "
        "precision tolerance allows — enables FMA and SIMD math."));
  }

  // Extra checks from PerfASTExtraChecks
  checkSmallVectorSize(CE, Ctx, Collector, CurrentLoopDepth);
  checkVirtualInLoop(CE, Ctx, Collector, CurrentLoopDepth);
  checkSlicingCopy(CE, Ctx, Collector, CurrentLoopDepth);

  return true;
}

// ---------------------------------------------------------------------------
// new expressions — heap to stack
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCXXNewExpr(CXXNewExpr *NE) {
  if (SM.isInSystemHeader(NE->getBeginLoc()))
    return true;

  if (!NE->isArray()) {
    QualType AllocType = NE->getAllocatedType();
    uint64_t Size = Ctx.getTypeSize(AllocType) / 8;
    if (Size <= 256) { // Small allocation
      Collector.addHint(makeHint(
          HintCategory::HeapToStack,
          CurrentLoopDepth > 0 ? Impact::High : Impact::Medium,
          NE->getBeginLoc(),
          "Small heap allocation (" + std::to_string(Size) +
              " bytes) could potentially be stack-allocated.",
          "If the lifetime is bounded to this scope, use a local variable "
          "or std::array instead of new — avoids allocator overhead."));
    }
  } else if (NE->getArraySize().has_value()) {
    // Fixed-size array new
    if (const auto *SizeExpr = NE->getArraySize().value()) {
      if (isCompileTimeConstant(SizeExpr)) {
        Collector.addHint(makeHint(
            HintCategory::HeapToStack, Impact::Medium, NE->getBeginLoc(),
            "Heap-allocated array with compile-time-constant size.",
            "Consider std::array or a stack buffer — avoids heap overhead "
            "and enables better optimization."));
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Throw expressions — noexcept / cold path
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCXXThrowExpr(CXXThrowExpr *TE) {
  if (SM.isInSystemHeader(TE->getBeginLoc()))
    return true;

  // Find enclosing function via parent map for checkThrowInNoexcept.
  clang::DynTypedNodeList Parents = Ctx.getParents(*TE);
  const FunctionDecl *EnclosingFD = nullptr;
  for (int Depth = 0; Depth < 20 && !Parents.empty(); ++Depth) {
    if (const auto *FD = Parents[0].get<FunctionDecl>()) {
      EnclosingFD = FD;
      break;
    }
    Parents = Ctx.getParents(Parents[0]);
  }
  if (EnclosingFD)
    checkThrowInNoexcept(TE, EnclosingFD, Ctx, Collector);

  return true;
}

// ---------------------------------------------------------------------------
// Binary operators — FMA contraction pattern detection
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitBinaryOperator(BinaryOperator *BO) {
  if (SM.isInSystemHeader(BO->getBeginLoc()))
    return true;

  // Detect a * b + c pattern (FMA candidate)
  if (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub) {
    if (BO->getType()->isFloatingType()) {
      auto isMul = [](const Expr *E) -> bool {
        E = E->IgnoreParenImpCasts();
        if (const auto *Inner = dyn_cast<BinaryOperator>(E))
          return Inner->getOpcode() == BO_Mul;
        return false;
      };
      if (isMul(BO->getLHS()) || isMul(BO->getRHS())) {
        if (CurrentLoopDepth > 0) {
          Collector.addHint(makeHint(
              HintCategory::FMAContraction, Impact::Medium, BO->getBeginLoc(),
              "Floating-point multiply-add pattern (a*b+c) in a loop.",
              "Enable FMA contraction with #pragma STDC FP_CONTRACT ON "
              "or -ffp-contract=fast for fused multiply-add instructions."));
        }
      }
    }
  }

  // Detect select-like ternary that could be branchless
  // (This is handled at IfStmt level for full if/else patterns)

  // Extra checks from PerfASTExtraChecks
  checkPowerOfTwo(BO, Ctx, Collector, CurrentLoopDepth);
  checkEmptyVsSize(BO, Ctx, Collector, CurrentLoopDepth);
  checkDivisionChain(BO, Ctx, Collector, CurrentLoopDepth);

  return true;
}

// ---------------------------------------------------------------------------
// Unary operators — pre-increment preference
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitUnaryOperator(UnaryOperator *UO) {
  if (SM.isInSystemHeader(UO->getBeginLoc()))
    return true;

  if (CurrentLoopDepth > 0)
    checkPreIncrement(UO, Ctx, Collector, CurrentLoopDepth);

  return true;
}

// ---------------------------------------------------------------------------
// Range-for statements — copy-in-range-for check
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCXXForRangeStmt(CXXForRangeStmt *S) {
  if (SM.isInSystemHeader(S->getBeginLoc()))
    return true;

  ++CurrentLoopDepth;

  checkCopyInRangeFor(S, Ctx, Collector, CurrentLoopDepth);

  // Also run loop-body checks on the range-for body.
  checkStringConcatInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkRegexInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkDynamicCastInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkGlobalVarInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkVolatileInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkTightLoopAllocation(S->getBody(), Ctx, Collector, CurrentLoopDepth);
  checkMutexInLoop(S->getBody(), Ctx, Collector, CurrentLoopDepth);

  --CurrentLoopDepth;
  return true;
}

// ---------------------------------------------------------------------------
// DeclRefExpr — move semantics hints
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitDeclRefExpr(DeclRefExpr *DRE) {
  // This is a placeholder for move-semantics detection.
  // Full implementation would track last-use of variables in return
  // statements and suggest std::move.
  return true;
}

// ---------------------------------------------------------------------------
// Lambda expressions — capture optimization
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitLambdaExpr(LambdaExpr *LE) {
  if (SM.isInSystemHeader(LE->getBeginLoc()))
    return true;

  checkLambdaCaptureOpt(LE);
  return true;
}

// ---------------------------------------------------------------------------
// Destructor declarations — exception safety
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCXXDestructorDecl(CXXDestructorDecl *DD) {
  if (!DD->doesThisDeclarationHaveABody())
    return true;
  if (SM.isInSystemHeader(DD->getLocation()))
    return true;

  checkExceptionInDestructor(DD, Ctx, Collector);
  return true;
}

// ---------------------------------------------------------------------------
// ASTConsumer — entry point
// ---------------------------------------------------------------------------

void PerfASTConsumer::HandleTranslationUnit(ASTContext &Ctx) {
  PerfASTVisitor Visitor(Ctx, Collector);
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (Collector.empty())
    return;
  Collector.finalize();

  // Filter by minimum score
  if (MinScore > 0) {
    auto &Hints = const_cast<std::vector<PerfHint> &>(Collector.getHints());
    Hints.erase(
        std::remove_if(Hints.begin(), Hints.end(),
                       [this](const PerfHint &H) { return H.Score < MinScore; }),
        Hints.end());
  }

  auto &SM = CI.getSourceManager();

  switch (Mode) {
  case PerfOutputMode::Report:
    Collector.emitToStream(llvm::errs());
    break;

  case PerfOutputMode::Fix: {
    PerfAutoFixer Fixer(SM, CI.getLangOpts());
    unsigned Applied = 0;
    for (const auto &H : Collector.getHints()) {
      auto Fixes = Fixer.generateFixes(H, Ctx);
      if (!Fixes.empty()) {
        Fixer.applyFixes(Fixes);
        ++Applied;
      }
    }
    if (Applied > 0) {
      Fixer.writeFixedFiles();
      llvm::errs() << "perfsanitizer: applied " << Applied << " auto-fixes ("
                    << Collector.size() << " total hints)\n";
    } else {
      llvm::errs() << "perfsanitizer: no fixable hints found ("
                    << Collector.size() << " total hints)\n";
    }
    break;
  }

  case PerfOutputMode::Diff: {
    PerfAutoFixer Fixer(SM, CI.getLangOpts());
    unsigned Fixable = 0;
    for (const auto &H : Collector.getHints()) {
      auto Fixes = Fixer.generateFixes(H, Ctx);
      if (!Fixes.empty()) {
        Fixer.applyFixes(Fixes);
        ++Fixable;
      }
    }
    std::string D = Fixer.getDiff();
    if (!D.empty()) {
      llvm::outs() << D;
    }
    llvm::errs() << "perfsanitizer: " << Fixable << " fixable / "
                  << Collector.size() << " total hints\n";
    break;
  }

  case PerfOutputMode::Diag:
    emitAllHintsAsDiagnostics(CI.getDiagnostics(), SM, Collector);
    break;

  case PerfOutputMode::Quiet:
    llvm::errs() << "perfsanitizer: " << Collector.size() << " hints\n";
    break;
  }
}
