//===- PerfIRPass.cpp - LLVM IR performance analysis pass -----------------===//

#include "PerfIRPass.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

using namespace perfsanitizer;
using namespace llvm;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static PerfHint makeIRHint(HintCategory Cat, unsigned Score,
                           const Instruction *I, StringRef Msg,
                           StringRef Suggestion, StringRef FuncName) {
  PerfHint H;
  H.Category = Cat;
  H.Layer = HintLayer::IR;
  H.Score = std::min(Score, 200u);
  H.Message = Msg.str();
  H.Suggestion = Suggestion.str();
  H.FunctionName = FuncName.str();

  if (I) {
    if (const auto &DL = I->getDebugLoc()) {
      H.File = DL->getFilename().str();
      H.Line = DL->getLine();
      H.Col = DL->getColumn();
    }
  }
  return H;
}

static unsigned getLoopDepth(const Instruction *I, const LoopInfo &LI) {
  if (const BasicBlock *BB = I->getParent())
    return LI.getLoopDepth(BB);
  return 0;
}

// ---------------------------------------------------------------------------
// Module pass entry point
// ---------------------------------------------------------------------------

PreservedAnalyses PerfIRPass::run(Module &M, ModuleAnalysisManager &MAM) {
  auto &FAMProxy = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M);
  auto &FAM = FAMProxy.getManager();

  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    analyzeFunction(F, FAM);
  }

  return PreservedAnalyses::all(); // Analysis only — no IR changes.
}

void PerfIRPass::analyzeFunction(Function &F, FunctionAnalysisManager &FAM) {
  checkVectorizationBlockers(F, FAM);
  checkAliasBarriers(F, FAM);
  checkLoopTripCounts(F, FAM);
  checkMissedTailCalls(F);
  checkRedundantLoads(F);
  checkSROAEscapes(F);
  checkInliningCandidates(F, FAM);
  checkBranchPatterns(F, FAM);
  checkDataLayoutIssues(F);
}

// ---------------------------------------------------------------------------
// Vectorization blockers
// ---------------------------------------------------------------------------

void PerfIRPass::checkVectorizationBlockers(Function &F,
                                            FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();
    BasicBlock *Header = L->getHeader();

    // Check for function calls that may block vectorization
    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          Function *Callee = CI->getCalledFunction();
          if (!Callee)
            continue;
          if (Callee->isDeclaration() && !Callee->isIntrinsic() &&
              !Callee->hasFnAttribute(Attribute::NoUnwind)) {
            unsigned Score =
                scaleByLoopDepth(Impact::High, Depth);
            Collector.addHint(makeIRHint(
                HintCategory::Vectorization, Score, &I,
                "Call to external function '" + Callee->getName().str() +
                    "' inside loop may block vectorization.",
                "If the function is pure/side-effect-free, add "
                "__attribute__((const)) to its declaration, or use "
                "#pragma clang loop vectorize(assume_safety).",
                F.getName()));
          }
        }

        // Check for non-unit stride memory accesses
        if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
          if (L->isLoopInvariant(GEP->getPointerOperand())) {
            // Check if the index is a non-trivial expression
            for (auto Idx = GEP->idx_begin(); Idx != GEP->idx_end(); ++Idx) {
              if (auto *BO = dyn_cast<BinaryOperator>(Idx->get())) {
                if (BO->getOpcode() == Instruction::Mul ||
                    BO->getOpcode() == Instruction::Shl) {
                  unsigned Score = scaleByLoopDepth(Impact::High, Depth);
                  Collector.addHint(makeIRHint(
                      HintCategory::Vectorization, Score, &I,
                      "Non-unit stride memory access in loop — may prevent "
                      "efficient vectorization.",
                      "Restructure data layout for unit-stride access (SoA "
                      "instead of AoS) or use gather/scatter intrinsics.",
                      F.getName()));
                  break;
                }
              }
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Alias barriers
// ---------------------------------------------------------------------------

void PerfIRPass::checkAliasBarriers(Function &F,
                                    FunctionAnalysisManager &FAM) {
  auto &AA = FAM.getResult<AAManager>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    SmallVector<StoreInst *, 8> Stores;
    SmallVector<LoadInst *, 8> Loads;

    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        if (auto *SI = dyn_cast<StoreInst>(&I))
          Stores.push_back(SI);
        else if (auto *LI = dyn_cast<LoadInst>(&I))
          Loads.push_back(LI);
      }
    }

    // Check for may-alias between stores and loads in the loop
    unsigned AliasCount = 0;
    for (auto *SI : Stores) {
      for (auto *LdI : Loads) {
        MemoryLocation StoreLoc = MemoryLocation::get(SI);
        MemoryLocation LoadLoc = MemoryLocation::get(LdI);
        if (!AA.isNoAlias(StoreLoc, LoadLoc)) {
          ++AliasCount;
        }
      }
    }

    if (AliasCount > 2 && !Stores.empty()) {
      unsigned Depth = L->getLoopDepth();
      unsigned Score = scaleByLoopDepth(Impact::Critical, Depth);
      Collector.addHint(makeIRHint(
          HintCategory::AliasBarrier, Score, Stores.front(),
          "Loop has " + std::to_string(AliasCount) +
              " potential pointer alias pairs — alias analysis cannot prove "
              "independence.",
          "Add __restrict__ to pointer parameters or use "
          "#pragma clang loop vectorize(assume_safety) if you know the "
          "pointers don't alias.",
          F.getName()));
    }
  }
}

// ---------------------------------------------------------------------------
// Loop trip counts (SCEV analysis)
// ---------------------------------------------------------------------------

void PerfIRPass::checkLoopTripCounts(Function &F,
                                     FunctionAnalysisManager &FAM) {
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();
    unsigned TripCount = SE.getSmallConstantTripCount(L);

    if (TripCount == 0) {
      // SCEV couldn't determine a constant trip count
      const SCEV *BackedgeTakenCount = SE.getBackedgeTakenCount(L);
      if (isa<SCEVCouldNotCompute>(BackedgeTakenCount)) {
        BasicBlock *Header = L->getHeader();
        Instruction *Term =
            Header->empty() ? nullptr : &Header->front();
        unsigned Score = scaleByLoopDepth(Impact::High, Depth);
        Collector.addHint(makeIRHint(
            HintCategory::LoopBound, Score, Term,
            "Loop trip count is not computable by SCEV — prevents "
            "vectorization width selection and unrolling.",
            "Provide a known upper bound: use a constexpr limit, or add "
            "__builtin_assume(n <= MAX) before the loop.",
            F.getName()));
      }
    } else if (TripCount > 0 && TripCount < 4) {
      BasicBlock *Header = L->getHeader();
      Instruction *Term =
          Header->empty() ? nullptr : &Header->front();
      unsigned Score = scaleByLoopDepth(Impact::Low, Depth);
      Collector.addHint(makeIRHint(
          HintCategory::LoopBound, Score, Term,
          "Loop has very small trip count (" + std::to_string(TripCount) +
              ") — vectorization overhead may exceed benefit.",
          "Consider fully unrolling with #pragma clang loop "
          "unroll(full), or replace with straight-line code.",
          F.getName()));
    }
  }
}

// ---------------------------------------------------------------------------
// Missed tail calls
// ---------------------------------------------------------------------------

void PerfIRPass::checkMissedTailCalls(Function &F) {
  for (BasicBlock &BB : F) {
    ReturnInst *Ret = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!Ret)
      continue;

    // Look at the instruction before the return
    auto It = Ret->getIterator();
    if (It == BB.begin())
      continue;
    --It;

    if (auto *CI = dyn_cast<CallInst>(&*It)) {
      if (!CI->isTailCall() && !CI->isNoTailCall()) {
        // Check if the call result is returned directly
        if (Ret->getReturnValue() == CI) {
          Collector.addHint(makeIRHint(
              HintCategory::TailCall, static_cast<unsigned>(Impact::Medium),
              CI,
              "Call to '" +
                  (CI->getCalledFunction()
                       ? CI->getCalledFunction()->getName().str()
                       : std::string("indirect")) +
                  "' is in tail position but not marked as tail call.",
              "Ensure the function is eligible for TCO: no address-taken "
              "locals, matching calling conventions, and use "
              "[[clang::musttail]] if critical.",
              F.getName()));
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Redundant loads
// ---------------------------------------------------------------------------

void PerfIRPass::checkRedundantLoads(Function &F) {
  for (BasicBlock &BB : F) {
    DenseMap<Value *, unsigned> LoadCount;
    for (Instruction &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Value *Ptr = LI->getPointerOperand();
        LoadCount[Ptr]++;
        if (LoadCount[Ptr] > 1 && !LI->isVolatile()) {
          Collector.addHint(makeIRHint(
              HintCategory::RedundantLoad, static_cast<unsigned>(Impact::Low),
              &I,
              "Same address loaded multiple times in basic block — may "
              "indicate a missed CSE opportunity.",
              "Store the result in a local variable, or check if an "
              "intervening store prevents CSE. Adding __restrict__ to "
              "pointer parameters may help.",
              F.getName()));
        }
      }
      // Reset count after a store (conservative)
      if (isa<StoreInst>(&I))
        LoadCount.clear();
    }
  }
}

// ---------------------------------------------------------------------------
// SROA escapes — allocas that escape to calls
// ---------------------------------------------------------------------------

void PerfIRPass::checkSROAEscapes(Function &F) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI)
        continue;

      // Check if the alloca is used as a call argument (escapes)
      bool Escapes = false;
      for (User *U : AI->users()) {
        if (isa<CallInst>(U) || isa<InvokeInst>(U)) {
          // Passed to a function — SROA can't decompose
          Escapes = true;
          break;
        }
        if (auto *BC = dyn_cast<BitCastInst>(U)) {
          for (User *BCU : BC->users()) {
            if (isa<CallInst>(BCU) || isa<InvokeInst>(BCU)) {
              Escapes = true;
              break;
            }
          }
        }
      }

      if (Escapes && AI->getAllocatedType()->isStructTy()) {
        Collector.addHint(makeIRHint(
            HintCategory::SROAEscape, static_cast<unsigned>(Impact::Medium),
            AI,
            "Stack allocation of struct type escapes to a function call — "
            "prevents scalar replacement (SROA).",
            "Pass struct fields individually instead of a pointer to the "
            "struct, or mark the callee as inline.",
            F.getName()));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Inlining candidates
// ---------------------------------------------------------------------------

void PerfIRPass::checkInliningCandidates(Function &F,
                                         FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *Callee = CI->getCalledFunction();
      if (!Callee || Callee->isDeclaration() || Callee->isIntrinsic())
        continue;

      unsigned LoopDepth = LI.getLoopDepth(&BB);
      if (LoopDepth == 0)
        continue;

      // Small function in a loop that wasn't inlined
      if (Callee->size() <= 5) { // <=5 basic blocks
        unsigned Score = scaleByLoopDepth(Impact::Medium, LoopDepth);
        Collector.addHint(makeIRHint(
            HintCategory::InliningCandidate, Score, &I,
            "Non-inlined call to small function '" +
                Callee->getName().str() + "' (" +
                std::to_string(Callee->size()) +
                " BBs) inside loop at depth " +
                std::to_string(LoopDepth) + ".",
            "Add __attribute__((always_inline)) or increase the inlining "
            "threshold with -mllvm -inline-threshold=N.",
            F.getName()));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Branch patterns — select opportunities
// ---------------------------------------------------------------------------

void PerfIRPass::checkBranchPatterns(Function &F,
                                     FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;

    unsigned LoopDepth = LI.getLoopDepth(&BB);
    if (LoopDepth == 0)
      continue;

    // Check for diamond pattern: both successors have a single instruction
    // and merge to the same block — could be a select
    BasicBlock *TrueBB = BI->getSuccessor(0);
    BasicBlock *FalseBB = BI->getSuccessor(1);

    if (TrueBB->size() <= 2 && FalseBB->size() <= 2 &&
        TrueBB->getSingleSuccessor() == FalseBB->getSingleSuccessor() &&
        TrueBB->getSingleSuccessor() != nullptr) {
      unsigned Score = scaleByLoopDepth(Impact::Medium, LoopDepth);
      Collector.addHint(makeIRHint(
          HintCategory::BranchlessSelect, Score, BI,
          "Conditional branch with trivial true/false blocks in loop — "
          "could be a branchless select.",
          "Use the ternary operator (cond ? a : b) instead of if/else to "
          "encourage cmov generation and avoid branch mispredicts.",
          F.getName()));
    }
  }
}

// ---------------------------------------------------------------------------
// Data layout issues
// ---------------------------------------------------------------------------

void PerfIRPass::checkDataLayoutIssues(Function &F) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // Check for unaligned loads/stores
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getAlign().value() < 4 &&
            LI->getType()->getPrimitiveSizeInBits() >= 32) {
          Collector.addHint(makeIRHint(
              HintCategory::DataLayout,
              static_cast<unsigned>(Impact::Low), &I,
              "Load with alignment < 4 on a 32+ bit type — may cause "
              "performance penalty on some architectures.",
              "Ensure the underlying data is properly aligned (alignas or "
              "aligned allocation).",
              F.getName()));
        }
      }
    }
  }
}
