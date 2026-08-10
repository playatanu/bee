// Buffer built-ins: Bee's contiguous typed array.
//
// This is the type bulk data travels in. A list of numbers costs 16 bytes per
// element and has to be converted one element at a time on the way to C++; a
// buffer is raw bytes with a dtype and a shape, so a native module gets a
// pointer and no copy happens at all. Image, audio and tensor work all depend on
// that difference.
#include "interpreter.hpp"

#include <cmath>
#include <cstring>
#include <sstream>

namespace bee {

namespace {

[[noreturn]] void fail(const std::string& msg) { throw RuntimeError(msg); }

double numberArg(const Value& v, const std::string& who) {
    if (!v.isNumber()) fail(who + ": expected a number");
    return v.asNumber();
}

// Shape may be given as a single length or as a list of dimensions.
std::vector<long long> shapeArg(const Value& v, const std::string& who) {
    std::vector<long long> shape;
    if (v.isNumber()) {
        double d = v.asNumber();
        if (d < 0 || d != std::floor(d)) fail(who + ": length must be a whole number >= 0");
        shape.push_back((long long)d);
        return shape;
    }
    if (!v.isList()) fail(who + ": shape must be a number or a list of numbers");
    for (auto& item : *v.asList()) {
        if (!item.isNumber()) fail(who + ": shape must contain only numbers");
        double d = item.asNumber();
        if (d < 0 || d != std::floor(d)) fail(who + ": shape dimensions must be whole numbers >= 0");
        shape.push_back((long long)d);
    }
    return shape;
}

DType dtypeArg(const Value& v, const std::string& who) {
    if (!v.isString()) fail(who + ": dtype must be a string like \"f32\" or \"u8\"");
    DType t;
    if (!Buffer::parseName(v.asString(), t))
        fail(who + ": unknown dtype '" + v.asString() +
             "' (f32, f64, i8, u8, i16, u16, i32, i64)");
    return t;
}

size_t elementCount(const std::vector<long long>& shape) {
    // An empty shape is a single scalar element, matching how a 0-d array works.
    size_t n = 1;
    for (long long d : shape) n *= (size_t)d;
    return shape.empty() ? 1 : n;
}

std::shared_ptr<Buffer> makeBuffer(const std::vector<long long>& shape, DType dtype) {
    auto b = std::make_shared<Buffer>();
    b->dtype = dtype;
    b->shape = shape;
    b->bytes.assign(elementCount(shape) * Buffer::widthOf(dtype), 0);
    return b;
}

std::shared_ptr<Buffer> bufferArg(const Value& v, const std::string& who) {
    if (!v.isBuffer()) fail(who + ": expected a buffer");
    return v.asBuffer();
}

Value shapeList(const Buffer& b) {
    auto out = std::make_shared<ValueList>();
    for (long long d : b.shape) out->push_back(Value((double)d));
    return Value(out);
}

}  // namespace

// Flatten a multi-dimensional index into an offset, in row-major order -- the
// layout every C library expects.
size_t bufferOffset(const Buffer& b, const std::vector<Value>& indices, size_t first,
                    const std::string& who) {
    const size_t given = indices.size() - first;
    if (b.shape.empty() || given != b.shape.size())
        throw RuntimeError(who + ": this buffer has " + std::to_string(b.shape.size()) +
                           " dimension(s), got " + std::to_string(given) + " index/indices");
    size_t offset = 0;
    for (size_t d = 0; d < b.shape.size(); ++d) {
        double raw = indices[first + d].isNumber() ? indices[first + d].asNumber() : -1;
        if (!indices[first + d].isNumber() || raw != std::floor(raw))
            throw RuntimeError(who + ": indices must be whole numbers");
        long long i = (long long)raw;
        if (i < 0) i += b.shape[d];
        if (i < 0 || i >= b.shape[d])
            throw RuntimeError(who + ": index " + std::to_string((long long)raw) +
                               " is out of range for dimension " + std::to_string(d) +
                               " of size " + std::to_string(b.shape[d]));
        offset = offset * (size_t)b.shape[d] + (size_t)i;
    }
    return offset;
}

std::string bufferSummary(const Buffer& b) {
    std::ostringstream o;
    o << "buffer<" << Buffer::name(b.dtype) << ">[";
    for (size_t i = 0; i < b.shape.size(); ++i) o << (i ? "," : "") << b.shape[i];
    o << "]";
    // A short preview, because printing a million elements is never what someone
    // wanted from `print(img)`.
    const size_t n = b.count();
    const size_t show = n < 8 ? n : 8;
    o << " ";
    if (n == 0) return o.str() + "[]";
    o << "[";
    for (size_t i = 0; i < show; ++i) {
        if (i) o << ", ";
        double v = b.get(i);
        if (v == std::floor(v) && std::fabs(v) < 1e15) o << (long long)v;
        else o << v;
    }
    if (show < n) o << ", ... " << (n - show) << " more";
    o << "]";
    return o.str();
}

void Interpreter::defineBufferBuiltins() {
    auto def = [&](const std::string& n, int arity,
                   std::function<Value(Interpreter&, std::vector<Value>&)> f) {
        auto b = std::make_shared<Builtin>();
        b->name = n;
        b->arity = arity;
        b->fn = std::move(f);
        globals->define(n, Value(b));
    };

    // ---------------------------------------------------------------
    // Creation
    // ---------------------------------------------------------------
    def("buffer", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 2)
            fail("buffer: expects (shape) or (shape, dtype)");
        DType t = a.size() == 2 ? dtypeArg(a[1], "buffer") : DType::F64;
        return Value(makeBuffer(shapeArg(a[0], "buffer"), t));
    });

    def("zeros", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 2) fail("zeros: expects (shape) or (shape, dtype)");
        DType t = a.size() == 2 ? dtypeArg(a[1], "zeros") : DType::F64;
        return Value(makeBuffer(shapeArg(a[0], "zeros"), t));
    });

    def("full", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.size() < 2 || a.size() > 3) fail("full: expects (shape, value) or (shape, value, dtype)");
        DType t = a.size() == 3 ? dtypeArg(a[2], "full") : DType::F64;
        auto b = makeBuffer(shapeArg(a[0], "full"), t);
        const double v = numberArg(a[1], "full");
        for (size_t i = 0, n = b->count(); i < n; ++i) b->set(i, v);
        return Value(b);
    });

    def("ones", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 2) fail("ones: expects (shape) or (shape, dtype)");
        DType t = a.size() == 2 ? dtypeArg(a[1], "ones") : DType::F64;
        auto b = makeBuffer(shapeArg(a[0], "ones"), t);
        for (size_t i = 0, n = b->count(); i < n; ++i) b->set(i, 1);
        return Value(b);
    });

    // A list is the only way to get data in from Bee source, so the
    // conversion goes both ways and keeps nesting.
    def("buffer_from", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 3)
            fail("buffer_from: expects (list), (list, dtype) or (list, dtype, shape)");
        DType t = a.size() >= 2 ? dtypeArg(a[1], "buffer_from") : DType::F64;

        // Flatten nested lists, learning the shape from the first branch at each
        // level -- the same rule numpy uses.
        std::vector<double> flat;
        std::vector<long long> shape;
        std::function<void(const Value&, size_t)> walk = [&](const Value& v, size_t depth) {
            if (v.isList()) {
                auto l = v.asList();
                if (shape.size() == depth) shape.push_back((long long)l->size());
                else if (shape[depth] != (long long)l->size())
                    fail("buffer_from: nested lists must all be the same length");
                for (auto& item : *l) walk(item, depth + 1);
                return;
            }
            if (v.isNumber()) { flat.push_back(v.asNumber()); return; }
            if (v.isBool()) { flat.push_back(v.asBool() ? 1 : 0); return; }
            fail("buffer_from: lists must contain only numbers");
        };
        walk(a[0], 0);
        (void)I;

        if (a.size() == 3) {
            auto wanted = shapeArg(a[2], "buffer_from");
            if (elementCount(wanted) != flat.size())
                fail("buffer_from: shape needs " + std::to_string(elementCount(wanted)) +
                     " element(s) but the list has " + std::to_string(flat.size()));
            shape = wanted;
        }

        auto b = makeBuffer(shape, t);
        for (size_t i = 0; i < flat.size(); ++i) b->set(i, flat[i]);
        return Value(b);
    });

    def("to_list", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto b = bufferArg(a[0], "to_list");
        // Rebuild the nesting the shape describes, so a round trip through
        // buffer_from() gives back what went in.
        std::function<Value(size_t, size_t&)> build = [&](size_t dim, size_t& cursor) -> Value {
            if (dim >= b->shape.size()) return Value(b->get(cursor++));
            auto out = std::make_shared<ValueList>();
            for (long long i = 0; i < b->shape[dim]; ++i) out->push_back(build(dim + 1, cursor));
            return Value(out);
        };
        size_t cursor = 0;
        if (b->shape.empty()) {
            auto out = std::make_shared<ValueList>();
            if (b->count()) out->push_back(Value(b->get(0)));
            return Value(out);
        }
        return build(0, cursor);
    });

    // ---------------------------------------------------------------
    // Inspection and element access
    // ---------------------------------------------------------------
    def("shape", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        return shapeList(*bufferArg(a[0], "shape"));
    });

    def("dtype", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        return Value(std::string(Buffer::name(bufferArg(a[0], "dtype")->dtype)));
    });

    def("byte_len", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        return Value((double)bufferArg(a[0], "byte_len")->bytes.size());
    });

    def("at", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.size() < 2) fail("at: expects (buffer, index...)");
        auto b = bufferArg(a[0], "at");
        return Value(b->get(bufferOffset(*b, a, 1, "at")));
    });

    def("set_at", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.size() < 3) fail("set_at: expects (buffer, value, index...)");
        auto b = bufferArg(a[0], "set_at");
        const double v = numberArg(a[1], "set_at");
        b->set(bufferOffset(*b, a, 2, "set_at"), v);
        return a[0];
    });

    def("fill", 2, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto b = bufferArg(a[0], "fill");
        const double v = numberArg(a[1], "fill");
        for (size_t i = 0, n = b->count(); i < n; ++i) b->set(i, v);
        return a[0];
    });

    // ---------------------------------------------------------------
    // Reshaping and copying
    // ---------------------------------------------------------------
    def("reshape", 2, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto b = bufferArg(a[0], "reshape");
        auto shape = shapeArg(a[1], "reshape");
        if (elementCount(shape) != b->count())
            fail("reshape: " + std::to_string(b->count()) + " element(s) cannot become shape with " +
                 std::to_string(elementCount(shape)));
        // A view would alias the original; a copy keeps the semantics obvious.
        auto out = std::make_shared<Buffer>(*b);
        out->shape = shape;
        return Value(out);
    });

    def("copy", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        return Value(std::make_shared<Buffer>(*bufferArg(a[0], "copy")));
    });

    def("astype", 2, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto b = bufferArg(a[0], "astype");
        DType t = dtypeArg(a[1], "astype");
        auto out = makeBuffer(b->shape, t);
        for (size_t i = 0, n = b->count(); i < n; ++i) out->set(i, b->get(i));
        return Value(out);
    });

    // ---------------------------------------------------------------
    // Whole-buffer arithmetic
    // ---------------------------------------------------------------
    // Enough to be useful without pretending to be a numeric library: the point
    // is to prepare and inspect data on its way to a native one.
    auto elementwise = [&def](const std::string& name, double (*op)(double, double)) {
        def(name, 2, [op, name](Interpreter&, std::vector<Value>& a) -> Value {
            auto b = bufferArg(a[0], name);
            auto out = std::make_shared<Buffer>(*b);
            if (a[1].isNumber()) {
                const double s = a[1].asNumber();
                for (size_t i = 0, n = b->count(); i < n; ++i) out->set(i, op(b->get(i), s));
                return Value(out);
            }
            auto other = bufferArg(a[1], name);
            if (other->count() != b->count())
                fail(name + ": buffers must have the same number of elements (" +
                     std::to_string(b->count()) + " vs " + std::to_string(other->count()) + ")");
            for (size_t i = 0, n = b->count(); i < n; ++i) out->set(i, op(b->get(i), other->get(i)));
            return Value(out);
        });
    };
    elementwise("buf_add", [](double x, double y) { return x + y; });
    elementwise("buf_sub", [](double x, double y) { return x - y; });
    elementwise("buf_mul", [](double x, double y) { return x * y; });
    elementwise("buf_div", [](double x, double y) { return y == 0 ? 0.0 : x / y; });

    def("buf_sum", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto b = bufferArg(a[0], "buf_sum");
        double total = 0;
        for (size_t i = 0, n = b->count(); i < n; ++i) total += b->get(i);
        return Value(total);
    });

    def("buf_min", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto b = bufferArg(a[0], "buf_min");
        if (!b->count()) fail("buf_min: the buffer is empty");
        double best = b->get(0);
        for (size_t i = 1, n = b->count(); i < n; ++i) best = std::fmin(best, b->get(i));
        return Value(best);
    });

    def("buf_max", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto b = bufferArg(a[0], "buf_max");
        if (!b->count()) fail("buf_max: the buffer is empty");
        double best = b->get(0);
        for (size_t i = 1, n = b->count(); i < n; ++i) best = std::fmax(best, b->get(i));
        return Value(best);
    });
}

}  // namespace bee
