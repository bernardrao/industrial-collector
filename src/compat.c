/* glibc 2.31 (Ubuntu 20) 호환 shim
 * third_party .a 파일들이 glibc 2.38+ 에서 빌드되어 아래 심볼이 없으면 링크 실패.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* strlcpy — BSD 함수, glibc 2.38 에 추가됨 (libmodbus 참조) */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    const char *s = src;
    size_t n = size;
    if (n && --n) {
        do { if (!(*dst++ = *s++)) break; } while (--n);
    }
    if (!n) {
        if (size) *dst = '\0';
        while (*s++) ;
    }
    return (size_t)(s - src) - 1;
}

/* __isoc23_strtol* — C23 ISO mapping, glibc 2.38 에 추가됨 (paho / openssl 참조) */
long __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return strtol(nptr, endptr, base);
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
    return strtoul(nptr, endptr, base);
}

long long __isoc23_strtoll(const char *nptr, char **endptr, int base)
{
    return strtoll(nptr, endptr, base);
}

unsigned long long __isoc23_strtoull(const char *nptr, char **endptr, int base)
{
    return strtoull(nptr, endptr, base);
}
