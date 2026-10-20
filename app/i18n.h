#pragma once

/* Translations. Every string the user reads is written in English in the
   code, inside tr("..."); i18n/translations.json holds what each one is in
   the other languages, and the build (tools/gen-i18n.py) refuses a string
   that has no entry there. A language without a string falls back to
   English.

   Placeholders: {0}, {1}... for trf() in C++, or the printf specifiers of a
   C format string; a translation must use the same ones. */

/* Marks a string in a table as translatable (the build checks it like tr());
   translate it with tr() where it is shown. */
#define N_(s) s


#ifdef __cplusplus
extern "C" {
#endif

/* The translation of an English UI string in the current language. Threads
   may call it at any time once i18n_init() has run. */
const char *tr(const char *english);

/* Picks the language: "auto" (the console's system language, else English) or a
   code from i18n_code(). Call from the UI thread. */
void i18n_init(const char *choice);
void i18n_set(const char *choice);
const char *i18n_choice(void);   /* what was asked: "auto" or a code */
const char *i18n_current(void);  /* the language in use: "en", "ja"... */
int i18n_count(void);
const char *i18n_code(int index);
const char *i18n_native_name(int index);  /* "English", "日本語"... */

#ifdef __cplusplus
}

#include <initializer_list>
#include <string>

/* tr(), then {0}, {1}... replaced by the arguments. */
std::string trf(const char *english, std::initializer_list<std::string> args);
#endif
