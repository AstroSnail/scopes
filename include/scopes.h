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

#ifndef SCOPES_H
#define SCOPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined __cplusplus
extern "C" {
#endif

#define SCOPES_VERSION_MAJOR 0
#define SCOPES_VERSION_MINOR 14
#define SCOPES_VERSION_PATCH 0

// trace partial evaluation and code generation
// produces a firehose of information
#define SCOPES_DEBUG_CODEGEN 0

// run LLVM optimization passes
// turning this on is detrimental to startup time
// scopes output is typically clean enough to provide fairly good performance
// on its own.
#define SCOPES_OPTIMIZE_ASSEMBLY 0

// any exception aborts immediately and can not be caught
#define SCOPES_EARLY_ABORT 0

// print a list of cumulative timers on program exit
#define SCOPES_PRINT_TIMERS 1

// maximum number of recursions permitted during partial evaluation
// if you think you need more, ask yourself if ad-hoc compiling a pure C function
// that you can then use at compile time isn't the better choice;
// 100% of the time, the answer is yes because the performance is much better.
#define SCOPES_MAX_RECURSIONS 32

// maximum number of jump skips permitted
#define SCOPES_MAX_SKIP_JUMPS 256

// compile native code with debug info if not otherwise specified
#define SCOPES_COMPILE_WITH_DEBUG_INFO 1

#ifndef SCOPES_WIN32
#   ifdef _WIN32
#   define SCOPES_WIN32
#   endif
#endif

// maximum size of process stack
#ifdef SCOPES_WIN32
// on windows, we only get 1 MB of stack
// #define SCOPES_MAX_STACK_SIZE ((1 << 10) * 768)
// but we build with "-Wl,--stack,8388608"
#define SCOPES_MAX_STACK_SIZE ((1 << 20) * 7)
#else
// on linux, the system typically gives us 8 MB
#define SCOPES_MAX_STACK_SIZE ((1 << 20) * 7)
#endif

#define CAT(a, ...) PRIMITIVE_CAT(a, __VA_ARGS__)
#define PRIMITIVE_CAT(a, ...) a ## __VA_ARGS__

extern const char *scopes_compiler_path;
extern const char *scopes_compiler_dir;
extern const char *scopes_clang_include_dir;
extern const char *scopes_include_dir;
extern size_t scopes_argc;
extern char **scopes_argv;

void scopes_strtod(double *v, const char *str, char **str_end, int base );
void scopes_strtoll(int64_t *v, const char* str, char** endptr);
void scopes_strtoull(uint64_t *v, const char* str, char** endptr);

bool scopes_is_debug();

const char *scopes_compile_time_date();

#if defined __cplusplus
}
#endif

#endif // SCOPES_H
