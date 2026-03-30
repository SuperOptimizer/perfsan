//===- PerfASTVisitor.cpp - AST-level performance hint detection ----------===//

#include "PerfASTVisitor.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/Type.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/ExceptionSpecificationType.h"
#include "clang/Basic/SourceManager.h"

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

  --CurrentLoopDepth;
  return true;
}

// ---------------------------------------------------------------------------
// If statements — branch prediction hints
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitIfStmt(IfStmt *S) {
  if (SM.isInSystemHeader(S->getBeginLoc()))
    return true;

  // Suggest [[likely]]/[[unlikely]] for error-handling patterns
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

  return true;
}

// ---------------------------------------------------------------------------
// Call expressions — inlining, FMA
// ---------------------------------------------------------------------------

bool PerfASTVisitor::VisitCallExpr(CallExpr *CE) {
  if (SM.isInSystemHeader(CE->getBeginLoc()))
    return true;

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
  // No hint needed here; we handle noexcept at the function level.
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
// ASTConsumer — entry point
// ---------------------------------------------------------------------------

void PerfASTConsumer::HandleTranslationUnit(ASTContext &Ctx) {
  PerfASTVisitor Visitor(Ctx, Collector);
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());

  // Emit report after AST analysis completes.
  if (!Collector.empty()) {
    Collector.finalize();
    Collector.emitToStream(llvm::errs());
  }
}
