// Translation lookup over the table tools/gen-i18n.py generates from
// i18n/translations.json (gen/i18n.c).

#include "i18n.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

extern "C" {
extern const int chiral_i18n_languages;
extern const char *const chiral_i18n_codes[];
extern const char *const chiral_i18n_names[];
extern const int chiral_i18n_strings;
extern const char *const *const chiral_i18n_rows[];  // [string][language]

#ifdef __PROSPERO__
int sceSystemServiceParamGetInt(int param, int *value);
#endif
}

namespace {

std::unordered_map<std::string, int> g_index;  // English -> row; built once, then read only
std::atomic<int> g_lang{0};                    // column in use; 0 is English
std::string g_choice = "auto";

int language_index(const char *code) {
  for(int i = 0; i < chiral_i18n_languages; i++) {
    if(!strcmp(chiral_i18n_codes[i], code)) return i;
  }
  return -1;
}

// The console's (or this computer's) language, as one of ours.
const char *system_language() {
#ifdef __PROSPERO__
  int id = -1;
  if(sceSystemServiceParamGetInt(1 /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */, &id) == 0) {
    switch(id) {
    case 0: return "ja";
    case 2: case 22: return "fr";             // French, French (Canada)
    case 3: case 20: return "es";             // Spanish, Spanish (Latin America)
    case 5: return "it";
    case 8: return "ru";
    case 9: return "ko";
    case 10: case 11: return "zh";            // Chinese (traditional, simplified)
    case 21: return "ar";
    default: return "en";
    }
  }
  return "en";
#else
  const char *lang = getenv("CHIRAL_LANG");
  if(!lang || !*lang) lang = getenv("LANG");
  if(!lang) return "en";
  static char code[3];
  code[0] = lang[0];
  code[1] = lang[0] ? lang[1] : 0;
  code[2] = 0;
  return language_index(code) >= 0 ? code : "en";
#endif
}

}  // namespace

extern "C" const char *tr(const char *english) {
  int lang = g_lang.load(std::memory_order_relaxed);
  if(!lang || !english) return english;
  auto it = g_index.find(english);
  if(it == g_index.end()) return english;
  const char *t = chiral_i18n_rows[it->second][lang];
  return t && *t ? t : english;
}

extern "C" void i18n_init(const char *choice) {
  if(g_index.empty()) {
    for(int i = 0; i < chiral_i18n_strings; i++) g_index.emplace(chiral_i18n_rows[i][0], i);
  }
  i18n_set(choice);
}

extern "C" void i18n_set(const char *choice) {
  g_choice = choice && *choice ? choice : "auto";
  int lang = language_index(g_choice == "auto" ? system_language() : g_choice.c_str());
  g_lang.store(lang < 0 ? 0 : lang, std::memory_order_relaxed);
}

extern "C" const char *i18n_choice(void) {
  return g_choice.c_str();
}

extern "C" const char *i18n_current(void) {
  return chiral_i18n_codes[g_lang.load(std::memory_order_relaxed)];
}

extern "C" int i18n_count(void) {
  return chiral_i18n_languages;
}

extern "C" const char *i18n_code(int index) {
  return index >= 0 && index < chiral_i18n_languages ? chiral_i18n_codes[index] : "";
}

extern "C" const char *i18n_native_name(int index) {
  return index >= 0 && index < chiral_i18n_languages ? chiral_i18n_names[index] : "";
}

std::string trf(const char *english, std::initializer_list<std::string> args) {
  std::string in = tr(english), out;
  out.reserve(in.size() + 32);
  for(size_t i = 0; i < in.size(); i++) {
    if(in[i] == '{' && i + 2 < in.size() && in[i + 1] >= '0' && in[i + 1] <= '9' && in[i + 2] == '}') {
      size_t n = (size_t)(in[i + 1] - '0');
      if(n < args.size()) out += *(args.begin() + n);
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}
