#pragma once
// The slice of libclang's C API that beegen uses, loaded at run time.
//
// libclang is reached through dlopen rather than <clang-c/Index.h> on purpose:
// building beegen then needs no clang headers at all, only a libclang shared
// library on the machine that runs it -- which any clang install provides. The C
// API and these struct layouts have been stable for a decade, and
// Clang::load() fails loudly if a symbol is missing rather than crashing later.
#include <functional>
#include <string>
#include <vector>

namespace beegen {

// ---------------------------------------------------------------------------
// ABI mirror -- layouts must match clang-c/Index.h exactly
// ---------------------------------------------------------------------------
extern "C" {
using CXIndex = void*;
using CXTranslationUnit = void*;
using CXFile = void*;
using CXDiagnostic = void*;

struct CXString { const void* data; unsigned private_flags; };
struct CXCursor { int kind; int xdata; const void* data[3]; };
struct CXType { int kind; void* data[2]; };
struct CXSourceLocation { const void* ptr_data[2]; unsigned int_data; };
}

// CXCursorKind values we care about.
enum CursorKind {
    Cursor_StructDecl = 2,
    Cursor_UnionDecl = 3,
    Cursor_ClassDecl = 4,
    Cursor_EnumDecl = 5,
    Cursor_FieldDecl = 6,
    Cursor_EnumConstantDecl = 7,
    Cursor_FunctionDecl = 8,
    Cursor_VarDecl = 9,
    Cursor_ParmDecl = 10,
    Cursor_TypedefDecl = 20,
    Cursor_CXXMethod = 21,
    Cursor_Namespace = 22,
    Cursor_Constructor = 24,
    Cursor_Destructor = 25,
    Cursor_ConversionFunction = 26,
    Cursor_FunctionTemplate = 30,
    Cursor_ClassTemplate = 31,
    Cursor_TypeAliasDecl = 36,
    Cursor_AccessSpecifier = 39,
    Cursor_BaseSpecifier = 44,
};

// CXTypeKind values we care about.
enum TypeKind {
    Type_Invalid = 0,
    Type_Unexposed = 1,
    Type_Void = 2,
    Type_Bool = 3,
    Type_Char_U = 4,
    Type_UChar = 5,
    Type_UShort = 8,
    Type_UInt = 9,
    Type_ULong = 10,
    Type_ULongLong = 11,
    Type_Char_S = 13,
    Type_SChar = 14,
    Type_WChar = 15,
    Type_Short = 16,
    Type_Int = 17,
    Type_Long = 18,
    Type_LongLong = 19,
    Type_Float = 21,
    Type_Double = 22,
    Type_LongDouble = 23,
    Type_Pointer = 101,
    Type_LValueReference = 103,
    Type_RValueReference = 104,
    Type_Record = 105,
    Type_Enum = 106,
    Type_Typedef = 107,
    Type_ConstantArray = 112,
    Type_Elaborated = 119,
};

enum Access { Access_Invalid = 0, Access_Public = 1, Access_Protected = 2, Access_Private = 3 };
enum VisitResult { Visit_Break = 0, Visit_Continue = 1, Visit_Recurse = 2 };

// ---------------------------------------------------------------------------
// The loaded library
// ---------------------------------------------------------------------------
class Clang {
public:
    // Try, in order: an explicit path, LIBCLANG_PATH, then the usual sonames and
    // llvm-*/lib locations. `err` explains what was tried on failure.
    bool load(const std::string& explicitPath, std::string& err);

    // Parse `header` with `args` (e.g. -x c++ -std=c++17 -I...). Diagnostics of
    // error severity are collected into `diagnostics`; parsing continues anyway,
    // since a header that half-parses still yields useful bindings.
    bool parse(const std::string& header, const std::vector<std::string>& args,
               std::vector<std::string>& diagnostics, std::string& err);
    void close();
    ~Clang() { close(); }

    CXCursor root() const { return getTUCursor_(tu_); }

    // ---- cursor queries ----
    std::string spelling(CXCursor c) const { return take(cursorSpelling_(c)); }
    int kind(CXCursor c) const { return c.kind; }
    CXType type(CXCursor c) const { return cursorType_(c); }
    CXType resultType(CXCursor c) const { return cursorResultType_(c); }
    int numArgs(CXCursor c) const { return numArguments_(c); }
    CXCursor arg(CXCursor c, unsigned i) const { return getArgument_(c, i); }
    int access(CXCursor c) const { return accessSpecifier_(c); }
    bool isStaticMethod(CXCursor c) const { return methodIsStatic_(c) != 0; }
    bool isVariadic(CXCursor c) const { return cursorIsVariadic_(c) != 0; }
    bool isDeleted(CXCursor c) const { return availability_(c) == 2 /* NotAvailable */; }
    // False for a forward declaration: `struct Foo;` names a type we can
    // neither construct nor size, so it isn't bindable.
    bool isDefinition(CXCursor c) const { return isDefinition_(c) != 0; }
    // A class with a pure virtual method can't be constructed with `new`, which
    // is exactly how interface-based APIs (TensorRT, ONNX Runtime) are shaped.
    bool isPureVirtual(CXCursor c) const { return isPureVirtual_(c) != 0; }
    CXCursor typeDeclaration(CXType t) const { return typeDeclaration_(t); }
    long long enumValue(CXCursor c) const { return enumConstantValue_(c); }
    std::string fileOf(CXCursor c) const;
    unsigned lineOf(CXCursor c) const;
    bool inSystemHeader(CXCursor c) const;

    // ---- type queries ----
    std::string typeSpelling(CXType t) const { return take(typeSpelling_(t)); }
    CXType canonical(CXType t) const { return canonicalType_(t); }
    CXType pointee(CXType t) const { return pointeeType_(t); }
    bool isConstQualified(CXType t) const { return isConstQualified_(t) != 0; }
    int numTemplateArgs(CXType t) const { return numTemplateArguments_(t); }
    CXType templateArg(CXType t, unsigned i) const { return templateArgumentAsType_(t, i); }

    // Walk `parent`'s direct children. Returning Visit_Recurse from `fn`
    // descends; the common case is Visit_Continue.
    void visit(CXCursor parent, const std::function<int(CXCursor, CXCursor)>& fn) const;

private:
    std::string take(CXString s) const;   // read then dispose

    void* lib_ = nullptr;
    CXIndex index_ = nullptr;
    CXTranslationUnit tu_ = nullptr;

    // Function pointers, named after the libclang symbols they hold.
    CXIndex (*createIndex_)(int, int) = nullptr;
    void (*disposeIndex_)(CXIndex) = nullptr;
    CXTranslationUnit (*parseTU_)(CXIndex, const char*, const char* const*, int, void*,
                                  unsigned, unsigned) = nullptr;
    void (*disposeTU_)(CXTranslationUnit) = nullptr;
    CXCursor (*getTUCursor_)(CXTranslationUnit) = nullptr;
    unsigned (*visitChildren_)(CXCursor, int (*)(CXCursor, CXCursor, void*), void*) = nullptr;
    const char* (*getCString_)(CXString) = nullptr;
    void (*disposeString_)(CXString) = nullptr;
    CXString (*cursorSpelling_)(CXCursor) = nullptr;
    CXType (*cursorType_)(CXCursor) = nullptr;
    CXType (*cursorResultType_)(CXCursor) = nullptr;
    CXString (*typeSpelling_)(CXType) = nullptr;
    int (*numArguments_)(CXCursor) = nullptr;
    CXCursor (*getArgument_)(CXCursor, unsigned) = nullptr;
    int (*accessSpecifier_)(CXCursor) = nullptr;
    unsigned (*methodIsStatic_)(CXCursor) = nullptr;
    unsigned (*cursorIsVariadic_)(CXCursor) = nullptr;
    int (*availability_)(CXCursor) = nullptr;
    unsigned (*isDefinition_)(CXCursor) = nullptr;
    unsigned (*isPureVirtual_)(CXCursor) = nullptr;
    CXCursor (*typeDeclaration_)(CXType) = nullptr;
    long long (*enumConstantValue_)(CXCursor) = nullptr;
    CXType (*canonicalType_)(CXType) = nullptr;
    CXType (*pointeeType_)(CXType) = nullptr;
    unsigned (*isConstQualified_)(CXType) = nullptr;
    int (*numTemplateArguments_)(CXType) = nullptr;
    CXType (*templateArgumentAsType_)(CXType, unsigned) = nullptr;
    CXSourceLocation (*cursorLocation_)(CXCursor) = nullptr;
    void (*spellingLocation_)(CXSourceLocation, CXFile*, unsigned*, unsigned*, unsigned*) = nullptr;
    CXString (*fileName_)(CXFile) = nullptr;
    int (*locationInSystemHeader_)(CXSourceLocation) = nullptr;
    unsigned (*numDiagnostics_)(CXTranslationUnit) = nullptr;
    CXDiagnostic (*getDiagnostic_)(CXTranslationUnit, unsigned) = nullptr;
    CXString (*formatDiagnostic_)(CXDiagnostic, unsigned) = nullptr;
    int (*diagnosticSeverity_)(CXDiagnostic) = nullptr;
    void (*disposeDiagnostic_)(CXDiagnostic) = nullptr;
};

}  // namespace beegen
