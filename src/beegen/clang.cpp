#include "clang.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace beegen {

namespace {
// libclang is loaded at run time, so beegen builds without clang headers or
// import libraries on any platform. Only two calls differ between them.
void* libOpen(const char* path) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_LAZY | RTLD_LOCAL);
#endif
}

void* libSym(void* lib, const char* name) {
#ifdef _WIN32
    // GetProcAddress returns FARPROC (a function pointer); every caller casts it
    // straight back to the right signature, as dlsym's void* is used below.
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}
}  // namespace

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
bool Clang::load(const std::string& explicitPath, std::string& err) {
    std::vector<std::string> candidates;
    if (!explicitPath.empty()) candidates.push_back(explicitPath);
    if (const char* env = std::getenv("LIBCLANG_PATH")) if (*env) candidates.push_back(env);
    // Plain sonames first (the dynamic loader knows where to look), then the
    // versioned directories distributions install alongside each LLVM release.
#ifdef _WIN32
    candidates.insert(candidates.end(), {
        "libclang.dll", "clang.dll",
        "C:\\Program Files\\LLVM\\bin\\libclang.dll",
        "C:\\Program Files (x86)\\LLVM\\bin\\libclang.dll",
    });
#else
    candidates.insert(candidates.end(), {
        "libclang.so.1", "libclang.so", "libclang.dylib",
        "/usr/lib/llvm-19/lib/libclang.so.1", "/usr/lib/llvm-18/lib/libclang.so.1",
        "/usr/lib/llvm-17/lib/libclang.so.1", "/usr/lib/llvm-16/lib/libclang.so.1",
        "/usr/lib/x86_64-linux-gnu/libclang-19.so.1", "/usr/lib/x86_64-linux-gnu/libclang-18.so.1",
        "/usr/local/lib/libclang.so", "/opt/homebrew/opt/llvm/lib/libclang.dylib",
        "/Library/Developer/CommandLineTools/usr/lib/libclang.dylib",
    });
#endif

    for (auto& path : candidates) {
        lib_ = libOpen(path.c_str());
        if (lib_) break;
    }
    if (!lib_) {
        err = "cannot find libclang. Tried: ";
        for (size_t i = 0; i < candidates.size(); ++i)
            err += (i ? ", " : "") + candidates[i];
#ifdef _WIN32
        err += "\n       Install LLVM (https://releases.llvm.org, or"
               " winget install LLVM.LLVM) or set LIBCLANG_PATH to libclang.dll.";
#else
        err += "\n       Install it (Debian/Ubuntu: sudo apt install libclang-dev)"
               " or set LIBCLANG_PATH to the library.";
#endif
        return false;
    }

    std::string missing;
    auto sym = [&](const char* name) -> void* {
        void* p = libSym(lib_, name);
        if (!p) missing += (missing.empty() ? "" : ", ") + std::string(name);
        return p;
    };

    createIndex_    = (decltype(createIndex_))sym("clang_createIndex");
    disposeIndex_   = (decltype(disposeIndex_))sym("clang_disposeIndex");
    parseTU_        = (decltype(parseTU_))sym("clang_parseTranslationUnit");
    disposeTU_      = (decltype(disposeTU_))sym("clang_disposeTranslationUnit");
    getTUCursor_    = (decltype(getTUCursor_))sym("clang_getTranslationUnitCursor");
    visitChildren_  = (decltype(visitChildren_))sym("clang_visitChildren");
    getCString_     = (decltype(getCString_))sym("clang_getCString");
    disposeString_  = (decltype(disposeString_))sym("clang_disposeString");
    cursorSpelling_ = (decltype(cursorSpelling_))sym("clang_getCursorSpelling");
    cursorType_     = (decltype(cursorType_))sym("clang_getCursorType");
    cursorResultType_ = (decltype(cursorResultType_))sym("clang_getCursorResultType");
    typeSpelling_   = (decltype(typeSpelling_))sym("clang_getTypeSpelling");
    numArguments_   = (decltype(numArguments_))sym("clang_Cursor_getNumArguments");
    getArgument_    = (decltype(getArgument_))sym("clang_Cursor_getArgument");
    accessSpecifier_ = (decltype(accessSpecifier_))sym("clang_getCXXAccessSpecifier");
    methodIsStatic_ = (decltype(methodIsStatic_))sym("clang_CXXMethod_isStatic");
    cursorIsVariadic_ = (decltype(cursorIsVariadic_))sym("clang_Cursor_isVariadic");
    availability_   = (decltype(availability_))sym("clang_getCursorAvailability");
    isDefinition_   = (decltype(isDefinition_))sym("clang_isCursorDefinition");
    isPureVirtual_  = (decltype(isPureVirtual_))sym("clang_CXXMethod_isPureVirtual");
    typeDeclaration_ = (decltype(typeDeclaration_))sym("clang_getTypeDeclaration");
    enumConstantValue_ = (decltype(enumConstantValue_))sym("clang_getEnumConstantDeclValue");
    canonicalType_  = (decltype(canonicalType_))sym("clang_getCanonicalType");
    pointeeType_    = (decltype(pointeeType_))sym("clang_getPointeeType");
    isConstQualified_ = (decltype(isConstQualified_))sym("clang_isConstQualifiedType");
    numTemplateArguments_ = (decltype(numTemplateArguments_))sym("clang_Type_getNumTemplateArguments");
    templateArgumentAsType_ = (decltype(templateArgumentAsType_))sym("clang_Type_getTemplateArgumentAsType");
    cursorLocation_ = (decltype(cursorLocation_))sym("clang_getCursorLocation");
    spellingLocation_ = (decltype(spellingLocation_))sym("clang_getSpellingLocation");
    fileName_       = (decltype(fileName_))sym("clang_getFileName");
    locationInSystemHeader_ = (decltype(locationInSystemHeader_))sym("clang_Location_isInSystemHeader");
    numDiagnostics_ = (decltype(numDiagnostics_))sym("clang_getNumDiagnostics");
    getDiagnostic_  = (decltype(getDiagnostic_))sym("clang_getDiagnostic");
    formatDiagnostic_ = (decltype(formatDiagnostic_))sym("clang_formatDiagnostic");
    diagnosticSeverity_ = (decltype(diagnosticSeverity_))sym("clang_getDiagnosticSeverity");
    disposeDiagnostic_ = (decltype(disposeDiagnostic_))sym("clang_disposeDiagnostic");

    if (!missing.empty()) {
        err = "this libclang is missing symbols beegen needs: " + missing;
        return false;
    }
    index_ = createIndex_(0, 0);
    return index_ != nullptr;
}

void Clang::close() {
    if (tu_ && disposeTU_) disposeTU_(tu_);
    if (index_ && disposeIndex_) disposeIndex_(index_);
    tu_ = nullptr;
    index_ = nullptr;
    // The library itself stays loaded: CXString buffers handed out earlier may
    // still be referenced, and the process is about to exit anyway.
}

std::string Clang::take(CXString s) const {
    const char* c = getCString_(s);
    std::string out = c ? c : "";
    disposeString_(s);
    return out;
}

bool Clang::parse(const std::string& header, const std::vector<std::string>& args,
                  std::vector<std::string>& diagnostics, std::string& err) {
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.c_str());

    // CXTranslationUnit_DetailedPreprocessingRecord is not needed; the default
    // options give us declarations, which is all a binding generator reads.
    tu_ = parseTU_(index_, header.c_str(), argv.empty() ? nullptr : argv.data(),
                   (int)argv.size(), nullptr, 0, 0);
    if (!tu_) {
        err = "clang could not parse '" + header + "'";
        return false;
    }

    const unsigned count = numDiagnostics_(tu_);
    for (unsigned i = 0; i < count; ++i) {
        CXDiagnostic d = getDiagnostic_(tu_, i);
        // 3 = CXDiagnostic_Error, 4 = Fatal. Warnings are the header's business.
        if (diagnosticSeverity_(d) >= 3) diagnostics.push_back(take(formatDiagnostic_(d, 1)));
        disposeDiagnostic_(d);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
std::string Clang::fileOf(CXCursor c) const {
    CXFile file = nullptr;
    unsigned line = 0, col = 0, offset = 0;
    spellingLocation_(cursorLocation_(c), &file, &line, &col, &offset);
    if (!file) return "";
    return take(fileName_(file));
}

unsigned Clang::lineOf(CXCursor c) const {
    CXFile file = nullptr;
    unsigned line = 0, col = 0, offset = 0;
    spellingLocation_(cursorLocation_(c), &file, &line, &col, &offset);
    return line;
}

bool Clang::inSystemHeader(CXCursor c) const {
    return locationInSystemHeader_(cursorLocation_(c)) != 0;
}

namespace {
// libclang's visitor is a plain function pointer, so the std::function travels
// through client_data.
struct VisitState { const std::function<int(CXCursor, CXCursor)>* fn; };

int trampoline(CXCursor cursor, CXCursor parent, void* data) {
    return (*static_cast<VisitState*>(data)->fn)(cursor, parent);
}
}  // namespace

void Clang::visit(CXCursor parent, const std::function<int(CXCursor, CXCursor)>& fn) const {
    VisitState state{&fn};
    visitChildren_(parent, trampoline, &state);
}

}  // namespace beegen
