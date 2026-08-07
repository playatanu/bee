#include "interpreter.hpp"

#include <iostream>
#include <string>
#include <vector>

#include <sstream>

#ifdef _WIN32
#include <io.h>
#else
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#endif

#ifndef BEE_VERSION
#define BEE_VERSION "0.3.1"
#endif

#ifndef _WIN32
namespace {

// The interpreter bounds its own call depth (see Interpreter::maxCallDepth), but
// a numeric function compiled by the JIT recurses on the real C++ stack without
// passing through callFunction, so it can still exhaust it. Without a handler
// that ends as a bare "Segmentation fault" with nothing to go on.
//
// The handler runs on its own stack -- the normal one is exactly what ran out --
// and may only use async-signal-safe calls, so it write()s a fixed message.
const char kStackOverflowMessage[] =
    "\nFatal: bee ran out of stack (SIGSEGV).\n"
    "  The usual cause is unbounded recursion in a numeric function, which the\n"
    "  JIT compiles to native code and so runs outside the interpreter's own\n"
    "  depth limit. Check that your recursion has a base case.\n"
    "  If the depth is intentional, raise the stack: ulimit -s 65536\n"
    "  If neither fits, this is a bug -- please report it:\n"
    "  https://github.com/beelang-project/bee/issues\n";

void onSegv(int) {
    ssize_t ignored = ::write(STDERR_FILENO, kStackOverflowMessage,
                              sizeof kStackOverflowMessage - 1);
    (void)ignored;
    _exit(70);  // EX_SOFTWARE, matching how other fatal errors exit
}

void installCrashHandler() {
    // An escape hatch for debugging the interpreter itself, where a core dump
    // is worth more than a friendly message.
    if (const char* off = std::getenv("BEE_NO_CRASH_HANDLER"))
        if (*off && *off != '0') return;

    // Fixed size: modern glibc makes SIGSTKSZ a sysconf() call, not a constant.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    if (sigaltstack(&ss, nullptr) != 0) return;

    struct sigaction sa{};
    sa.sa_handler = onSegv;
    sa.sa_flags = SA_ONSTACK | SA_RESETHAND;   // reset, so a fault *in* the
    sigemptyset(&sa.sa_mask);                  // handler still dumps normally
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

}  // namespace
#endif

int main(int argc, char** argv) {
    const char* prog = (argc ? argv[0] : "bee");

    // Handle flags that take the place of a script path.
    if (argc >= 2) {
        std::string arg = argv[1];
        if (arg == "-v" || arg == "--version") {
            std::cout << "bee " << BEE_VERSION << "\n";
            return 0;
        }
        if (arg == "-e" || arg == "--eval") {
            if (argc < 3) {
                std::cerr << "bee: " << arg << " needs some code to run\n";
                return 64; // EX_USAGE
            }
#ifndef _WIN32
            installCrashHandler();
#endif
            bee::Interpreter interp;
            std::vector<std::string> scriptArgs(argv + 3, argv + argc);
            interp.setScriptArgs(scriptArgs);
            try {
                interp.runSource(argv[2], "<eval>", ".");
            } catch (const std::exception& e) {
                std::cerr << e.what() << "\n";
                return 70; // EX_SOFTWARE
            }
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            std::cout << "bee " << BEE_VERSION << " - a small, friendly scripting language\n\n";
            std::cout << "usage: " << prog << " <script.bee> [args...]\n";
            std::cout << "       " << prog << " -e '<code>'      run one line of code\n";
            std::cout << "       " << prog << "                  start the interactive REPL\n";
            std::cout << "       " << prog << " < script.bee     run a program from stdin\n\n";
            std::cout << "options:\n";
            std::cout << "  -e, --eval <code>  run the given code and exit\n";
            std::cout << "  -v, --version      print the version and exit\n";
            std::cout << "  -h, --help         print this help and exit\n";
            return 0;
        }
    }

#ifndef _WIN32
    installCrashHandler();
#endif

    // No script named: talk to a person if there's a terminal, otherwise read the
    // program from stdin so `bee < script.bee` and pipelines work.
    if (argc < 2) {
#ifdef _WIN32
        const bool interactive = _isatty(_fileno(stdin)) != 0;
#else
        const bool interactive = isatty(STDIN_FILENO) != 0;
#endif
        bee::Interpreter interp;
        if (interactive) {
            std::cout << "bee " << BEE_VERSION << " - interactive session\n";
            std::cout << "type an expression to see its value; 'exit' or Ctrl-D to quit\n";
            interp.runRepl();
            return 0;
        }
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        try {
            interp.runSource(buffer.str(), "<stdin>", ".");
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 70; // EX_SOFTWARE
        }
        return 0;
    }

    bee::Interpreter interp;
    std::vector<std::string> scriptArgs(argv + 2, argv + argc); // args after the script path
    interp.setScriptArgs(scriptArgs);
    try {
        interp.runFile(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 70; // EX_SOFTWARE
    }
    return 0;
}
