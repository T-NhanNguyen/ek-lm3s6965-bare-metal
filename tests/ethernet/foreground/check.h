#ifndef FOREGROUND_CHECK_H
#define FOREGROUND_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FOREGROUND_ASSERT %s:%d %s\n", __FILE__, __LINE__, #x); \
    exit(1); } } while (0)
#endif
