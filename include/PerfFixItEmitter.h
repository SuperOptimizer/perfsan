//===- PerfFixItEmitter.h - Emit PerfHints as Clang FixIts ------*- C++ -*-===//
//
// Part of the PerfSanitizer project.
// Translates PerfHints into Clang diagnostics with optional FixItHints,
// enabling `clang -fplugin=PerfSanitizer.so -fix` to auto-apply suggestions.
//
//===----------------------------------------------------------------------===//

#ifndef PERFSANITIZER_PERFFIXITEMITTER_H
#define PERFSANITIZER_PERFFIXITEMITTER_H

#include "PerfHint.h"
#include "clang/Basic/Diagnostic.h" // FixItHint is defined here
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include <optional>

namespace perfsanitizer {

/// Takes PerfHints and emits them as Clang diagnostics, attaching mechanical
/// FixItHints where possible so that `-fix` can auto-apply the suggestions.
class PerfFixItEmitter {
public:
  PerfFixItEmitter(clang::DiagnosticsEngine &Diag, clang::SourceManager &SM);

  /// Emit a PerfHint as a clang diagnostic with optional FixItHint.
  void emit(const PerfHint &Hint);

  /// Try to generate a FixItHint for hints that have mechanical fixes.
  /// Returns std::nullopt for categories where no automatic rewrite is safe.
  std::optional<clang::FixItHint> generateFixIt(const PerfHint &Hint);

private:
  clang::DiagnosticsEngine &Diag;
  clang::SourceManager &SM;
  unsigned DiagID_Warning;
  unsigned DiagID_Remark;
  unsigned DiagID_Note;
};

/// Convenience: emit every hint in a collector as a Clang diagnostic.
void emitAllHintsAsDiagnostics(clang::DiagnosticsEngine &Diag,
                               clang::SourceManager &SM,
                               const PerfHintCollector &Collector);

} // namespace perfsanitizer

#endif // PERFSANITIZER_PERFFIXITEMITTER_H
