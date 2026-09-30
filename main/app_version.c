/* app_version.c - 版本字符串解析/比较（不依赖任何 ESP-IDF 组件，纯 C） */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app_version.h"

bool app_version_parse(const char *s, int *major, int *minor, int *patch)
{
    int mj = 0, mi = 0, pt = 0;
    bool any = false;

    if (!s) {
        s = "";
    }

    /* 最多解析三段：跳过后面的多余部分（例如 "1.2.3-rc1" 的 -rc1 被忽略） */
    int *out[3] = { &mj, &mi, &pt };
    for (int field = 0; field < 3; field++) {
        while (*s && !isdigit((unsigned char)*s)) {
            /* 遇到 '-' / '+' 说明版本主体已经结束 */
            if (*s == '-' || *s == '+') {
                goto done;
            }
            s++;
        }
        if (!*s) {
            break;
        }
        int v = 0;
        while (isdigit((unsigned char)*s)) {
            if (v < 100000) {          /* 防止溢出，和 Kconfig 的 0..999 量级一致 */
                v = v * 10 + (*s - '0');
            }
            any = true;
            s++;
        }
        *out[field] = v;
        if (*s != '.') {
            break;                     /* 没有下一段了 */
        }
        s++;                           /* 吃掉 '.' */
    }

done:
    if (major) *major = mj;
    if (minor) *minor = mi;
    if (patch) *patch = pt;
    return any;
}

int app_version_cmp(const char *a, const char *b)
{
    int a1, a2, a3, b1, b2, b3;
    app_version_parse(a, &a1, &a2, &a3);
    app_version_parse(b, &b1, &b2, &b3);

    if (a1 != b1) return a1 > b1 ? 1 : -1;
    if (a2 != b2) return a2 > b2 ? 1 : -1;
    if (a3 != b3) return a3 > b3 ? 1 : -1;
    return 0;
}

bool app_version_is_newer(const char *candidate, const char *current)
{
    return app_version_cmp(candidate, current) > 0;
}
