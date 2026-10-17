#include "json.h"

#include <cstdio>
#include <cstdlib>

namespace json {
namespace {

struct Parser {
  const std::string &t;
  size_t i = 0;

  void ws() {
    while(i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++;
  }

  static void put_utf8(std::string &out, unsigned cp) {
    if(cp < 0x80) {
      out += (char)cp;
    } else if(cp < 0x800) {
      out += (char)(0xC0 | cp >> 6);
      out += (char)(0x80 | (cp & 0x3F));
    } else if(cp < 0x10000) {
      out += (char)(0xE0 | cp >> 12);
      out += (char)(0x80 | (cp >> 6 & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | cp >> 18);
      out += (char)(0x80 | (cp >> 12 & 0x3F));
      out += (char)(0x80 | (cp >> 6 & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(unsigned &v) {
    if(i + 4 > t.size()) return false;
    v = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
    i += 4;
    return true;
  }

  bool str(std::string &out) {
    if(t[i] != '"') return false;
    i++;
    while(i < t.size() && t[i] != '"') {
      char c = t[i++];
      if(c != '\\') {
        out += c;
        continue;
      }
      if(i >= t.size()) return false;
      c = t[i++];
      switch(c) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        unsigned cp, lo;
        if(!hex4(cp)) return false;
        if(cp >= 0xD800 && cp < 0xDC00 && t.compare(i, 2, "\\u") == 0) {
          i += 2;
          if(!hex4(lo)) return false;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        put_utf8(out, cp);
        break;
      }
      default: out += c; break;
      }
    }
    if(i >= t.size()) return false;
    i++;
    return true;
  }

  // Skips any value, including nested arrays/objects we don't model.
  bool skip() {
    ws();
    if(i >= t.size()) return false;
    if(t[i] == '"') {
      std::string s;
      return str(s);
    }
    if(t[i] == '{' || t[i] == '[') {
      int depth = 0;
      do {
        if(t[i] == '"') {
          std::string s;
          if(!str(s)) return false;
          continue;
        }
        if(t[i] == '{' || t[i] == '[') depth++;
        if(t[i] == '}' || t[i] == ']') depth--;
        i++;
      } while(i < t.size() && depth > 0);
      return depth == 0;
    }
    while(i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']') i++;
    return true;
  }

  bool value(Value &v) {
    ws();
    if(i >= t.size()) return false;
    if(t[i] == '"') {
      v.kind = Value::String;
      return str(v.s);
    }
    if(!t.compare(i, 4, "true")) {
      v.kind = Value::Bool;
      v.b = true;
      i += 4;
      return true;
    }
    if(!t.compare(i, 5, "false")) {
      v.kind = Value::Bool;
      i += 5;
      return true;
    }
    if(!t.compare(i, 4, "null")) {
      i += 4;
      return true;
    }
    if(t[i] == '-' || (t[i] >= '0' && t[i] <= '9')) {
      char *end;
      v.kind = Value::Number;
      v.n = strtod(t.c_str() + i, &end);
      i = (size_t)(end - t.c_str());
      return true;
    }
    return skip();
  }

  bool object(Object &o) {
    ws();
    if(i >= t.size() || t[i] != '{') return false;
    i++;
    ws();
    if(i < t.size() && t[i] == '}') {
      i++;
      return true;
    }
    for(;;) {
      std::string key;
      Value v;
      ws();
      if(!str(key)) return false;
      ws();
      if(i >= t.size() || t[i] != ':') return false;
      i++;
      if(!value(v)) return false;
      o[key] = v;
      ws();
      if(i < t.size() && t[i] == ',') {
        i++;
        continue;
      }
      if(i < t.size() && t[i] == '}') {
        i++;
        return true;
      }
      return false;
    }
  }
};

void dump_str(std::string &out, const std::string &s) {
  out += '"';
  for(unsigned char c : s) {
    switch(c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if(c < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += (char)c;
      }
    }
  }
  out += '"';
}

}  // namespace

bool parse_array(const std::string &text, std::vector<Object> &out) {
  Parser p{text};
  p.ws();
  if(p.i >= text.size() || text[p.i] != '[') return false;
  p.i++;
  p.ws();
  if(p.i < text.size() && text[p.i] == ']') return true;
  for(;;) {
    Object o;
    p.ws();
    if(p.i < text.size() && text[p.i] == '{') {
      if(!p.object(o)) return false;
      out.push_back(o);
    } else if(!p.skip()) {
      return false;
    }
    p.ws();
    if(p.i < text.size() && text[p.i] == ',') {
      p.i++;
      continue;
    }
    return p.i < text.size() && text[p.i] == ']';
  }
}

std::string dump_array(const std::vector<Object> &items) {
  std::string out = "[";
  for(size_t k = 0; k < items.size(); k++) {
    bool first = true;
    if(k) out += ',';
    out += '{';
    for(const auto &kv : items[k]) {
      if(!first) out += ',';
      first = false;
      dump_str(out, kv.first);
      out += ':';
      switch(kv.second.kind) {
      case Value::String: dump_str(out, kv.second.s); break;
      case Value::Bool: out += kv.second.b ? "true" : "false"; break;
      case Value::Number: {
        char buf[32];
        double n = kv.second.n;
        if(n == (double)(long long)n) snprintf(buf, sizeof(buf), "%lld", (long long)n);
        else snprintf(buf, sizeof(buf), "%.17g", n);
        out += buf;
        break;
      }
      default: out += "null";
      }
    }
    out += '}';
  }
  return out + "]";
}

std::string get_str(const Object &o, const char *key, const char *def) {
  auto it = o.find(key);
  if(it == o.end()) return def;
  if(it->second.kind == Value::String) return it->second.s;
  if(it->second.kind == Value::Number) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)it->second.n);
    return buf;
  }
  return def;
}

double get_num(const Object &o, const char *key, double def) {
  auto it = o.find(key);
  if(it == o.end()) return def;
  if(it->second.kind == Value::Number) return it->second.n;
  if(it->second.kind == Value::String && !it->second.s.empty()) return atof(it->second.s.c_str());
  return def;
}

bool get_bool(const Object &o, const char *key, bool def) {
  auto it = o.find(key);
  if(it == o.end()) return def;
  if(it->second.kind == Value::Bool) return it->second.b;
  if(it->second.kind == Value::Number) return it->second.n != 0;
  return def;
}

void set_str(Object &o, const char *key, const std::string &v) {
  Value x;
  x.kind = Value::String;
  x.s = v;
  o[key] = x;
}

void set_num(Object &o, const char *key, double v) {
  Value x;
  x.kind = Value::Number;
  x.n = v;
  o[key] = x;
}

void set_bool(Object &o, const char *key, bool v) {
  Value x;
  x.kind = Value::Bool;
  x.b = v;
  o[key] = x;
}

}  // namespace json
