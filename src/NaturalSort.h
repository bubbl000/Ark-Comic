#pragma once
// 自然排序：文件名中的数字按数值比较（如 2 < 10）
// 与 C# 版 NaturalStringComparer 行为一致
#include <string>
#include <cctype>

namespace ark {

inline int NaturalCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[j];
        // 数字段按数值比较
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t si = i, sj = j;
            while (i < a.size() && std::isdigit((unsigned char)a[i])) i++;
            while (j < b.size() && std::isdigit((unsigned char)b[j])) j++;
            // 去前导零
            size_t ni = si, nj = sj;
            while (ni + 1 < i && a[ni] == '0') ni++;
            while (nj + 1 < j && b[nj] == '0') nj++;
            size_t lenI = i - ni, lenJ = j - nj;
            if (lenI != lenJ) return lenI < lenJ ? -1 : 1;
            for (size_t k = 0; k < lenI; k++) {
                if (a[ni + k] != b[nj + k]) return a[ni + k] < b[nj + k] ? -1 : 1;
            }
            continue;
        }
        // 普通字符：不区分大小写
        unsigned char la = (unsigned char)std::tolower(ca);
        unsigned char lb = (unsigned char)std::tolower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        i++; j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

} // namespace ark