# Adds standard headers a translation unit uses but only gets transitively (MSVC STL differs from libstdc++).
import re, os, sys, glob
RULES = {
 '<algorithm>': r'std::(sort|stable_sort|min|max|clamp|find|find_if|remove|remove_if|reverse|unique|count|count_if|any_of|all_of|none_of|fill|copy|transform|swap_ranges|lower_bound|upper_bound|min_element|max_element|replace|rotate|equal|binary_search|nth_element|partial_sort)\b',
 '<functional>': r'std::function\b', '<cstring>': r'\b(std::)?(memcpy|memset|memcmp|memmove|strlen|strcmp|strncmp|strchr|strstr|strncpy)\s*\(',
 '<cmath>': r'std::(sqrt|fabs|floor|ceil|round|sin|cos|atan2|pow|fmod|hypot|log10|lround|isfinite|abs\s*\(\s*[-\w.]*\s*[\.\)])',
 '<map>': r'std::map\b', '<set>': r'std::set\b', '<unordered_map>': r'std::unordered_map\b', '<unordered_set>': r'std::unordered_set\b',
 '<memory>': r'std::(unique_ptr|shared_ptr|make_unique|make_shared)\b', '<limits>': r'std::numeric_limits\b',
 '<cstdio>': r'\b(std::)?(snprintf|sprintf|printf|fprintf|fopen|fclose|fread|fwrite|sscanf|popen|puts|fgets)\s*\(',
 '<cstdlib>': r'\b(std::)?(strtod|strtol|strtoul|atoi|atof|abs|qsort|getenv)\s*\(', '<cctype>': r'\b(std::)?(isdigit|isalpha|isspace|toupper|tolower|isalnum|isxdigit|isupper|islower)\s*\(',
 '<cwchar>': r'\b(std::)?(wcslen|wcscmp|swprintf|wcstod|wcstoul|wcsncmp|wmemcpy|wcschr)\s*\(', '<string>': r'std::(string|to_string|wstring)\b',
 '<vector>': r'std::vector\b', '<utility>': r'std::(pair|move|swap|exchange)\b', '<optional>': r'std::optional\b', '<array>': r'std::array\b',
 '<sstream>': r'std::(ostringstream|istringstream|stringstream)\b', '<numeric>': r'std::(accumulate|iota)\b', '<cstdint>': r'\b(u?int(8|16|32|64)_t)\b',
 '<iterator>': r'std::(back_inserter|distance|next|prev)\b', '<tuple>': r'std::(tuple|tie)\b', '<deque>': r'std::deque\b',
}
def includes(path, seen):
    if path in seen or not os.path.exists(path): return set()
    seen.add(path)
    txt = open(path, encoding='utf-8', errors='replace').read()
    res = set(re.findall(r'#include\s*(<[^>]+>)', txt))
    for loc in re.findall(r'#include\s*"([^"]+)"', txt):
        for base in (os.path.dirname(path), 'src', 'sdk'):
            p = os.path.normpath(os.path.join(base, loc))
            if os.path.exists(p): res |= includes(p, seen); break
    return res
changed = 0
for f in sorted(glob.glob('src/**/*.cpp', recursive=True) + glob.glob('src/**/*.h', recursive=True) + glob.glob('sdk/**/*.cpp', recursive=True)):
    txt = open(f, encoding='utf-8', errors='replace').read()
    code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', '', txt, flags=re.S)
    have = includes(f, set())
    # windows.h brings C runtime string/stdio in practice; still add explicit for portability
    need = [h for h, rx in RULES.items() if h not in have and re.search(rx, code)]
    if not need: continue
    lines = txt.split('\n')
    idx = max([i for i, l in enumerate(lines) if l.startswith('#include')], default=-1)
    if idx < 0:
        idx = next((i for i, l in enumerate(lines) if l.startswith('#pragma once')), -1)
    lines[idx + 1:idx + 1] = ['#include %s' % h for h in need]
    open(f, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
    changed += 1
    print(f, ' '.join(need))
print('files changed', changed)
