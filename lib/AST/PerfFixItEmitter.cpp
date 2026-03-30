//===- PerfFixItEmitter.cpp - Emit PerfHints as Clang FixIts --------------===//
//
// Part of the PerfSanitizer project.
// Translates PerfHints into Clang diagnostics with optional FixItHints.
//
//===----------------------------------------------------------------------===//

#include "PerfFixItEmitter.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/StringRef.h"

using namespace clang;

namespace perfsanitizer {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PerfFixItEmitter::PerfFixItEmitter(DiagnosticsEngine &Diag, SourceManager &SM)
    : Diag(Diag), SM(SM) {
  // Register three custom diagnostic IDs, one per severity.
  DiagID_Warning = Diag.getCustomDiagID(DiagnosticsEngine::Warning,
                                         "perf-sanitizer: %0");
  DiagID_Remark = Diag.getCustomDiagID(DiagnosticsEngine::Remark,
                                        "perf-sanitizer: %0");
  DiagID_Note =
      Diag.getCustomDiagID(DiagnosticsEngine::Note, "perf-sanitizer: %0");
}

// ---------------------------------------------------------------------------
// Helpers for source-level token scanning
// ---------------------------------------------------------------------------

/// Return the SourceLocation immediately after the token at \p Loc.
static SourceLocation getLocAfterToken(SourceLocation Loc,
                                       const SourceManager &SM,
                                       const LangOptions &LangOpts) {
  return Lexer::getLocForEndOfToken(Loc, /*Offset=*/0, SM, LangOpts);
}

/// Starting from \p Loc, scan forward through the source buffer looking for
/// the first occurrence of the character \p C.  Returns an invalid
/// SourceLocation on failure.
static SourceLocation findCharAfter(SourceLocation Loc, char C,
                                    const SourceManager &SM) {
  if (Loc.isInvalid())
    return {};
  bool Invalid = false;
  const char *Buf = SM.getCharacterData(Loc, &Invalid);
  if (Invalid || !Buf)
    return {};
  // Scan a bounded distance (2 KiB) to avoid running off the buffer.
  for (unsigned I = 0; I < 2048; ++I) {
    if (Buf[I] == C)
      return Loc.getLocWithOffset(I);
    if (Buf[I] == '\0')
      break;
  }
  return {};
}

/// Starting from \p Loc, scan forward for a keyword token (e.g. "const",
/// "int").  Returns the CharSourceRange covering that token, or an invalid
/// range on failure.
static CharSourceRange findKeywordRange(SourceLocation Loc,
                                        llvm::StringRef Keyword,
                                        const SourceManager &SM) {
  if (Loc.isInvalid())
    return {};
  bool Invalid = false;
  const char *Buf = SM.getCharacterData(Loc, &Invalid);
  if (Invalid || !Buf)
    return {};
  llvm::StringRef Text(Buf, 2048);
  size_t Pos = Text.find(Keyword);
  if (Pos == llvm::StringRef::npos)
    return {};
  // Verify that it is a whole token (not a substring of another identifier).
  if (Pos > 0 && clang::isAsciiIdentifierContinue(Text[Pos - 1]))
    return {};
  if (Pos + Keyword.size() < Text.size() &&
      clang::isAsciiIdentifierContinue(Text[Pos + Keyword.size()]))
    return {};
  SourceLocation Start = Loc.getLocWithOffset(Pos);
  SourceLocation End = Start.getLocWithOffset(Keyword.size());
  return CharSourceRange::getCharRange(Start, End);
}

// ---------------------------------------------------------------------------
// generateFixIt — mechanical rewrites for known categories
// ---------------------------------------------------------------------------

std::optional<FixItHint> PerfFixItEmitter::generateFixIt(const PerfHint &Hint) {
  // FixIts require valid source locations (AST-level hints only).
  if (Hint.SrcLoc.isInvalid())
    return std::nullopt;

  switch (Hint.Category) {

  // 1. ConstexprPromotion — replace "const" with "constexpr".
  case HintCategory::ConstexprPromotion: {
    CharSourceRange Range = findKeywordRange(Hint.SrcLoc, "const", SM);
    if (Range.isInvalid())
      return std::nullopt;
    return FixItHint::CreateReplacement(Range, "constexpr");
  }

  // 2. NoExcept — insert " noexcept" after the closing paren of the
  //    function parameter list.
  case HintCategory::NoExcept: {
    SourceLocation RParen = findCharAfter(Hint.SrcLoc, ')', SM);
    if (RParen.isInvalid())
      return std::nullopt;
    // Insert right after the ')'.
    SourceLocation InsertLoc = RParen.getLocWithOffset(1);
    return FixItHint::CreateInsertion(InsertLoc, " noexcept");
  }

  // 3. MissingNodiscard — insert "[[nodiscard]] " before the return type
  //    (i.e. at the very start of the declaration).
  case HintCategory::MissingNodiscard: {
    return FixItHint::CreateInsertion(Hint.SrcLoc, "[[nodiscard]] ");
  }

  // 4. SignedLoopCounter — replace "int" with "size_t" in for-loop init.
  case HintCategory::SignedLoopCounter: {
    CharSourceRange Range = findKeywordRange(Hint.SrcLoc, "int", SM);
    if (Range.isInvalid())
      return std::nullopt;
    return FixItHint::CreateReplacement(Range, "size_t");
  }

  // 5. BranchPrediction — insert "[[unlikely]] " after the opening paren of
  //    the if-condition.  (The hint's SrcLoc points at the `if` keyword.)
  case HintCategory::BranchPrediction: {
    SourceLocation LParen = findCharAfter(Hint.SrcLoc, '(', SM);
    if (LParen.isInvalid())
      return std::nullopt;
    SourceLocation InsertLoc = LParen.getLocWithOffset(1);
    return FixItHint::CreateInsertion(InsertLoc, "[[unlikely]] ");
  }

  default:
    // No mechanical fix available for this category.
    return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// emit — route a PerfHint through DiagnosticsEngine
// ---------------------------------------------------------------------------

void PerfFixItEmitter::emit(const PerfHint &Hint) {
  // Pick diagnostic ID based on score.
  unsigned ID;
  if (Hint.Score >= 90)
    ID = DiagID_Warning;
  else if (Hint.Score >= 40)
    ID = DiagID_Remark;
  else
    ID = DiagID_Note;

  // Build the diagnostic text: combine message and suggestion.
  std::string Text = Hint.Message;
  if (!Hint.Suggestion.empty()) {
    Text += " [suggestion: ";
    Text += Hint.Suggestion;
    Text += "]";
  }

  // Use real SourceLocation when available, otherwise fall back to invalid
  // (the diagnostic engine will omit the location).
  SourceLocation Loc = Hint.SrcLoc;

  // Try to generate a mechanical fix.
  std::optional<FixItHint> Fix = generateFixIt(Hint);

  if (Fix) {
    Diag.Report(Loc, ID) << Text << *Fix;
  } else {
    Diag.Report(Loc, ID) << Text;
  }
}

// ---------------------------------------------------------------------------
// Free function: emit every collected hint
// ---------------------------------------------------------------------------

void emitAllHintsAsDiagnostics(DiagnosticsEngine &Diag, SourceManager &SM,
                               const PerfHintCollector &Collector) {
  PerfFixItEmitter Emitter(Diag, SM);
  for (const PerfHint &H : Collector.getHints())
    Emitter.emit(H);
}

} // namespace perfsanitizer
