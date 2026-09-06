#include <wchar.h>
#include <stdint.h>
#include <stdio.h>
#include <locale.h>
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
    return 0;
}
