#include "interpreter.hpp"

#include <iostream>
#include <string>
#include <vector>

#ifndef BEE_VERSION
#define BEE_VERSION "0.1.1"
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
        if (arg == "-h" || arg == "--help") {
            std::cout << "bee " << BEE_VERSION << " - a small, friendly scripting language\n\n";
            std::cout << "usage: " << prog << " <script.bee> [args...]\n\n";
            std::cout << "options:\n";
            std::cout << "  -v, --version   print the version and exit\n";
            std::cout << "  -h, --help      print this help and exit\n";
            return 0;
        }
    }

    if (argc < 2) {
        std::cerr << "bee " << BEE_VERSION << " - a small, friendly scripting language\n";
        std::cerr << "usage: " << prog << " <script.bee> [args...]\n";
        std::cerr << "try '" << prog << " --help' for more information.\n";
        return 64; // EX_USAGE
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
