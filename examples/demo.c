/* qtmonaco-native C highlighting demo — open with:
   ./build/src/app/qmn-spike examples/demo.c
   Every role comes from the byte-upstream tree-sitter-c highlights.scm. */

#include <stdio.h>   /* system_lib_string gets the String ink (rare!) */
#include "demo.h"    /* a quoted include path is a string_literal too  */

#define MAX_NAME 64            /* macro BODY is a preproc_arg token: plain */
#define SQ(x) ((x) * (x))      /* only the name SQ and the param x get ink */
#define DEBUG 1

enum Color { RED, GREEN, BLUE };          /* enumerators ride the ALL_CAPS rule */
typedef struct { char name[MAX_NAME]; int hits; } Entry;

static int compare(const void *a, const void *b)
{
    const Entry *x = a, *y = b;           /* -> is an operator, name a Property */
    if (x->hits != y->hits)
        return y->hits - x->hits;         /* <=>-free C: != - all Operators */
    return strcmp(x->name, y->name);
}

int main(void)
{
    Entry book[] = { "alpha", 3 }, beta = { "beta", 1 };
    long total = 0, mask = 0xF0F0u, bits = 1UL << 5;
    char letter = 'z', tab = '\t';        /* char_literal: whole thing Number ink
                                            (the c scm never captures escapes) */

    for (size_t i = 0; i < 2; ++i) {
        total += book[i].hits + SQ(2);    /* SQ() call: the macro name is Function */
        if (total > MAX_NAME)
            goto done;                    /* goto stays plain; the label below is Function */
    }
    switch (total) {
    case RED:   /* fallthrough */ break;
    default: break;
    }

    printf("%s: %ld\n", "demo", total);   /* printf/puts hit call @function — c
                                            scm has no @function.builtin list */
done:
#if DEBUG
    puts("debug build");                  /* #if/#else/#endif are Keywords */
#else
    puts("release build");
#endif
    return EXIT_SUCCESS;                  /* EXIT_SUCCESS: ALL_CAPS Constant */
}
