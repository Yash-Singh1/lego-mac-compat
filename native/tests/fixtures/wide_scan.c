#define _DONT_USE_CTYPE_INLINE_
#include <wchar.h>
#include <stdint.h>
#include <stdio.h>
#include <locale.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
/* Force actual imports: Darwin normally inlines the isw/tow family. */
#include <wctype.h>
static int scan_va(const wchar_t *input, const wchar_t *format, ...)
{
    va_list values;
    va_start(values, format);
    int result = vswscanf(input, format, values);
    va_end(values);
    return result;
}

int check_wide_scan(void)
{
    if (!setlocale(LC_ALL, "en_US.UTF-8")) return -234;
    struct { long value; uint32_t guard; } number = {0, 0x1234abcd};
    struct { short value; uint16_t guard; } consumed = {0, 0x5678};
    struct { wchar_t text[3]; uint32_t guard; } text = {{0}, 0xabcdef01};
    if (swscanf(L"é猫 42 -123!", L"%2ls %*u %ld%hn", text.text, &number.value, &consumed.value) != 2 ||
        wcscmp(text.text, L"é猫") || text.guard != 0xabcdef01 || number.value != -123 ||
        number.guard != 0x1234abcd || consumed.value != 10 || consumed.guard != 0x5678) return -230;
    struct { float value; uint32_t guard; } a = {0, 0x1234abcd};
    double b = 0;
    if (swscanf(L"1.25 -2.5", L"%f %lf", &a.value, &b) != 2 || a.value != 1.25f ||
        b != -2.5 || a.guard != 0x1234abcd) return -231;
    unsigned long long large = 0;
    struct { void *value; uint32_t guard; } pointer = {0, 0x1234abcd};
    if (swscanf(L"fedcba9876543210 0x12345", L"%llx %p", &large, &pointer.value) != 2 ||
        large != UINT64_C(0xfedcba9876543210) || pointer.value != (void *)0x12345 ||
        pointer.guard != 0x1234abcd) return -232;
    int x = 0, y = 0;
    if (swscanf(L"1234", L"%2d%2d", &x, &y) != 2 || x != 12 || y != 34 ||
        swscanf(L"", L"%d", &x) != EOF || swscanf(L"x", L"%d", &x) != 0) return -233;
    struct { wchar_t *end; uint32_t guard; } end = {0, 0x1234abcd};
    if (wcstol(L"2147483648!", &end.end, 10) != INT32_MAX || *end.end != '!' ||
        end.guard != 0x1234abcd || wcstof(L"1.25x", &end.end) != 1.25f || *end.end != 'x' ||
        wcstod(L"-2.5!", &end.end) != -2.5 || *end.end != '!' || end.guard != 0x1234abcd) return -235;
    wchar_t letters[8] = L"ab";
    if (wcsncat(letters, L"cdx", 2) != letters || wcscoll(letters, L"abcd") ||
        wcsstr(letters, L"bc") != letters + 1 || wcsrchr(letters, 'd') != letters + 3) return -236;
    if (scan_va(L"-2147483648 18446744073709551615 é猫", L"%ld %llu %2ls",
        &number.value, &large, text.text) != 3 || number.value != INT32_MIN ||
        number.guard != 0x1234abcd || large != UINT64_MAX || wcscmp(text.text, L"é猫") ||
        text.guard != 0xabcdef01) return -237;
    errno = 0;
    if (wcstoul(L"4294967295!", &end.end, 10) != UINT32_MAX || errno || *end.end != L'!' ||
        end.guard != 0x1234abcd || wcstoul(L" -1!", &end.end, 10) != UINT32_MAX || errno) return -238;
    const wchar_t *overflow[] = {L"4294967296!", L"-4294967296!", L"18446744073709551616!"};
    for (unsigned i = 0; i < 3; ++i) {
        errno = 0;
        if (wcstoul(overflow[i], &end.end, 10) != UINT32_MAX || errno != ERANGE ||
            *end.end != L'!' || end.guard != 0x1234abcd) return -239;
    }
    const wchar_t *invalid[] = {L"-+1", L"- 1", L"+", L"xyz"};
    for (unsigned i = 0; i < 4; ++i)
        if (wcstoul(invalid[i], &end.end, 10) || end.end != invalid[i]) return -240;
    if (wcstoll(L"-9223372036854775808!", &end.end, 10) != INT64_MIN || *end.end != L'!' ||
        wcstoull(L"18446744073709551615!", &end.end, 10) != UINT64_MAX || *end.end != L'!' ||
        end.guard != 0x1234abcd || atoll("-4294967297") != -INT64_C(4294967297)) return -241;
    wchar_t *copy = wcsdup(L"Abé猫");
    if (!copy || wcscmp(copy, L"Abé猫") || wcspbrk(copy, L"猫z") != copy + 3 ||
        wcsspn(copy, L"bAé") != 3 || wcscspn(copy, L"猫") != 3 ||
        wcscasecmp(copy, L"abÉ猫") || wcsncasecmp(copy, L"aBxyz", 2)) return -242;
    copy[0] = L'Z'; free(copy);
    if (!iswalnum(L'猫') || !iswalpha(L'é') || !iswblank(L' ') || !iswcntrl(L'\n') ||
        !iswdigit(L'5') || !iswgraph(L'!') || !iswlower(L'é') || !iswprint(L' ') ||
        !iswpunct(L'!') || !iswspace(L'\t') || !iswupper(L'É') || !iswxdigit(L'f') ||
        iswalpha(WEOF) || towlower(L'É') != L'é' || towupper(L'é') != L'É' ||
        towupper(WEOF) != WEOF || !iswctype(L'é', wctype("alpha")) ||
        towctrans(L'É', wctrans("tolower")) != L'é') return -243;
    return 0;
}
