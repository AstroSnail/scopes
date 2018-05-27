/*
Scopes Compiler
Copyright (c) 2016, 2017, 2018 Leonard Ritter

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "utils.hpp"

#include "scopes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#define STB_SPRINTF_DECORATE(name) stb_##name
#define STB_SPRINTF_NOUNALIGNED
#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

#include <cstdlib>
#include <cmath>
#include <string>

//------------------------------------------------------------------------------
// UTILITIES
//------------------------------------------------------------------------------

void scopes_strtod(double *v, const char *str, char **str_end, int base ) {
    *v = std::strtod(str, str_end);
}
static const char *skip_0b_prefix(const char *str, bool is_signed) {
    if (str[0]) {
        if (is_signed && (str[0] == '-') && str[1]) {
            str++;
        }
        if ((str[0] == '0') && str[1] && (str[1] == 'b')) {
            return str + 2;
        }
    }
    return nullptr;
}
void scopes_strtoll(int64_t *v, const char* str, char** endptr) {
    const char *binstr = skip_0b_prefix(str, true);
    if (binstr) {
        *v = std::strtoll(binstr, endptr, 2);
    } else {
        *v = std::strtoll(str, endptr, 0);
    }
}
void scopes_strtoull(uint64_t *v, const char* str, char** endptr) {
    const char *binstr = skip_0b_prefix(str, false);
    if (binstr) {
        *v = std::strtoull(str, endptr, 2);
    } else {
        *v = std::strtoull(str, endptr, 0);
    }
}

extern "C" {
// used in test_assorted.sc
#pragma GCC visibility push(default)
extern int scopes_test_add(int a, int b) { return a + b; }
#pragma GCC visibility pop
}

float powimpl(float a, float b) { return std::pow(a, b); }
double powimpl(double a, double b) { return std::pow(a, b); }
// thx to fabian for this one
template<typename T>
inline T powimpl(T base, T exponent) {
    T result = 1, cur = base;
    while (exponent) {
        if (exponent & 1) result *= cur;
        cur *= cur;
        exponent >>= 1;
    }
    return result;
}

bool scopes_is_debug() {
#ifdef SCOPES_DEBUG
        return true;
#else
        return false;
#endif
}

const char *scopes_compile_time_date() {
    return __DATE__ ", " __TIME__;
}

typedef struct stb_printf_ctx {
    FILE *dest;
    char tmp[STB_SPRINTF_MIN];
} stb_printf_ctx;

static char *_printf_cb(char * buf, void * user, int len) {
    stb_printf_ctx *ctx = (stb_printf_ctx *)user;
    fwrite (buf, 1, len, ctx->dest);
    return ctx->tmp;
}

#if 0
int stb_vprintf(const char *fmt, va_list va) {
    stb_printf_ctx ctx;
    ctx.dest = stdout;
    return stb_vsprintfcb(_printf_cb, &ctx, ctx.tmp, fmt, va);
}
#endif

namespace scopes {

int stb_printf(const char *fmt, ...) {
    stb_printf_ctx ctx;
    ctx.dest = stdout;
    va_list va;
    va_start(va, fmt);
    int c = stb_vsprintfcb(_printf_cb, &ctx, ctx.tmp, fmt, va);
    va_end(va);
    return c;
}

int stb_fprintf(FILE *out, const char *fmt, ...) {
    stb_printf_ctx ctx;
    ctx.dest = out;
    va_list va;
    va_start(va, fmt);
    int c = stb_vsprintfcb(_printf_cb, &ctx, ctx.tmp, fmt, va);
    va_end(va);
    return c;
}

} // namespace scopes