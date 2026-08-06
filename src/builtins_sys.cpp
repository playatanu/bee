// System built-ins: file I/O, time/date, random, environment, processes, and
// GIL-scheduled threads. Kept separate from the core builtins for readability.
#include "interpreter.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <array>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace bee {

static double numArgS(const Value& v, const std::string& who) {
    if (!v.isNumber()) throw RuntimeError(who + ": expected a number");
    return v.asNumber();
}
static const std::string& strArgS(const Value& v, const std::string& who) {
    if (!v.isString()) throw RuntimeError(who + ": expected a string");
    return v.asString();
}

// Release the GIL for the duration of a blocking call, then reacquire it.
namespace {
struct GilOff {
    Interpreter& i;
    explicit GilOff(Interpreter& x) : i(x) { i.gilRelease(); }
    ~GilOff() { i.gilAcquire(); }
    GilOff(const GilOff&) = delete;
    GilOff& operator=(const GilOff&) = delete;
};
}

void Interpreter::defineSystemBuiltins() {
    auto def = [&](const std::string& n, int arity,
                   std::function<Value(Interpreter&, std::vector<Value>&)> f) {
        auto b = std::make_shared<Builtin>();
        b->name = n;
        b->arity = arity;
        b->fn = std::move(f);
        globals->define(n, Value(b));
    };

    // ---------------------------------------------------------------
    // File I/O
    // ---------------------------------------------------------------
    def("read_file", 1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        const std::string path = strArgS(a[0], "read_file");
        GilOff off(I);
        std::ifstream f(path, std::ios::binary);
        if (!f) throw RuntimeError("read_file: cannot open '" + path + "'");
        std::ostringstream ss;
        ss << f.rdbuf();
        return Value(ss.str());
    });

    def("read_lines", 1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        const std::string path = strArgS(a[0], "read_lines");
        auto out = std::make_shared<ValueList>();
        {
            GilOff off(I);
            std::ifstream f(path);
            if (!f) throw RuntimeError("read_lines: cannot open '" + path + "'");
            std::string line;
            while (std::getline(f, line)) out->push_back(Value(line));
        }
        return Value(out);
    });

    def("write_file", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        const std::string path = strArgS(a[0], "write_file");
        const std::string text = I.stringify(a[1]);
        GilOff off(I);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) throw RuntimeError("write_file: cannot open '" + path + "'");
        f << text;
        return Value();
    });

    def("append_file", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        const std::string path = strArgS(a[0], "append_file");
        const std::string text = I.stringify(a[1]);
        GilOff off(I);
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f) throw RuntimeError("append_file: cannot open '" + path + "'");
        f << text;
        return Value();
    });

    def("file_exists", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        std::error_code ec;
        return Value(fs::exists(strArgS(a[0], "file_exists"), ec));
    });

    def("remove_file", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        std::error_code ec;
        return Value(fs::remove(strArgS(a[0], "remove_file"), ec));
    });

    def("make_dir", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        std::error_code ec;
        return Value(fs::create_directories(strArgS(a[0], "make_dir"), ec));
    });

    def("list_dir", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const std::string path = strArgS(a[0], "list_dir");
        auto out = std::make_shared<ValueList>();
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(path, ec))
            out->push_back(Value(entry.path().filename().string()));
        if (ec) throw RuntimeError("list_dir: cannot read '" + path + "'");
        return Value(out);
    });

    // ---------------------------------------------------------------
    // Time / date
    // ---------------------------------------------------------------
    def("clock", 0, [](Interpreter&, std::vector<Value>&) -> Value {
        // Monotonic seconds, for measuring elapsed time.
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return Value(std::chrono::duration<double>(now).count());
    });

    def("time", 0, [](Interpreter&, std::vector<Value>&) -> Value {
        // Seconds since the Unix epoch (with sub-second precision).
        auto now = std::chrono::system_clock::now().time_since_epoch();
        return Value(std::chrono::duration<double>(now).count());
    });

    def("now", 0, [](Interpreter&, std::vector<Value>&) -> Value {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        auto d = std::make_shared<ValueDict>();
        (*d)["year"]    = Value((double)(tm.tm_year + 1900));
        (*d)["month"]   = Value((double)(tm.tm_mon + 1));
        (*d)["day"]     = Value((double)tm.tm_mday);
        (*d)["hour"]    = Value((double)tm.tm_hour);
        (*d)["minute"]  = Value((double)tm.tm_min);
        (*d)["second"]  = Value((double)tm.tm_sec);
        (*d)["weekday"] = Value((double)tm.tm_wday); // 0 = Sunday
        (*d)["yearday"] = Value((double)tm.tm_yday);
        return Value(d);
    });

    def("format_time", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 2)
            throw RuntimeError("format_time: expects (fmt) or (fmt, epoch_seconds)");
        const std::string fmt = strArgS(a[0], "format_time");
        std::time_t t = (a.size() == 2) ? (std::time_t)numArgS(a[1], "format_time")
                                        : std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::array<char, 256> buf{};
        size_t n = std::strftime(buf.data(), buf.size(), fmt.c_str(), &tm);
        return Value(std::string(buf.data(), n));
    });

    def("sleep", 1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        double secs = numArgS(a[0], "sleep");
        GilOff off(I); // let other threads run while we wait
        std::this_thread::sleep_for(std::chrono::duration<double>(secs));
        return Value();
    });

    // ---------------------------------------------------------------
    // Random
    // ---------------------------------------------------------------
    def("random", 0, [](Interpreter& I, std::vector<Value>&) -> Value {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return Value(dist(I.rng));
    });

    def("random_int", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        long long lo = (long long)numArgS(a[0], "random_int");
        long long hi = (long long)numArgS(a[1], "random_int");
        if (lo > hi) std::swap(lo, hi);
        std::uniform_int_distribution<long long> dist(lo, hi);
        return Value((double)dist(I.rng));
    });

    def("random_range", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        double lo = numArgS(a[0], "random_range");
        double hi = numArgS(a[1], "random_range");
        std::uniform_real_distribution<double> dist(lo, hi);
        return Value(dist(I.rng));
    });

    def("random_choice", 1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (!a[0].isList()) throw RuntimeError("random_choice: expected a list");
        auto l = a[0].asList();
        if (l->empty()) throw RuntimeError("random_choice: list is empty");
        std::uniform_int_distribution<size_t> dist(0, l->size() - 1);
        return (*l)[dist(I.rng)];
    });

    def("random_seed", 1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        I.rng.seed((unsigned long long)numArgS(a[0], "random_seed"));
        return Value();
    });

    // ---------------------------------------------------------------
    // Environment / arguments
    // ---------------------------------------------------------------
    def("env", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 2)
            throw RuntimeError("env: expects (name) or (name, default)");
        const char* v = std::getenv(strArgS(a[0], "env").c_str());
        if (v) return Value(std::string(v));
        return a.size() == 2 ? a[1] : Value();
    });

    def("set_env", 2, [](Interpreter&, std::vector<Value>& a) -> Value {
        const std::string name = strArgS(a[0], "set_env");
        const std::string val = strArgS(a[1], "set_env");
#ifdef _WIN32
        _putenv_s(name.c_str(), val.c_str());
#else
        setenv(name.c_str(), val.c_str(), 1);
#endif
        return Value();
    });

    def("args", 0, [](Interpreter& I, std::vector<Value>&) -> Value {
        auto out = std::make_shared<ValueList>();
        for (auto& s : I.scriptArgs) out->push_back(Value(s));
        return Value(out);
    });

    // ---------------------------------------------------------------
    // Processes
    // ---------------------------------------------------------------
    def("exec", 1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        const std::string cmd = strArgS(a[0], "exec");
        std::string output;
        int code = -1;
        {
            GilOff off(I); // external command may block for a while
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) throw RuntimeError("exec: failed to start command");
            std::array<char, 4096> buf{};
            size_t n;
            while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0)
                output.append(buf.data(), n);
            int status = pclose(pipe);
#ifndef _WIN32
            code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#else
            code = status;
#endif
        }
        auto d = std::make_shared<ValueDict>();
        (*d)["code"] = Value((double)code);
        (*d)["output"] = Value(output);
        return Value(d);
    });

    // ---------------------------------------------------------------
    // Threads (GIL-scheduled: safe, no true CPU parallelism)
    // ---------------------------------------------------------------
    def("spawn", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].isCallable())
            throw RuntimeError("spawn: first argument must be a function");
        Value fn = a[0];
        std::vector<Value> callArgs(a.begin() + 1, a.end());
        double id = I.nextThreadId++;
        auto rec = std::make_shared<ThreadRec>();
        I.threads[id] = rec;
        Interpreter* ip = &I;
        rec->th = std::thread([ip, rec, fn, callArgs]() mutable {
            ip->gilAcquire(); // wait our turn, then run Bee under the GIL
            try {
                rec->result = ip->callValue(fn, callArgs, 0);
            } catch (RuntimeError& e) {
                rec->failed = true;
                rec->error = e.what();
            } catch (BeeThrow& t) {
                rec->failed = true;
                rec->error = ip->stringify(t.value);
            } catch (...) {
                rec->failed = true;
                rec->error = "unknown error in thread";
            }
            ip->gilRelease();
        });
        return Value(id);
    });

    def("join", 1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        double id = numArgS(a[0], "join");
        auto it = I.threads.find(id);
        if (it == I.threads.end())
            throw RuntimeError("join: unknown or already-joined thread");
        auto rec = it->second;
        {
            GilOff off(I); // let the joined thread take the GIL and finish
            if (rec->th.joinable()) rec->th.join();
        }
        I.threads.erase(id);
        if (rec->failed) throw RuntimeError("in thread: " + rec->error);
        return rec->result;
    });
}

} // namespace bee
