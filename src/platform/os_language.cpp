/**
 * @file    platform/os_language.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "platform/os_language.h"

#if defined(__SWITCH__)
extern "C" {
#include <switch/types.h>
#include <switch/result.h>
#include <switch/services/set.h>
}
#else
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#endif

#include <string_view>

namespace bd::platform {
#if defined(__SWITCH__)

// The console's system language (Settings > System > Language).
u32 DetectOsXLanguage() {
  u32 xlang = 1;
  if (R_FAILED(setInitialize()))
    return xlang;
  u64 code = 0;
  SetLanguage language = SetLanguage_ENUS;
  if (R_SUCCEEDED(setGetSystemLanguage(&code)) &&
      R_SUCCEEDED(setMakeLanguage(code, &language))) {
    switch (language) {
    case SetLanguage_JA: xlang = 2; break;
    case SetLanguage_DE: xlang = 3; break;
    case SetLanguage_FR:
    case SetLanguage_FRCA: xlang = 4; break;
    case SetLanguage_ES:
    case SetLanguage_ES419: xlang = 5; break;
    case SetLanguage_IT: xlang = 6; break;
    case SetLanguage_KO: xlang = 7; break;
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT: xlang = 8; break;
    case SetLanguage_PT:
    case SetLanguage_PTBR: xlang = 9; break;
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS: xlang = 10; break;
    case SetLanguage_RU: xlang = 12; break;
    default: xlang = 1; break;
    }
  }
  setExit();
  return xlang;
}

#else

namespace {

// SDL reports no script subtag, so Traditional Chinese goes by region.
u32 XLangFromLocale(const SDL_Locale &locale) {
  const std::string_view lang(locale.language ? locale.language : "");
  if (lang == "zh") {
    const std::string_view country(locale.country ? locale.country : "");
    return (country == "TW" || country == "HK" || country == "MO") ? 8 : 10;
  }
  if (lang == "pl")
    return 11;
  if (lang == "ru")
    return 12;
  return XLanguageFromCode(lang);
}

} // namespace

u32 DetectOsXLanguage() {
  int count = 0;
  SDL_Locale **locales = SDL_GetPreferredLocales(&count);
  if (!locales)
    return 1;
  u32 xlang = 0;
  for (int i = 0; i < count && !xlang; ++i) {
    if (locales[i])
      xlang = XLangFromLocale(*locales[i]);
  }
  SDL_free(locales);
  return xlang ? xlang : 1;
}

#endif // __SWITCH__

u32 XLanguageFromCode(std::string_view code) {
  char c[8] = {};
  const size_t n = code.size() < sizeof(c) - 1 ? code.size() : sizeof(c) - 1;
  for (size_t i = 0; i < n; ++i)
    c[i] = (code[i] >= 'A' && code[i] <= 'Z') ? static_cast<char>(code[i] + 32)
                                              : code[i];
  // Codes follow BD's Language Set (bd_boot.ini): US JP DE FR ES IT KR TW CN
  // PO, with common ISO aliases also accepted. Values are XLanguage ids.
  const std::string_view s(c, n);
  if (s == "us" || s == "en")
    return 1;
  if (s == "jp" || s == "ja")
    return 2;
  if (s == "de")
    return 3;
  if (s == "fr")
    return 4;
  if (s == "es")
    return 5;
  if (s == "it")
    return 6;
  if (s == "kr" || s == "ko")
    return 7;
  if (s == "tw")
    return 8;
  if (s == "po" || s == "pt")
    return 9;
  if (s == "cn")
    return 10;
  return 0;
}

const char *XLanguageName(u32 id) {
  switch (id) {
  case 1:
    return "English";
  case 2:
    return "Japanese";
  case 3:
    return "German";
  case 4:
    return "French";
  case 5:
    return "Spanish";
  case 6:
    return "Italian";
  case 7:
    return "Korean";
  case 8:
    return "Chinese (Traditional)";
  case 9:
    return "Portuguese";
  case 10:
    return "Chinese (Simplified)";
  case 11:
    return "Polish";
  case 12:
    return "Russian";
  default:
    return "Unknown";
  }
}
} // namespace bd::platform
