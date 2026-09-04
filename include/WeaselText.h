#pragma once

#include <string>

namespace weasel {

// Weasel label_format only supports one %s substitution. Parse that small
// grammar explicitly so malformed user configuration cannot reach the CRT
// invalid-parameter handler used by swprintf_s.
inline std::wstring FormatCandidateLabel(const std::wstring& label,
                                         const wchar_t* format) {
  if (!format || !*format)
    return label;

  bool has_substitution = false;
  std::wstring result;
  for (size_t i = 0; format[i] != L'\0'; ++i) {
    if (format[i] != L'%') {
      result.push_back(format[i]);
      continue;
    }

    const wchar_t specifier = format[++i];
    if (specifier == L'%') {
      result.push_back(L'%');
    } else if (specifier == L's' && !has_substitution) {
      result.append(label);
      has_substitution = true;
    } else {
      return label;
    }
  }

  return has_substitution ? result : label;
}

}  // namespace weasel
