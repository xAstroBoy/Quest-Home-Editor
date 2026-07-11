#pragma once
// Minimal single-header fork of nlohmann/json v3.11 for HSTF parsing.
// Only what's needed: string, int, float, bool, null, object, array.
// Size-optimized for embedded use in the HSR renderer.

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace tinyjson {

enum class Type { Null, Bool, Int, Float, String, Array, Object };

class Value {
public:
    Type type = Type::Null;
    bool boolVal = false;
    int64_t intVal = 0;
    double floatVal = 0.0;
    std::string strVal;
    std::vector<Value> arrVal;
    std::map<std::string, Value> objVal;

    Value() = default;
    Value(Type t) : type(t) {}

    bool isNull()    const { return type == Type::Null; }
    bool isBool()    const { return type == Type::Bool; }
    bool isInt()     const { return type == Type::Int; }
    bool isFloat()   const { return type == Type::Float; }
    bool isString()  const { return type == Type::String; }
    bool isArray()   const { return type == Type::Array; }
    bool isObject()  const { return type == Type::Object; }
    bool isNumber()  const { return type == Type::Int || type == Type::Float; }

    std::string asString() const { return strVal; }
    int64_t asInt() const {
        if (type == Type::Int) return intVal;
        if (type == Type::Float) return (int64_t)floatVal;
        return 0;
    }
    double asFloat() const {
        if (type == Type::Float) return floatVal;
        if (type == Type::Int) return (double)intVal;
        return 0.0;
    }
    bool asBool() const { return boolVal; }

    Value& operator[](const std::string& k) { return objVal[k]; }
    Value& operator[](size_t i) { return arrVal[i]; }
    const Value& operator[](const std::string& k) const {
        static Value empty;
        auto it = objVal.find(k);
        return it != objVal.end() ? it->second : empty;
    }
    const Value& operator[](size_t i) const { return arrVal[i]; }
    size_t size() const { return isArray() ? arrVal.size() : objVal.size(); }

    bool has(const std::string& k) const { return objVal.find(k) != objVal.end(); }
};

// Fast JSON parser — no extra allocations for simple values
class Parser {
    const char* p;
    const char* end;
public:
    Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    Value parse() {
        skipWS();
        return parseValue();
    }

private:
    void skipWS() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    char peek() { return p < end ? *p : 0; }
    char next() { return p < end ? *p++ : 0; }

    Value parseValue() {
        skipWS();
        switch (peek()) {
            case '"': return parseString();
            case '{': return parseObject();
            case '[': return parseArray();
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default: return parseNumber();
        }
    }

    Value parseNull() {
        if (p + 4 <= end && memcmp(p, "null", 4) == 0) { p += 4; return Value(Type::Null); }
        throw std::runtime_error("Expected null");
    }

    Value parseBool() {
        if (p + 4 <= end && memcmp(p, "true", 4) == 0) {
            p += 4; Value v(Type::Bool); v.boolVal = true; return v;
        }
        if (p + 5 <= end && memcmp(p, "false", 5) == 0) {
            p += 5; Value v(Type::Bool); v.boolVal = false; return v;
        }
        throw std::runtime_error("Expected bool");
    }

    Value parseNumber() {
        bool neg = false;
        if (peek() == '-') { neg = true; next(); }

        // Try to parse as integer first
        bool isFloat = false;
        int64_t intVal = 0;
        double floatVal = 0;
        const char* start = p;

        while (p < end && *p >= '0' && *p <= '9') {
            intVal = intVal * 10 + (*p - '0');
            ++p;
        }
        if (peek() == '.') {
            isFloat = true;
            next();
            double frac = 0, scale = 0.1;
            while (p < end && *p >= '0' && *p <= '9') {
                frac += (*p - '0') * scale;
                scale *= 0.1;
                ++p;
            }
            floatVal = (double)intVal + frac;
        }
        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            next();
            bool eNeg = (peek() == '-');
            if (eNeg || peek() == '+') next();
            double exp = 0;
            while (p < end && *p >= '0' && *p <= '9') { exp = exp * 10 + (*p - '0'); ++p; }
            floatVal = floatVal * std::pow(10.0, eNeg ? -exp : exp);
            if (!isFloat) { floatVal = (double)intVal; isFloat = true; }
        }
        if (p == start) throw std::runtime_error("Expected number");

        if (neg) { intVal = -intVal; floatVal = -floatVal; }
        Value v(isFloat ? Type::Float : Type::Int);
        v.intVal = intVal;
        v.floatVal = floatVal;
        return v;
    }

    Value parseString() {
        next(); // skip opening "
        Value v(Type::String);
        while (p < end && peek() != '"') {
            if (peek() == '\\') {
                next();
                switch (next()) {
                    case '"': v.strVal += '"'; break;
                    case '\\': v.strVal += '\\'; break;
                    case '/': v.strVal += '/'; break;
                    case 'n': v.strVal += '\n'; break;
                    case 't': v.strVal += '\t'; break;
                    case 'r': v.strVal += '\r'; break;
                    default: v.strVal += '\\'; break;
                }
            } else {
                v.strVal += next();
            }
        }
        if (peek() == '"') next();
        return v;
    }

    Value parseArray() {
        next(); // skip '['
        Value v(Type::Array);
        skipWS();
        while (peek() != ']' && p < end) {
            v.arrVal.push_back(parseValue());
            skipWS();
            if (peek() == ',') { next(); skipWS(); }
        }
        if (peek() == ']') next();
        return v;
    }

    Value parseObject() {
        next(); // skip '{'
        Value v(Type::Object);
        skipWS();
        while (peek() != '}' && p < end) {
            Value key = parseValue();
            skipWS();
            if (peek() == ':') next();
            Value val = parseValue();
            v.objVal[key.asString()] = std::move(val);
            skipWS();
            if (peek() == ',') { next(); skipWS(); }
        }
        if (peek() == '}') next();
        return v;
    }
};

inline Value parse(const std::string& s) { return Parser(s).parse(); }

} // namespace tinyjson
