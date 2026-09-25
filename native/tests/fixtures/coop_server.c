#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct node { const char *name; void *table; struct node *next; int id, baseline; };
struct node first __asm__("__ZL17g_CFirst_ClassReg");
struct node survey __asm__("__ZL23g_CPointSurvey_ClassReg");
struct node last __asm__("__ZL16g_CLast_ClassReg");
struct node *g_pServerClassHead;

__attribute__((constructor)) static void initialize(void)
{
    first = (struct node){"CFirst", &first, &survey, 11, 111};
    survey = (struct node){"CPointSurvey", &survey, &last, 22, 222};
    last = (struct node){"CLast", &last, 0, 33, 333};
    g_pServerClassHead = &first;
    const char *mode = getenv("COOP_CLASS_MODE");
    if (!mode) return;
    if (!strcmp(mode, "head")) g_pServerClassHead = &survey;
    if (!strcmp(mode, "cycle")) last.next = &first;
    if (!strcmp(mode, "bad-next")) last.next = (void *)(uintptr_t)0xfffffffc;
    if (!strcmp(mode, "bad-name")) survey.name = (void *)(uintptr_t)0xfffffffe;
}

int fixture_classes(void)
{
    if (first.table != &first || survey.table != &survey || last.table != &last ||
        first.id != 11 || survey.id != 22 || last.id != 33 ||
        first.baseline != 111 || survey.baseline != 222 || last.baseline != 333) return -1;
    const char *mode = getenv("COOP_CLASS_MODE");
    if (mode && (!strcmp(mode, "cycle") || !strcmp(mode, "bad-next")))
        return g_pServerClassHead == &first && first.next == &survey && survey.next == &last ? 7 : -2;
    unsigned mask = 0, count = 0;
    for (struct node *p = g_pServerClassHead; p && count++ < 4; p = p->next)
        mask |= p == &first ? 1 : p == &survey ? 2 : p == &last ? 4 : 8;
    return count > 3 ? -3 : (int)mask;
}
