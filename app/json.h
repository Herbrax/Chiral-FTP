// Minimal JSON for the files shared with the web UI: arrays of flat objects
// whose values are strings, numbers or booleans. Unknown keys are kept.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace json {

struct Value {
  enum Kind { Null, Bool, Number, String } kind = Null;
  bool b = false;
  double n = 0;
  std::string s;
};

using Object = std::map<std::string, Value>;

// Returns false on malformed input; nested values are skipped.
bool parse_array(const std::string &text, std::vector<Object> &out);
std::string dump_array(const std::vector<Object> &items);

std::string get_str(const Object &o, const char *key, const char *def = "");
double get_num(const Object &o, const char *key, double def = 0);
bool get_bool(const Object &o, const char *key, bool def = false);
void set_str(Object &o, const char *key, const std::string &v);
void set_num(Object &o, const char *key, double v);
void set_bool(Object &o, const char *key, bool v);

}  // namespace json
