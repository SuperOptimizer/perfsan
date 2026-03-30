//===- Plugin.cpp - PerfSanitizer plugin registration ---------------------===//
//
// Registers both the Clang frontend plugin (AST analysis) and the LLVM
// pass plugin (IR analysis). When loaded, hints from both layers are
// collected, deduplicated, sorted by impact score, and emitted.
//
//===----------------------------------------------------------------------===//

#include "PerfASTVisitor.h"
#include "PerfHint.h"
#include "PerfIRPass.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

using namespace perfsanitizer;

// ===== Clang Frontend Plugin =====

namespace {

class PerfSanitizerAction : public clang::PluginASTAction {
public:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override {
    return std::make_unique<PerfASTConsumer>(CI,
                                             PerfHintCollector::instance());
  }

  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &Args) override {
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
