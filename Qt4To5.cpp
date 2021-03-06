//===- examples/Tooling/RemoveCStrCalls.cpp - Redundant c_str call removal ===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements a tool that ports from Qt 4 to Qt 5.
//
//  Usage:
//  qt4to5 <options> <source-dir> <cmake-output-dir> <file1> <file2> ...
//
//  Where <cmake-output-dir> is a CMake build directory in which a file named
//  compile_commands.json exists (enable -DCMAKE_EXPORT_COMPILE_COMMANDS in
//  CMake to get this output).
//
//  <file1> ... specify the paths of files in the CMake source tree. This path
//  is looked up in the compile command database. If the path of a file is
//  absolute, it needs to point into CMake's source tree. If the path is
//  relative, the current working directory needs to be in the CMake source
//  tree and the file must be in a subdirectory of the current working
//  directory. "./" prefixes in the relative files will be automatically
//  removed, but the rest of a relative path must be a suffix of a path in
//  the compile command line database.
//
//  For example, to use qt4to5 on all files in a subtree of the
//  source tree, use:
//
//    /path/in/subtree $ find . -name '*.cpp'|
//        xargs qt4to5 $PWD /path/to/build
//
//===----------------------------------------------------------------------===//

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <system_error>

#include <iostream>

#include "Utils.h"

using std::error_code;
using namespace clang;
using namespace clang::ast_matchers;
using namespace llvm;
using clang::tooling::newFrontendActionFactory;
using clang::tooling::Replacement;
using clang::tooling::Replacements;
using clang::tooling::CompilationDatabase;

cl::opt<std::string> SourceDir(
  cl::Positional,
  cl::desc("<source-dir>"));

cl::opt<std::string> BuildPath(
  cl::Positional,
  cl::desc("<build-path>"));

cl::opt<bool> CreateIfdefs(
  "create-ifdefs",
  cl::desc("Create ifdefs instead of rewriting in-place")
);

cl::opt<bool> PortQMetaMethodSignature(
  "port-qmetamethod-signature",
  cl::desc("Port from QMetaMethod::signature to QMetaMethod::methodSignature")
);

cl::opt<bool> PortQtEscape(
  "port-qt-escape",
  cl::desc("Port from Qt::escape to QString::toHtmlEscaped")
);

cl::opt<bool> PortAtomics(
  "port-atomics",
  cl::desc("Port from QAtomic operator casts to explicit function calls.")
);

cl::opt<std::string> RenameEnum(
  "rename-enum",
  cl::desc("Port renamed enum")
);

cl::opt<std::string> RenameMethod_Class(
  "rename-class",
  cl::desc("The class containing the method to rename"),
  cl::value_desc("class")
);

cl::opt<std::string> Rename_Old(
  "rename-old",
  cl::desc("The method to rename"),
  cl::value_desc("old method")
);

cl::opt<std::string> Rename_New(
  "rename-new",
  cl::desc("The new name of the method"),
  cl::value_desc("new method")
);

cl::opt<bool> Port_QImage_text(
  "port-qimage-text",
  cl::desc("Port uses of QImage::text")
);

cl::opt<bool> Port_QAbstractItemView_dataChanged(
  "port-qabstractitemview-datachanged",
  cl::desc("Port uses of QAbstractItemView::dataChanged")
);

cl::list<std::string> SourcePaths(
  cl::Positional,
  cl::desc("<source0> [... <sourceN>]"),
  cl::OneOrMore);

// FIXME: Pull out helper methods in here into more fitting places.

// Returns the text that makes up 'node' in the source.
// Returns an empty string if the text cannot be found.
template <typename T>
static std::string getText(const SourceManager &SourceManager, const T &Node) {
  SourceLocation StartSpellingLocation =
      SourceManager.getSpellingLoc(Node.getLocStart());
  SourceLocation EndSpellingLocation =
      SourceManager.getSpellingLoc(Node.getLocEnd());
  if (!StartSpellingLocation.isValid() || !EndSpellingLocation.isValid()) {
    return std::string();
  }
  bool Invalid = true;
  const char *Text =
      SourceManager.getCharacterData(StartSpellingLocation, &Invalid);
  if (Invalid) {
    return std::string();
  }
  std::pair<FileID, unsigned> Start =
      SourceManager.getDecomposedLoc(StartSpellingLocation);
  std::pair<FileID, unsigned> End =
      SourceManager.getDecomposedLoc(Lexer::getLocForEndOfToken(
          EndSpellingLocation, 0, SourceManager, LangOptions()));
  if (Start.first != End.first) {
    // Start and end are in different files.
    std::cout << "Start and end are in different files. " << Start.first.getHashValue() << " -- "<< End.first.getHashValue() << std::endl;
    return std::string();
  }
  if (End.second < Start.second) {
    // Shuffling text with macros may cause this.
    std::cout << "Shuffling text with macros may cause this." << Start.second << " -- "<< End.second << std::endl;
    return std::string();
  }
  return std::string(Text, End.second - Start.second);
}

template <typename T>
void insertIfdef(clang::SourceManager * const SourceManager, const T *Node, std::map<std::string, Replacements> *Replace)
{
  SourceLocation StartSpellingLocation =
      SourceManager->getSpellingLoc(Node->getLocStart());
  SourceLocation EndSpellingLocation =
      SourceManager->getSpellingLoc(Node->getLocEnd());
  if (!StartSpellingLocation.isValid() || !EndSpellingLocation.isValid()) {
    return;
  }

  FullSourceLoc fs(StartSpellingLocation, *SourceManager);

  bool Invalid = true;
  SourceLocation StartOfLine = StartSpellingLocation.getLocWithOffset(-fs.getSpellingColumnNumber(&Invalid) + 1);
  if (Invalid) {
    return;
  }

  const char *Text =
      SourceManager->getCharacterData(StartOfLine, &Invalid);
  std::pair<FileID, unsigned> Start =
      SourceManager->getDecomposedLoc(StartOfLine);
  std::pair<FileID, unsigned> End =
      SourceManager->getDecomposedLoc(Lexer::getLocForEndOfToken(
          EndSpellingLocation, 0, *SourceManager, LangOptions()));
  if (Start.first != End.first) {
    std::cout << "Start and end are in different files. " << Start.first.getHashValue() << " -- "<< End.first.getHashValue() << std::endl;
    return;
  }
  if (End.second < Start.second) {
    std::cout << "Shuffling text with macros may cause this." << Start.second << " -- "<< End.second << std::endl;
    return;
  }

  unsigned eol = End.second - Start.second;
  while (Text[eol] != '\n') {
    ++eol;
  }

  std::string ExistingText = std::string(Text, eol);

  SourceLocation EndOfLine = StartOfLine.getLocWithOffset(eol);
  
  Utils::AddReplacement(
      SourceManager->getFileEntryForID(Start.first),
      Replacement(*SourceManager, StartOfLine, 0, "#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)\n" + ExistingText + "\n#else\n"),
      Replace
    );
  //Replace->add(Replacement(*SourceManager, StartOfLine, 0, "#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)\n" + ExistingText + "\n#else\n"));
  Utils::AddReplacement(
      SourceManager->getFileEntryForID(Start.first),
      Replacement(*SourceManager, EndOfLine, 0, "\n#endif"),
      Replace
  );
  //Replace->add(Replacement(*SourceManager, EndOfLine, 0, "\n#endif"));
}

#define QStringClassName "QString"
#define QLatin1StringClassName "QLatin1String"
#define QtEscapeFunction "::Qt::escape"

namespace {
class PortQtEscape4To5 : public ast_matchers::MatchFinder::MatchCallback {
 public:
  PortQtEscape4To5(std::map<std::string, Replacements> *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    const CallExpr *Call =
        Result.Nodes.getNodeAs<CallExpr>("call");
    const Expr *CTor =
        Result.Nodes.getNodeAs<Expr>("ctor");
    const Expr *E =
        Result.Nodes.getNodeAs<Expr>("expr");
    const Expr *O =
        Result.Nodes.getNodeAs<Expr>("operator");

    const std::string ArgText = CTor ? getText(*Result.SourceManager, *CTor)
                                 : E ? getText(*Result.SourceManager, *E)
                                 : getText(*Result.SourceManager, *O);

    if (ArgText.empty()) return;

    const std::string output = (CTor || O) ? QStringClassName "(" + ArgText + ").toHtmlEscaped()"
                               : ArgText + ".toHtmlEscaped()";

    SourceManager &srcMgr = Result.Context->getSourceManager();
    Utils::AddReplacement(
      srcMgr.getFileEntryForID(srcMgr.getFileID(Call->getLocStart())),
      Replacement(*Result.SourceManager, Call, output),
      Replace
    );
    //Replace->add(Replacement(*Result.SourceManager, Call, output));

    if (CreateIfdefs)
      insertIfdef(Result.SourceManager, Call, Replace);
  }

 private:
  std::map<std::string, Replacements> *Replace;
};

class PortMetaMethods : public ast_matchers::MatchFinder::MatchCallback {
 public:
  PortMetaMethods(std::map<std::string, Replacements> *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    const Expr *Call =
        Result.Nodes.getNodeAs<Expr>("call");

    std::string ArgText = getText(*Result.SourceManager, *Call);

    if (ArgText.empty())
      return;

    std::string target = "signature";
    std::string replacement = "methodSignature";

    ArgText.replace(ArgText.find(target), target.size(), replacement);

    SourceManager &srcMgr = Result.Context->getSourceManager();
    Utils::AddReplacement(
      srcMgr.getFileEntryForID(srcMgr.getFileID(Call->getLocStart())),
      Replacement(*Result.SourceManager, Call, ArgText),
      Replace
    );
    //Replace->add(Replacement(*Result.SourceManager, Call, ArgText));

    if (CreateIfdefs)
      insertIfdef(Result.SourceManager, Call, Replace);
  }

 private:
  std::map<std::string, Replacements> *Replace;
};

class PortAtomic : public ast_matchers::MatchFinder::MatchCallback {
 public:
  PortAtomic(std::map<std::string, Replacements> *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    const CallExpr *Call =
        Result.Nodes.getNodeAs<CallExpr>("call");

    std::string ArgText = getText(*Result.SourceManager, *Call);

    if (ArgText.empty()) return;

    SourceManager &srcMgr = Result.Context->getSourceManager();
    
    Utils::AddReplacement(
      srcMgr.getFileEntryForID(srcMgr.getFileID(Call->getLocStart())),
      Replacement(*Result.SourceManager, Call, ArgText + ".load()"),
      Replace
    );
    //Replace->add(Replacement(*Result.SourceManager, Call, ArgText + ".load()"));

    if (CreateIfdefs)
      insertIfdef(Result.SourceManager, Call, Replace);
  }

private:
  std::map<std::string, Replacements> *Replace;
};

class PortEnum : public ast_matchers::MatchFinder::MatchCallback {
 public:
  PortEnum(std::map<std::string, Replacements> *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    const DeclRefExpr *Call =
        Result.Nodes.getNodeAs<DeclRefExpr>("call");

    std::string ArgText = getText(*Result.SourceManager, *Call);

    if (ArgText.empty()) return;

    ArgText.replace(ArgText.find(Rename_Old), Rename_Old.size(), Rename_New);
    
    SourceManager &srcMgr = Result.Context->getSourceManager();

    Utils::AddReplacement(
      srcMgr.getFileEntryForID(srcMgr.getFileID(Call->getLocStart())),
      Replacement(*Result.SourceManager, Call, Rename_New),
      Replace
    );
    //Replace->add(Replacement(*Result.SourceManager, Call, Rename_New));

    if (CreateIfdefs)
      insertIfdef(Result.SourceManager, Call, Replace);
  }

private:
  std::map<std::string, Replacements> *Replace;
};

class PortView2 : public ast_matchers::MatchFinder::MatchCallback {
 public:
  PortView2(std::map<std::string, Replacements> *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {

    const CXXMethodDecl *F =
        Result.Nodes.getNodeAs<CXXMethodDecl>("funcDecl");

    SourceLocation StartSpellingLocation =
        Result.SourceManager->getSpellingLoc(F->getLocStart());
    if (!StartSpellingLocation.isValid()) {
      return;
    }

    std::pair<FileID, unsigned> Start =
        Result.SourceManager->getDecomposedLoc(StartSpellingLocation);

    llvm::StringRef location = Result.SourceManager->getFileEntryForID(Start.first)->getName();

    if (!location.startswith(SourceDir))
      return;

    const ParmVarDecl *P = F->getParamDecl(F->getNumParams() - 1);

    std::string ArgText = getText(*Result.SourceManager, *P);

    std::string NewArg = "const QVector<int> &";
    if (!F->isThisDeclarationADefinition() || F->hasInlineBody())
      NewArg += " = QVector<int>()";

    SourceManager &srcMgr = Result.Context->getSourceManager();
    Utils::AddReplacement(
      srcMgr.getFileEntryForID(srcMgr.getFileID(P->getLocStart())),
      Replacement(*Result.SourceManager, P, ArgText + ", " + NewArg),
      Replace
    );
    //Replace->add(Replacement(*Result.SourceManager, P, ArgText + ", " + NewArg));

    if (CreateIfdefs)
      insertIfdef(Result.SourceManager, P, Replace);
  }

private:
  std::map<std::string, Replacements> *Replace;
};

class PortRenamedMethods : public ast_matchers::MatchFinder::MatchCallback {
 public:
  PortRenamedMethods(std::map<std::string, Replacements> *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    const CallExpr *Call =
        Result.Nodes.getNodeAs<CallExpr>("call");
    const MemberExpr *E =
        Result.Nodes.getNodeAs<MemberExpr>("expr");
    const MemberExpr *Exact =
        Result.Nodes.getNodeAs<MemberExpr>("exact");
    const Expr *Func =
        Result.Nodes.getNodeAs<Expr>("func");

    bool overriddenVirtual = false;
    if (!Exact && !Func) {
      ValueDecl *V = E->getMemberDecl();
      CXXMethodDecl *M = dyn_cast<CXXMethodDecl>(V);

      for(CXXMethodDecl::method_iterator O = M->begin_overridden_methods(); O < M->end_overridden_methods(); ++O) {
        const std::string FullNameString = "::" + (*O)->getQualifiedNameAsString();
        const llvm::StringRef FullName = FullNameString;
        const llvm::StringRef Pattern = RenameMethod_Class;
        if (FullName.find(Pattern) != llvm::StringRef::npos) {
            overriddenVirtual = true;
            break;
        }
      }
    }

    if (!(overriddenVirtual || Exact || Func))
      return;

    std::string ArgText = getText(*Result.SourceManager, *Call);

    if (ArgText.empty()) return;

    ArgText.replace(ArgText.find(Rename_Old), Rename_Old.size(), Rename_New);

    //TODO Can SourceManager be moved to helper function?
    SourceManager &srcMgr = Result.Context->getSourceManager();
    Utils::AddReplacement(
      srcMgr.getFileEntryForID(srcMgr.getFileID(Call->getLocStart())),
      Replacement(*Result.SourceManager, Call, ArgText),
      Replace
    );
    /*SourceManager& SrcMgr = Result.Context->getSourceManager();
    const FileEntry* Entry = SrcMgr.getFileEntryForID(SrcMgr.getFileID(Call->getLocStart()));
    llvm::StringRef FileName = Entry->getName();
    Replace->insert(std::pair<std::string, Replacements>(FileName, Replacement(*Result.SourceManager, Call, ArgText)));*/
  }

 private:
  std::map<std::string, Replacements> *Replace;
};

class RemoveArgument : public ast_matchers::MatchFinder::MatchCallback {
 public:
  RemoveArgument(std::map<std::string, Replacements> *Replace)
      : Replace(Replace) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    const CallExpr *Call =
        Result.Nodes.getNodeAs<CallExpr>("call");
    const Expr *Key =
        Result.Nodes.getNodeAs<Expr>("prevArg");
    const Expr *Lang =
        Result.Nodes.getNodeAs<Expr>("arg");

    SourceLocation StartSpellingLocation =
        Result.SourceManager->getSpellingLoc(Key->getLocEnd());

    SourceLocation EndSpellingLocation =
        Result.SourceManager->getSpellingLoc(Lang->getLocEnd());
    if (!StartSpellingLocation.isValid() || !EndSpellingLocation.isValid()) {
      return;
    }
    std::pair<FileID, unsigned> Start =
        Result.SourceManager->getDecomposedLoc(Lexer::getLocForEndOfToken(
            StartSpellingLocation, 0, *Result.SourceManager, LangOptions()));
    std::pair<FileID, unsigned> End =
        Result.SourceManager->getDecomposedLoc(Lexer::getLocForEndOfToken(
            EndSpellingLocation, 0, *Result.SourceManager, LangOptions()));
    if (Start.first != End.first) {
      // Start and end are in different files.
      std::cout << "Start and end are in different files. " << Start.first.getHashValue() << " -- "<< End.first.getHashValue() << std::endl;
      return;
    }
    if (End.second < Start.second) {
      // Shuffling text with macros may cause this.
      std::cout << "Shuffling text with macros may cause this." << Start.second << " -- "<< End.second << std::endl;
      return;
    }

    // Key->getLocEnd() doesn't get the end of the token, but the start.
    // Use this hack to get the real ends.
    SourceLocation StartOfFile = StartSpellingLocation.getLocWithOffset(-Result.SourceManager->getFileOffset(Key->getLocStart()));
    StartSpellingLocation = StartOfFile.getLocWithOffset(Start.second);
    EndSpellingLocation = StartOfFile.getLocWithOffset(End.second);

    CharSourceRange range;
    range.setBegin(StartSpellingLocation);
    range.setEnd(EndSpellingLocation);
    
    SourceManager &srcMgr = Result.Context->getSourceManager();
    Utils::AddReplacement(
      srcMgr.getFileEntryForID(Start.first),
      Replacement(*Result.SourceManager, range, ""),
      Replace
    );
    //Replace->add(Replacement(*Result.SourceManager, range, ""));

    if (CreateIfdefs)
      insertIfdef(Result.SourceManager, Call, Replace);
  }

 private:
  std::map<std::string, Replacements> *Replace;
};
} // end namespace

int portMethod(const CompilationDatabase &Compilations)
{
  tooling::RefactoringTool Tool(Compilations, SourcePaths);

  ast_matchers::MatchFinder Finder;

  std::string matchName = RenameMethod_Class.size() ? RenameMethod_Class : std::string();
  matchName += "::" + Rename_Old;

  PortRenamedMethods RenameMethodCallback(&Tool.getReplacements());

  Finder.addMatcher(
      callExpr(
        anyOf(
          allOf(
            callee(functionDecl(hasName(matchName))),
            callee(memberExpr().bind("exact"))
          ),
          allOf(
            callee(functionDecl(hasName(Rename_Old))),
            callee(memberExpr().bind("expr"))
          ),
          allOf(
            callee(functionDecl(hasName(matchName))),
            callee(expr().bind("func"))
          )
        )
      ).bind("call"), 
      &RenameMethodCallback);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}

int portQMetaMethodSignature(const CompilationDatabase &Compilations)
{
  tooling::RefactoringTool Tool(Compilations, SourcePaths);

  ast_matchers::MatchFinder Finder;

  PortMetaMethods MetaMethodCallback(&Tool.getReplacements());

  Finder.addMatcher(
    	stmt(
      	stmt(
          has(
            callExpr(
              callee(
                memberExpr()
              )
            ).bind("call")
          ),
          has(callExpr(callee(functionDecl(hasName("::QMetaMethod::signature")))))
        ),
        expr(unless(clang::ast_matchers::binaryOperator()))
      )
    , &MetaMethodCallback);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}

int portQtEscape(const CompilationDatabase &Compilations)
{
  tooling::RefactoringTool Tool(Compilations, SourcePaths);

  ast_matchers::MatchFinder Finder;

  PortQtEscape4To5 Callback(&Tool.getReplacements());

  Finder.addMatcher(
    callExpr(
      callee(functionDecl(hasName(QtEscapeFunction))),
      hasArgument(
        0,
        anyOf(
          cxxBindTemporaryExpr(has(cxxOperatorCallExpr().bind("operator"))),//unclear wheather or not this is needed still
          cxxOperatorCallExpr().bind("operator"),
          cxxConstructExpr().bind("ctor"),
          expr().bind("expr")
        )
      )
    ).bind("call"),
    &Callback);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}

int portAtomics(const CompilationDatabase &Compilations)
{
  tooling::RefactoringTool Tool(Compilations, SourcePaths);

  ast_matchers::MatchFinder Finder;

  PortAtomic AtomicCallback(&Tool.getReplacements());

  Finder.addMatcher(
      callExpr(
        callee(functionDecl(hasName("::QBasicAtomicInt::operator int")))
      ).bind("call"), &AtomicCallback);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}

int portQImageText(const CompilationDatabase &Compilations)
{
  tooling::RefactoringTool Tool(Compilations, SourcePaths);

  ast_matchers::MatchFinder Finder;

  RemoveArgument ImageTextCallback(&Tool.getReplacements());

  Finder.addMatcher(
      callExpr(
        callee(functionDecl(hasName("::QImage::text"))),
        hasArgument(
          0,
          expr().bind("prevArg")
        ),
        hasArgument(
          1,
          expr(clang::ast_matchers::integerLiteral(equals(0)).bind("arg"))
        )
      ).bind("call"), &ImageTextCallback);

  Finder.addMatcher(
      callExpr(
        callee(functionDecl(hasName("::QImage::setText"))),
        hasArgument(
          0,
          expr().bind("prevArg")
        ),
        hasArgument(
          1,
          expr(clang::ast_matchers::integerLiteral(equals(0)).bind("arg"))
        )
      ).bind("call"), &ImageTextCallback);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}

int portViewDataChanged(const CompilationDatabase &Compilations)
{
  tooling::RefactoringTool Tool(Compilations, SourcePaths);

  ast_matchers::MatchFinder Finder;

  PortView2 ViewCallback2(&Tool.getReplacements());

  Finder.addMatcher(
      cxxMethodDecl(
        hasName("dataChanged"),
        ofClass(
          allOf(
            isDerivedFrom("QAbstractItemView"),
            unless(hasName("QAbstractItemView"))
          )
        )
      ).bind("funcDecl"), &ViewCallback2);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}

namespace clang {
namespace ast_matchers {
const internal::VariadicDynCastAllOfMatcher<clang::Decl, clang::EnumConstantDecl> enumeratorConstant;
}
}

int portEnum(const CompilationDatabase &Compilations)
{
  tooling::RefactoringTool Tool(Compilations, SourcePaths);

  ast_matchers::MatchFinder Finder;

  PortEnum Callback(&Tool.getReplacements());

  Finder.addMatcher(
    declRefExpr(to(enumeratorConstant(hasName(RenameEnum + "::" + Rename_Old)))).bind("call"),
    &Callback);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);
  std::string ErrorMessage;
  std::unique_ptr<CompilationDatabase> Compilations(
    CompilationDatabase::loadFromDirectory(BuildPath, ErrorMessage));


  if (!Compilations)
    llvm::report_fatal_error(ErrorMessage);

  if (RenameEnum != std::string())
    return portEnum(*Compilations);

  if (Rename_Old != std::string() && Rename_New != std::string())
    return portMethod(*Compilations);

  if (PortQMetaMethodSignature)
    return portQMetaMethodSignature(*Compilations);

  if (PortQtEscape)
    return portQtEscape(*Compilations);

  if (PortAtomics)
    return portAtomics(*Compilations);

  if(Port_QImage_text)
    return portQImageText(*Compilations);

  if (Port_QAbstractItemView_dataChanged)
    return portViewDataChanged(*Compilations);

  return 1; // No useful arguments.
}
