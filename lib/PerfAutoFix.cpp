//===- PerfAutoFix.cpp - Auto-fix generation for perf hints ---------------===//
//
// Part of the PerfSanitizer project.
// Generates source-level fixes for performance hints using clang::Rewriter.
//
//===----------------------------------------------------------------------===//

#include "PerfAutoFix.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/ADT/RewriteBuffer.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

using namespace clang;

namespace perfsanitizer {

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Return true if Loc is valid and not inside a system header.
static bool isFixableLoc(SourceLocation Loc, const SourceManager &SM) {
  return Loc.isValid() && !SM.isInSystemHeader(Loc);
}

/// Read up to \p MaxLen characters of source text starting at \p Loc.
static StringRef getSourceSnippet(SourceLocation Loc, unsigned MaxLen,
                                  const SourceManager &SM,
                                  const LangOptions &LO) {
  if (!Loc.isValid())
    return {};
  bool Invalid = false;
  const char *Start = SM.getCharacterData(Loc, &Invalid);
  if (Invalid || !Start)
    return {};
  // Don't read past end of buffer.
  FileID FID = SM.getFileID(Loc);
  unsigned Offset = SM.getFileOffset(Loc);
  unsigned BufSize = SM.getBufferData(FID).size();
  unsigned Available = BufSize > Offset ? BufSize - Offset : 0;
  unsigned Len = std::min(MaxLen, Available);
  return StringRef(Start, Len);
}

/// Find the location of the first occurrence of \p Needle in the source text
/// starting at \p Loc, searching up to \p MaxScan characters forward.
static SourceLocation findTextAfter(SourceLocation Loc, StringRef Needle,
                                    unsigned MaxScan,
                                    const SourceManager &SM,
                                    const LangOptions &LO) {
  StringRef Buf = getSourceSnippet(Loc, MaxScan, SM, LO);
  if (Buf.empty())
    return {};
  size_t Pos = Buf.find(Needle);
  if (Pos == StringRef::npos)
    return {};
  return Loc.getLocWithOffset(Pos);
}

/// Find the location immediately after the first occurrence of \p Needle.
static SourceLocation findLocAfterText(SourceLocation Loc, StringRef Needle,
                                       unsigned MaxScan,
                                       const SourceManager &SM,
                                       const LangOptions &LO) {
  SourceLocation Found = findTextAfter(Loc, Needle, MaxScan, SM, LO);
  if (Found.isInvalid())
    return {};
  return Found.getLocWithOffset(Needle.size());
}

//===----------------------------------------------------------------------===//
// PerfAutoFixer
//===----------------------------------------------------------------------===//

PerfAutoFixer::PerfAutoFixer(SourceManager &SM, const LangOptions &LangOpts)
    : SM(SM) {
  Rewrite.setSourceMgr(SM, LangOpts);
}

std::vector<AutoFix> PerfAutoFixer::generateFixes(const PerfHint &Hint,
                                                   ASTContext &Ctx) {
  if (!isFixableLoc(Hint.SrcLoc, SM))
    return {};

  switch (Hint.Category) {
  case HintCategory::ConstexprPromotion:
    return fixConstexprPromotion(Hint, Ctx);
  case HintCategory::NoExcept:
    return fixNoexcept(Hint, Ctx);
  case HintCategory::MissingNodiscard:
    return fixNodiscard(Hint, Ctx);
  case HintCategory::BranchPrediction:
    return fixBranchPrediction(Hint, Ctx);
  case HintCategory::SignedLoopCounter:
    return fixSignedLoopCounter(Hint, Ctx);
  case HintCategory::StrengthReduction:
    return fixPreIncrement(Hint, Ctx);
  case HintCategory::ContainerReserve:
    return fixEmptyCheck(Hint, Ctx);
  case HintCategory::RestrictAnnotation:
    return fixRestrict(Hint, Ctx);
  case HintCategory::ConstevalPromotion:
    return fixConstVariable(Hint, Ctx);
  default:
    return {};
  }
}

//===----------------------------------------------------------------------===//
// Fix application and output
//===----------------------------------------------------------------------===//

bool PerfAutoFixer::applyFixes(const std::vector<AutoFix> &Fixes) {
  bool AllOk = true;
  for (const AutoFix &F : Fixes) {
    if (!F.Loc.isValid()) {
      AllOk = false;
      continue;
    }
    switch (F.FixKind) {
    case AutoFix::Insert:
      if (Rewrite.InsertText(F.Loc, F.NewText, /*InsertAfter=*/false))
        AllOk = false;
      break;
    case AutoFix::InsertAfter:
      if (Rewrite.InsertText(F.Loc, F.NewText, /*InsertAfter=*/true))
        AllOk = false;
      break;
    case AutoFix::Replace: {
      // Find the old text near Loc and replace it.
      const LangOptions &LO = Rewrite.getLangOpts();
      SourceLocation Found =
          findTextAfter(F.Loc, F.OldText, /*MaxScan=*/512, SM, LO);
      if (Found.isInvalid()) {
        AllOk = false;
        break;
      }
      SourceLocation End = Found.getLocWithOffset(F.OldText.size());
      if (Rewrite.ReplaceText(SourceRange(Found, End.getLocWithOffset(-1)),
                              F.NewText))
        AllOk = false;
      break;
    }
    }
  }
  return AllOk;
}

bool PerfAutoFixer::writeFixedFiles() {
  return Rewrite.overwriteChangedFiles();
}

std::string PerfAutoFixer::getDiff() {
  std::string Result;
  llvm::raw_string_ostream OS(Result);

  for (auto It = Rewrite.buffer_begin(), End = Rewrite.buffer_end();
       It != End; ++It) {
    FileID FID = It->first;
    const llvm::RewriteBuffer &RB = It->second;

    OptionalFileEntryRef Entry = SM.getFileEntryRefForID(FID);
    StringRef FileName = Entry ? Entry->getName() : "<unknown>";

    // Get original buffer.
    StringRef OrigBuf = SM.getBufferData(FID);

    // Get rewritten buffer.
    std::string NewBuf;
    llvm::raw_string_ostream NewOS(NewBuf);
    RB.write(NewOS);

    // Produce a simple unified-diff-style output.
    OS << "--- a/" << FileName << "\n";
    OS << "+++ b/" << FileName << "\n";

    // Split both into lines and produce hunks.
    SmallVector<StringRef, 256> OldLines, NewLines;
    StringRef(OrigBuf).split(OldLines, '\n', /*MaxSplit=*/-1,
                             /*KeepEmpty=*/true);
    StringRef(NewBuf).split(NewLines, '\n', /*MaxSplit=*/-1,
                            /*KeepEmpty=*/true);

    // Simple line-by-line diff: find changed regions and emit context.
    unsigned OldIdx = 0, NewIdx = 0;
    unsigned ContextLines = 3;

    while (OldIdx < OldLines.size() || NewIdx < NewLines.size()) {
      // Find next differing line.
      if (OldIdx < OldLines.size() && NewIdx < NewLines.size() &&
          OldLines[OldIdx] == NewLines[NewIdx]) {
        ++OldIdx;
        ++NewIdx;
        continue;
      }

      // Found a difference. Emit a hunk with context.
      unsigned HunkOldStart =
          OldIdx > ContextLines ? OldIdx - ContextLines : 0;
      unsigned HunkNewStart =
          NewIdx > ContextLines ? NewIdx - ContextLines : 0;

      // Find the end of the changed region.
      unsigned DiffOldEnd = OldIdx + 1;
      unsigned DiffNewEnd = NewIdx + 1;

      // Scan ahead to find where lines match again.
      while (DiffOldEnd < OldLines.size() || DiffNewEnd < NewLines.size()) {
        if (DiffOldEnd < OldLines.size() && DiffNewEnd < NewLines.size() &&
            OldLines[DiffOldEnd] == NewLines[DiffNewEnd])
          break;
        // Advance whichever side is shorter, or both.
        if (DiffOldEnd < OldLines.size())
          ++DiffOldEnd;
        if (DiffNewEnd < NewLines.size())
          ++DiffNewEnd;
      }

      unsigned HunkOldEnd =
          std::min(static_cast<unsigned>(OldLines.size()),
                   DiffOldEnd + ContextLines);
      unsigned HunkNewEnd =
          std::min(static_cast<unsigned>(NewLines.size()),
                   DiffNewEnd + ContextLines);

      OS << "@@ -" << (HunkOldStart + 1) << ","
         << (HunkOldEnd - HunkOldStart) << " +"
         << (HunkNewStart + 1) << ","
         << (HunkNewEnd - HunkNewStart) << " @@\n";

      // Context before.
      for (unsigned I = HunkOldStart; I < OldIdx; ++I)
        OS << " " << OldLines[I] << "\n";

      // Removed lines.
      for (unsigned I = OldIdx; I < DiffOldEnd; ++I)
        OS << "-" << OldLines[I] << "\n";

      // Added lines.
      for (unsigned I = NewIdx; I < DiffNewEnd; ++I)
        OS << "+" << NewLines[I] << "\n";

      // Context after.
      for (unsigned I = DiffOldEnd;
           I < HunkOldEnd && I < OldLines.size(); ++I)
        OS << " " << OldLines[I] << "\n";

      OldIdx = HunkOldEnd;
      NewIdx = HunkNewEnd;
    }
  }

  return Result;
}

//===----------------------------------------------------------------------===//
// Category-specific fix generators
//===----------------------------------------------------------------------===//

std::vector<AutoFix>
PerfAutoFixer::fixConstexprPromotion(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // Look for "const " at or near the SrcLoc and replace with "constexpr ".
  StringRef Snippet = getSourceSnippet(Loc, 128, SM, LO);
  if (Snippet.empty())
    return {};

  SourceLocation ConstLoc = findTextAfter(Loc, "const ", 128, SM, LO);
  if (ConstLoc.isInvalid())
    return {};

  // Make sure we don't accidentally match "constexpr" already there.
  StringRef AtConst = getSourceSnippet(ConstLoc, 16, SM, LO);
  if (AtConst.starts_with("constexpr"))
    return {};

  AutoFix Fix;
  Fix.Loc = ConstLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = "const ";
  Fix.NewText = "constexpr ";
  Fix.Description = "promote const to constexpr for compile-time evaluation";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixNoexcept(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // The SrcLoc points to the function declaration. We need to find the
  // closing ')' of the parameter list and insert " noexcept" after it.
  // Scan forward for the ')' that ends the parameter list. We need to
  // handle nested parens.
  StringRef Snippet = getSourceSnippet(Loc, 1024, SM, LO);
  if (Snippet.empty())
    return {};

  // Find the opening '(' first.
  size_t OpenParen = Snippet.find('(');
  if (OpenParen == StringRef::npos)
    return {};

  // Walk forward tracking paren depth.
  unsigned Depth = 0;
  size_t CloseParen = StringRef::npos;
  for (size_t I = OpenParen; I < Snippet.size(); ++I) {
    if (Snippet[I] == '(')
      ++Depth;
    else if (Snippet[I] == ')') {
      --Depth;
      if (Depth == 0) {
        CloseParen = I;
        break;
      }
    }
  }
  if (CloseParen == StringRef::npos)
    return {};

  // Check that "noexcept" isn't already present after the ')'.
  StringRef AfterClose = Snippet.substr(CloseParen + 1);
  StringRef Trimmed = AfterClose.ltrim();
  if (Trimmed.starts_with("noexcept"))
    return {};

  SourceLocation InsertLoc = Loc.getLocWithOffset(CloseParen + 1);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::InsertAfter;
  Fix.NewText = " noexcept";
  Fix.Description = "add noexcept to enable move optimizations";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixNodiscard(const PerfHint &H, ASTContext &Ctx) {
  SourceLocation Loc = H.SrcLoc;

  // Check that [[nodiscard]] isn't already present.
  const LangOptions &LO = Ctx.getLangOpts();
  StringRef Snippet = getSourceSnippet(Loc, 64, SM, LO);
  if (Snippet.empty())
    return {};
  if (Snippet.starts_with("[[nodiscard]]"))
    return {};

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "[[nodiscard]] ";
  Fix.Description = "add [[nodiscard]] to warn on ignored return values";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixBranchPrediction(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the 'if'. Find the '(' after it.
  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  size_t ParenPos = Snippet.find('(');
  if (ParenPos == StringRef::npos)
    return {};

  // Check that [[unlikely]] isn't already there.
  StringRef BeforeParen = Snippet.substr(0, ParenPos);
  if (BeforeParen.contains("unlikely") || BeforeParen.contains("likely"))
    return {};

  // Also check after the '(' for an existing attribute.
  if (Snippet.size() > ParenPos + 1) {
    StringRef AfterParen = Snippet.substr(ParenPos + 1).ltrim();
    if (AfterParen.starts_with("[[unlikely]]") ||
        AfterParen.starts_with("[[likely]]"))
      return {};
  }

  // Insert "[[unlikely]] " right after the opening '('.
  SourceLocation InsertLoc = Loc.getLocWithOffset(ParenPos + 1);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::InsertAfter;
  Fix.NewText = "[[unlikely]] ";
  Fix.Description = "add [[unlikely]] hint for branch prediction";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixSignedLoopCounter(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the for statement. Find "int " in the init statement.
  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Find the '(' to locate the init-statement region.
  size_t ParenPos = Snippet.find('(');
  if (ParenPos == StringRef::npos)
    return {};

  // Find "int " after the '('.
  size_t IntPos = Snippet.find("int ", ParenPos);
  if (IntPos == StringRef::npos)
    return {};

  // Verify we haven't already converted to size_t or unsigned.
  StringRef BeforeInt = Snippet.substr(ParenPos, IntPos - ParenPos);
  if (BeforeInt.contains("size_t") || BeforeInt.contains("unsigned"))
    return {};

  SourceLocation IntLoc = Loc.getLocWithOffset(IntPos);

  AutoFix Fix;
  Fix.Loc = IntLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = "int ";
  Fix.NewText = "size_t ";
  Fix.Description = "use size_t for loop counter to avoid sign-extension";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixPreIncrement(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the UnaryOperator (post-increment).
  // Read the source text: it should look like "VARNAME++".
  StringRef Snippet = getSourceSnippet(Loc, 128, SM, LO);
  if (Snippet.empty())
    return {};

  // Find "++" in the snippet.
  size_t PlusPlusPos = Snippet.find("++");
  if (PlusPlusPos == StringRef::npos)
    return {};

  // If "++" is at position 0, this is already a pre-increment.
  if (PlusPlusPos == 0)
    return {};

  // Extract the variable name (everything before "++").
  // The variable name is from the start of the snippet to "++".
  StringRef VarName = Snippet.substr(0, PlusPlusPos);

  // Validate: variable name should be an identifier (alphanumeric/underscore).
  bool ValidName = !VarName.empty();
  for (char C : VarName) {
    if (!isalnum(C) && C != '_') {
      ValidName = false;
      break;
    }
  }
  if (!ValidName)
    return {};

  std::string OldExpr = (VarName + "++").str();
  std::string NewExpr = ("++" + VarName).str();

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldExpr;
  Fix.NewText = NewExpr;
  Fix.Description = "use pre-increment for non-trivial iterators";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixEmptyCheck(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Look for ".size() == 0" or ".size() != 0" patterns.
  std::vector<AutoFix> Fixes;

  size_t SizeEqZero = Snippet.find(".size() == 0");
  if (SizeEqZero != StringRef::npos) {
    AutoFix Fix;
    Fix.Loc = Loc.getLocWithOffset(SizeEqZero);
    Fix.FixKind = AutoFix::Replace;
    Fix.OldText = ".size() == 0";
    Fix.NewText = ".empty()";
    Fix.Description = "use .empty() instead of .size() == 0";
    Fixes.push_back(Fix);
    return Fixes;
  }

  size_t SizeNeZero = Snippet.find(".size() != 0");
  if (SizeNeZero != StringRef::npos) {
    AutoFix Fix;
    Fix.Loc = Loc.getLocWithOffset(SizeNeZero);
    Fix.FixKind = AutoFix::Replace;
    Fix.OldText = ".size() != 0";
    Fix.NewText = "!.empty()";
    Fix.Description = "use !.empty() instead of .size() != 0";
    Fixes.push_back(Fix);
    return Fixes;
  }

  // Also check ".size() > 0".
  size_t SizeGtZero = Snippet.find(".size() > 0");
  if (SizeGtZero != StringRef::npos) {
    AutoFix Fix;
    Fix.Loc = Loc.getLocWithOffset(SizeGtZero);
    Fix.FixKind = AutoFix::Replace;
    Fix.OldText = ".size() > 0";
    Fix.NewText = "!.empty()";
    Fix.Description = "use !.empty() instead of .size() > 0";
    Fixes.push_back(Fix);
    return Fixes;
  }

  return {};
}

std::vector<AutoFix>
PerfAutoFixer::fixRestrict(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the ParmVarDecl. Find the '*' in the pointer type.
  StringRef Snippet = getSourceSnippet(Loc, 128, SM, LO);
  if (Snippet.empty())
    return {};

  size_t StarPos = Snippet.find('*');
  if (StarPos == StringRef::npos)
    return {};

  // Check that __restrict__ isn't already present.
  if (Snippet.contains("__restrict__") || Snippet.contains("restrict"))
    return {};

  // Insert " __restrict__" after the '*'.
  SourceLocation InsertLoc = Loc.getLocWithOffset(StarPos + 1);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::InsertAfter;
  Fix.NewText = " __restrict__";
  Fix.Description = "add __restrict__ to enable alias analysis optimizations";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixConstVariable(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 128, SM, LO);
  if (Snippet.empty())
    return {};

  SourceLocation ConstLoc = findTextAfter(Loc, "const ", 128, SM, LO);
  if (ConstLoc.isInvalid())
    return {};

  // Don't double-convert if already constexpr.
  StringRef AtConst = getSourceSnippet(ConstLoc, 16, SM, LO);
  if (AtConst.starts_with("constexpr"))
    return {};

  AutoFix Fix;
  Fix.Loc = ConstLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = "const ";
  Fix.NewText = "constexpr ";
  Fix.Description = "promote const variable to constexpr for compile-time evaluation";
  return {Fix};
}

} // namespace perfsanitizer
