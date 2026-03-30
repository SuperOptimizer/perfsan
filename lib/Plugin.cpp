//===- Plugin.cpp - PerfSanitizer plugin registration ---------------------===//
//
// Registers both the Clang frontend plugin (AST analysis) and the LLVM
// pass plugin (IR analysis). Supports fix/diff/diag/quiet modes.
//
// Usage:
//   clang++ -fplugin=PerfSanitizer.so -O2 -c file.cpp          # report mode
//   clang++ -fplugin=PerfSanitizer.so -Xclang -plugin-arg-perf-sanitizer -Xclang fix -O2 -c file.cpp    # auto-fix
//   clang++ -fplugin=PerfSanitizer.so -Xclang -plugin-arg-perf-sanitizer -Xclang diff -O2 -c file.cpp   # show diff
//   clang++ -fplugin=PerfSanitizer.so -Xclang -plugin-arg-perf-sanitizer -Xclang quiet -O2 -c file.cpp  # count only
//
//===----------------------------------------------------------------------===//

#include "PerfASTVisitor.h"
#include "PerfHint.h"
#include "PerfIRPass.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

using namespace perfsanitizer;

// ===== Clang Frontend Plugin =====

namespace {

class PerfSanitizerAction : public clang::PluginASTAction {
  PerfOutputMode Mode = PerfOutputMode::Report;
  unsigned MinScore = 0;

public:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override {
    return std::make_unique<PerfASTConsumer>(CI,
                                             PerfHintCollector::instance(),
                                             Mode, MinScore);
  }

  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &Args) override {
    for (const auto &Arg : Args) {
      if (Arg == "fix")
        Mode = PerfOutputMode::Fix;
      else if (Arg == "diff")
        Mode = PerfOutputMode::Diff;
      else if (Arg == "diag")
        Mode = PerfOutputMode::Diag;
      else if (Arg == "quiet")
        Mode = PerfOutputMode::Quiet;
      else if (Arg.size() > 10 && Arg.substr(0, 10) == "min-score=")
        MinScore = std::stoi(Arg.substr(10));
    }
    return true;
  }

  ActionType getActionType() override { return AddBeforeMainAction; }
};

} // namespace

static clang::FrontendPluginRegistry::Add<PerfSanitizerAction>
    X("perf-sanitizer",
      "Suggest source-level changes to improve codegen quality");

// ===== LLVM Pass Plugin =====

static void registerPerfIRPass(llvm::PassBuilder &PB) {
  PB.registerOptimizerLastEPCallback(
      [](llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level,
         llvm::ThinOrFullLTOPhase) {
        MPM.addPass(PerfIRPass(PerfHintCollector::instance()));
      });

  PB.registerPipelineParsingCallback(
      [](llvm::StringRef Name, llvm::ModulePassManager &MPM,
         llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
        if (Name == "perf-sanitizer") {
          MPM.addPass(PerfIRPass(PerfHintCollector::instance()));
          return true;
        }
        return false;
      });
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "PerfSanitizer", LLVM_VERSION_STRING,
          registerPerfIRPass};
}
