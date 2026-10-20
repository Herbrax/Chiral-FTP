// Translation lookup over the table tools/gen-i18n.py generates from
// i18n/translations.json (gen/i18n.c).

#include "i18n.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

extern "C" {
#include "system.h"
}

#include <vector>

extern "C" {
extern const int chiral_i18n_languages;
extern const char *const chiral_i18n_codes[];
extern const char *const chiral_i18n_names[];
extern const int chiral_i18n_strings;
extern const char *const *const chiral_i18n_rows[];  // [string][language]

}


namespace {

std::unordered_map<std::string, int> g_index;  // English -> row; built once, then read only
std::atomic<int> g_lang{0};                    // column in use; 0 is English
std::string g_choice = "auto";

/* Which of the catalogue's languages this build can draw comes from the
   platform (engine/system.h): one font covers everything on a computer,
   while a console may have no face for some scripts. */
bool renderable(const char *code) {
  const char *codes[16];
  const int n = system_renderable_languages(codes, (int)(sizeof(codes) / sizeof(codes[0])));

  if(n == 0) return true;  /* no restriction */
  for(int i = 0; i < n; i++) {
    if(!strcmp(codes[i], code)) return true;
  }
  return false;
}

/* Catalogue columns we offer, in catalogue order; built once by i18n_init. */
std::vector<int> g_offered;

int language_index(const char *code) {
  for(int i = 0; i < chiral_i18n_languages; i++) {
    if(!strcmp(chiral_i18n_codes[i], code)) return i;
  }
  return -1;
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
  g_offered.clear();
  for(int i = 0; i < chiral_i18n_languages; i++) {
    if(renderable(chiral_i18n_codes[i])) g_offered.push_back(i);
  }
  i18n_set(choice);
}

extern "C" void i18n_set(const char *choice) {
  g_choice = choice && *choice ? choice : "auto";
  const char *code = g_choice == "auto" ? system_language() : g_choice.c_str();
  /* A language saved by an older build (or another platform) that this one
     can't draw falls back to English rather than a screen of empty boxes. */
  int lang = renderable(code) ? language_index(code) : 0;
  g_lang.store(lang < 0 ? 0 : lang, std::memory_order_relaxed);
}

extern "C" const char *i18n_choice(void) {
  return g_choice.c_str();
}

extern "C" const char *i18n_current(void) {
  return chiral_i18n_codes[g_lang.load(std::memory_order_relaxed)];
}

extern "C" int i18n_count(void) {
  return (int)g_offered.size();
}

extern "C" const char *i18n_code(int index) {
  return index >= 0 && index < (int)g_offered.size() ? chiral_i18n_codes[g_offered[index]] : "";
}

extern "C" const char *i18n_native_name(int index) {
  return index >= 0 && index < (int)g_offered.size() ? chiral_i18n_names[g_offered[index]] : "";
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
