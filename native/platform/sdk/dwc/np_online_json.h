// np_online_json.h: just enough JSON and base64 for the service's answers and requests (no dependencies). Parsing is strict about syntax,
// bounded in depth, and tolerant of unknown members.
#ifndef NP_ONLINE_JSON_H
#define NP_ONLINE_JSON_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace NpOnline {

struct JValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type type;
    bool b;
    double n;
    std::string s;
    std::vector<JValue> a;
    std::vector<std::pair<std::string, JValue> > o;
    JValue() : type(Null), b(false), n(0) {}
    const JValue* Get(const char* key) const;          // member of an object, or null
    bool IsStr() const { return type == Str; }
    bool IsNum() const { return type == Num; }
    bool IsArr() const { return type == Arr; }
    bool IsObj() const { return type == Obj; }
    // Accessors with defaults when the member is missing or of another type.
    std::string Str_(const char* key, const char* def = "") const;
    double Num_(const char* key, double def = 0) const;
    bool Bool_(const char* key, bool def = false) const;
};

bool JsonParse(const char* text, size_t size, JValue* out);   // false on any syntax error; depth <= 12
std::string JsonQuote(const std::string& s);                  // "..." with escapes

std::string Base64Encode(const uint8_t* data, size_t size);
bool Base64Decode(const std::string& text, std::vector<uint8_t>* out);   // standard alphabet, padding required

}  // namespace NpOnline

#endif
