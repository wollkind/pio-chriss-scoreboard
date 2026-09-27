#pragma once

#include <cctype>
#include <cstring>

// Shortens a name in place until width(text) <= max_width, dropping letters from the middle so it
// stays readable ("Washington" -> "Washngtn" -> "Wshngtn", "Hockenson" -> "Hocknsn") instead of cutting off the end.
//
// Each step removes one character, re-measures, and stops as soon as the name fits, so only as
// much as necessary goes. Steps, in order:
//   1. Dots, apostrophes and hyphens ("St. Brown" -> "St Brown", "Smith-Njigba" -> "SmithNjigba").
//   2. The second letter of a doubled consonant ("McCaffrey" -> "McCafrey").
//   3. Lowercase vowels (a e i o u), rightmost first. The first letter of each word and the last
//      letter of the name are kept, so the start and end of the name stay recognisable.
//   4. Spaces.
//   5. Lowercase consonants, rightmost first, keeping the first two letters of the first word and
//      the last letter of the name.
//   6. Plain truncation from the end.
// `width` is any callable taking const char * and returning the rendered width in pixels.

namespace abbrev_detail {

inline bool isVowel(char c)
{
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

inline bool wordStart(const char *s, size_t i)
{
  return i == 0 || !isalpha(static_cast<unsigned char>(s[i - 1]));
}

inline void eraseAt(char *s, size_t i)
{
  memmove(s + i, s + i + 1, strlen(s + i + 1) + 1);
}

// Removes the rightmost character (not the last one of the name) that `pick` accepts.
template <typename Pick>
inline bool dropRightmost(char *s, Pick pick)
{
  const size_t n = strlen(s);
  if (n < 3) {
    return false;
  }
  for (size_t i = n - 2; i >= 1; --i) {
    if (pick(s, i)) {
      eraseAt(s, i);
      return true;
    }
  }
  return false;
}

}  // namespace abbrev_detail

template <typename Width>
void abbreviate(char *s, int max_width, Width width)
{
  using namespace abbrev_detail;
  if (width(s) <= max_width) {
    return;
  }
  auto punct = [](const char *t, size_t i) { return t[i] == '.' || t[i] == '\'' || t[i] == '-'; };
  auto vowel = [](const char *t, size_t i) { return isVowel(t[i]) && !wordStart(t, i); };
  auto doubled = [](const char *t, size_t i) {
    return islower(static_cast<unsigned char>(t[i])) && tolower(static_cast<unsigned char>(t[i - 1])) == t[i] &&
           !isVowel(t[i]);
  };
  auto space = [](const char *t, size_t i) { return t[i] == ' '; };
  auto consonant = [](const char *t, size_t i) {
    return i >= 2 && islower(static_cast<unsigned char>(t[i])) && !isVowel(t[i]) && !wordStart(t, i);
  };
  // Each rule repeats until the name fits or the rule has nothing left to remove.
  while (width(s) > max_width && dropRightmost(s, punct)) {}
  while (width(s) > max_width && dropRightmost(s, doubled)) {}
  while (width(s) > max_width && dropRightmost(s, vowel)) {}
  while (width(s) > max_width && dropRightmost(s, space)) {}
  while (width(s) > max_width && dropRightmost(s, consonant)) {}
  for (size_t n = strlen(s); n > 0 && width(s) > max_width; --n) {
    s[n - 1] = '\0';
  }
}
