// hive -- the BeeLang package manager.
#include "hive.hpp"

#include <iostream>
#include <string>
#include <vector>

#ifndef HIVE_VERSION
#define HIVE_VERSION "0.2.0"
#endif

namespace {

void usage(std::ostream& out, const char* prog) {
    out << "hive " << HIVE_VERSION << " - the BeeLang package manager\n\n"
        << "usage: " << prog << " <command> [arguments] [options]\n\n"
        << "commands:\n"
        << "  install                    install the dependencies in hive.json\n"
        << "  install <name>[@version]   install a package from the registry\n"
        << "  install <file.hive>        install a package from a local archive\n"
        << "  uninstall <name>...        remove installed packages\n"
        << "  list                       show what is installed\n"
        << "  info <name>                show a package's versions and dependencies\n"
        << "  search <query>             search the registry\n"
        << "  init [dir]                 create a hive.json (and a starter init.bee)\n"
        << "  pack [dir]                 build a .hive archive from a package\n"
        << "  cache [dir|clean]          show or clear the download cache\n\n"
        << "options:\n"
        << "  -g, --global               act on the global library instead of ./hive_modules\n"
        << "      --registry <url>       use this registry for this run\n"
        << "      --offline              never hit the network; use the cache only\n"
        << "      --no-save              don't record the package in hive.json\n"
        << "      --force                overwrite files hive doesn't own\n"
        << "  -u, --update               ignore hive.lock pins and take the newest match\n"
        << "  -o, --output <file>        where `pack` writes the archive\n"
        << "  -C, --dir <dir>            treat this directory as the project root\n"
        << "  -q, --quiet                only print errors\n"
        << "  -v, --version              print the version and exit\n"
        << "  -h, --help                 print this help and exit\n\n"
        << "packages install into ./hive_modules (or " << hive::globalLibDir() << " with -g),\n"
        << "where `bee` finds them for `import <name>`.\n";
}

}  // namespace

int main(int argc, char** argv) {
    const char* prog = (argc ? argv[0] : "hive");
    std::vector<std::string> raw(argv + (argc ? 1 : 0), argv + argc);

    hive::Options opts;
    std::string command;
    std::vector<std::string> args;

    for (size_t i = 0; i < raw.size(); ++i) {
        const std::string& a = raw[i];
        auto valueFor = [&](const char* flag) -> std::string {
            if (i + 1 >= raw.size()) {
                std::cerr << "hive: " << flag << " needs a value\n";
                std::exit(64);
            }
            return raw[++i];
        };

        if (a == "-h" || a == "--help") {
            usage(std::cout, prog);
            return 0;
        }
        if (a == "-v" || a == "--version") {
            std::cout << "hive " << HIVE_VERSION << "\n";
            return 0;
        }
        if (a == "-g" || a == "--global")   { opts.global = true; continue; }
        if (a == "--offline")               { opts.offline = true; continue; }
        if (a == "--force")                 { opts.force = true; continue; }
        if (a == "-u" || a == "--update")   { opts.update = true; continue; }
        if (a == "--no-save")               { opts.save = false; continue; }
        if (a == "--save")                  { opts.save = true; continue; }
        if (a == "-q" || a == "--quiet")    { opts.quiet = true; continue; }
        if (a == "--registry")              { opts.registry = valueFor("--registry"); continue; }
        if (a == "-o" || a == "--output")   { opts.outFile = valueFor("--output"); continue; }
        if (a == "-C" || a == "--dir")      { opts.dir = valueFor("--dir"); continue; }
        if (a.size() > 1 && a[0] == '-' && !hive::isFile(a)) {
            std::cerr << "hive: unknown option '" << a << "'\n";
            std::cerr << "try '" << prog << " --help' for more information.\n";
            return 64;  // EX_USAGE
        }
        if (command.empty()) command = a;
        else args.push_back(a);
    }

    if (command.empty()) {
        usage(std::cerr, prog);
        return 64;
    }

    // `hive add` reads naturally too, and `hive remove` matches what people type.
    if (command == "install" || command == "add" || command == "i")
        return hive::cmdInstall(args, opts);
    if (command == "uninstall" || command == "remove" || command == "rm")
        return hive::cmdUninstall(args, opts);
    if (command == "list" || command == "ls")     return hive::cmdList(args, opts);
    if (command == "info" || command == "show")   return hive::cmdInfo(args, opts);
    if (command == "search")                      return hive::cmdSearch(args, opts);
    if (command == "init")                        return hive::cmdInit(args, opts);
    if (command == "pack" || command == "build")  return hive::cmdPack(args, opts);
    if (command == "cache")                       return hive::cmdCache(args, opts);

    std::cerr << "hive: unknown command '" << command << "'\n";
    std::cerr << "try '" << prog << " --help' for more information.\n";
    return 64;
}
