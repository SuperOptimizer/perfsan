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
#include <algorithm>
#include <set>
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

/// Adjust an Insert fix so that it goes on the line BEFORE Loc instead of at
/// Loc.  This prevents comments from being spliced into the middle of
/// declarations / parameter lists / struct headers.
static SourceLocation getLineStartLoc(SourceLocation Loc,
                                       const SourceManager &SM) {
  if (Loc.isInvalid())
    return Loc;
  unsigned Line = SM.getSpellingLineNumber(Loc);
  FileID FID = SM.getFileID(Loc);
  return SM.translateLineCol(FID, Line, 1);
}

/// Return true if \p Text contains unbalanced braces, parens, or brackets.
static bool hasUnbalancedDelimiters(StringRef Text) {
  int Parens = 0, Braces = 0, Brackets = 0;
  for (char C : Text) {
    switch (C) {
    case '(': ++Parens; break;
    case ')': --Parens; break;
    case '{': ++Braces; break;
    case '}': --Braces; break;
    case '[': ++Brackets; break;
    case ']': --Brackets; break;
    default: break;
    }
    if (Parens < 0 || Braces < 0 || Brackets < 0)
      return true;
  }
  return Parens != 0 || Braces != 0 || Brackets != 0;
}

/// Return true if \p Snippet looks like it is inside a class body or is a
/// member function definition (not a free function at file scope).
static bool looksLikeMemberContext(StringRef Snippet) {
  // If '{' appears before '(' it's likely inside a class body.
  size_t BracePos = Snippet.find('{');
  size_t ParenPos = Snippet.find('(');
  if (BracePos != StringRef::npos && ParenPos != StringRef::npos &&
      BracePos < ParenPos)
    return true;
  // Lambda: starts with '['
  StringRef Trimmed = Snippet.ltrim();
  if (!Trimmed.empty() && Trimmed[0] == '[')
    return true;
  // Contains '::' before '(' — likely a qualified member definition.
  if (ParenPos != StringRef::npos) {
    StringRef BeforeParen = Snippet.substr(0, ParenPos);
    if (BeforeParen.contains("::"))
      return true;
  }
  return false;
}

/// Return true if \p Snippet contains types that are not constexpr-eligible.
static bool hasNonConstexprTypes(StringRef Snippet) {
  return Snippet.contains("mutex") || Snippet.contains("function<") ||
         Snippet.contains("thread") || Snippet.contains("lock_guard") ||
         Snippet.contains("unique_lock") || Snippet.contains("try") ||
         Snippet.contains("catch") || Snippet.contains("throw") ||
         Snippet.contains("regex") || Snippet.contains("fstream") ||
         Snippet.contains("iostream");
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
// Standalone fix helpers (no header declaration needed)
//===----------------------------------------------------------------------===//

/// Fix CopyInRangeFor: change `for (auto ` to `for (const auto& `.
static std::vector<AutoFix>
fixCopyInRangeForStatic(const PerfHint &H, ASTContext &Ctx,
                        SourceManager &SM) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Must look like a range-for with "auto".
  size_t ForPos = Snippet.find("for");
  if (ForPos == StringRef::npos)
    return {};

  // Find "auto" after "for".
  size_t AutoPos = Snippet.find("auto", ForPos);
  if (AutoPos == StringRef::npos)
    return {};

  // Check what comes before "auto" (skip whitespace).
  StringRef BeforeAuto = Snippet.substr(ForPos, AutoPos - ForPos);

  // Already has const or reference — bail.
  if (BeforeAuto.contains("const"))
    return {};

  // Check what comes after "auto" — if already "auto&" or "auto &", bail.
  StringRef AfterAuto = Snippet.substr(AutoPos + 4).ltrim();
  if (!AfterAuto.empty() && (AfterAuto[0] == '&' || AfterAuto[0] == '*'))
    return {};

  // Insert "const " before "auto" and "& " after "auto".
  SourceLocation AutoLoc = Loc.getLocWithOffset(AutoPos);
  SourceLocation AfterAutoLoc = Loc.getLocWithOffset(AutoPos + 4);

  AutoFix Fix1;
  Fix1.Loc = AutoLoc;
  Fix1.FixKind = AutoFix::Insert;
  Fix1.NewText = "const ";
  Fix1.Description = "add const to range-for variable";

  AutoFix Fix2;
  Fix2.Loc = AfterAutoLoc;
  Fix2.FixKind = AutoFix::Insert;
  Fix2.NewText = "& ";
  Fix2.Description = "add reference to range-for variable";

  return {Fix1, Fix2};
}

/// Read up to \p MaxLen characters of source text ending at \p Loc
/// (i.e. scanning backward).
static StringRef getSourceSnippetBefore(SourceLocation Loc, unsigned MaxLen,
                                        const SourceManager &SM,
                                        const LangOptions &LO) {
  if (!Loc.isValid())
    return {};
  unsigned Offset = SM.getFileOffset(Loc);
  unsigned ScanBack = std::min(Offset, MaxLen);
  if (ScanBack == 0)
    return {};
  SourceLocation Start = Loc.getLocWithOffset(-(int)ScanBack);
  bool Invalid = false;
  const char *Data = SM.getCharacterData(Start, &Invalid);
  if (Invalid || !Data)
    return {};
  return StringRef(Data, ScanBack);
}

/// Fix StringConcatInLoop: insert .reserve() before the loop.
/// SrcLoc points to the += operator inside the loop body.
static std::vector<AutoFix>
fixStringConcatInLoopStatic(const PerfHint &H, ASTContext &Ctx,
                            SourceManager &SM) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // Read backward from the += operator to find the string variable name
  // and the enclosing loop.
  StringRef Before = getSourceSnippetBefore(Loc, 512, SM, LO);
  if (Before.empty())
    return {};

  // The text right before Loc should be "VARNAME " (then we're at +=).
  // Scan backward past whitespace, then read the identifier.
  size_t Pos = Before.size();
  while (Pos > 0 && Before[Pos - 1] == ' ')
    --Pos;
  size_t NameEnd = Pos;
  while (Pos > 0 && (isalnum(Before[Pos - 1]) || Before[Pos - 1] == '_'))
    --Pos;
  StringRef VarName = Before.substr(Pos, NameEnd - Pos);
  if (VarName.empty())
    return {};

  // Validate variable name.
  for (char C : VarName) {
    if (!isalnum(C) && C != '_')
      return {};
  }

  // Find the last "for" or "while" keyword before Loc.
  size_t ForPos = Before.rfind("for");
  size_t WhilePos = Before.rfind("while");
  size_t LoopPos = StringRef::npos;
  if (ForPos != StringRef::npos && WhilePos != StringRef::npos)
    LoopPos = std::max(ForPos, WhilePos);
  else if (ForPos != StringRef::npos)
    LoopPos = ForPos;
  else if (WhilePos != StringRef::npos)
    LoopPos = WhilePos;
  if (LoopPos == StringRef::npos)
    return {};

  SourceLocation LoopLoc = Loc.getLocWithOffset(-(int)(Before.size() - LoopPos));

  // Read forward from the loop to analyze the header.
  StringRef LoopSnippet = getSourceSnippet(LoopLoc, 256, SM, LO);
  if (LoopSnippet.empty())
    return {};

  // Find '(' and matching ')'.
  size_t ParenOpen = LoopSnippet.find('(');
  if (ParenOpen == StringRef::npos)
    return {};

  unsigned Depth = 0;
  size_t ParenClose = StringRef::npos;
  for (size_t I = ParenOpen; I < LoopSnippet.size(); ++I) {
    if (LoopSnippet[I] == '(') ++Depth;
    else if (LoopSnippet[I] == ')') {
      --Depth;
      if (Depth == 0) { ParenClose = I; break; }
    }
  }
  if (ParenClose == StringRef::npos)
    return {};

  // Determine the reserve size.
  std::string ReserveArg;
  StringRef ForHeader = LoopSnippet.substr(ParenOpen + 1, ParenClose - ParenOpen - 1);
  size_t ColonPos = ForHeader.find(':');
  if (ColonPos != StringRef::npos) {
    // Range-for: extract container name after ':'
    StringRef Container = ForHeader.substr(ColonPos + 1).trim();
    if (Container.empty())
      return {};
    // Validate container is a simple identifier.
    bool Valid = true;
    for (char C : Container) {
      if (!isalnum(C) && C != '_') { Valid = false; break; }
    }
    if (!Valid)
      return {};
    ReserveArg = (Container + ".size() * 16").str();
  } else {
    ReserveArg = "1024";
  }

  // Get indentation.
  unsigned Col = SM.getSpellingColumnNumber(LoopLoc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  AutoFix Fix;
  Fix.Loc = LoopLoc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + VarName.str() + ".reserve(" + ReserveArg + ");\n";
  Fix.Description = "insert .reserve() before loop to reduce string reallocations";
  return {Fix};
}

/// Fix RegexInLoop: add `static const` to the regex variable inside the loop.
/// SrcLoc points to the CXXConstructExpr begin, e.g. at the variable name
/// in "std::regex pat(R"(\d+)")".
/// Making the regex `static const` means it's only compiled once (on first call)
/// even though it remains inside the loop syntactically.
static std::vector<AutoFix>
fixRegexInLoopStatic(const PerfHint &H, ASTContext &Ctx,
                     SourceManager &SM) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // Read backward from Loc to find "std::regex " preceding the var name.
  StringRef Before = getSourceSnippetBefore(Loc, 256, SM, LO);
  if (Before.empty())
    return {};

  // Find the "std::regex " or "regex " that precedes the variable name.
  size_t RegexTypePos = Before.rfind("std::regex ");
  if (RegexTypePos == StringRef::npos) {
    RegexTypePos = Before.rfind("regex ");
    if (RegexTypePos == StringRef::npos)
      return {};
  }

  // Already has "static" before the type?
  StringRef BeforeType = Before.substr(0, RegexTypePos).rtrim();
  if (BeforeType.ends_with("static"))
    return {};

  // Compute the source location of the type keyword.
  SourceLocation TypeLoc = Loc.getLocWithOffset(-(int)(Before.size() - RegexTypePos));

  // Verify it has a string literal pattern (read forward).
  StringRef FromType = getSourceSnippet(TypeLoc, 256, SM, LO);
  if (FromType.empty() || !FromType.contains("\""))
    return {};

  // Insert "static const " before "std::regex" / "regex".
  AutoFix Fix;
  Fix.Loc = TypeLoc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "static const ";
  Fix.Description = "make regex static const to avoid recompilation each iteration";
  return {Fix};
}

/// Fix ThrowInNoexcept: remove noexcept from the enclosing function.
static std::vector<AutoFix>
fixThrowInNoexceptStatic(const PerfHint &H, ASTContext &Ctx,
                         SourceManager &SM) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the throw expression or the function.
  // Scan backward to find "noexcept" in the function signature.
  StringRef Before = getSourceSnippetBefore(Loc, 1024, SM, LO);
  if (Before.empty())
    return {};

  // Find the last "noexcept" before Loc.
  size_t NoexceptPos = Before.rfind("noexcept");
  if (NoexceptPos == StringRef::npos)
    return {};

  // Make sure this isn't "noexcept(false)" or "noexcept(expr)" — only
  // remove plain "noexcept" or "noexcept(true)".
  StringRef AfterNE = Before.substr(NoexceptPos + 8).ltrim();
  std::string OldText = "noexcept";
  if (AfterNE.starts_with("(")) {
    // Check for noexcept(true) or noexcept(false).
    size_t CloseP = AfterNE.find(')');
    if (CloseP == StringRef::npos)
      return {};
    StringRef Inside = AfterNE.substr(1, CloseP - 1).trim();
    if (Inside == "true") {
      // noexcept(true) — safe to remove.
      // Calculate the full extent including parens.
      size_t FullEnd = NoexceptPos + 8 + (AfterNE.data() - Before.substr(NoexceptPos + 8).data()) + CloseP + 1;
      OldText = Before.substr(NoexceptPos, FullEnd - NoexceptPos).str();
    } else if (Inside == "false") {
      // Already noexcept(false) — nothing to do.
      return {};
    } else {
      // Complex expression — don't touch.
      return {};
    }
  }

  // Also consume a space before "noexcept" if present.
  std::string RemoveText = OldText;
  size_t SpaceBefore = NoexceptPos;
  while (SpaceBefore > 0 && Before[SpaceBefore - 1] == ' ')
    --SpaceBefore;
  if (SpaceBefore < NoexceptPos) {
    // Include one space before.
    RemoveText = " " + OldText;
    NoexceptPos = NoexceptPos - 1;
  }

  SourceLocation NeLoc = Loc.getLocWithOffset(-(int)(Before.size() - NoexceptPos));

  AutoFix Fix;
  Fix.Loc = NeLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = RemoveText;
  Fix.NewText = "";
  Fix.Description = "remove noexcept from function that contains throw";
  return {Fix};
}

/// Fix DuplicateCondition: remove duplicated sub-expression in &&.
static std::vector<AutoFix>
fixDuplicateConditionStatic(const PerfHint &H, ASTContext &Ctx,
                            SourceManager &SM) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Find the condition inside if(...).
  size_t IfPos = Snippet.find("if");
  if (IfPos == StringRef::npos)
    return {};

  size_t ParenOpen = Snippet.find('(', IfPos);
  if (ParenOpen == StringRef::npos)
    return {};

  // Find matching ')'.
  unsigned Depth = 0;
  size_t ParenClose = StringRef::npos;
  for (size_t I = ParenOpen; I < Snippet.size(); ++I) {
    if (Snippet[I] == '(') ++Depth;
    else if (Snippet[I] == ')') {
      --Depth;
      if (Depth == 0) { ParenClose = I; break; }
    }
  }
  if (ParenClose == StringRef::npos)
    return {};

  StringRef Condition = Snippet.substr(ParenOpen + 1, ParenClose - ParenOpen - 1).trim();
  if (Condition.empty())
    return {};

  // Look for " && " in the condition.
  size_t AndPos = Condition.find(" && ");
  if (AndPos == StringRef::npos)
    return {};

  StringRef LHS = Condition.substr(0, AndPos).trim();
  StringRef RHS = Condition.substr(AndPos + 4).trim();

  // Only fix if LHS == RHS (exact duplicate).
  if (LHS != RHS)
    return {};

  if (LHS.empty())
    return {};

  // Replace "LHS && RHS" with just "LHS".
  std::string OldCond = Condition.str();
  std::string NewCond = LHS.str();

  SourceLocation CondLoc = Loc.getLocWithOffset(ParenOpen + 1 +
    (Condition.data() - Snippet.substr(ParenOpen + 1).data()));

  AutoFix Fix;
  Fix.Loc = CondLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldCond;
  Fix.NewText = NewCond;
  Fix.Description = "remove duplicate condition in logical AND";
  return {Fix};
}

/// Fix DivisionChain: rewrite `a / b / c` to `a / (b * c)`.
static std::vector<AutoFix>
fixDivisionChainStatic(const PerfHint &H, ASTContext &Ctx,
                       SourceManager &SM) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Find "EXPR / IDENT / IDENT" pattern. We need two '/' operators.
  // Find the statement (up to ';').
  size_t SemiPos = Snippet.find(';');
  if (SemiPos == StringRef::npos)
    return {};

  StringRef Stmt = Snippet.substr(0, SemiPos);

  // Find "return " prefix if present.
  StringRef Expr = Stmt;
  size_t ReturnPos = Stmt.find("return ");
  if (ReturnPos != StringRef::npos)
    Expr = Stmt.substr(ReturnPos + 7).trim();

  // Find the two '/' operators. Must not be '//' (comment) or '/=' or '/*'.
  size_t Div1 = StringRef::npos;
  size_t Div2 = StringRef::npos;
  for (size_t I = 0; I < Expr.size(); ++I) {
    if (Expr[I] == '/') {
      // Skip //, /*, /=
      if (I + 1 < Expr.size() &&
          (Expr[I + 1] == '/' || Expr[I + 1] == '*' || Expr[I + 1] == '='))
        continue;
      // Skip if preceded by '/' (already part of //)
      if (I > 0 && Expr[I - 1] == '/')
        continue;
      if (Div1 == StringRef::npos)
        Div1 = I;
      else if (Div2 == StringRef::npos)
        Div2 = I;
      else
        return {}; // More than 2 divisions — too complex.
    }
  }

  if (Div1 == StringRef::npos || Div2 == StringRef::npos)
    return {};

  // Extract: a / b / c
  StringRef A = Expr.substr(0, Div1).trim();
  StringRef B = Expr.substr(Div1 + 1, Div2 - Div1 - 1).trim();
  StringRef C = Expr.substr(Div2 + 1).trim();

  if (A.empty() || B.empty() || C.empty())
    return {};

  // Validate B and C are simple identifiers.
  auto isSimpleIdent = [](StringRef S) {
    for (char Ch : S) {
      if (!isalnum(Ch) && Ch != '_' && Ch != '.')
        return false;
    }
    return !S.empty();
  };

  if (!isSimpleIdent(B) || !isSimpleIdent(C))
    return {};

  // Build: "a / (b * c)"
  std::string NewExpr = (A + " / (" + B + " * " + C + ")").str();
  std::string OldExpr = Expr.str();

  // Find the expression in the original snippet to replace it.
  SourceLocation ExprLoc = Loc;
  if (ReturnPos != StringRef::npos)
    ExprLoc = Loc.getLocWithOffset(ReturnPos + 7 +
      (Expr.data() - Stmt.substr(ReturnPos + 7).ltrim().data()));

  // Simpler approach: replace the whole expression in the snippet.
  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldExpr;
  Fix.NewText = NewExpr;
  Fix.Description = "combine chained divisions into single division with multiplication";
  return {Fix};
}

/// Fix VirtualDtorMissing: insert `virtual ` before `~ClassName`.
static std::vector<AutoFix>
fixVirtualDtorMissingStatic(const PerfHint &H, ASTContext &Ctx,
                            SourceManager &SM) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Find the destructor '~'.
  size_t TildePos = Snippet.find('~');
  if (TildePos == StringRef::npos)
    return {};

  // Already virtual?
  StringRef BeforeTilde = Snippet.substr(0, TildePos);
  if (BeforeTilde.contains("virtual"))
    return {};

  SourceLocation TildeLoc = Loc.getLocWithOffset(TildePos);

  AutoFix Fix;
  Fix.Loc = TildeLoc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "virtual ";
  Fix.Description = "add virtual to destructor";
  return {Fix};
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
  case HintCategory::RestrictAnnotation:
    return fixRestrict(Hint, Ctx);
  case HintCategory::ConstevalPromotion:
    return fixConstVariable(Hint, Ctx);
  case HintCategory::PureConst:
    return fixPureConst(Hint, Ctx);
  case HintCategory::HeapToStack:
    return fixHeapToStack(Hint, Ctx);
  case HintCategory::FMAContraction:
    return fixFMA(Hint, Ctx);
  case HintCategory::VirtualDevirt:
    return fixVirtualDevirt(Hint, Ctx);
  case HintCategory::AlignmentHint:
    return fixAlignment(Hint, Ctx);
  case HintCategory::DataLayout:
    return fixDataLayout(Hint, Ctx);
  case HintCategory::LoopInvariant:
    return fixLoopInvariant(Hint, Ctx);
  case HintCategory::InliningCandidate:
    return fixInlining(Hint, Ctx);
  case HintCategory::ColdPathOutlining:
    return fixColdPath(Hint, Ctx);
  case HintCategory::ContainerReserve:
    return fixContainerReserve(Hint, Ctx);
  case HintCategory::StringByValue:
    return fixStringByValue(Hint, Ctx);
  case HintCategory::RangeForConversion:
    return fixRangeForConversion(Hint, Ctx);
  case HintCategory::TailCall:
    return fixTailCall(Hint, Ctx);
  case HintCategory::ExceptionInDestructor:
    return fixExceptionInDestructor(Hint, Ctx);
  case HintCategory::VectorBoolAvoid:
    return fixVectorBool(Hint, Ctx);
  case HintCategory::SharedPtrOverhead:
    return fixSharedPtr(Hint, Ctx);
  case HintCategory::UnnecessaryCopy:
    return fixUnnecessaryCopy(Hint, Ctx);
  case HintCategory::BoolBranching:
    return fixBoolBranching(Hint, Ctx);
  case HintCategory::PowerOfTwo:
    return fixPowerOfTwo(Hint, Ctx);
  // Real source rewrites:
  case HintCategory::Vectorization:
    return fixVectorizePragma(Hint, Ctx);
  case HintCategory::LoopBound:
    return fixLoopBound(Hint, Ctx);
  case HintCategory::LambdaCaptureOpt:
    return fixLambdaCapture(Hint, Ctx);
  case HintCategory::HotColdSplit:
    return fixHotColdSplit(Hint, Ctx);
  case HintCategory::MoveSemantics:
    return fixMoveSemantics(Hint, Ctx);
  case HintCategory::SIMDWidth:
    return fixSIMDWidth(Hint, Ctx);
  case HintCategory::ConstexprIf:
    return fixConstexprIf(Hint, Ctx);
  case HintCategory::SmallFunctionNotInline:
    return fixSmallFunctionInline(Hint, Ctx);
  case HintCategory::BranchlessSelect:
    return fixBranchlessSelect(Hint, Ctx);
  case HintCategory::FalseSharing:
    return fixFalseSharing(Hint, Ctx);
  case HintCategory::AliasBarrier:
    return fixAliasBarrier(Hint, Ctx);
  // No source rewrite — report mode handles these via stderr:
  case HintCategory::SoAvsAoS:
  case HintCategory::ExceptionCost:
  case HintCategory::MutexInLoop:
  case HintCategory::StdFunctionOverhead:
  case HintCategory::TightLoopAllocation:
  case HintCategory::RedundantComputation:
  case HintCategory::RedundantLoad:
  case HintCategory::SROAEscape:
  case HintCategory::LoopUnswitching:
  case HintCategory::OutputParamToReturn:
  case HintCategory::UnusedInclude:
  case HintCategory::SortAlgorithm:
  case HintCategory::BitManipulation:
  case HintCategory::RedundantAtomic:
  case HintCategory::CrossTUInlining:
    return {}; // Report-only — suggestions emitted to stderr
  case HintCategory::CacheLineSplit:
    return fixCacheLineSplit(Hint, Ctx);
  case HintCategory::HotColdFunction:
    return fixHotColdFunc(Hint, Ctx);
  // Fixable new categories:
  case HintCategory::CopyInRangeFor:
    return fixCopyInRangeForStatic(Hint, Ctx, SM);
  case HintCategory::VirtualDtorMissing:
    return fixVirtualDtorMissingStatic(Hint, Ctx, SM);
  case HintCategory::SmallFunctionInline:
    return fixSmallFunctionInline(Hint, Ctx);
  // Fixable categories with real source rewrites:
  case HintCategory::StringConcatInLoop:
    return fixStringConcatInLoopStatic(Hint, Ctx, SM);
  case HintCategory::RegexInLoop:
    return fixRegexInLoopStatic(Hint, Ctx, SM);
  case HintCategory::ThrowInNoexcept:
    return fixThrowInNoexceptStatic(Hint, Ctx, SM);
  case HintCategory::DuplicateCondition:
    return fixDuplicateConditionStatic(Hint, Ctx, SM);
  case HintCategory::DivisionChain:
    return fixDivisionChainStatic(Hint, Ctx, SM);
  // Categories that need structural or semantic changes — report only:
  case HintCategory::BranchOnFloat:
  case HintCategory::EmptyLoopBody:
  case HintCategory::DynamicCastInLoop:
  case HintCategory::GlobalVarInLoop:
  case HintCategory::VolatileInLoop:
  case HintCategory::ImplicitConversion:
  case HintCategory::SlicingCopy:
  case HintCategory::SpillPressure:
  case HintCategory::UnrollingBlocker:
  case HintCategory::MemoryAccessPattern:
    return {};
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

    // Safety: reject inserts with unbalanced delimiters.
    if ((F.FixKind == AutoFix::Insert || F.FixKind == AutoFix::InsertAfter) &&
        hasUnbalancedDelimiters(F.NewText)) {
      AllOk = false;
      continue;
    }

    // Deduplicate: skip if we already inserted the same text at the same loc.
    if (F.FixKind == AutoFix::Insert || F.FixKind == AutoFix::InsertAfter) {
      unsigned Offset = SM.getFileOffset(F.Loc);
      auto Key = std::make_pair(Offset, F.NewText);
      if (!SeenInserts.insert(Key).second)
        continue; // duplicate — skip
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

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Don't touch virtual, override, or final functions — constexpr on those is tricky.
  if (Snippet.contains("virtual") || Snippet.contains("override") ||
      Snippet.contains("final"))
    return {};

  // Don't add constexpr to functions using non-literal / non-constexpr types.
  if (hasNonConstexprTypes(Snippet))
    return {};

  // Don't add constexpr to destructors.
  if (Snippet.contains("~"))
    return {};

  // For function constexpr promotion: insert "constexpr " before the return type.
  // For variable constexpr promotion: replace "const " with "constexpr ".
  // Distinguish: if there's a '(' it's a function, otherwise a variable.
  bool IsFunction = Snippet.contains("(");

  if (IsFunction) {
    // Skip if already constexpr or if it's a class method with qualifiers
    if (Snippet.ltrim().starts_with("constexpr"))
      return {};
    // Don't auto-fix methods — too many edge cases (const, override, virtual, final)
    // Only fix free functions: no "const" after ")" and no class-related keywords
    size_t RP = Snippet.find(')');
    if (RP != StringRef::npos) {
      StringRef After = Snippet.substr(RP + 1).ltrim();
      if (After.starts_with("const") || After.starts_with("override") ||
          After.starts_with("final"))
        return {};
    }
    AutoFix Fix;
    Fix.Loc = Loc;
    Fix.FixKind = AutoFix::Insert;
    Fix.NewText = "constexpr ";
    Fix.Description = "add constexpr to enable compile-time evaluation";
    return {Fix};
  }

  // Variable case: replace "const " with "constexpr "
  SourceLocation ConstLoc = findTextAfter(Loc, "const ", 128, SM, LO);
  if (ConstLoc.isInvalid())
    return {};

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

  // Check what follows the ')' — need to handle const, override, final, etc.
  StringRef AfterClose = Snippet.substr(CloseParen + 1);
  StringRef Trimmed = AfterClose.ltrim();

  // Don't add noexcept if already present.
  if (Trimmed.starts_with("noexcept"))
    return {};

  // Don't add noexcept near throw expressions — we'd be annotating the wrong
  // thing (e.g. a throw statement, not a function declaration).
  StringRef BeforeParen = Snippet.substr(0, OpenParen);
  if (BeforeParen.contains("throw"))
    return {};

  // Verify we're on a function declaration line (must have a type + name +
  // parens), not an expression line.  A bare "(" without any identifier-like
  // text before it is suspicious.
  StringRef TrimBefore = BeforeParen.trim();
  if (TrimBefore.empty() || (!isalnum(TrimBefore.back()) && TrimBefore.back() != '_' && TrimBefore.back() != '>'))
    return {};

  // Don't auto-fix virtual functions — noexcept changes the signature.
  if (BeforeParen.contains("virtual"))
    return {};

  // Find the right insertion point: after 'const' if present, before '{' or ';'
  size_t InsertOffset = CloseParen + 1;
  if (Trimmed.starts_with("const")) {
    InsertOffset = CloseParen + 1 + (AfterClose.size() - AfterClose.ltrim().size()) + 5; // skip " const"
  }

  SourceLocation InsertLoc = Loc.getLocWithOffset(InsertOffset);

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

  // Only look for '*' before any '{' (stay in the declaration, not the body)
  size_t BracePos = Snippet.find('{');
  StringRef DeclPart = (BracePos != StringRef::npos) ? Snippet.substr(0, BracePos) : Snippet;
  size_t StarPos = DeclPart.find('*');
  if (StarPos == StringRef::npos)
    return {};

  // Check that __restrict__ isn't already present.
  if (Snippet.contains("__restrict__") || Snippet.contains("restrict"))
    return {};

  // Verify the '*' is a pointer declarator, not multiplication.
  // Heuristic: must be preceded by a type name (letter/digit) and followed
  // by space+identifier or just identifier. Also reject if inside a function body.
  if (StarPos > 0) {
    char Before = Snippet[StarPos - 1];
    // In "data[i] * 2.0f", before '*' is ']' or ' ' from an expression
    if (Before == ']' || Before == ')' || std::isdigit(Before))
      return {}; // multiplication, not pointer
  }
  if (StarPos + 1 < Snippet.size()) {
    char After = Snippet[StarPos + 1];
    // After pointer '*', expect space or identifier start
    if (std::isdigit(After) || After == '(' || After == '[')
      return {}; // multiplication context
  }
  // Also reject if the snippet doesn't look like a parameter declaration
  // (should contain a type name before the *)
  StringRef BeforeStar = Snippet.substr(0, StarPos).rtrim();
  if (BeforeStar.empty() || BeforeStar.back() == '=' || BeforeStar.back() == ',')
    return {};

  // Insert " __restrict__ " after the '*' with proper spacing.
  SourceLocation InsertLoc = Loc.getLocWithOffset(StarPos + 1);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::InsertAfter;
  Fix.NewText = " __restrict__ ";
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

  // Only apply to actual variable declarations, not method qualifiers.
  // A "const " that's a variable qualifier appears before '=' or at line start.
  // A "const" that's a method qualifier appears after ')'.
  if (Snippet.contains("(") && Snippet.contains(")")) {
    // Looks like a function — don't touch it
    return {};
  }

  SourceLocation ConstLoc = findTextAfter(Loc, "const ", 128, SM, LO);
  if (ConstLoc.isInvalid())
    return {};

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

//===----------------------------------------------------------------------===//
// New category-specific fix generators
//===----------------------------------------------------------------------===//

std::vector<AutoFix>
PerfAutoFixer::fixPureConst(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Safety: skip if already has pure or const attribute.
  if (Snippet.contains("__attribute__((pure))") ||
      Snippet.contains("__attribute__((const))") ||
      Snippet.contains("[[gnu::pure]]") ||
      Snippet.contains("[[gnu::const]]"))
    return {};

  // Safety: skip virtual or override functions.
  if (Snippet.contains("virtual") || Snippet.contains("override"))
    return {};

  // Verify this looks like a function declaration (has a '(').
  if (!Snippet.contains("("))
    return {};

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "__attribute__((pure)) ";
  Fix.Description = "add __attribute__((pure)) to indicate no side effects";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixHeapToStack(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // We only handle the simple pattern: Type *var = new Type(args);
  // This is inherently complex, so bail on anything that doesn't match exactly.
  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Must contain "new " to be a heap allocation we can fix.
  size_t NewPos = Snippet.find("new ");
  if (NewPos == StringRef::npos)
    return {};

  // Skip array new — too complex.
  size_t NewEndPos = NewPos + 4;
  StringRef AfterNew = Snippet.substr(NewEndPos).ltrim();
  if (AfterNew.starts_with("["))
    return {};

  // Skip placement new.
  if (NewPos > 0) {
    StringRef BeforeNew = Snippet.substr(0, NewPos);
    if (BeforeNew.rtrim().ends_with("("))
      return {};
  }

  // We need to find: Type *var = new Type(args);
  // Look for '*' before "new" and '=' between them.
  size_t StarPos = Snippet.find('*');
  if (StarPos == StringRef::npos || StarPos >= NewPos)
    return {};

  size_t EqPos = Snippet.find('=');
  if (EqPos == StringRef::npos || EqPos >= NewPos || EqPos <= StarPos)
    return {};

  // Extract variable name: between '*' and '='
  StringRef VarPart = Snippet.substr(StarPos + 1, EqPos - StarPos - 1).trim();
  if (VarPart.empty())
    return {};

  // Validate variable name is a simple identifier.
  bool ValidName = true;
  for (char C : VarPart) {
    if (!isalnum(C) && C != '_') {
      ValidName = false;
      break;
    }
  }
  if (!ValidName)
    return {};

  // Extract type name: from start to '*'
  StringRef TypePart = Snippet.substr(0, StarPos).trim();
  if (TypePart.empty())
    return {};

  // Extract the constructor arguments: everything inside parens after "new Type"
  // Find the type name after "new "
  StringRef AfterNewFull = Snippet.substr(NewEndPos);
  size_t ParenOpen = AfterNewFull.find('(');
  size_t ParenClose = AfterNewFull.find(')');
  if (ParenOpen == StringRef::npos || ParenClose == StringRef::npos ||
      ParenClose <= ParenOpen)
    return {};

  StringRef Args = AfterNewFull.substr(ParenOpen + 1,
                                        ParenClose - ParenOpen - 1).trim();

  // Find the semicolon to know the full statement extent.
  size_t SemiPos = Snippet.find(';');
  if (SemiPos == StringRef::npos)
    return {};

  // Build the replacement: "Type var_storage = args; Type *var = &var_storage;"
  // If no args (default ctor), use "Type var_storage{};"
  std::string StorageName = (VarPart + "_storage").str();
  std::string Replacement;
  if (Args.empty()) {
    Replacement = (TypePart + " " + StorageName + "{}; " + TypePart + " *" +
                   VarPart + " = &" + StorageName + ";")
                      .str();
  } else {
    Replacement = (TypePart + " " + StorageName + " = " + Args + "; " +
                   TypePart + " *" + VarPart + " = &" + StorageName + ";")
                      .str();
  }

  std::string OldText = Snippet.substr(0, SemiPos + 1).str();

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldText;
  Fix.NewText = Replacement;
  Fix.Description = "replace heap allocation with stack variable";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixFMA(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the expression inside a loop. We need to find the loop
  // start (for/while) and insert a pragma before it.
  // Scan backward from SrcLoc by reading text before it.

  // First check if the pragma isn't already present nearby.
  // Read some context around the location.
  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // We need to find the beginning of the line to insert the pragma.
  // Get the column number to compute the start of the line.
  unsigned Col = SM.getSpellingColumnNumber(Loc);
  if (Col == 0)
    return {};

  // Go back to start of the current line.
  SourceLocation LineStart = Loc.getLocWithOffset(-(int)(Col - 1));

  // Read backward to find "for" or "while" — check a few lines back.
  // We'll scan up to 512 chars back from LineStart.
  unsigned Offset = SM.getFileOffset(LineStart);
  unsigned ScanBack = std::min(Offset, 512u);
  SourceLocation ScanStart = LineStart.getLocWithOffset(-(int)ScanBack);

  StringRef BackSnippet = getSourceSnippet(ScanStart, ScanBack + 1, SM, LO);
  if (BackSnippet.empty())
    return {};

  // Check if pragma is already present.
  if (BackSnippet.contains("FP_CONTRACT"))
    return {};

  // Find the last "for" or "while" in the backward scan.
  size_t ForPos = BackSnippet.rfind("for");
  size_t WhilePos = BackSnippet.rfind("while");
  size_t LoopPos = StringRef::npos;
  if (ForPos != StringRef::npos && WhilePos != StringRef::npos)
    LoopPos = std::max(ForPos, WhilePos);
  else if (ForPos != StringRef::npos)
    LoopPos = ForPos;
  else if (WhilePos != StringRef::npos)
    LoopPos = WhilePos;

  if (LoopPos == StringRef::npos)
    return {};

  // Find the start of the line containing the loop keyword.
  // Scan backward from LoopPos for a newline.
  size_t LoopLineStart = LoopPos;
  while (LoopLineStart > 0 && BackSnippet[LoopLineStart - 1] != '\n')
    --LoopLineStart;

  // Compute the indentation.
  StringRef LoopLine = BackSnippet.substr(LoopLineStart, LoopPos - LoopLineStart);
  std::string Indent;
  for (char C : LoopLine) {
    if (C == ' ' || C == '\t')
      Indent += C;
    else
      break;
  }

  SourceLocation InsertLoc = ScanStart.getLocWithOffset(LoopLineStart);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "#pragma STDC FP_CONTRACT ON\n";
  Fix.Description =
      "add FP_CONTRACT pragma to enable fused multiply-add operations";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixVirtualDevirt(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Safety: skip if already final (with or without spaces).
  if (Snippet.contains("final"))
    return {};

  // Don't apply to struct/class declarations — only to methods.
  // If the snippet starts with "struct" or "class", this is a type decl.
  StringRef TrimSnippet = Snippet.ltrim();
  if (TrimSnippet.starts_with("struct ") || TrimSnippet.starts_with("class "))
    return {};

  // Verify this looks like a function (must have parens).
  if (!Snippet.contains("("))
    return {};

  // Don't apply if this looks like a class/struct with inheritance.
  // Pattern: "Name : BaseClass {" without '(' before the ':'.
  size_t ColonPos = Snippet.find(':');
  size_t FirstParen = Snippet.find('(');
  if (ColonPos != StringRef::npos && FirstParen != StringRef::npos &&
      ColonPos < FirstParen) {
    // Colon before paren — likely inheritance, not a method.
    return {};
  }

  // Look for "override" in the method declaration.
  size_t OverridePos = Snippet.find("override");
  if (OverridePos != StringRef::npos) {
    // Insert " final" right after "override".
    SourceLocation InsertLoc =
        Loc.getLocWithOffset(OverridePos + strlen("override"));

    AutoFix Fix;
    Fix.Loc = InsertLoc;
    Fix.FixKind = AutoFix::InsertAfter;
    Fix.NewText = " final";
    Fix.Description = "add final to enable devirtualization";
    return {Fix};
  }

  // No "override" — only insert "final" if this looks like a method
  // definition with a closing ')' before '{'.
  // Walk forward to find the first '{' that is directly preceded by ')' or
  // ')' + qualifiers (const, volatile, etc.), indicating a function body.
  // Reject '{' that follows a class name / base-class list.
  size_t BracePos = Snippet.find('{');
  if (BracePos == StringRef::npos)
    return {};

  // Ensure the last non-whitespace before '{' is ')' or a qualifier keyword
  // (const, volatile), not a class name.
  StringRef BeforeBrace = Snippet.substr(0, BracePos).rtrim();
  if (BeforeBrace.empty())
    return {};

  // The text before '{' should end with ')' or a qualifier after ')'.
  // Find the last ')' before '{'.
  size_t LastCloseParen = BeforeBrace.rfind(')');
  if (LastCloseParen == StringRef::npos)
    return {};

  // Between ')' and '{' should only be whitespace and qualifiers
  // (const, volatile, noexcept, override, =0, etc.), not a class name.
  StringRef BetweenParenAndBrace = BeforeBrace.substr(LastCloseParen + 1).trim();
  // If there's content that's not a known qualifier, bail.
  if (!BetweenParenAndBrace.empty()) {
    // Check that remaining tokens are known method qualifiers.
    StringRef Remaining = BetweenParenAndBrace;
    while (!Remaining.empty()) {
      Remaining = Remaining.ltrim();
      bool FoundQual = false;
      for (const char *Qual : {"const", "volatile", "noexcept", "=", "0",
                                "override", "final", "&", "&&"}) {
        if (Remaining.starts_with(Qual)) {
          Remaining = Remaining.substr(strlen(Qual));
          FoundQual = true;
          break;
        }
      }
      if (!FoundQual)
        return {}; // Unknown token — likely a class name, bail.
    }
  }

  SourceLocation InsertLoc = Loc.getLocWithOffset(BracePos);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "final ";
  Fix.Description = "add final to enable devirtualization";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixAlignment(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 128, SM, LO);
  if (Snippet.empty())
    return {};

  // Safety: skip if already has alignas.
  if (Snippet.contains("alignas") || Snippet.contains("__attribute__((aligned"))
    return {};

  // Only apply to array fields — must contain '[' (array syntax).
  if (!Snippet.contains("["))
    return {};
  // Don't apply to function parameters.
  if (Snippet.contains("(") || Snippet.contains(")"))
    return {};

  // Find the type keyword and insert before it.
  size_t TypePos = StringRef::npos;
  for (auto T : {"float ", "double ", "int ", "char ", "uint8_t ", "int32_t "}) {
    TypePos = Snippet.find(T);
    if (TypePos != StringRef::npos)
      break;
  }
  if (TypePos == StringRef::npos)
    return {};

  AutoFix Fix;
  Fix.Loc = Loc.getLocWithOffset(TypePos);
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "alignas(32) ";
  Fix.Description = "add alignas(32) for SIMD-friendly alignment";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixDataLayout(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Don't insert if the comment is already there.
  if (Snippet.contains("PERF: reorder fields"))
    return {};

  // Insert the comment on the line containing Loc (at column 1), not at Loc
  // itself, to avoid breaking "struct Name {".
  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "// PERF: reorder fields by size (largest first) to reduce "
                "padding\n";
  Fix.Description = "suggest field reordering to reduce struct padding";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixLoopInvariant(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the for/while statement.
  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // We only handle the simple pattern: for(init; i < getFunc(); incr)
  // Find the opening '(' of the for loop.
  size_t ForPos = Snippet.find("for");
  if (ForPos == StringRef::npos)
    return {};

  size_t ParenOpen = Snippet.find('(', ForPos);
  if (ParenOpen == StringRef::npos)
    return {};

  // Find the first ';' (end of init).
  size_t FirstSemi = Snippet.find(';', ParenOpen);
  if (FirstSemi == StringRef::npos)
    return {};

  // Find the second ';' (end of condition).
  size_t SecondSemi = Snippet.find(';', FirstSemi + 1);
  if (SecondSemi == StringRef::npos)
    return {};

  // Extract the condition part.
  StringRef Condition =
      Snippet.substr(FirstSemi + 1, SecondSemi - FirstSemi - 1).trim();
  if (Condition.empty())
    return {};

  // Look for a function call in the condition: something with '(' and ')'.
  // Pattern: var < funcCall() or var <= funcCall()
  size_t LtPos = Condition.find('<');
  if (LtPos == StringRef::npos)
    return {};

  // The RHS of the comparison is the potential invariant call.
  StringRef RHS = Condition.substr(LtPos + 1).ltrim();
  if (RHS.starts_with("="))
    RHS = RHS.substr(1).ltrim(); // handle <=

  // The RHS must contain a function call: name followed by '()'.
  size_t CallOpen = RHS.find('(');
  size_t CallClose = RHS.find(')');
  if (CallOpen == StringRef::npos || CallClose == StringRef::npos ||
      CallClose <= CallOpen)
    return {};

  // Extract the function call expression.
  StringRef CallExpr = RHS.substr(0, CallClose + 1).trim();
  if (CallExpr.empty())
    return {};

  // Verify it looks like a simple call (no nested complex expressions).
  // Count parens — should be balanced with just one pair.
  int Depth = 0;
  for (char C : CallExpr) {
    if (C == '(') ++Depth;
    if (C == ')') --Depth;
    if (Depth < 0)
      return {};
  }
  if (Depth != 0)
    return {};

  // Build: "const auto _bound = callExpr;\n" to insert before the for loop.
  // Get the indentation of the for loop.
  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  std::string HoistLine = Indent + "const auto _bound = " +
                           CallExpr.str() + ";\n";

  // Also need to replace the call in the condition with _bound.
  // First fix: insert the hoisted variable before the loop.
  AutoFix InsertFix;
  InsertFix.Loc = Loc;
  InsertFix.FixKind = AutoFix::Insert;
  InsertFix.NewText = HoistLine;
  InsertFix.Description = "hoist loop-invariant expression before loop";

  // Second fix: replace the call in the condition with _bound.
  SourceLocation CallLoc =
      findTextAfter(Loc, CallExpr, 512, SM, LO);
  if (CallLoc.isInvalid())
    return {InsertFix}; // At least insert the variable; user can fix the rest.

  AutoFix ReplaceFix;
  ReplaceFix.Loc = CallLoc;
  ReplaceFix.FixKind = AutoFix::Replace;
  ReplaceFix.OldText = CallExpr.str();
  ReplaceFix.NewText = "_bound";
  ReplaceFix.Description = "replace loop-invariant call with hoisted variable";

  return {InsertFix, ReplaceFix};
}

std::vector<AutoFix>
PerfAutoFixer::fixInlining(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Safety: skip if already has always_inline or inline attribute.
  if (Snippet.contains("always_inline") ||
      Snippet.contains("__forceinline"))
    return {};

  // Verify this looks like a function declaration.
  if (!Snippet.contains("("))
    return {};

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "__attribute__((always_inline)) ";
  Fix.Description = "add always_inline attribute to encourage inlining";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixColdPath(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the 'if' statement on a cold path.
  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Must start with or contain "if".
  size_t IfPos = Snippet.find("if");
  if (IfPos == StringRef::npos)
    return {};

  size_t ParenPos = Snippet.find('(', IfPos);
  if (ParenPos == StringRef::npos)
    return {};

  // Check that [[unlikely]] isn't already there.
  StringRef BeforeParen = Snippet.substr(IfPos, ParenPos - IfPos);
  if (BeforeParen.contains("unlikely") || BeforeParen.contains("likely"))
    return {};

  // Also check after '('.
  if (Snippet.size() > ParenPos + 1) {
    StringRef AfterParen = Snippet.substr(ParenPos + 1).ltrim();
    if (AfterParen.starts_with("[[unlikely]]") ||
        AfterParen.starts_with("[[likely]]"))
      return {};
  }

  // Insert "[[unlikely]] " after '('.
  SourceLocation InsertLoc = Loc.getLocWithOffset(ParenPos + 1);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::InsertAfter;
  Fix.NewText = "[[unlikely]] ";
  Fix.Description = "add [[unlikely]] hint for cold path outlining";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixContainerReserve(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // SrcLoc points to the for loop. Pattern:
  // for(i=0; i<N; i++) container.push_back(...)
  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Find the for loop structure.
  size_t ForPos = Snippet.find("for");
  if (ForPos == StringRef::npos)
    return {};

  size_t ParenOpen = Snippet.find('(', ForPos);
  if (ParenOpen == StringRef::npos)
    return {};

  // Find the condition (between first and second semicolons).
  size_t FirstSemi = Snippet.find(';', ParenOpen);
  if (FirstSemi == StringRef::npos)
    return {};

  size_t SecondSemi = Snippet.find(';', FirstSemi + 1);
  if (SecondSemi == StringRef::npos)
    return {};

  StringRef Condition =
      Snippet.substr(FirstSemi + 1, SecondSemi - FirstSemi - 1).trim();

  // Extract the bound (RHS of '<' or '<=').
  size_t LtPos = Condition.find('<');
  if (LtPos == StringRef::npos)
    return {};

  StringRef Bound = Condition.substr(LtPos + 1).ltrim();
  if (Bound.starts_with("="))
    Bound = Bound.substr(1).ltrim();
  Bound = Bound.trim();
  if (Bound.empty())
    return {};

  // Safety: only allow pure integer literals as the bound.
  // Reject variable names, function calls, complex expressions, etc.
  if (Bound.empty())
    return {};
  if (!std::all_of(Bound.begin(), Bound.end(), ::isdigit))
    return {};

  // Find push_back in the loop body to identify the container.
  size_t PushPos = Snippet.find("push_back");
  if (PushPos == StringRef::npos)
    return {};

  // The container name is immediately before ".push_back".
  // Scan backward from the '.' before "push_back".
  size_t DotPos = Snippet.rfind('.', PushPos);
  if (DotPos == StringRef::npos || DotPos == 0)
    return {};

  // Extract container name: scan backward from dot for identifier chars.
  size_t NameEnd = DotPos;
  size_t NameStart = DotPos;
  while (NameStart > 0) {
    char C = Snippet[NameStart - 1];
    if (isalnum(C) || C == '_') {
      --NameStart;
    } else {
      break;
    }
  }

  StringRef ContainerName = Snippet.substr(NameStart, NameEnd - NameStart);
  if (ContainerName.empty())
    return {};

  // Validate container name is a simple identifier.
  bool ValidName = true;
  for (char C : ContainerName) {
    if (!isalnum(C) && C != '_') {
      ValidName = false;
      break;
    }
  }
  if (!ValidName)
    return {};

  // Get indentation.
  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  std::string ReserveLine = Indent + ContainerName.str() + ".reserve(" +
                             Bound.str() + ");\n";

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = ReserveLine;
  Fix.Description = "insert .reserve() before loop to avoid repeated "
                    "reallocations";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixStringByValue(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Look for "string " in the parameter (from std::string or string).
  // Must be in a function parameter context (inside parens).
  size_t StringPos = Snippet.find("string ");
  if (StringPos == StringRef::npos)
    return {};

  // Skip if already a reference or const reference.
  // Check for "const" before "string" and "&" after "string ".
  StringRef BeforeString = Snippet.substr(0, StringPos).rtrim();
  if (BeforeString.ends_with("const"))
    return {};

  // Check for '&' after "string "
  StringRef AfterString = Snippet.substr(StringPos + 7); // "string " is 7 chars
  if (AfterString.ltrim().starts_with("&"))
    return {};

  // Check for string_view (already optimized).
  if (Snippet.contains("string_view"))
    return {};

  // Determine if we should use string_view (C++17+) or const ref.
  bool UseSV = LO.CPlusPlus17;

  if (UseSV) {
    // Replace "string " with "string_view ".
    // Also need to check the std:: prefix.
    std::string OldType;
    std::string NewType;
    size_t StdPos = Snippet.substr(0, StringPos).rfind("std::");
    if (StdPos != StringRef::npos && (StringPos - StdPos) <= 6) {
      // Has "std::" prefix
      OldType = "std::string ";
      NewType = "std::string_view ";
      StringPos = StdPos;
    } else {
      OldType = "string ";
      NewType = "string_view ";
    }

    AutoFix Fix;
    Fix.Loc = Loc.getLocWithOffset(StringPos);
    Fix.FixKind = AutoFix::Replace;
    Fix.OldText = OldType;
    Fix.NewText = NewType;
    Fix.Description = "use string_view instead of string by value";
    return {Fix};
  }

  // C++11/14: use const std::string&
  // Insert "const " before the type and "&" after "string".
  std::vector<AutoFix> Fixes;

  // Find the actual start of the type (could be "std::string" or "string").
  size_t TypeStart = StringPos;
  size_t StdPos = Snippet.substr(0, StringPos).rfind("std::");
  if (StdPos != StringRef::npos && (StringPos - StdPos) <= 6)
    TypeStart = StdPos;

  AutoFix ConstFix;
  ConstFix.Loc = Loc.getLocWithOffset(TypeStart);
  ConstFix.FixKind = AutoFix::Insert;
  ConstFix.NewText = "const ";
  ConstFix.Description = "add const qualifier for string parameter";
  Fixes.push_back(ConstFix);

  // Replace "string " with "string& ".
  AutoFix RefFix;
  RefFix.Loc = Loc.getLocWithOffset(StringPos);
  RefFix.FixKind = AutoFix::Replace;
  RefFix.OldText = "string ";
  RefFix.NewText = "string& ";
  RefFix.Description = "pass string by const reference";
  Fixes.push_back(RefFix);

  return Fixes;
}

std::vector<AutoFix>
PerfAutoFixer::fixRangeForConversion(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // This conversion is too complex for safe auto-fix.
  // Insert a comment suggesting the conversion.

  // Don't duplicate the comment.
  if (Snippet.contains("PERF: consider range-for"))
    return {};

  // Get indentation and insert at line start to avoid breaking code.
  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText =
      Indent +
      "// PERF: consider range-for: for (auto& elem : container)\n";
  Fix.Description = "suggest range-based for loop conversion";
  return {Fix};
}

//===----------------------------------------------------------------------===//
// New auto-fix generators (batch 2)
//===----------------------------------------------------------------------===//

std::vector<AutoFix>
PerfAutoFixer::fixTailCall(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Find "return " at or near SrcLoc.
  SourceLocation RetLoc = findTextAfter(Loc, "return ", 256, SM, LO);
  if (RetLoc.isInvalid())
    return {};

  // Check that [[clang::musttail]] isn't already present.
  StringRef AtRet = getSourceSnippet(RetLoc, 64, SM, LO);
  if (AtRet.contains("musttail"))
    return {};

  // Verify there's a function call after "return ".
  StringRef AfterReturn = getSourceSnippet(RetLoc.getLocWithOffset(7), 128, SM, LO);
  if (AfterReturn.empty() || !AfterReturn.contains("("))
    return {};

  AutoFix Fix;
  Fix.Loc = RetLoc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "[[clang::musttail]] ";
  Fix.Description = "add [[clang::musttail]] to enable tail call optimization";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixExceptionInDestructor(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Already noexcept?
  if (Snippet.contains("noexcept"))
    return {};

  // Don't add noexcept if the snippet is a throw expression, not a function
  // declaration.
  StringRef FirstLine = Snippet.split('\n').first.trim();
  if (FirstLine.starts_with("throw") || FirstLine.contains("throw "))
    return {};

  // Verify we're looking at a destructor declaration (should contain '~').
  if (!Snippet.contains("~"))
    return {};

  // Find the closing ')' of the destructor parameter list.
  size_t OpenParen = Snippet.find('(');
  if (OpenParen == StringRef::npos)
    return {};

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

  SourceLocation InsertLoc = Loc.getLocWithOffset(CloseParen + 1);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::InsertAfter;
  Fix.NewText = " noexcept";
  Fix.Description = "add noexcept to destructor declaration";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixVectorBool(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  SourceLocation VBLoc = findTextAfter(Loc, "vector<bool>", 256, SM, LO);
  if (VBLoc.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = VBLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = "vector<bool>";
  Fix.NewText = "vector<char>";
  Fix.Description = "replace vector<bool> with vector<char> to avoid bitfield overhead";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixSharedPtr(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Look for "shared_ptr<...> param" pattern (by value).
  // Already const ref?
  if (Snippet.contains("const") && Snippet.contains("&"))
    return {};

  // Find "shared_ptr<".
  SourceLocation SPLoc = findTextAfter(Loc, "shared_ptr<", 256, SM, LO);
  if (SPLoc.isInvalid())
    return {};

  // Read from shared_ptr< to find the closing '>' and param name.
  StringRef FromSP = getSourceSnippet(SPLoc, 256, SM, LO);
  if (FromSP.empty())
    return {};

  // Find the closing '>'.
  unsigned Depth = 0;
  size_t CloseAngle = StringRef::npos;
  for (size_t I = 10; I < FromSP.size(); ++I) { // skip "shared_ptr"
    if (FromSP[I] == '<')
      ++Depth;
    else if (FromSP[I] == '>') {
      --Depth;
      if (Depth == 0) {
        CloseAngle = I;
        break;
      }
    }
  }
  if (CloseAngle == StringRef::npos)
    return {};

  // Extract the full type including shared_ptr<T>.
  StringRef SPType = FromSP.substr(0, CloseAngle + 1);

  // After the type, expect a space and param name. Check for '&' (already ref).
  StringRef AfterType = FromSP.substr(CloseAngle + 1).ltrim();
  if (AfterType.starts_with("&"))
    return {};

  // Check for std:: prefix before shared_ptr.
  unsigned SPOffset = SM.getFileOffset(SPLoc) - SM.getFileOffset(Loc);
  std::string OldType;
  std::string NewType;
  if (SPOffset >= 5) {
    StringRef BeforeSP = Snippet.substr(SPOffset - 5, 5);
    if (BeforeSP == "std::") {
      OldType = ("std::" + SPType).str();
      NewType = ("const std::" + SPType + "&").str();
      SPLoc = SPLoc.getLocWithOffset(-5);
    } else {
      OldType = SPType.str();
      NewType = ("const " + SPType + "&").str();
    }
  } else {
    OldType = SPType.str();
    NewType = ("const " + SPType + "&").str();
  }

  AutoFix Fix;
  Fix.Loc = SPLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldType;
  Fix.NewText = NewType;
  Fix.Description = "pass shared_ptr by const reference to avoid refcount overhead";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixUnnecessaryCopy(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Already a reference?
  if (Snippet.contains("&"))
    return {};
  // Already const ref?
  if (Snippet.contains("const"))
    return {};

  // The hint message should contain the type name. We look for a pattern
  // like "Type param" in the snippet. Find the first '(' to locate params.
  // If we're at the param directly, look for the space before the param name.
  // Heuristic: insert "const " before and "& " replacing " " after type.
  // This is inherently tricky — be conservative.

  // Look for the first identifier followed by space and another identifier
  // (type followed by param name). Only handle simple single-word types.
  size_t SpacePos = Snippet.find(' ');
  if (SpacePos == StringRef::npos || SpacePos == 0)
    return {};

  StringRef TypePart = Snippet.substr(0, SpacePos);
  // Validate type is a simple identifier.
  for (char C : TypePart) {
    if (!isalnum(C) && C != '_' && C != ':')
      return {};
  }

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = (TypePart + " ").str();
  Fix.NewText = ("const " + TypePart + "& ").str();
  Fix.Description = "pass large parameter by const reference to avoid copy";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixBoolBranching(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Look for pattern: "if (EXPR) return true; else return false;"
  // or "if (EXPR) return true;\n  else return false;"
  // or "if (EXPR) return true;\nreturn false;"
  size_t IfPos = Snippet.find("if");
  if (IfPos == StringRef::npos)
    return {};

  size_t ParenOpen = Snippet.find('(', IfPos);
  if (ParenOpen == StringRef::npos)
    return {};

  // Find matching ')'.
  unsigned Depth = 0;
  size_t ParenClose = StringRef::npos;
  for (size_t I = ParenOpen; I < Snippet.size(); ++I) {
    if (Snippet[I] == '(')
      ++Depth;
    else if (Snippet[I] == ')') {
      --Depth;
      if (Depth == 0) {
        ParenClose = I;
        break;
      }
    }
  }
  if (ParenClose == StringRef::npos)
    return {};

  // Extract the condition expression.
  StringRef Cond = Snippet.substr(ParenOpen + 1, ParenClose - ParenOpen - 1).trim();
  if (Cond.empty())
    return {};

  // Check for "return true" after the condition.
  StringRef AfterCond = Snippet.substr(ParenClose + 1);
  StringRef Trimmed = AfterCond.ltrim();

  // Handle both braced and non-braced forms.
  bool HasReturnTrue = false;
  if (Trimmed.starts_with("return true;"))
    HasReturnTrue = true;
  else if (Trimmed.starts_with("{")) {
    StringRef InBrace = Trimmed.substr(1).ltrim();
    if (InBrace.starts_with("return true;"))
      HasReturnTrue = true;
  }
  if (!HasReturnTrue)
    return {};

  // Check for "return false" after that.
  size_t RetFalsePos = AfterCond.find("return false;");
  if (RetFalsePos == StringRef::npos)
    return {};

  // Find the end of the entire if/else construct.
  size_t EndPos = AfterCond.find("return false;") + strlen("return false;");
  // Only consume a closing '}' if it belongs to the else-block, not the
  // enclosing function body.  Check whether there's an "else {" before
  // "return false;" — if so, the '}' after is the else-block's brace.
  StringRef BeforeRetFalse = AfterCond.substr(0, RetFalsePos);
  bool ElseIsBraced = BeforeRetFalse.rfind('{') != StringRef::npos &&
                      BeforeRetFalse.rfind('{') > BeforeRetFalse.rfind("else");
  if (ElseIsBraced) {
    StringRef AfterRetFalse = AfterCond.substr(EndPos).ltrim();
    if (AfterRetFalse.starts_with("}"))
      EndPos += (AfterRetFalse.data() - AfterCond.substr(EndPos).data()) + 1;
  }

  // Total extent from "if" to end.
  size_t TotalLen = (ParenClose + 1 - IfPos) + EndPos;
  std::string OldText = Snippet.substr(IfPos, TotalLen).str();
  std::string NewText = ("return " + Cond + ";").str();

  AutoFix Fix;
  Fix.Loc = Loc.getLocWithOffset(IfPos);
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldText;
  Fix.NewText = NewText;
  Fix.Description = "simplify bool branching to direct return";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixPowerOfTwo(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Look for "x % N" or "x / N" where N is a literal power of 2.
  // Find '%' or '/' operator.
  size_t ModPos = Snippet.find('%');
  size_t DivPos = Snippet.find('/');

  // Skip "//", "/=" etc.
  if (DivPos != StringRef::npos && DivPos + 1 < Snippet.size()) {
    char Next = Snippet[DivPos + 1];
    if (Next == '/' || Next == '*' || Next == '=')
      DivPos = StringRef::npos;
  }

  size_t OpPos = StringRef::npos;
  bool IsMod = false;
  if (ModPos != StringRef::npos && DivPos != StringRef::npos) {
    if (ModPos < DivPos) {
      OpPos = ModPos;
      IsMod = true;
    } else {
      OpPos = DivPos;
    }
  } else if (ModPos != StringRef::npos) {
    OpPos = ModPos;
    IsMod = true;
  } else if (DivPos != StringRef::npos) {
    OpPos = DivPos;
  }

  if (OpPos == StringRef::npos)
    return {};

  // Extract LHS (variable before operator).
  StringRef LHS = Snippet.substr(0, OpPos).rtrim();
  if (LHS.empty())
    return {};
  // Get the rightmost token as the variable.
  size_t LHSStart = LHS.size();
  while (LHSStart > 0 && (isalnum(LHS[LHSStart - 1]) || LHS[LHSStart - 1] == '_'))
    --LHSStart;
  StringRef Var = LHS.substr(LHSStart);
  if (Var.empty())
    return {};

  // Extract RHS (number after operator).
  StringRef RHS = Snippet.substr(OpPos + 1).ltrim();
  // Read digits.
  size_t NumEnd = 0;
  while (NumEnd < RHS.size() && std::isdigit(RHS[NumEnd]))
    ++NumEnd;
  if (NumEnd == 0)
    return {};
  StringRef NumStr = RHS.substr(0, NumEnd);

  // Parse the number and check if it's a power of 2.
  unsigned long long N = 0;
  if (NumStr.getAsInteger(10, N) || N == 0)
    return {};
  if ((N & (N - 1)) != 0)
    return {}; // Not a power of 2.

  // Compute log2.
  unsigned Log2N = 0;
  {
    unsigned long long Tmp = N;
    while (Tmp > 1) {
      Tmp >>= 1;
      ++Log2N;
    }
  }

  // Build the old and new text.
  // Old: "var % N" or "var / N" (with surrounding whitespace as found).
  std::string OldExpr = Snippet.substr(LHSStart, (OpPos + 1 + NumEnd) - LHSStart).str();
  // Trim to actual expression extent.
  StringRef OldExprRef(OldExpr);

  std::string NewExpr;
  if (IsMod) {
    NewExpr = ("(" + Var + " & " + std::to_string(N - 1) + ")").str();
  } else {
    NewExpr = ("(" + Var + " >> " + std::to_string(Log2N) + ")").str();
  }

  AutoFix Fix;
  Fix.Loc = Loc.getLocWithOffset(LHSStart);
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldExpr;
  Fix.NewText = NewExpr;
  Fix.Description = IsMod ? "replace modulo with bitwise AND for power-of-2"
                          : "replace division with right shift for power-of-2";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixSoAComment(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: restructure to SoA"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: restructure to SoA for vectorization\n";
  Fix.Description = "suggest SoA restructuring for better vectorization";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixVectorizePragma(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("vectorize(enable)") ||
      Snippet.contains("vectorize(assume_safety)"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "#pragma clang loop vectorize(enable)\n";
  Fix.Description = "add vectorization pragma before loop";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixExceptionComment(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: move try/catch outside"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: move try/catch outside this loop\n";
  Fix.Description = "suggest moving exception handling outside loop";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixMutexComment(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: move lock outside loop"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: move lock outside loop, or use lock-free\n";
  Fix.Description = "suggest moving mutex lock outside loop";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixStdFunctionComment(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: use template param"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: use template param instead of std::function\n";
  Fix.Description = "suggest template parameter instead of std::function";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixLoopBound(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Find the for loop structure.
  size_t ForPos = Snippet.find("for");
  if (ForPos == StringRef::npos)
    return {};

  size_t ParenOpen = Snippet.find('(', ForPos);
  if (ParenOpen == StringRef::npos)
    return {};

  // Find the condition (between first and second semicolons).
  size_t FirstSemi = Snippet.find(';', ParenOpen);
  if (FirstSemi == StringRef::npos)
    return {};

  size_t SecondSemi = Snippet.find(';', FirstSemi + 1);
  if (SecondSemi == StringRef::npos)
    return {};

  StringRef Condition =
      Snippet.substr(FirstSemi + 1, SecondSemi - FirstSemi - 1).trim();

  // Extract the bound variable (RHS of '<' or '<=').
  size_t LtPos = Condition.find('<');
  if (LtPos == StringRef::npos)
    return {};

  StringRef Bound = Condition.substr(LtPos + 1).ltrim();
  if (Bound.starts_with("="))
    Bound = Bound.substr(1).ltrim();
  Bound = Bound.trim();
  if (Bound.empty())
    return {};

  // Only handle simple variable names (not expressions or literals).
  for (char C : Bound) {
    if (!isalnum(C) && C != '_')
      return {};
  }

  // Skip if it's already a literal number (not useful to add assume).
  if (!Bound.empty() && std::isdigit(Bound[0]))
    return {};

  // Check if __builtin_assume is already present nearby.
  if (Snippet.contains("__builtin_assume"))
    return {};

  // Compute indentation from the for-loop line.
  // If ForPos > 0, adjust to the for-loop's actual location.
  SourceLocation ForLoc = Loc.getLocWithOffset(ForPos);
  unsigned ForCol = SM.getSpellingColumnNumber(ForLoc);
  std::string Indent;
  if (ForCol > 1)
    Indent.assign(ForCol - 1, ' ');

  std::string AssumeLine = Indent + "__builtin_assume(" + Bound.str() +
                            " > 0 && " + Bound.str() + " <= 1024);\n";

  SourceLocation LineStart = getLineStartLoc(ForLoc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = AssumeLine;
  Fix.Description = "add __builtin_assume to help loop bound analysis";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixLambdaCapture(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Find "[=]" at or near SrcLoc.
  SourceLocation EqCapLoc = findTextAfter(Loc, "[=]", 256, SM, LO);
  if (EqCapLoc.isInvalid())
    return {};

  // Already [&]?
  if (Snippet.contains("[&]"))
    return {};

  AutoFix Fix;
  Fix.Loc = EqCapLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = "[=]";
  Fix.NewText = "[&]";
  Fix.Description = "change lambda capture from by-value to by-reference";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixTightAllocComment(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: hoist allocation outside loop"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: hoist allocation outside loop\n";
  Fix.Description = "suggest hoisting allocation outside tight loop";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixRedundantComputComment(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: hoist invariant computation outside loop"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent;
  if (Col > 1)
    Indent.assign(Col - 1, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: hoist invariant computation outside loop\n";
  Fix.Description = "suggest hoisting invariant computation outside loop";
  return {Fix};
}

//===----------------------------------------------------------------------===//
// New auto-fix generators (batch 3) — previously unhandled categories
//===----------------------------------------------------------------------===//

std::vector<AutoFix>
PerfAutoFixer::fixAliasBarrier(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Already annotated?
  if (Snippet.contains("__restrict__") ||
      Snippet.contains("assume_safety"))
    return {};

  // Find the first "for" or "while" keyword in the snippet to place the pragma.
  size_t ForPos = Snippet.find("for");
  size_t WhilePos = Snippet.find("while");
  size_t LoopPos = StringRef::npos;
  if (ForPos != StringRef::npos && WhilePos != StringRef::npos)
    LoopPos = std::min(ForPos, WhilePos);
  else if (ForPos != StringRef::npos)
    LoopPos = ForPos;
  else if (WhilePos != StringRef::npos)
    LoopPos = WhilePos;

  if (LoopPos == StringRef::npos)
    return {};

  // Compute the insertion point: the start of the line containing the loop.
  SourceLocation LoopLoc = Loc.getLocWithOffset(LoopPos);
  SourceLocation LineStart = getLineStartLoc(LoopLoc, SM);
  if (LineStart.isInvalid())
    return {};

  unsigned Col = SM.getSpellingColumnNumber(LoopLoc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "#pragma clang loop vectorize(assume_safety)\n";
  Fix.Description = "add vectorize(assume_safety) pragma to enable vectorization";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixHotColdSplit(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Safety: skip if already has __attribute__((cold)).
  if (Snippet.contains("__attribute__((cold))"))
    return {};

  // Verify this looks like a function declaration.
  if (!Snippet.contains("("))
    return {};

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "__attribute__((cold)) ";
  Fix.Description = "add __attribute__((cold)) for hot/cold splitting";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixRedundantLoad(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: hoist repeated load"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: hoist repeated load to a local variable\n";
  Fix.Description = "suggest hoisting repeated load to a local variable";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixSROAEscape(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: pass struct fields individually"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: pass struct fields individually instead of "
                "pointer\n";
  Fix.Description = "suggest passing struct fields individually to enable SROA";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixMoveSemantics(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Find "return expr;" pattern.
  SourceLocation RetLoc = findTextAfter(Loc, "return ", 512, SM, LO);
  if (RetLoc.isInvalid())
    return {};

  StringRef FromRet = getSourceSnippet(RetLoc, 256, SM, LO);
  if (FromRet.empty())
    return {};

  // Already uses std::move?
  if (FromRet.contains("std::move"))
    return {};

  // Extract the expression after "return ".
  StringRef AfterReturn = FromRet.substr(7).ltrim(); // skip "return "
  size_t SemiPos = AfterReturn.find(';');
  if (SemiPos == StringRef::npos)
    return {};

  StringRef Expr = AfterReturn.substr(0, SemiPos).trim();
  if (Expr.empty())
    return {};

  // Safety: only when expr is a simple identifier (local variable name).
  // Must be alphanumeric/underscore only — not a function call, not a complex
  // expression.
  for (char C : Expr) {
    if (!isalnum(C) && C != '_')
      return {};
  }

  // Don't transform if the expression starts with a digit (it's a literal).
  if (!Expr.empty() && std::isdigit(Expr[0]))
    return {};

  std::string OldText = ("return " + Expr + ";").str();
  std::string NewText = ("return std::move(" + Expr + ");").str();

  AutoFix Fix;
  Fix.Loc = RetLoc;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = OldText;
  Fix.NewText = NewText;
  Fix.Description = "use std::move to enable move semantics on return";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixBranchlessSelect(const PerfHint &H, ASTContext &Ctx) {
  // Pattern: if (COND) VAR = V1; else VAR = V2;
  // Rewrite: VAR = COND ? V1 : V2;
  // Only when both branches are single assignment to the same variable.
  // If the pattern doesn't match cleanly, bail out.
  (void)Ctx;
  return {};
}

std::vector<AutoFix>
PerfAutoFixer::fixLoopUnswitching(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: hoist loop-invariant condition"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: hoist loop-invariant condition outside the "
                "loop\n";
  Fix.Description = "suggest hoisting loop-invariant condition outside loop";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixSIMDWidth(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("vectorize(enable)") ||
      Snippet.contains("vectorize(assume_safety)"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "#pragma clang loop vectorize(enable)\n";
  Fix.Description = "add vectorization pragma to widen SIMD width";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixFalseSharing(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  // The hint Loc typically points to the first atomic field's name, so
  // searching forward for "atomic" or "volatile" finds the NEXT field.
  StringRef Snippet = getSourceSnippet(Loc, 512, SM, LO);
  if (Snippet.empty())
    return {};

  // Already has alignas(64)?
  if (Snippet.contains("alignas(64)"))
    return {};

  // Find the next "atomic" or "volatile" keyword in the snippet — this
  // corresponds to the adjacent field that needs cache-line separation.
  size_t TargetPos = Snippet.find("atomic");
  if (TargetPos == StringRef::npos)
    TargetPos = Snippet.find("volatile");
  if (TargetPos == StringRef::npos)
    return {};

  // Walk backwards from TargetPos to find "std::" prefix if present,
  // so we insert before "std::atomic" not in the middle.
  if (TargetPos >= 5 && Snippet.substr(TargetPos - 5, 5) == "std::")
    TargetPos -= 5;

  // Insert "alignas(64) " right before the target field type.
  SourceLocation InsertLoc = Loc.getLocWithOffset(TargetPos);

  AutoFix Fix;
  Fix.Loc = InsertLoc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "alignas(64) ";
  Fix.Description = "add alignas(64) to prevent false sharing between fields";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixConstexprIf(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Already constexpr if?
  if (Snippet.contains("if constexpr"))
    return {};

  // Find "if (sizeof" or "if (std::is_" pattern.
  SourceLocation IfSizeof = findTextAfter(Loc, "if (sizeof", 256, SM, LO);
  SourceLocation IfIsType = findTextAfter(Loc, "if (std::is_", 256, SM, LO);

  SourceLocation Target;
  if (IfSizeof.isValid())
    Target = IfSizeof;
  else if (IfIsType.isValid())
    Target = IfIsType;
  else
    return {};

  AutoFix Fix;
  Fix.Loc = Target;
  Fix.FixKind = AutoFix::Replace;
  Fix.OldText = "if (";
  Fix.NewText = "if constexpr (";
  Fix.Description = "use if constexpr for compile-time constant condition";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixOutputParam(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: return by value"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: return by value instead of output "
                "parameter\n";
  Fix.Description = "suggest returning by value instead of output parameter";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixUnusedInclude(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: consider forward declaration"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: consider forward declaration instead of "
                "full include\n";
  Fix.Description = "suggest forward declaration instead of full include";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixSmallFunctionInline(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Safety: skip if already inline or __forceinline.
  if (Snippet.contains("inline") ||
      Snippet.contains("__forceinline") ||
      Snippet.contains("always_inline"))
    return {};

  // Verify this looks like a function declaration.
  if (!Snippet.contains("("))
    return {};

  // Don't insert 'inline' on member functions inside a class body or lambdas.
  if (looksLikeMemberContext(Snippet))
    return {};

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = "inline ";
  Fix.Description = "add inline hint for small function";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixSortAlgorithm(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: use std::sort()"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: use std::sort() instead of manual sort "
                "implementation\n";
  Fix.Description = "suggest std::sort() instead of manual sort";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixBitManip(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: use __builtin_popcount"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: use __builtin_popcount/__builtin_ctz "
                "instead of manual bit counting\n";
  Fix.Description = "suggest compiler builtins for bit manipulation";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixRedundantAtomic(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: batch sequential atomic"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: batch sequential atomic operations or use "
                "relaxed ordering\n";
  Fix.Description = "suggest batching sequential atomic operations";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixCacheLineSplit(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: add padding (alignas(64))") ||
      Snippet.contains("alignas(64)"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: add padding (alignas(64)) to prevent cache "
                "line splitting\n";
  Fix.Description = "suggest alignas(64) to prevent cache line splitting";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixCrossTUInline(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  if (Snippet.contains("PERF: move function definition to header"))
    return {};

  unsigned Col = SM.getSpellingColumnNumber(Loc);
  std::string Indent(Col > 1 ? Col - 1 : 0, ' ');

  SourceLocation LineStart = getLineStartLoc(Loc, SM);
  if (LineStart.isInvalid())
    return {};

  AutoFix Fix;
  Fix.Loc = LineStart;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Indent + "// PERF: move function definition to header or "
                "enable LTO for cross-TU inlining\n";
  Fix.Description = "suggest header definition or LTO for cross-TU inlining";
  return {Fix};
}

std::vector<AutoFix>
PerfAutoFixer::fixHotColdFunc(const PerfHint &H, ASTContext &Ctx) {
  const LangOptions &LO = Ctx.getLangOpts();
  SourceLocation Loc = H.SrcLoc;

  StringRef Snippet = getSourceSnippet(Loc, 256, SM, LO);
  if (Snippet.empty())
    return {};

  // Already annotated?
  if (Snippet.contains("__attribute__((hot))") ||
      Snippet.contains("__attribute__((cold))"))
    return {};

  // Verify this looks like a function declaration.
  if (!Snippet.contains("("))
    return {};

  // Determine hot vs cold from the hint message.
  bool IsCold = H.Message.find("cold") != std::string::npos;
  bool IsHot = H.Message.find("hot") != std::string::npos;

  std::string Attr;
  std::string Desc;
  if (IsCold && !IsHot) {
    Attr = "__attribute__((cold)) ";
    Desc = "add __attribute__((cold)) to mark cold function";
  } else if (IsHot && !IsCold) {
    Attr = "__attribute__((hot)) ";
    Desc = "add __attribute__((hot)) to mark hot function";
  } else if (IsHot && IsCold) {
    // Ambiguous — prefer hot since hint message mentions both
    Attr = "__attribute__((hot)) ";
    Desc = "add __attribute__((hot)) to mark hot function";
  } else {
    // Neither keyword found — default to hot
    Attr = "__attribute__((hot)) ";
    Desc = "add __attribute__((hot)) to mark frequently called function";
  }

  AutoFix Fix;
  Fix.Loc = Loc;
  Fix.FixKind = AutoFix::Insert;
  Fix.NewText = Attr;
  Fix.Description = Desc;
  return {Fix};
}

} // namespace perfsanitizer
