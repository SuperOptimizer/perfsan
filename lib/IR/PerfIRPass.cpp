//===- PerfIRPass.cpp - LLVM IR performance analysis pass -----------------===//

#include "PerfIRPass.h"
#include "llvm/ADT/SmallSet.h"
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
  checkSoAvsAoS(F, FAM);
  checkSIMDWidth(F, FAM);
  checkStrengthReduction(F, FAM);
  checkBitManipulation(F, FAM);
  checkRedundantAtomics(F);
  checkCacheLineSplits(F, FAM);
  checkCrossTUInlining(F, FAM);
  checkHotColdFunction(F, FAM);
  checkSpillPressure(F);
  checkUnrollingBlockers(F, FAM);
  checkDivisionChainIR(F, FAM);
  checkBranchOnFloat(F, FAM);
  checkMemoryAccessPattern(F, FAM);
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

// ---------------------------------------------------------------------------
// SoA vs AoS detection — strided GEP patterns in loops
// ---------------------------------------------------------------------------

void PerfIRPass::checkSoAvsAoS(Function &F, FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();

    // Track struct-field GEP accesses: map from base pointer
    // to set of field indices accessed. We key on just the pointer since
    // the struct type is determined by the GEP source element type.
    DenseMap<Value *, SmallSet<unsigned, 8>> StructFieldAccesses;
    DenseMap<Value *, Type *> BaseStructType;

    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        auto *GEP = dyn_cast<GetElementPtrInst>(&I);
        if (!GEP)
          continue;

        // Look for GEPs into struct types with constant field indices.
        // Pattern: getelementptr %struct.Foo, ptr %base, i64 %idx, i32 <field>
        Type *SrcElemTy = GEP->getSourceElementType();
        if (!SrcElemTy->isStructTy())
          continue;

        // Need at least 2 indices: an array index and a struct field index.
        if (GEP->getNumIndices() < 2)
          continue;

        // The last index should be a constant (struct field index).
        auto IdxIt = GEP->idx_end();
        --IdxIt;
        auto *FieldIdx = dyn_cast<ConstantInt>(IdxIt->get());
        if (!FieldIdx)
          continue;

        // The first index should be loop-varying (array index).
        auto FirstIdx = GEP->idx_begin();
        if (L->isLoopInvariant(FirstIdx->get()))
          continue;

        Value *Base = GEP->getPointerOperand();
        StructFieldAccesses[Base].insert(FieldIdx->getZExtValue());
        BaseStructType[Base] = SrcElemTy;
      }
    }

    // If multiple fields of the same struct are accessed in the loop,
    // this is an AoS pattern that could benefit from SoA.
    for (auto &[Base, Fields] : StructFieldAccesses) {
      if (Fields.size() >= 2) {
        // Find a representative instruction for the diagnostic location.
        Instruction *Rep = nullptr;
        for (BasicBlock *BB : L->blocks()) {
          for (Instruction &I : *BB) {
            if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
              if (GEP->getPointerOperand() == Base &&
                  GEP->getSourceElementType() == BaseStructType[Base]) {
                Rep = &I;
                break;
              }
            }
          }
          if (Rep)
            break;
        }

        unsigned Score = scaleByLoopDepth(Impact::High, Depth);
        Collector.addHint(makeIRHint(
            HintCategory::SoAvsAoS, Score, Rep,
            "Loop accesses " + std::to_string(Fields.size()) +
                " different fields of the same struct array (AoS pattern) — "
                "strided accesses inhibit vectorization.",
            "Restructure to Structure-of-Arrays (SoA): use separate parallel "
            "arrays for each field to enable contiguous memory access and "
            "auto-vectorization.",
            F.getName()));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// SIMD width hints — loops on float/double arrays without vectorization
// ---------------------------------------------------------------------------

void PerfIRPass::checkSIMDWidth(Function &F, FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();
    bool HasFloatOps = false;
    bool HasVectorOps = false;
    bool HasVectorizePragma = false;
    unsigned FloatLoadStoreCount = 0;

    // Check for llvm.loop metadata indicating vectorization pragmas.
    if (BasicBlock *Latch = L->getLoopLatch()) {
      if (Instruction *Term = Latch->getTerminator()) {
        if (MDNode *LoopMD = Term->getMetadata(LLVMContext::MD_loop)) {
          for (unsigned I = 1, E = LoopMD->getNumOperands(); I < E; ++I) {
            if (auto *InnerMD = dyn_cast<MDNode>(LoopMD->getOperand(I))) {
              if (InnerMD->getNumOperands() > 0) {
                if (auto *S = dyn_cast<MDString>(InnerMD->getOperand(0))) {
                  if (S->getString().starts_with("llvm.loop.vectorize"))
                    HasVectorizePragma = true;
                }
              }
            }
          }
        }
      }
    }

    if (HasVectorizePragma)
      continue;

    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        Type *Ty = I.getType();
        if (Ty->isVectorTy()) {
          HasVectorOps = true;
          break;
        }
        if (Ty->isFloatTy() || Ty->isDoubleTy()) {
          if (I.isBinaryOp())
            HasFloatOps = true;
        }
        if (auto *LdI = dyn_cast<LoadInst>(&I)) {
          Type *LoadTy = LdI->getType();
          if (LoadTy->isFloatTy() || LoadTy->isDoubleTy())
            ++FloatLoadStoreCount;
        }
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          Type *ValTy = SI->getValueOperand()->getType();
          if (ValTy->isFloatTy() || ValTy->isDoubleTy())
            ++FloatLoadStoreCount;
        }
      }
      if (HasVectorOps)
        break;
    }

    // If there are scalar float operations in the loop with multiple
    // load/stores and no vector ops, suggest vectorization.
    if (HasFloatOps && !HasVectorOps && FloatLoadStoreCount >= 4) {
      BasicBlock *Header = L->getHeader();
      Instruction *Rep = Header->empty() ? nullptr : &Header->front();
      unsigned Score = scaleByLoopDepth(Impact::High, Depth);
      Collector.addHint(makeIRHint(
          HintCategory::SIMDWidth, Score, Rep,
          "Loop performs scalar floating-point operations with " +
              std::to_string(FloatLoadStoreCount) +
              " float/double load/stores but no vectorization.",
          "Add '#pragma clang loop vectorize(enable)' before the loop, or "
          "use explicit SIMD types (__m128, __m256) for guaranteed "
          "vectorization.",
          F.getName()));
    }
  }
}

// ---------------------------------------------------------------------------
// Strength reduction — div/mod by non-power-of-2 constants in loops
// ---------------------------------------------------------------------------

void PerfIRPass::checkStrengthReduction(Function &F,
                                        FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (BasicBlock &BB : F) {
    unsigned LoopDepth = LI.getLoopDepth(&BB);
    if (LoopDepth == 0)
      continue;

    for (Instruction &I : BB) {
      unsigned Opcode = I.getOpcode();
      bool IsDiv = (Opcode == Instruction::SDiv || Opcode == Instruction::UDiv);
      bool IsRem = (Opcode == Instruction::SRem || Opcode == Instruction::URem);
      bool IsMul = (Opcode == Instruction::Mul);

      if (!IsDiv && !IsRem && !IsMul)
        continue;

      // Check if the RHS operand is a constant integer.
      auto *CI = dyn_cast<ConstantInt>(I.getOperand(1));
      if (!CI)
        continue;

      uint64_t Val = CI->getZExtValue();
      if (Val == 0)
        continue;

      bool IsPowerOf2 = (Val & (Val - 1)) == 0;

      if ((IsDiv || IsRem) && !IsPowerOf2) {
        unsigned Score = scaleByLoopDepth(Impact::High, LoopDepth);
        std::string OpStr = IsDiv ? "Division" : "Modulo";
        Collector.addHint(makeIRHint(
            HintCategory::StrengthReduction, Score, &I,
            OpStr + " by non-power-of-2 constant " + std::to_string(Val) +
                " inside a loop — requires expensive multiply+shift sequence.",
            "If possible, round the divisor to a power of 2 (enables shift "
            "replacement), or precompute a reciprocal multiplier. Consider "
            "restructuring the algorithm to avoid the division.",
            F.getName()));
      }

      if (IsMul && !IsPowerOf2 && Val > 2) {
        // Check if the constant could be expressed as a small shift+add.
        // e.g., x*3 = (x<<1)+x, x*5 = (x<<2)+x, x*9 = (x<<3)+x
        bool IsShiftAdd = false;
        for (unsigned Shift = 1; Shift < 6; ++Shift) {
          uint64_t Base = 1ULL << Shift;
          if (Val == Base + 1 || Val == Base - 1)
            IsShiftAdd = true;
        }
        // Only emit for non-trivial multiplications that the compiler
        // might not already strength-reduce.
        if (!IsShiftAdd) {
          unsigned Score = scaleByLoopDepth(Impact::Medium, LoopDepth);
          Collector.addHint(makeIRHint(
              HintCategory::StrengthReduction, Score, &I,
              "Multiplication by constant " + std::to_string(Val) +
                  " inside a loop — may benefit from strength reduction.",
              "Consider replacing with equivalent shift-and-add operations "
              "if the compiler hasn't already done so, or restructure to "
              "use incremental addition.",
              F.getName()));
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Bit manipulation patterns — manual popcount, ctz, clz
// ---------------------------------------------------------------------------

/// Check if a loop implements a manual popcount pattern:
///   while (x) { count++; x &= (x - 1); }
/// or the shift-and-mask variant:
///   while (x) { count += x & 1; x >>= 1; }
static bool isPopcountLoop(const Loop *L) {
  // Must be a simple loop with a single latch.
  BasicBlock *Latch = L->getLoopLatch();
  BasicBlock *Header = L->getHeader();
  if (!Latch || !Header)
    return false;

  // Look for the characteristic AND + SUB pattern: x &= (x - 1)
  // or the shift-and-mask pattern: (x & 1) ... x >>= 1
  bool HasAndSubPattern = false;
  bool HasShiftMaskPattern = false;

  for (BasicBlock *BB : L->blocks()) {
    for (const Instruction &I : *BB) {
      // Pattern 1: x & (x - 1)  — Brian Kernighan's trick
      if (I.getOpcode() == Instruction::And) {
        for (unsigned OpI = 0; OpI < 2; ++OpI) {
          auto *Sub = dyn_cast<BinaryOperator>(I.getOperand(OpI));
          if (Sub && Sub->getOpcode() == Instruction::Sub) {
            if (auto *One = dyn_cast<ConstantInt>(Sub->getOperand(1))) {
              if (One->isOne() && Sub->getOperand(0) == I.getOperand(1 - OpI))
                HasAndSubPattern = true;
            }
          }
        }
      }

      // Pattern 2: x & 1 followed by x >>= 1
      if (I.getOpcode() == Instruction::And) {
        if (auto *One = dyn_cast<ConstantInt>(I.getOperand(1))) {
          if (One->isOne())
            HasShiftMaskPattern = true;
        }
      }
    }
  }

  // Also require a right shift for the shift-mask pattern.
  if (HasShiftMaskPattern) {
    bool HasRightShift = false;
    for (BasicBlock *BB : L->blocks()) {
      for (const Instruction &I : *BB) {
        if (I.getOpcode() == Instruction::LShr ||
            I.getOpcode() == Instruction::AShr) {
          if (auto *Amt = dyn_cast<ConstantInt>(I.getOperand(1))) {
            if (Amt->isOne())
              HasRightShift = true;
          }
        }
      }
    }
    if (!HasRightShift)
      HasShiftMaskPattern = false;
  }

  return HasAndSubPattern || HasShiftMaskPattern;
}

/// Check if a loop implements counting leading/trailing zeros:
///   while (x) { count++; x >>= 1; }  (leading zeros variant)
///   while (x & 1 == 0) { count++; x >>= 1; } (trailing zeros)
static bool isCountZerosLoop(const Loop *L, bool &IsTrailing) {
  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return false;

  bool HasShift = false;
  bool HasMaskTest = false;

  for (BasicBlock *BB : L->blocks()) {
    for (const Instruction &I : *BB) {
      if (I.getOpcode() == Instruction::LShr ||
          I.getOpcode() == Instruction::AShr) {
        if (auto *Amt = dyn_cast<ConstantInt>(I.getOperand(1))) {
          if (Amt->isOne())
            HasShift = true;
        }
      }
      // Check for (x & 1) == 0 test (trailing zero check)
      if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
        if (Cmp->getPredicate() == ICmpInst::ICMP_EQ ||
            Cmp->getPredicate() == ICmpInst::ICMP_NE) {
          for (unsigned OpI = 0; OpI < 2; ++OpI) {
            if (auto *AndI =
                    dyn_cast<BinaryOperator>(Cmp->getOperand(OpI))) {
              if (AndI->getOpcode() == Instruction::And) {
                if (auto *One = dyn_cast<ConstantInt>(AndI->getOperand(1))) {
                  if (One->isOne())
                    HasMaskTest = true;
                }
              }
            }
          }
        }
      }
    }
  }

  if (!HasShift)
    return false;

  // Check if there's an increment (the count variable).
  bool HasIncrement = false;
  for (BasicBlock *BB : L->blocks()) {
    for (const Instruction &I : *BB) {
      if (I.getOpcode() == Instruction::Add) {
        if (auto *One = dyn_cast<ConstantInt>(I.getOperand(1))) {
          if (One->isOne())
            HasIncrement = true;
        }
      }
    }
  }

  if (!HasIncrement)
    return false;

  // If there's a mask test of bit 0, it's trailing; otherwise leading.
  IsTrailing = HasMaskTest;
  return true;
}

void PerfIRPass::checkBitManipulation(Function &F,
                                      FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();
    BasicBlock *Header = L->getHeader();
    Instruction *Rep = Header && !Header->empty() ? &Header->front() : nullptr;

    // Check for popcount loop pattern.
    if (isPopcountLoop(L)) {
      unsigned Score = scaleByLoopDepth(Impact::Medium, Depth);
      // Placeholder: use StrengthReduction until BitManipulation enum is added
      Collector.addHint(makeIRHint(
          HintCategory::StrengthReduction, Score, Rep,
          "Loop appears to implement a manual population count (popcount) "
          "via bit manipulation.",
          "Replace with __builtin_popcount() / __builtin_popcountll() which "
          "maps to a single hardware instruction (POPCNT) on modern CPUs. "
          "Compile with -mpopcnt or -march=native.",
          F.getName()));
      continue; // Don't also flag as count-zeros.
    }

    // Check for leading/trailing zero counting loops.
    bool IsTrailing = false;
    if (isCountZerosLoop(L, IsTrailing)) {
      unsigned Score = scaleByLoopDepth(Impact::Medium, Depth);
      std::string Builtin =
          IsTrailing ? "__builtin_ctz() / __builtin_ctzll()"
                     : "__builtin_clz() / __builtin_clzll()";
      std::string Kind = IsTrailing ? "trailing" : "leading";
      // Placeholder: use StrengthReduction until BitManipulation enum is added
      Collector.addHint(makeIRHint(
          HintCategory::StrengthReduction, Score, Rep,
          "Loop appears to implement manual " + Kind +
              " zero counting via shift and test.",
          "Replace with " + Builtin +
              " which maps to a single hardware instruction (BSF/BSR or "
              "TZCNT/LZCNT) on modern CPUs.",
          F.getName()));
    }
  }
}

// ---------------------------------------------------------------------------
// Redundant atomics — sequential atomic ops on the same address
// ---------------------------------------------------------------------------

void PerfIRPass::checkRedundantAtomics(Function &F) {
  for (BasicBlock &BB : F) {
    // Track the last atomic operation on each pointer within this BB.
    // Key: pointer operand, Value: the last atomic instruction.
    DenseMap<Value *, Instruction *> LastAtomicOnAddr;

    for (Instruction &I : BB) {
      Value *Ptr = nullptr;
      bool IsAtomic = false;

      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->isAtomic()) {
          Ptr = LI->getPointerOperand();
          IsAtomic = true;
        }
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (SI->isAtomic()) {
          Ptr = SI->getPointerOperand();
          IsAtomic = true;
        }
      } else if (auto *RMW = dyn_cast<AtomicRMWInst>(&I)) {
        Ptr = RMW->getPointerOperand();
        IsAtomic = true;
      } else if (auto *CX = dyn_cast<AtomicCmpXchgInst>(&I)) {
        Ptr = CX->getPointerOperand();
        IsAtomic = true;
      }

      if (!IsAtomic)
        continue;

      // Check if there's a previous atomic on the same address.
      auto It = LastAtomicOnAddr.find(Ptr);
      if (It != LastAtomicOnAddr.end()) {
        // Found sequential atomics on the same address — check if there
        // are any intervening atomics on OTHER addresses (which would act
        // as a barrier and make this intentional).
        bool InterveningOtherAtomic = false;
        Instruction *Prev = It->second;
        for (auto ChkIt = std::next(Prev->getIterator());
             &*ChkIt != &I; ++ChkIt) {
          bool ChkAtomic = false;
          if (auto *ChkLd = dyn_cast<LoadInst>(&*ChkIt))
            ChkAtomic = ChkLd->isAtomic();
          else if (auto *ChkSt = dyn_cast<StoreInst>(&*ChkIt))
            ChkAtomic = ChkSt->isAtomic();
          else if (isa<AtomicRMWInst>(&*ChkIt) ||
                   isa<AtomicCmpXchgInst>(&*ChkIt))
            ChkAtomic = true;
          else if (isa<FenceInst>(&*ChkIt))
            ChkAtomic = true;

          if (ChkAtomic) {
            InterveningOtherAtomic = true;
            break;
          }
        }

        if (!InterveningOtherAtomic) {
          unsigned Score = static_cast<unsigned>(Impact::High);
          // Placeholder: use RedundantLoad until RedundantAtomic enum is added
          Collector.addHint(makeIRHint(
              HintCategory::RedundantLoad, Score, &I,
              "Sequential atomic operations on the same address without "
              "intervening atomic barriers — may indicate redundant "
              "synchronization.",
              "Consider batching atomic operations, combining into a single "
              "atomic RMW, or relaxing memory ordering to "
              "std::memory_order_relaxed if strict ordering is not required.",
              F.getName()));
        }
      }

      LastAtomicOnAddr[Ptr] = &I;
    }
  }
}

// ---------------------------------------------------------------------------
// Cache line split — struct fields spanning 64-byte boundary in loops
// ---------------------------------------------------------------------------

void PerfIRPass::checkCacheLineSplits(Function &F,
                                      FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  const DataLayout &DL = F.getParent()->getDataLayout();
  constexpr uint64_t CacheLineSize = 64;

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();

    // Collect GEP accesses into struct types within this loop.
    // Map: struct type -> set of (field index, field offset, field size).
    struct FieldAccess {
      unsigned FieldIdx;
      uint64_t Offset;
      uint64_t Size;
      Instruction *Rep;
    };
    DenseMap<Type *, SmallVector<FieldAccess, 8>> StructAccesses;

    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        auto *GEP = dyn_cast<GetElementPtrInst>(&I);
        if (!GEP || GEP->getNumIndices() < 2)
          continue;

        Type *SrcTy = GEP->getSourceElementType();
        auto *STy = dyn_cast<StructType>(SrcTy);
        if (!STy)
          continue;

        // Get the struct field index (last constant index).
        auto IdxIt = GEP->idx_end();
        --IdxIt;
        auto *FieldCI = dyn_cast<ConstantInt>(IdxIt->get());
        if (!FieldCI)
          continue;

        unsigned FieldIdx = FieldCI->getZExtValue();
        if (FieldIdx >= STy->getNumElements())
          continue;

        const StructLayout *SL = DL.getStructLayout(STy);
        uint64_t FieldOffset = SL->getElementOffset(FieldIdx);
        uint64_t FieldSize =
            DL.getTypeAllocSize(STy->getElementType(FieldIdx));

        StructAccesses[STy].push_back({FieldIdx, FieldOffset, FieldSize, &I});
      }
    }

    // For each struct type accessed, check if any pair of accessed fields
    // spans a cache line boundary.
    for (auto &[STy, Accesses] : StructAccesses) {
      if (Accesses.size() < 2)
        continue;

      // Deduplicate by field index.
      SmallSet<unsigned, 8> SeenFields;
      SmallVector<FieldAccess, 8> UniqueAccesses;
      for (auto &FA : Accesses) {
        if (SeenFields.insert(FA.FieldIdx).second)
          UniqueAccesses.push_back(FA);
      }

      if (UniqueAccesses.size() < 2)
        continue;

      // Check pairs: do two accessed fields straddle a 64-byte boundary?
      for (unsigned I = 0; I < UniqueAccesses.size(); ++I) {
        for (unsigned J = I + 1; J < UniqueAccesses.size(); ++J) {
          uint64_t StartA = UniqueAccesses[I].Offset;
          uint64_t EndA = StartA + UniqueAccesses[I].Size;
          uint64_t StartB = UniqueAccesses[J].Offset;

          uint64_t MinOffset = std::min(StartA, StartB);
          uint64_t MaxEnd = std::max(EndA, StartB + UniqueAccesses[J].Size);

          // If the fields together span more than one cache line from
          // the start of the struct, flag it.
          uint64_t FirstCacheLine = MinOffset / CacheLineSize;
          uint64_t LastCacheLine = (MaxEnd - 1) / CacheLineSize;

          if (FirstCacheLine != LastCacheLine) {
            unsigned Score = scaleByLoopDepth(Impact::Medium, Depth);
            // Placeholder: use DataLayout until CacheLineSplit enum is added
            Collector.addHint(makeIRHint(
                HintCategory::DataLayout, Score, UniqueAccesses[I].Rep,
                "Loop accesses struct fields at offsets " +
                    std::to_string(UniqueAccesses[I].Offset) + " and " +
                    std::to_string(UniqueAccesses[J].Offset) +
                    " which span a 64-byte cache line boundary.",
                "Reorder struct fields to keep hot fields within the same "
                "cache line, or split the struct into hot/cold parts. "
                "Consider using __attribute__((aligned(64))) for "
                "cache-line alignment.",
                F.getName()));
            goto next_struct; // One hint per struct type per loop.
          }
        }
      }
    next_struct:;
    }
  }
}

// ---------------------------------------------------------------------------
// Cross-TU inlining — calls to small external functions in loops
// ---------------------------------------------------------------------------

void PerfIRPass::checkCrossTUInlining(Function &F,
                                      FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (BasicBlock &BB : F) {
    unsigned LoopDepth = LI.getLoopDepth(&BB);
    if (LoopDepth == 0)
      continue;

    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;

      Function *Callee = CI->getCalledFunction();
      if (!Callee || !Callee->isDeclaration() || Callee->isIntrinsic())
        continue;

      // Skip well-known runtime/library functions that are expected to be
      // external (malloc, free, math functions, etc.)
      StringRef Name = Callee->getName();
      if (Name.starts_with("llvm.") || Name.starts_with("__") ||
          Name == "malloc" || Name == "free" || Name == "calloc" ||
          Name == "realloc" || Name == "memcpy" || Name == "memset" ||
          Name == "memmove" || Name == "printf" || Name == "puts" ||
          Name == "fprintf" || Name == "sqrt" || Name == "sin" ||
          Name == "cos" || Name == "exp" || Name == "log" ||
          Name == "pow" || Name == "fabs")
        continue;

      // Heuristic: the function has a small number of parameters (likely
      // small) and is called in a loop — a good LTO candidate.
      if (Callee->arg_size() <= 4) {
        unsigned Score = scaleByLoopDepth(Impact::Medium, LoopDepth);
        // Placeholder: use InliningCandidate until CrossTUInlining enum added
        Collector.addHint(makeIRHint(
            HintCategory::InliningCandidate, Score, &I,
            "Call to external function '" + Callee->getName().str() +
                "' (declaration only, " +
                std::to_string(Callee->arg_size()) +
                " args) inside loop at depth " +
                std::to_string(LoopDepth) +
                " — definition is in another translation unit and cannot "
                "be inlined.",
            "Enable Link-Time Optimization (-flto) to allow cross-TU "
            "inlining, or move the function definition to a header file "
            "as an inline function.",
            F.getName()));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Hot/cold function attributes — PGO mismatch detection
// ---------------------------------------------------------------------------

void PerfIRPass::checkHotColdFunction(Function &F,
                                      FunctionAnalysisManager &FAM) {
  // This check requires BlockFrequencyInfo which is only meaningful with PGO.
  // If no profile data is available, getBlockProfileCount returns None.
  if (!F.hasProfileData())
    return;

  auto &BFI = FAM.getResult<BlockFrequencyAnalysis>(F);

  // Sum up the entry block frequency as a proxy for function hotness.
  BasicBlock &Entry = F.getEntryBlock();
  auto EntryCount = BFI.getBlockProfileCount(&Entry);
  if (!EntryCount)
    return;

  uint64_t Count = *EntryCount;

  bool HasHotAttr = F.hasFnAttribute(Attribute::Hot);
  bool HasColdAttr = F.hasFnAttribute(Attribute::Cold);

  // Thresholds are heuristic; in practice these should be tunable.
  constexpr uint64_t HotThreshold = 1000;
  constexpr uint64_t ColdThreshold = 10;

  Instruction *Rep = Entry.empty() ? nullptr : &Entry.front();

  if (Count >= HotThreshold && !HasHotAttr) {
    unsigned Score = static_cast<unsigned>(Impact::Low);
    // Placeholder: use HotColdSplit until HotColdFunction enum is added
    Collector.addHint(makeIRHint(
        HintCategory::HotColdSplit, Score, Rep,
        "Function '" + F.getName().str() +
            "' has high execution count (" + std::to_string(Count) +
            ") from PGO profile but is not marked __attribute__((hot)).",
        "Add __attribute__((hot)) to prioritize this function for "
        "optimization: aggressive inlining, better code placement, and "
        "higher optimization effort.",
        F.getName()));
  }

  if (Count <= ColdThreshold && !HasColdAttr && Count > 0) {
    unsigned Score = static_cast<unsigned>(Impact::Low);
    // Placeholder: use HotColdSplit until HotColdFunction enum is added
    Collector.addHint(makeIRHint(
        HintCategory::HotColdSplit, Score, Rep,
        "Function '" + F.getName().str() +
            "' has very low execution count (" + std::to_string(Count) +
            ") from PGO profile but is not marked __attribute__((cold)).",
        "Add __attribute__((cold)) to deprioritize this function: it will "
        "be optimized for size instead of speed and placed in a cold "
        "section to improve instruction cache utilization.",
        F.getName()));
  }
}

// ---------------------------------------------------------------------------
// Spill pressure — excessive alloca-based loads/stores
// ---------------------------------------------------------------------------

void PerfIRPass::checkSpillPressure(Function &F) {
  // Collect allocas that are not simple single-use SROA candidates (i.e.,
  // they have more than a handful of load/store users, suggesting the
  // backend will need to spill them to the stack).
  unsigned AllocaLoadStoreCount = 0;
  Instruction *FirstAlloca = nullptr;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI)
        continue;

      if (!FirstAlloca)
        FirstAlloca = AI;

      // Count loads and stores that directly use this alloca.
      for (User *U : AI->users()) {
        if (isa<LoadInst>(U) || isa<StoreInst>(U))
          ++AllocaLoadStoreCount;
      }
    }
  }

  if (AllocaLoadStoreCount > 20 && FirstAlloca) {
    unsigned Score = static_cast<unsigned>(Impact::Medium);
    Collector.addHint(makeIRHint(
        HintCategory::SROAEscape, Score, FirstAlloca,
        "Function has " + std::to_string(AllocaLoadStoreCount) +
            " loads/stores to stack allocations — potential register "
            "pressure issue causing excessive spills/reloads.",
        "Reduce the number of live local variables, break the function "
        "into smaller pieces, or mark infrequently-used variables as "
        "'volatile' to hint they can stay in memory.",
        F.getName()));
  }
}

// ---------------------------------------------------------------------------
// Unrolling blockers — unknown trip count + function calls in loop
// ---------------------------------------------------------------------------

void PerfIRPass::checkUnrollingBlockers(Function &F,
                                        FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();

    // Check if trip count is unknown.
    const SCEV *BackedgeTakenCount = SE.getBackedgeTakenCount(L);
    if (!isa<SCEVCouldNotCompute>(BackedgeTakenCount))
      continue;

    // Check for non-intrinsic function calls in the loop.
    bool HasFunctionCall = false;
    Instruction *CallSite = nullptr;
    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          Function *Callee = CI->getCalledFunction();
          if (!Callee || !Callee->isIntrinsic()) {
            HasFunctionCall = true;
            CallSite = &I;
            break;
          }
        }
      }
      if (HasFunctionCall)
        break;
    }

    if (!HasFunctionCall)
      continue;

    unsigned Score = scaleByLoopDepth(Impact::High, Depth);
    Collector.addHint(makeIRHint(
        HintCategory::LoopBound, Score, CallSite,
        "Loop has unknown trip count and contains function calls — "
        "cannot be unrolled by the compiler.",
        "Provide a compile-time upper bound for the trip count with "
        "__builtin_assume(n <= MAX), or move function calls outside "
        "the loop to enable partial unrolling.",
        F.getName()));
  }
}

// ---------------------------------------------------------------------------
// Division chain — repeated sdiv/udiv with same divisor in loop
// ---------------------------------------------------------------------------

void PerfIRPass::checkDivisionChainIR(Function &F,
                                      FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();

    for (BasicBlock *BB : L->blocks()) {
      // Map divisor Value* to the list of div instructions using it.
      DenseMap<Value *, SmallVector<Instruction *, 4>> DivisorMap;

      for (Instruction &I : *BB) {
        unsigned Opcode = I.getOpcode();
        if (Opcode != Instruction::SDiv && Opcode != Instruction::UDiv)
          continue;

        Value *Divisor = I.getOperand(1);
        DivisorMap[Divisor].push_back(&I);
      }

      for (auto &[Divisor, Divs] : DivisorMap) {
        if (Divs.size() < 2)
          continue;

        unsigned Score = scaleByLoopDepth(Impact::Medium, Depth);
        Collector.addHint(makeIRHint(
            HintCategory::StrengthReduction, Score, Divs.front(),
            "Multiple division instructions (" +
                std::to_string(Divs.size()) +
                ") with the same divisor in a loop basic block — "
                "redundant expensive operations.",
            "Precompute the reciprocal of the divisor once before the "
            "loop and multiply instead of dividing, or refactor to "
            "share the division result.",
            F.getName()));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Branch on float — fcmp + br patterns inside loops
// ---------------------------------------------------------------------------

void PerfIRPass::checkBranchOnFloat(Function &F,
                                    FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (BasicBlock &BB : F) {
    unsigned LoopDepth = LI.getLoopDepth(&BB);
    if (LoopDepth == 0)
      continue;

    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;

    // Check if the branch condition is an fcmp.
    if (auto *FC = dyn_cast<FCmpInst>(BI->getCondition())) {
      unsigned Score = scaleByLoopDepth(Impact::Low, LoopDepth);
      Collector.addHint(makeIRHint(
          HintCategory::BranchPrediction, Score, FC,
          "Branch conditioned on floating-point comparison inside loop — "
          "FP comparisons have higher latency than integer comparisons "
          "and may cause pipeline stalls.",
          "If possible, convert the comparison to integer arithmetic "
          "(e.g., compare bit representations or use integer thresholds), "
          "or restructure to use branchless select (ternary operator).",
          F.getName()));
    }
  }
}

// ---------------------------------------------------------------------------
// Memory access pattern — non-sequential GEP strides in loops
// ---------------------------------------------------------------------------

void PerfIRPass::checkMemoryAccessPattern(Function &F,
                                          FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);

  for (Loop *L : LI.getLoopsInPreorder()) {
    unsigned Depth = L->getLoopDepth();

    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        auto *GEP = dyn_cast<GetElementPtrInst>(&I);
        if (!GEP)
          continue;

        // Check each index for a non-unit stride AddRec.
        for (auto Idx = GEP->idx_begin(); Idx != GEP->idx_end(); ++Idx) {
          const SCEV *IdxSCEV = SE.getSCEV(Idx->get());
          auto *AR = dyn_cast<SCEVAddRecExpr>(IdxSCEV);
          if (!AR || AR->getLoop() != L)
            continue;

          // Check if the step is a constant greater than 1.
          if (auto *StepConst = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE))) {
            int64_t Step = StepConst->getAPInt().getSExtValue();
            if (Step > 1 || Step < -1) {
              unsigned Score = scaleByLoopDepth(Impact::Medium, Depth);
              Collector.addHint(makeIRHint(
                  HintCategory::Vectorization, Score, &I,
                  "GEP index has stride " + std::to_string(Step) +
                      " per iteration — non-sequential memory access "
                      "pattern hurts hardware prefetching and cache "
                      "utilization.",
                  "Restructure data layout or loop order for unit-stride "
                  "(sequential) access. Consider tiling/blocking the loop "
                  "or transposing the data structure.",
                  F.getName()));
              break; // One hint per GEP.
            }
          }
        }
      }
    }
  }
}
