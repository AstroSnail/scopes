
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

/*
BEWARE: If you build this with anything else but a recent enough clang,
        you will have a bad time.

        an exception is windows where with mingw64, only gcc will work.
*/

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
#define SCOPES_PRINT_TIMERS 0

// maximum number of recursions permitted during partial evaluation
// if you think you need more, ask yourself if ad-hoc compiling a pure C function
// that you can then use at compile time isn't the better choice;
// 100% of the time, the answer is yes because the performance is much better.
#define SCOPES_MAX_RECURSIONS 32

// maximum number of jump skips permitted
#define SCOPES_MAX_SKIP_JUMPS 256

// compile native code with debug info if not otherwise specified
#define SCOPES_COMPILE_WITH_DEBUG_INFO 1

// skip labels that directly forward all return arguments
// except the ones that truncate them
// improves LLVM optimization time
#define SCOPES_TRUNCATE_FORWARDING_CONTINUATIONS 1

// inline a function from its template rather than mangling it
// otherwise the function is mangled, and only re-specialized if it
// returns closures, which is the faster option.
// leaving this off improves LLVM optimization time
#define SCOPES_INLINE_FUNCTION_FROM_TEMPLATE 0

// cleanup useless labels after lower2cff
// improves LLVM optimization time
#define SCOPES_CLEANUP_LABELS 1

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

#ifndef SCOPES_CPP
#define SCOPES_CPP

//------------------------------------------------------------------------------
// C HEADER
//------------------------------------------------------------------------------

#include <sys/types.h>
#ifdef SCOPES_WIN32
#include "mman.h"
#include "stdlib_ex.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "linenoise-ng/include/linenoise.h"
#include <ctype.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#define STB_SPRINTF_DECORATE(name) stb_##name
#define STB_SPRINTF_NOUNALIGNED
#include "stb_sprintf.h"
#include "cityhash/city.h"

#include <ffi.h>

#if defined __cplusplus
extern "C" {
#endif

#define CAT(a, ...) PRIMITIVE_CAT(a, __VA_ARGS__)
#define PRIMITIVE_CAT(a, ...) a ## __VA_ARGS__

const char *scopes_compiler_path;
const char *scopes_compiler_dir;
const char *scopes_clang_include_dir;
const char *scopes_include_dir;
size_t scopes_argc;
char **scopes_argv;

// C namespace exports
int unescape_string(char *buf);
int escape_string(char *buf, const char *str, int strcount, const char *quote_chars);

void scopes_strtod(double *v, const char *str, char **str_end, int base );
void scopes_strtoll(int64_t *v, const char* str, char** endptr, int base);
void scopes_strtoull(uint64_t *v, const char* str, char** endptr, int base);

bool scopes_is_debug();

const char *scopes_compile_time_date();

#if defined __cplusplus
}
#endif

#endif // SCOPES_CPP
#ifdef SCOPES_CPP_IMPL

//#define SCOPES_DEBUG_IL

#undef NDEBUG
#ifdef SCOPES_WIN32
#include <windows.h>
#include "stdlib_ex.h"
#include "dlfcn.h"
#else
// for backtrace
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include <assert.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdlib.h>
#include <libgen.h>

#include <cstdlib>
//#include <string>
#include <sstream>
#include <iostream>
#include <unordered_set>
#include <deque>
#include <csignal>

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Transforms/PassManagerBuilder.h>
#include <llvm-c/Disassembler.h>
#include <llvm-c/Support.h>

#include "llvm/IR/Module.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/Object/SymbolSize.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_os_ostream.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/RecordLayout.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/LiteralSupport.h"

#include "glslang/SpvBuilder.h"
#include "glslang/disassemble.h"
#include "glslang/GLSL.std.450.h"
#include "SPIRV-Cross/spirv_glsl.hpp"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

#pragma GCC diagnostic ignored "-Wvla-extension"
#pragma GCC diagnostic ignored "-Wzero-length-array"
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
// #pragma GCC diagnostic ignored "-Wembedded-directive"
// #pragma GCC diagnostic ignored "-Wgnu-statement-expression"
#pragma GCC diagnostic ignored "-Wc99-extensions"
// #pragma GCC diagnostic ignored "-Wmissing-braces"
// this one is only enabled for code cleanup
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wdate-time"
#pragma GCC diagnostic ignored "-Wabsolute-value"

#ifdef SCOPES_WIN32
#include <setjmpex.h>
#else
#include <setjmp.h>
#endif
#include "minilibs/regexp.cpp"

#include "cityhash/city.cpp"

//------------------------------------------------------------------------------
// UTILITIES
//------------------------------------------------------------------------------

void scopes_strtod(double *v, const char *str, char **str_end, int base ) {
    *v = std::strtod(str, str_end);
}
const char *skip_0b_prefix(const char *str, bool is_signed) {
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

static size_t align(size_t offset, size_t align) {
    return (offset + align - 1) & ~(align - 1);
}

static char parse_hexchar(char c) {
    if ((c >= '0') && (c <= '9')) {
        return c - '0';
    } else if ((c >= 'a') && (c <= 'f')) {
        return c - 'a' + 10;
    } else if ((c >= 'A') && (c <= 'F')) {
        return c - 'A' + 10;
    }
    return -1;
}

int unescape_string(char *buf) {
    char *dst = buf;
    char *src = buf;
    while (*src) {
        if (*src == '\\') {
            src++;
            if (*src == 0) {
                break;
            } if (*src == 'n') {
                *dst = '\n';
            } else if (*src == 't') {
                *dst = '\t';
            } else if (*src == 'r') {
                *dst = '\r';
            } else if (*src == 'x') {
                char c0 = parse_hexchar(*(src + 1));
                char c1 = parse_hexchar(*(src + 2));
                if ((c0 >= 0) && (c1 >= 0)) {
                    *dst = (c0 << 4) | c1;
                    src += 2;
                } else {
                    src--;
                    *dst = *src;
                }
            } else {
                *dst = *src;
            }
        } else {
            *dst = *src;
        }
        src++;
        dst++;
    }
    // terminate
    *dst = 0;
    return dst - buf;
}

#define B_SNFORMAT 512 // how many characters per callback
typedef char *(*vsformatcb_t)(const char *buf, void *user, int len);

struct vsformat_cb_ctx {
    int count;
    char *dest;
    char tmp[B_SNFORMAT];
};

static char *vsformat_cb(const char *buf, void *user, int len) {
    vsformat_cb_ctx *ctx = (vsformat_cb_ctx *)user;
    if (buf != ctx->dest) {
        char *d = ctx->dest;
        char *e = d + len;
        while (d != e) {
            *d++ = *buf++;
        }
    }
    ctx->dest += len;
    return ctx->tmp;
}

static char *vsformat_cb_null(const char *buf, void *user, int len) {
    vsformat_cb_ctx *ctx = (vsformat_cb_ctx *)user;
    ctx->count += len;
    return ctx->tmp;
}

static int escapestrcb(vsformatcb_t cb, void *user, char *buf,
    const char *str, int strcount,
    const char *quote_chars = nullptr) {
    assert(buf);
    const char *fmt_start = str;
    const char *fmt = fmt_start;
    char *p = buf;
#define VSFCB_CHECKWRITE(N) \
    if (((p - buf) + (N)) > B_SNFORMAT) { buf = p = cb(buf, user, p - buf); }
#define VSFCB_PRINT(MAXCOUNT, FMT, SRC) { \
        VSFCB_CHECKWRITE(MAXCOUNT+1); \
        p += snprintf(p, B_SNFORMAT - (p - buf), FMT, SRC); }
    for(;;) {
        char c = *fmt;
        switch(c) {
        case '\n': VSFCB_CHECKWRITE(2); *p++ = '\\'; *p++ = 'n'; break;
        case '\r': VSFCB_CHECKWRITE(2); *p++ = '\\'; *p++ = 'r'; break;
        case '\t': VSFCB_CHECKWRITE(2); *p++ = '\\'; *p++ = 't'; break;
        case 0: if ((fmt - fmt_start) == strcount) goto done;
            // otherwise, fall through
        default:
            if ((c < 32) || (c >= 127)) {
                VSFCB_PRINT(4, "\\x%02x", (unsigned char)c);
            } else {
                if ((c == '\\') || (quote_chars && strchr(quote_chars, c))) {
                    VSFCB_CHECKWRITE(1);
                    *p++ = '\\';
                }
                *p++ = c;
            }
            break;
        }
        fmt++;
    }
done:
    VSFCB_CHECKWRITE(B_SNFORMAT); // force flush if non-empty
    return 0;
#undef VSFCB_CHECKWRITE
#undef VSFCB_PRINT
}

int escape_string(char *buf, const char *str, int strcount, const char *quote_chars) {
    vsformat_cb_ctx ctx;
    if (buf) {
        ctx.dest = buf;
        escapestrcb(vsformat_cb, &ctx, ctx.tmp, str, strcount, quote_chars);
        int l = ctx.dest - buf;
        buf[l] = 0;
        return l;
    } else {
        ctx.count = 0;
        escapestrcb(vsformat_cb_null, &ctx, ctx.tmp, str, strcount, quote_chars);
        return ctx.count + 1;
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

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// GetMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement GetMainExecutable
// without being given the address of a function in the main executable).
std::string GetExecutablePath(const char *Argv0) {
  // This just needs to be some symbol in the binary; C++ doesn't
  // allow taking the address of ::main however.
  void *MainAddr = (void*) (intptr_t) GetExecutablePath;
  return llvm::sys::fs::getMainExecutable(Argv0, MainAddr);
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
static int stb_vprintf(const char *fmt, va_list va) {
    stb_printf_ctx ctx;
    ctx.dest = stdout;
    return stb_vsprintfcb(_printf_cb, &ctx, ctx.tmp, fmt, va);
}
extern "C" {
int stb_printf(const char *fmt, ...) {
    stb_printf_ctx ctx;
    ctx.dest = stdout;
    va_list va;
    va_start(va, fmt);
    int c = stb_vsprintfcb(_printf_cb, &ctx, ctx.tmp, fmt, va);
    va_end(va);
    return c;
}
}

static int stb_fprintf(FILE *out, const char *fmt, ...) {
    stb_printf_ctx ctx;
    ctx.dest = out;
    va_list va;
    va_start(va, fmt);
    int c = stb_vsprintfcb(_printf_cb, &ctx, ctx.tmp, fmt, va);
    va_end(va);
    return c;
}

namespace scopes {

static char *g_stack_start;
static size_t g_largest_stack_size = 0;

static size_t memory_stack_size() {
    char c; char *_stack_addr = &c;
    size_t ss = (size_t)(g_stack_start - _stack_addr);
    g_largest_stack_size = std::max(ss, g_largest_stack_size);
    return ss;
}

// for allocated pointers, register the size of the range
static std::map<void *, size_t> tracked_allocations;
static void track(void *ptr, size_t size) {
    tracked_allocations.insert({ptr,size});
}
static void *tracked_malloc(size_t size) {
    void *ptr = malloc(size);
    track(ptr, size);
    return ptr;
}
static bool find_allocation(void *srcptr,  void *&start, size_t &size) {
    auto it = tracked_allocations.upper_bound(srcptr);
    if (it == tracked_allocations.begin())
        return false;
    it--;
    start = it->first;
    size = it->second;
    return (srcptr >= start)&&((uint8_t*)srcptr < ((uint8_t*)start + size));
}

using llvm::isa;
using llvm::cast;
using llvm::dyn_cast;

template <typename R, typename... Args>
static std::function<R (Args...)> memoize(R (*fn)(Args...)) {
    std::map<std::tuple<Args...>, R> table;
    return [fn, table](Args... args) mutable -> R {
        auto argt = std::make_tuple(args...);
        auto memoized = table.find(argt);
        if(memoized == table.end()) {
            auto result = fn(args...);
            table[argt] = result;
            return result;
        } else {
            return memoized->second;
        }
    };
}

//------------------------------------------------------------------------------
// SYMBOL ENUM
//------------------------------------------------------------------------------

// list of symbols to be exposed as builtins to the default global namespace
#define B_GLOBALS() \
    T(FN_Branch) T(KW_Fn) T(KW_ImpureFn) T(KW_Label) T(KW_Quote) \
    T(KW_Call) T(KW_RawCall) T(KW_CCCall) T(SYM_QuoteForm) T(FN_Dump) T(KW_Do) \
    T(FN_FunctionType) T(FN_TupleType) T(FN_UnionType) T(FN_Alloca) T(FN_AllocaOf) T(FN_Malloc) \
    T(FN_AllocaArray) T(FN_MallocArray) T(FN_ReturnLabelType) T(KW_DoIn) T(FN_AllocaExceptionPad) \
    T(FN_StaticAlloc) \
    T(FN_AnyExtract) T(FN_AnyWrap) T(FN_IsConstant) T(FN_Free) T(KW_Defer) \
    T(OP_ICmpEQ) T(OP_ICmpNE) T(FN_Sample) T(FN_ImageRead) T(FN_ImageWrite) \
    T(OP_ICmpUGT) T(OP_ICmpUGE) T(OP_ICmpULT) T(OP_ICmpULE) \
    T(OP_ICmpSGT) T(OP_ICmpSGE) T(OP_ICmpSLT) T(OP_ICmpSLE) \
    T(OP_FCmpOEQ) T(OP_FCmpONE) T(OP_FCmpORD) \
    T(OP_FCmpOGT) T(OP_FCmpOGE) T(OP_FCmpOLT) T(OP_FCmpOLE) \
    T(OP_FCmpUEQ) T(OP_FCmpUNE) T(OP_FCmpUNO) \
    T(OP_FCmpUGT) T(OP_FCmpUGE) T(OP_FCmpULT) T(OP_FCmpULE) \
    T(FN_Purify) T(FN_Unconst) T(FN_TypeOf) T(FN_Bitcast) \
    T(FN_IntToPtr) T(FN_PtrToInt) T(FN_Load) T(FN_Store) \
    T(FN_VolatileLoad) T(FN_VolatileStore) T(SFXFN_ExecutionMode) \
    T(FN_ExtractElement) T(FN_InsertElement) T(FN_ShuffleVector) \
    T(FN_ExtractValue) T(FN_InsertValue) T(FN_ITrunc) T(FN_ZExt) T(FN_SExt) \
    T(FN_GetElementPtr) T(FN_OffsetOf) T(SFXFN_CompilerError) T(FN_VaCountOf) T(FN_VaAt) \
    T(FN_VaKeys) T(FN_VaKey) T(FN_VaValues) T(FN_CompilerMessage) T(FN_Undef) T(FN_NullOf) T(KW_Let) \
    T(KW_If) T(SFXFN_SetTypeSymbol) T(SFXFN_DelTypeSymbol) T(FN_ExternSymbol) \
    T(SFXFN_SetTypenameStorage) T(FN_ExternNew) \
    T(SFXFN_Discard) \
    T(FN_TypeAt) T(FN_TypeLocalAt) T(KW_SyntaxExtend) T(FN_Location) T(SFXFN_Unreachable) \
    T(FN_FPTrunc) T(FN_FPExt) T(FN_ScopeOf) \
    T(FN_FPToUI) T(FN_FPToSI) \
    T(FN_UIToFP) T(FN_SIToFP) \
    T(OP_Add) T(OP_AddNUW) T(OP_AddNSW) \
    T(OP_Sub) T(OP_SubNUW) T(OP_SubNSW) \
    T(OP_Mul) T(OP_MulNUW) T(OP_MulNSW) \
    T(OP_SDiv) T(OP_UDiv) \
    T(OP_SRem) T(OP_URem) \
    T(OP_Shl) T(OP_LShr) T(OP_AShr) \
    T(OP_BAnd) T(OP_BOr) T(OP_BXor) \
    T(OP_FAdd) T(OP_FSub) T(OP_FMul) T(OP_FDiv) T(OP_FRem) \
    T(OP_Tertiary) T(KW_SyntaxLog) \
    T(OP_Mix) T(OP_Step) T(OP_SmoothStep) \
    T(FN_Round) T(FN_RoundEven) T(OP_Trunc) \
    T(OP_FAbs) T(OP_FSign) T(OP_SSign) \
    T(OP_Floor) T(FN_Ceil) T(FN_Fract) \
    T(OP_Radians) T(OP_Degrees) \
    T(OP_Sin) T(OP_Cos) T(OP_Tan) \
    T(OP_Asin) T(OP_Acos) T(OP_Atan) T(OP_Atan2) \
    T(OP_Exp) T(OP_Log) T(OP_Exp2) T(OP_Log2) \
    T(OP_Pow) T(OP_Sqrt) T(OP_InverseSqrt) \
    T(FN_Fma) T(FN_Frexp) T(FN_Ldexp) \
    T(FN_Length) T(FN_Distance) T(FN_Cross) T(FN_Normalize)

#define B_SPIRV_STORAGE_CLASS() \
    T(UniformConstant) \
    T(Input) \
    T(Uniform) \
    T(Output) \
    T(Workgroup) \
    T(CrossWorkgroup) \
    T(Private) \
    T(Function) \
    T(Generic) \
    T(PushConstant) \
    T(AtomicCounter) \
    T(Image) \
    T(StorageBuffer)

#define B_SPIRV_DIM() \
    T(1D) \
    T(2D) \
    T(3D) \
    T(Cube) \
    T(Rect) \
    T(Buffer) \
    T(SubpassData)

#define B_SPIRV_IMAGE_FORMAT() \
    T(Unknown) \
    T(Rgba32f) \
    T(Rgba16f) \
    T(R32f) \
    T(Rgba8) \
    T(Rgba8Snorm) \
    T(Rg32f) \
    T(Rg16f) \
    T(R11fG11fB10f) \
    T(R16f) \
    T(Rgba16) \
    T(Rgb10A2) \
    T(Rg16) \
    T(Rg8) \
    T(R16) \
    T(R8) \
    T(Rgba16Snorm) \
    T(Rg16Snorm) \
    T(Rg8Snorm) \
    T(R16Snorm) \
    T(R8Snorm) \
    T(Rgba32i) \
    T(Rgba16i) \
    T(Rgba8i) \
    T(R32i) \
    T(Rg32i) \
    T(Rg16i) \
    T(Rg8i) \
    T(R16i) \
    T(R8i) \
    T(Rgba32ui) \
    T(Rgba16ui) \
    T(Rgba8ui) \
    T(R32ui) \
    T(Rgb10a2ui) \
    T(Rg32ui) \
    T(Rg16ui) \
    T(Rg8ui) \
    T(R16ui) \
    T(R8ui)

#define B_SPIRV_BUILTINS() \
    T(Position) \
    T(PointSize) \
    T(ClipDistance) \
    T(CullDistance) \
    T(VertexId) \
    T(InstanceId) \
    T(PrimitiveId) \
    T(InvocationId) \
    T(Layer) \
    T(ViewportIndex) \
    T(TessLevelOuter) \
    T(TessLevelInner) \
    T(TessCoord) \
    T(PatchVertices) \
    T(FragCoord) \
    T(PointCoord) \
    T(FrontFacing) \
    T(SampleId) \
    T(SamplePosition) \
    T(SampleMask) \
    T(FragDepth) \
    T(HelperInvocation) \
    T(NumWorkgroups) \
    T(WorkgroupSize) \
    T(WorkgroupId) \
    T(LocalInvocationId) \
    T(GlobalInvocationId) \
    T(LocalInvocationIndex) \
    T(WorkDim) \
    T(GlobalSize) \
    T(EnqueuedWorkgroupSize) \
    T(GlobalOffset) \
    T(GlobalLinearId) \
    T(SubgroupSize) \
    T(SubgroupMaxSize) \
    T(NumSubgroups) \
    T(NumEnqueuedSubgroups) \
    T(SubgroupId) \
    T(SubgroupLocalInvocationId) \
    T(VertexIndex) \
    T(InstanceIndex) \
    T(SubgroupEqMaskKHR) \
    T(SubgroupGeMaskKHR) \
    T(SubgroupGtMaskKHR) \
    T(SubgroupLeMaskKHR) \
    T(SubgroupLtMaskKHR) \
    T(BaseVertex) \
    T(BaseInstance) \
    T(DrawIndex) \
    T(DeviceIndex) \
    T(ViewIndex) \
    T(BaryCoordNoPerspAMD) \
    T(BaryCoordNoPerspCentroidAMD) \
    T(BaryCoordNoPerspSampleAMD) \
    T(BaryCoordSmoothAMD) \
    T(BaryCoordSmoothCentroidAMD) \
    T(BaryCoordSmoothSampleAMD) \
    T(BaryCoordPullModelAMD) \
    T(ViewportMaskNV) \
    T(SecondaryPositionNV) \
    T(SecondaryViewportMaskNV) \
    T(PositionPerViewNV) \
    T(ViewportMaskPerViewNV)

#define B_SPIRV_EXECUTION_MODE() \
    T(Invocations) \
    T(SpacingEqual) \
    T(SpacingFractionalEven) \
    T(SpacingFractionalOdd) \
    T(VertexOrderCw) \
    T(VertexOrderCcw) \
    T(PixelCenterInteger) \
    T(OriginUpperLeft) \
    T(OriginLowerLeft) \
    T(EarlyFragmentTests) \
    T(PointMode) \
    T(Xfb) \
    T(DepthReplacing) \
    T(DepthGreater) \
    T(DepthLess) \
    T(DepthUnchanged) \
    T(LocalSize) \
    T(LocalSizeHint) \
    T(InputPoints) \
    T(InputLines) \
    T(InputLinesAdjacency) \
    T(Triangles) \
    T(InputTrianglesAdjacency) \
    T(Quads) \
    T(Isolines) \
    T(OutputVertices) \
    T(OutputPoints) \
    T(OutputLineStrip) \
    T(OutputTriangleStrip) \
    T(VecTypeHint) \
    T(ContractionOff) \
    T(PostDepthCoverage)

#define B_SPIRV_IMAGE_OPERAND() \
    T(Bias) \
    T(Lod) \
    T(GradX) \
    T(GradY) \
    T(ConstOffset) \
    T(Offset) \
    T(ConstOffsets) \
    T(Sample) \
    T(MinLod) \
    /* extra operands not part of mask */ \
    T(Dref) \
    T(Proj) \
    T(Fetch) \
    T(Gather) \
    T(Sparse)

#define B_MAP_SYMBOLS() \
    T(SYM_Unnamed, "") \
    \
    /* keywords and macros */ \
    T(KW_CatRest, "::*") T(KW_CatOne, "::@") \
    T(KW_SyntaxLog, "syntax-log") T(KW_DoIn, "do-in") T(KW_Defer, "__defer") \
    T(KW_Assert, "assert") T(KW_Break, "break") T(KW_Label, "label") \
    T(KW_Call, "call") T(KW_RawCall, "rawcall") T(KW_CCCall, "cc/call") T(KW_Continue, "continue") \
    T(KW_Define, "define") T(KW_Do, "do") T(KW_DumpSyntax, "dump-syntax") \
    T(KW_Else, "else") T(KW_ElseIf, "elseif") T(KW_EmptyList, "empty-list") \
    T(KW_EmptyTuple, "empty-tuple") T(KW_Escape, "escape") \
    T(KW_Except, "except") T(KW_False, "false") T(KW_Fn, "fn") T(KW_ImpureFn, "fn!") \
    T(KW_FnTypes, "fn-types") T(KW_FnCC, "fn/cc") T(KW_Globals, "globals") \
    T(KW_If, "if") T(KW_In, "in") T(KW_Let, "let") T(KW_Loop, "loop") \
    T(KW_LoopFor, "loop-for") T(KW_None, "none") T(KW_Null, "null") \
    T(KW_QQuoteSyntax, "qquote-syntax") T(KW_Quote, "quote") \
    T(KW_QuoteSyntax, "quote-syntax") T(KW_Raise, "raise") T(KW_Recur, "recur") \
    T(KW_Return, "return") T(KW_Splice, "splice") \
    T(KW_SyntaxExtend, "syntax-extend") T(KW_True, "true") T(KW_Try, "try") \
    T(KW_Unquote, "unquote") T(KW_UnquoteSplice, "unquote-splice") T(KW_ListEmpty, "eol") \
    T(KW_With, "with") T(KW_XFn, "xfn") T(KW_XLet, "xlet") T(KW_Yield, "yield") \
    \
    /* builtin and global functions */ \
    T(FN_Alignof, "alignof") T(FN_OffsetOf, "offsetof") \
    T(FN_Args, "args") T(FN_Alloc, "alloc") T(FN_Arrayof, "arrayof") \
    T(FN_AnchorPath, "Anchor-path") T(FN_AnchorLineNumber, "Anchor-line-number") \
    T(FN_AnchorColumn, "Anchor-column") T(FN_AnchorOffset, "Anchor-offset") \
    T(FN_AnchorSource, "Anchor-source") \
    T(OP_Mix, "mix") T(OP_Step, "step") T(OP_SmoothStep, "smoothstep") \
    T(FN_Round, "round") T(FN_RoundEven, "roundeven") T(OP_Trunc, "trunc") \
    T(OP_FAbs, "fabs") T(OP_FSign, "fsign") T(OP_SSign, "ssign") \
    T(OP_Floor, "floor") T(FN_Ceil, "ceil") T(FN_Fract, "fract") \
    T(OP_Radians, "radians") T(OP_Degrees, "degrees") \
    T(OP_Sin, "sin") T(OP_Cos, "cos") T(OP_Tan, "tan") \
    T(OP_Asin, "asin") T(OP_Acos, "acos") T(OP_Atan, "atan") T(OP_Atan2, "atan2") \
    T(OP_Exp, "exp") T(OP_Log, "log") T(OP_Exp2, "exp2") T(OP_Log2, "log2") \
    T(OP_Sqrt, "sqrt") T(OP_InverseSqrt, "inversesqrt") \
    T(FN_Fma, "fma") T(FN_Frexp, "frexp") T(FN_Ldexp, "ldexp") \
    T(FN_Length, "length") T(FN_Distance, "distance") T(FN_Cross, "cross") T(FN_Normalize, "normalize") \
    T(FN_AnyExtract, "Any-extract-constant") T(FN_AnyWrap, "Any-wrap") \
    T(FN_ActiveAnchor, "active-anchor") T(FN_ActiveFrame, "active-frame") \
    T(FN_BitCountOf, "bitcountof") T(FN_IsSigned, "signed?") \
    T(FN_Bitcast, "bitcast") T(FN_IntToPtr, "inttoptr") T(FN_PtrToInt, "ptrtoint") \
    T(FN_BlockMacro, "block-macro") \
    T(FN_BlockScopeMacro, "block-scope-macro") T(FN_BoolEq, "bool==") \
    T(FN_BuiltinEq, "Builtin==") \
    T(FN_Branch, "branch") T(FN_IsCallable, "callable?") T(FN_Cast, "cast") \
    T(FN_Concat, "concat") T(FN_Cons, "cons") T(FN_IsConstant, "constant?") \
    T(FN_Countof, "countof") \
    T(FN_Compile, "__compile") T(FN_CompileSPIRV, "__compile-spirv") \
    T(FN_CompileGLSL, "__compile-glsl") \
    T(FN_CompileObject, "__compile-object") \
    T(FN_ElementIndex, "element-index") \
    T(FN_ElementName, "element-name") \
    T(FN_CompilerMessage, "compiler-message") \
    T(FN_CStr, "cstr") T(FN_DatumToSyntax, "datum->syntax") \
    T(FN_DatumToQuotedSyntax, "datum->quoted-syntax") \
    T(FN_LabelDocString, "Label-docstring") \
    T(FN_DefaultStyler, "default-styler") T(FN_StyleToString, "style->string") \
    T(FN_Disqualify, "disqualify") T(FN_Dump, "dump") \
    T(FN_DumpLabel, "dump-label") \
    T(FN_DumpList, "dump-list") \
    T(FN_DumpFrame, "dump-frame") \
    T(FN_ClosureLabel, "Closure-label") \
    T(FN_ClosureFrame, "Closure-frame") \
    T(FN_FormatFrame, "Frame-format") \
    T(FN_ElementType, "element-type") T(FN_IsEmpty, "empty?") \
    T(FN_TypeCountOf, "type-countof") \
    T(FN_Enumerate, "enumerate") T(FN_Eval, "eval") \
    T(FN_Exit, "exit") T(FN_Expand, "expand") \
    T(FN_ExternLibrary, "extern-library") \
    T(FN_ExternSymbol, "extern-symbol") \
    T(FN_ExtractMemory, "extract-memory") \
    T(FN_EnterSolverCLI, "enter-solver-cli!") \
    T(FN_ExtractValue, "extractvalue") T(FN_InsertValue, "insertvalue") \
    T(FN_ExtractElement, "extractelement") T(FN_InsertElement, "insertelement") \
    T(FN_ShuffleVector, "shufflevector") T(FN_GetElementPtr, "getelementptr") \
    T(FN_FFISymbol, "ffi-symbol") T(FN_FFICall, "ffi-call") \
    T(FN_FrameEq, "Frame==") T(FN_Free, "free") \
    T(FN_GetExceptionHandler, "get-exception-handler") \
    T(FN_GetScopeSymbol, "get-scope-symbol") T(FN_Hash, "__hash") \
    T(FN_Hash2x64, "__hash2x64") T(FN_HashBytes, "__hashbytes") \
    T(FN_Sample, "sample") \
    T(FN_ImageRead, "Image-read") T(FN_ImageWrite, "Image-write") \
    T(FN_RealPath, "realpath") \
    T(FN_DirName, "dirname") T(FN_BaseName, "basename") \
    T(OP_ICmpEQ, "icmp==") T(OP_ICmpNE, "icmp!=") \
    T(OP_ICmpUGT, "icmp>u") T(OP_ICmpUGE, "icmp>=u") T(OP_ICmpULT, "icmp<u") T(OP_ICmpULE, "icmp<=u") \
    T(OP_ICmpSGT, "icmp>s") T(OP_ICmpSGE, "icmp>=s") T(OP_ICmpSLT, "icmp<s") T(OP_ICmpSLE, "icmp<=s") \
    T(OP_FCmpOEQ, "fcmp==o") T(OP_FCmpONE, "fcmp!=o") T(OP_FCmpORD, "fcmp-ord") \
    T(OP_FCmpOGT, "fcmp>o") T(OP_FCmpOGE, "fcmp>=o") T(OP_FCmpOLT, "fcmp<o") T(OP_FCmpOLE, "fcmp<=o") \
    T(OP_FCmpUEQ, "fcmp==u") T(OP_FCmpUNE, "fcmp!=u") T(OP_FCmpUNO, "fcmp-uno") \
    T(OP_FCmpUGT, "fcmp>u") T(OP_FCmpUGE, "fcmp>=u") T(OP_FCmpULT, "fcmp<u") T(OP_FCmpULE, "fcmp<=u") \
    T(OP_Add, "add") T(OP_AddNUW, "add-nuw") T(OP_AddNSW, "add-nsw") \
    T(OP_Sub, "sub") T(OP_SubNUW, "sub-nuw") T(OP_SubNSW, "sub-nsw") \
    T(OP_Mul, "mul") T(OP_MulNUW, "mul-nuw") T(OP_MulNSW, "mul-nsw") \
    T(OP_SDiv, "sdiv") T(OP_UDiv, "udiv") \
    T(OP_SRem, "srem") T(OP_URem, "urem") \
    T(OP_Shl, "shl") T(OP_LShr, "lshr") T(OP_AShr, "ashr") \
    T(OP_BAnd, "band") T(OP_BOr, "bor") T(OP_BXor, "bxor") \
    T(FN_IsFile, "file?") T(FN_IsDirectory, "directory?") \
    T(OP_FAdd, "fadd") T(OP_FSub, "fsub") T(OP_FMul, "fmul") T(OP_FDiv, "fdiv") T(OP_FRem, "frem") \
    T(FN_FPTrunc, "fptrunc") T(FN_FPExt, "fpext") \
    T(FN_FPToUI, "fptoui") T(FN_FPToSI, "fptosi") \
    T(FN_UIToFP, "uitofp") T(FN_SIToFP, "sitofp") \
    T(FN_ImportC, "import-c") T(FN_IsInteger, "integer?") \
    T(FN_IntegerType, "integer-type") \
    T(FN_CompilerVersion, "compiler-version") \
    T(FN_Iter, "iter") T(FN_FormatMessage, "format-message") \
    T(FN_IsIterator, "iterator?") T(FN_IsLabel, "label?") \
    T(FN_LabelEq, "Label==") \
    T(FN_LabelNew, "Label-new") T(FN_LabelParameterCount, "Label-parameter-count") \
    T(FN_LabelParameter, "Label-parameter") \
    T(FN_LabelAnchor, "Label-anchor") T(FN_LabelName, "Label-name") \
    T(FN_ClosureEq, "Closure==") T(FN_CheckStack, "verify-stack!") \
    T(FN_ListAtom, "list-atom?") T(FN_ListCountOf, "list-countof") \
    T(FN_ListLoad, "list-load") T(FN_ListJoin, "list-join") \
    T(FN_ListParse, "list-parse") T(FN_IsList, "list?") T(FN_Load, "load") \
    T(FN_LoadLibrary, "load-library") \
    T(FN_VolatileLoad, "volatile-load") \
    T(FN_VolatileStore, "volatile-store") \
    T(FN_LabelCountOfReachable, "Label-countof-reachable") \
    T(FN_ListAt, "list-at") T(FN_ListNext, "list-next") T(FN_ListCons, "list-cons") \
    T(FN_IsListEmpty, "list-empty?") \
    T(FN_Malloc, "malloc") T(FN_MallocArray, "malloc-array") T(FN_Unconst, "unconst") \
    T(FN_Macro, "macro") T(FN_Max, "max") T(FN_Min, "min") \
    T(FN_MemCopy, "memcopy") \
    T(FN_IsMutable, "mutable?") \
    T(FN_IsNone, "none?") \
    T(FN_IsNull, "null?") T(FN_OrderedBranch, "ordered-branch") \
    T(FN_ParameterEq, "Parameter==") \
    T(FN_ParameterNew, "Parameter-new") T(FN_ParameterName, "Parameter-name") \
    T(FN_ParameterAnchor, "Parameter-anchor") \
    T(FN_ParameterIndex, "Parameter-index") \
    T(FN_ParseC, "parse-c") T(FN_PointerOf, "pointerof") \
    T(FN_PointerType, "pointer-type") \
    T(FN_PointerFlags, "pointer-type-flags") \
    T(FN_PointerSetFlags, "pointer-type-set-flags") \
    T(FN_PointerStorageClass, "pointer-type-storage-class") \
    T(FN_PointerSetStorageClass, "pointer-type-set-storage-class") \
    T(FN_PointerSetElementType, "pointer-type-set-element-type") \
    T(FN_ExternLocation, "extern-type-location") \
    T(FN_ExternBinding, "extern-type-binding") \
    T(FN_FunctionType, "function-type") \
    T(FN_FunctionTypeIsVariadic, "function-type-variadic?") \
    T(FN_TupleType, "tuple-type") \
    T(FN_UnionType, "union-type") \
    T(FN_ReturnLabelType, "ReturnLabel-type") \
    T(FN_ArrayType, "array-type") T(FN_ImageType, "Image-type") \
    T(FN_SampledImageType, "SampledImage-type") \
    T(FN_TypenameType, "typename-type") \
    T(FN_Purify, "purify") \
    T(FN_Write, "io-write!") \
    T(FN_Flush, "io-flush") \
    T(FN_Product, "product") T(FN_Prompt, "__prompt") T(FN_Qualify, "qualify") \
    T(FN_SetAutocompleteScope, "set-autocomplete-scope!") \
    T(FN_Range, "range") T(FN_RefNew, "ref-new") T(FN_RefAt, "ref@") \
    T(FN_Repeat, "repeat") T(FN_Repr, "Any-repr") T(FN_AnyString, "Any-string") \
    T(FN_Require, "require") T(FN_ScopeOf, "scopeof") T(FN_ScopeAt, "Scope@") \
    T(FN_ScopeLocalAt, "Scope-local@") \
    T(FN_ScopeEq, "Scope==") \
    T(FN_ScopeNew, "Scope-new") \
    T(FN_ScopeCopy, "Scope-clone") \
    T(FN_ScopeDocString, "Scope-docstring") \
    T(FN_SetScopeDocString, "set-scope-docstring!") \
    T(FN_ScopeNewSubscope, "Scope-new-expand") \
    T(FN_ScopeCopySubscope, "Scope-clone-expand") \
    T(FN_ScopeParent, "Scope-parent") \
    T(FN_ScopeNext, "Scope-next") T(FN_SizeOf, "sizeof") \
    T(FN_TypeNext, "type-next") \
    T(FN_Slice, "slice") T(FN_Store, "store") \
    T(FN_StringAt, "string@") T(FN_StringCmp, "string-compare") \
    T(FN_StringCountOf, "string-countof") T(FN_StringNew, "string-new") \
    T(FN_StringJoin, "string-join") T(FN_StringSlice, "string-slice") \
    T(FN_StructOf, "structof") T(FN_TypeStorage, "storageof") \
    T(FN_IsOpaque, "opaque?") \
    T(FN_SymbolEq, "Symbol==") T(FN_SymbolNew, "string->Symbol") \
    T(FN_StringToRawstring, "string->rawstring") \
    T(FN_IsSymbol, "symbol?") \
    T(FN_SyntaxToAnchor, "syntax->anchor") T(FN_SyntaxToDatum, "syntax->datum") \
    T(FN_SyntaxCons, "syntax-cons") T(FN_SyntaxDo, "syntax-do") \
    T(FN_IsSyntaxHead, "syntax-head?") \
    T(FN_SyntaxList, "syntax-list") T(FN_SyntaxQuote, "syntax-quote") \
    T(FN_IsSyntaxQuoted, "syntax-quoted?") \
    T(FN_SyntaxUnquote, "syntax-unquote") \
    T(FN_SymbolToString, "Symbol->string") \
    T(FN_StringMatch, "string-match?") \
    T(FN_SuperOf, "superof") \
    T(FN_SyntaxNew, "Syntax-new") \
    T(FN_SyntaxWrap, "Syntax-wrap") \
    T(FN_SyntaxStrip, "Syntax-strip") \
    T(FN_Translate, "translate") T(FN_ITrunc, "itrunc") \
    T(FN_ZExt, "zext") T(FN_SExt, "sext") \
    T(FN_TupleOf, "tupleof") T(FN_TypeNew, "type-new") T(FN_TypeName, "type-name") \
    T(FN_TypeSizeOf, "type-sizeof") \
    T(FN_Typify, "__typify") \
    T(FN_TypeEq, "type==") T(FN_IsType, "type?") T(FN_TypeOf, "typeof") \
    T(FN_TypeKind, "type-kind") \
    T(FN_TypeDebugABI, "type-debug-abi") \
    T(FN_TypeAt, "type@") \
    T(FN_TypeLocalAt, "type-local@") \
    T(FN_RuntimeTypeAt, "runtime-type@") \
    T(FN_Undef, "undef") T(FN_NullOf, "nullof") T(FN_Alloca, "alloca") \
    T(FN_AllocaExceptionPad, "alloca-exception-pad") \
    T(FN_AllocaOf, "allocaof") \
    T(FN_AllocaArray, "alloca-array") \
    T(FN_StaticAlloc, "static-alloc") \
    T(FN_Location, "compiler-anchor") \
    T(FN_ExternNew, "extern-new") \
    T(FN_VaCountOf, "va-countof") T(FN_VaKeys, "va-keys") \
    T(FN_VaValues, "va-values") T(FN_VaAt, "va@") \
    T(FN_VaKey, "va-key") \
    T(FN_VectorOf, "vectorof") T(FN_XPCall, "xpcall") T(FN_Zip, "zip") \
    T(FN_VectorType, "vector-type") \
    T(FN_ZipFill, "zip-fill") \
    \
    /* builtin and global functions with side effects */ \
    T(SFXFN_CopyMemory, "copy-memory!") \
    T(SFXFN_Unreachable, "unreachable!") \
    T(SFXFN_Discard, "discard!") \
    T(SFXFN_Error, "__error!") \
    T(SFXFN_AnchorError, "__anchor-error!") \
    T(SFXFN_Raise, "__raise!") \
    T(SFXFN_Abort, "abort!") \
    T(SFXFN_CompilerError, "compiler-error!") \
    T(SFXFN_SetAnchor, "set-anchor!") \
    T(SFXFN_LabelAppendParameter, "label-append-parameter!") \
    T(SFXFN_RefSet, "ref-set!") \
    T(SFXFN_SetExceptionHandler, "set-exception-handler!") \
    T(SFXFN_SetGlobals, "set-globals!") \
    T(SFXFN_SetTypenameSuper, "set-typename-super!") \
    T(SFXFN_SetGlobalApplyFallback, "set-global-apply-fallback!") \
    T(SFXFN_SetScopeSymbol, "__set-scope-symbol!") \
    T(SFXFN_DelScopeSymbol, "delete-scope-symbol!") \
    T(SFXFN_SetTypeSymbol, "set-type-symbol!") \
    T(SFXFN_DelTypeSymbol, "delete-type-symbol!") \
    T(SFXFN_SetTypenameStorage, "set-typename-storage!") \
    T(SFXFN_ExecutionMode, "set-execution-mode!") \
    T(SFXFN_TranslateLabelBody, "translate-label-body!") \
    \
    /* builtin operator functions that can also be used as infix */ \
    T(OP_NotEq, "!=") T(OP_Mod, "%") T(OP_InMod, "%=") T(OP_BitAnd, "&") T(OP_InBitAnd, "&=") \
    T(OP_IFXMul, "*") T(OP_Pow, "powf") T(OP_InMul, "*=") T(OP_IFXAdd, "+") T(OP_Incr, "++") \
    T(OP_InAdd, "+=") T(OP_Comma, ",") T(OP_IFXSub, "-") T(OP_Decr, "--") T(OP_InSub, "-=") \
    T(OP_Dot, ".") T(OP_Join, "..") T(OP_Div, "/") T(OP_InDiv, "/=") \
    T(OP_Colon, ":") T(OP_Let, ":=") T(OP_Less, "<") T(OP_LeftArrow, "<-") T(OP_Subtype, "<:") \
    T(OP_ShiftL, "<<") T(OP_LessThan, "<=") T(OP_Set, "=") T(OP_Eq, "==") \
    T(OP_Greater, ">") T(OP_GreaterThan, ">=") T(OP_ShiftR, ">>") T(OP_Tertiary, "?") \
    T(OP_At, "@") T(OP_Xor, "^") T(OP_InXor, "^=") T(OP_And, "and") T(OP_Not, "not") \
    T(OP_Or, "or") T(OP_BitOr, "|") T(OP_InBitOr, "|=") T(OP_BitNot, "~") \
    T(OP_InBitNot, "~=") \
    \
    /* globals */ \
    T(SYM_DebugBuild, "debug-build?") \
    T(SYM_CompilerDir, "compiler-dir") \
    T(SYM_CompilerPath, "compiler-path") \
    T(SYM_CompilerTimestamp, "compiler-timestamp") \
    \
    /* parse-c keywords */ \
    T(SYM_Struct, "struct") \
    T(SYM_Union, "union") \
    T(SYM_TypeDef, "typedef") \
    T(SYM_Enum, "enum") \
    T(SYM_Array, "array") \
    T(SYM_Vector, "vector") \
    T(SYM_FNType, "fntype") \
    T(SYM_Extern, "extern") \
    \
    /* styles */ \
    T(Style_None, "style-none") \
    T(Style_Symbol, "style-symbol") \
    T(Style_String, "style-string") \
    T(Style_Number, "style-number") \
    T(Style_Keyword, "style-keyword") \
    T(Style_Function, "style-function") \
    T(Style_SfxFunction, "style-sfxfunction") \
    T(Style_Operator, "style-operator") \
    T(Style_Instruction, "style-instruction") \
    T(Style_Type, "style-type") \
    T(Style_Comment, "style-comment") \
    T(Style_Error, "style-error") \
    T(Style_Warning, "style-warning") \
    T(Style_Location, "style-location") \
    \
    /* builtins, forms, etc */ \
    T(SYM_FnCCForm, "form-fn-body") \
    T(SYM_QuoteForm, "form-quote") \
    T(SYM_DoForm, "form-do") \
    T(SYM_SyntaxScope, "syntax-scope") \
    T(SYM_CallHandler, "__call") \
    \
    /* varargs */ \
    T(SYM_Parenthesis, "...") \
    \
    T(SYM_ListWildcard, "#list") \
    T(SYM_SymbolWildcard, "#symbol") \
    T(SYM_ThisFnCC, "#this-fn/cc") \
    \
    T(SYM_Compare, "compare") \
    T(SYM_Size, "size") \
    T(SYM_Alignment, "alignment") \
    T(SYM_Unsigned, "unsigned") \
    T(SYM_Bitwidth, "bitwidth") \
    T(SYM_Super, "super") \
    T(SYM_ApplyType, "apply-type") \
    T(SYM_ScopeCall, "scope-call") \
    T(SYM_Styler, "styler") \
    \
    /* list styles */ \
    T(SYM_SquareList, "square-list") \
    T(SYM_CurlyList, "curly-list") \
    \
    /* compile flags */ \
    T(SYM_DumpDisassembly, "compile-flag-dump-disassembly") \
    T(SYM_DumpModule, "compile-flag-dump-module") \
    T(SYM_DumpFunction, "compile-flag-dump-function") \
    T(SYM_DumpTime, "compile-flag-dump-time") \
    T(SYM_NoDebugInfo, "compile-flag-no-debug-info") \
    T(SYM_O1, "compile-flag-O1") \
    T(SYM_O2, "compile-flag-O2") \
    T(SYM_O3, "compile-flag-O3") \
    \
    /* function flags */ \
    T(SYM_Variadic, "variadic") \
    T(SYM_Pure, "pure") \
    \
    /* compile targets */ \
    T(SYM_TargetVertex, "vertex") \
    T(SYM_TargetFragment, "fragment") \
    T(SYM_TargetGeometry, "geometry") \
    T(SYM_TargetCompute, "compute") \
    \
    /* extern attributes */ \
    T(SYM_Location, "location") \
    T(SYM_Binding, "binding") \
    T(SYM_Storage, "storage") \
    T(SYM_Buffer, "buffer") \
    T(SYM_Coherent, "coherent") \
    T(SYM_Volatile, "volatile") \
    T(SYM_Restrict, "restrict") \
    T(SYM_ReadOnly, "readonly") \
    T(SYM_WriteOnly, "writeonly") \
    \
    /* PE debugger commands */ \
    T(SYM_C, "c") \
    T(SYM_Skip, "skip") \
    T(SYM_Original, "original") \
    T(SYM_Help, "help") \
    \
    /* timer names */ \
    T(TIMER_Compile, "compile()") \
    T(TIMER_CompileSPIRV, "compile_spirv()") \
    T(TIMER_Generate, "generate()") \
    T(TIMER_GenerateSPIRV, "generate_spirv()") \
    T(TIMER_Optimize, "build_and_run_opt_passes()") \
    T(TIMER_MCJIT, "mcjit()") \
    T(TIMER_Lower2CFF, "lower2cff()") \
    T(TIMER_CleanupLabels, "cleanup_labels()") \
    \
    /* ad-hoc builtin names */ \
    T(SYM_ExecuteReturn, "execute-return") \
    T(SYM_RCompare, "rcompare") \
    T(SYM_CountOfForwarder, "countof-forwarder") \
    T(SYM_SliceForwarder, "slice-forwarder") \
    T(SYM_JoinForwarder, "join-forwarder") \
    T(SYM_RCast, "rcast") \
    T(SYM_ROp, "rop") \
    T(SYM_CompareListNext, "compare-list-next") \
    T(SYM_ReturnSafecall, "return-safecall") \
    T(SYM_ReturnError, "return-error") \
    T(SYM_XPCallReturn, "xpcall-return")

enum KnownSymbol {
#define T(sym, name) sym,
#define T0 T
#define T1 T2
#define T2T T2
#define T2(UNAME, LNAME, PFIX, OP) \
    FN_ ## UNAME ## PFIX,
    B_MAP_SYMBOLS()
#undef T
#undef T0
#undef T1
#undef T2
#undef T2T
#define T(NAME) \
SYM_SPIRV_StorageClass ## NAME,
    B_SPIRV_STORAGE_CLASS()
#undef T
#define T(NAME) \
    SYM_SPIRV_BuiltIn ## NAME,
    B_SPIRV_BUILTINS()
#undef T
#define T(NAME) \
    SYM_SPIRV_ExecutionMode ## NAME,
    B_SPIRV_EXECUTION_MODE()
#undef T
#define T(NAME) \
    SYM_SPIRV_Dim ## NAME,
    B_SPIRV_DIM()
#undef T
#define T(NAME) \
    SYM_SPIRV_ImageFormat ## NAME,
    B_SPIRV_IMAGE_FORMAT()
#undef T
#define T(NAME) \
    SYM_SPIRV_ImageOperand ## NAME,
    B_SPIRV_IMAGE_OPERAND()
#undef T
    SYM_Count,
};

enum {
    KEYWORD_FIRST = KW_CatRest,
    KEYWORD_LAST = KW_Yield,

    FUNCTION_FIRST = FN_Alignof,
    FUNCTION_LAST = FN_ZipFill,

    SFXFUNCTION_FIRST = SFXFN_CopyMemory,
    SFXFUNCTION_LAST = SFXFN_TranslateLabelBody,

    OPERATOR_FIRST = OP_NotEq,
    OPERATOR_LAST = OP_InBitNot,

    STYLE_FIRST = Style_None,
    STYLE_LAST = Style_Location,
};

static const char *get_known_symbol_name(KnownSymbol sym) {
    switch(sym) {
#define T(SYM, NAME) case SYM: return #SYM;
#define T0 T
#define T1 T2
#define T2T T2
#define T2(UNAME, LNAME, PFIX, OP) \
    case FN_ ## UNAME ## PFIX: return "FN_" #UNAME #PFIX;
    B_MAP_SYMBOLS()
#undef T
#undef T0
#undef T1
#undef T2
#undef T2T

#define T(NAME) \
case SYM_SPIRV_StorageClass ## NAME: return "SYM_SPIRV_StorageClass" #NAME;
B_SPIRV_STORAGE_CLASS()
#undef T
#define T(NAME) \
    case SYM_SPIRV_BuiltIn ## NAME: return "SYM_SPIRV_BuiltIn" #NAME;
B_SPIRV_BUILTINS()
#undef T
#define T(NAME) \
case SYM_SPIRV_ExecutionMode ## NAME: return "SYM_SPIRV_ExecutionMode" #NAME;
B_SPIRV_EXECUTION_MODE()
#undef T
#define T(NAME) \
    case SYM_SPIRV_Dim ## NAME: return "SYM_SPIRV_Dim" #NAME;
B_SPIRV_DIM()
#undef T
#define T(NAME) \
    case SYM_SPIRV_ImageFormat ## NAME: return "SYM_SPIRV_ImageFormat" #NAME;
B_SPIRV_IMAGE_FORMAT()
#undef T
#define T(NAME) \
    case SYM_SPIRV_ImageOperand ## NAME: return "SYM_SPIRV_ImageOperand" #NAME;
B_SPIRV_IMAGE_OPERAND()
#undef T
case SYM_Count: return "SYM_Count";
    }
}

class NullBuffer : public std::streambuf {
public:
  int overflow(int c) { return c; }
};

class NullStream : public std::ostream {
    public: NullStream() : std::ostream(&m_sb) {}
private:
    NullBuffer m_sb;
};

static NullStream nullout;

//------------------------------------------------------------------------------
// ANSI COLOR FORMATTING
//------------------------------------------------------------------------------

namespace ANSI {
static const char RESET[]           = "\033[0m";
static const char COLOR_BLACK[]     = "\033[30m";
static const char COLOR_RED[]       = "\033[31m";
static const char COLOR_GREEN[]     = "\033[32m";
static const char COLOR_YELLOW[]    = "\033[33m";
static const char COLOR_BLUE[]      = "\033[34m";
static const char COLOR_MAGENTA[]   = "\033[35m";
static const char COLOR_CYAN[]      = "\033[36m";
static const char COLOR_GRAY60[]    = "\033[37m";

static const char COLOR_GRAY30[]    = "\033[30;1m";
static const char COLOR_XRED[]      = "\033[31;1m";
static const char COLOR_XGREEN[]    = "\033[32;1m";
static const char COLOR_XYELLOW[]   = "\033[33;1m";
static const char COLOR_XBLUE[]     = "\033[34;1m";
static const char COLOR_XMAGENTA[]  = "\033[35;1m";
static const char COLOR_XCYAN[]     = "\033[36;1m";
static const char COLOR_WHITE[]     = "\033[37;1m";

static void COLOR_RGB(std::ostream &ost, const char prefix[], int hexcode) {
    ost << prefix
        << ((hexcode >> 16) & 0xff) << ";"
        << ((hexcode >> 8) & 0xff) << ";"
        << (hexcode & 0xff) << "m";
}

static void COLOR_RGB_FG(std::ostream &ost, int hexcode) {
    return COLOR_RGB(ost, "\033[38;2;", hexcode);
}
static void COLOR_RGB_BG(std::ostream &ost, int hexcode) {
    return COLOR_RGB(ost, "\033[48;2;", hexcode);
}


} // namespace ANSI

typedef KnownSymbol Style;

// support 24-bit ANSI colors (ISO-8613-3)
// works on most bash shells as well as windows 10
#define RGBCOLORS
static void ansi_from_style(std::ostream &ost, Style style) {
    switch(style) {
#ifdef RGBCOLORS
    case Style_None: ost << ANSI::RESET; break;
    case Style_Symbol: ANSI::COLOR_RGB_FG(ost, 0xCCCCCC); break;
    case Style_String: ANSI::COLOR_RGB_FG(ost, 0x99CC99); break;
    case Style_Number: ANSI::COLOR_RGB_FG(ost, 0xF99157); break;
    case Style_Keyword: ANSI::COLOR_RGB_FG(ost, 0xCC99CC); break;
    case Style_Function: ANSI::COLOR_RGB_FG(ost, 0x6699CC); break;
    case Style_SfxFunction: ANSI::COLOR_RGB_FG(ost, 0xCC6666); break;
    case Style_Operator: ANSI::COLOR_RGB_FG(ost, 0x66CCCC); break;
    case Style_Instruction: ost << ANSI::COLOR_YELLOW; break;
    case Style_Type: ANSI::COLOR_RGB_FG(ost, 0xFFCC66); break;
    case Style_Comment: ANSI::COLOR_RGB_FG(ost, 0x999999); break;
    case Style_Error: ost << ANSI::COLOR_XRED; break;
    case Style_Warning: ost << ANSI::COLOR_XYELLOW; break;
    case Style_Location: ANSI::COLOR_RGB_FG(ost, 0x999999); break;
#else
    case Style_None: ost << ANSI::RESET; break;
    case Style_Symbol: ost << ANSI::COLOR_GRAY60; break;
    case Style_String: ost << ANSI::COLOR_XMAGENTA; break;
    case Style_Number: ost << ANSI::COLOR_XGREEN; break;
    case Style_Keyword: ost << ANSI::COLOR_XBLUE; break;
    case Style_Function: ost << ANSI::COLOR_GREEN; break;
    case Style_SfxFunction: ost << ANSI::COLOR_RED; break;
    case Style_Operator: ost << ANSI::COLOR_XCYAN; break;
    case Style_Instruction: ost << ANSI::COLOR_YELLOW; break;
    case Style_Type: ost << ANSI::COLOR_XYELLOW; break;
    case Style_Comment: ost << ANSI::COLOR_GRAY30; break;
    case Style_Error: ost << ANSI::COLOR_XRED; break;
    case Style_Warning: ost << ANSI::COLOR_XYELLOW; break;
    case Style_Location: ost << ANSI::COLOR_GRAY30; break;
#endif
    default: break;
    }
}

typedef void (*StreamStyleFunction)(std::ostream &, Style);

static void stream_ansi_style(std::ostream &ost, Style style) {
    ansi_from_style(ost, style);
}

static void stream_plain_style(std::ostream &ost, Style style) {
}

static StreamStyleFunction stream_default_style = stream_plain_style;

struct StyledStream {
    StreamStyleFunction _ssf;
    std::ostream &_ost;

    StyledStream(std::ostream &ost, StreamStyleFunction ssf) :
        _ssf(ssf),
        _ost(ost)
    {}

    StyledStream(std::ostream &ost) :
        _ssf(stream_default_style),
        _ost(ost)
    {}

    StyledStream() :
        _ssf(stream_default_style),
        _ost(std::cerr)
    {}

    static StyledStream plain(std::ostream &ost) {
        return StyledStream(ost, stream_plain_style);
    }

    static StyledStream plain(StyledStream &ost) {
        return StyledStream(ost._ost, stream_plain_style);
    }

    template<typename T>
    StyledStream& operator<<(const T &o) { _ost << o; return *this; }
    template<typename T>
    StyledStream& operator<<(const T *o) { _ost << o; return *this; }
    template<typename T>
    StyledStream& operator<<(T &o) { _ost << o; return *this; }
    StyledStream& operator<<(std::ostream &(*o)(std::ostream&)) {
        _ost << o; return *this; }

    StyledStream& operator<<(Style s) {
        _ssf(_ost, s);
        return *this;
    }

    StyledStream& operator<<(bool s) {
        _ssf(_ost, Style_Keyword);
        _ost << (s?"true":"false");
        _ssf(_ost, Style_None);
        return *this;
    }

    StyledStream& stream_number(int8_t x) {
        _ssf(_ost, Style_Number); _ost << (int)x; _ssf(_ost, Style_None);
        return *this;
    }
    StyledStream& stream_number(uint8_t x) {
        _ssf(_ost, Style_Number); _ost << (int)x; _ssf(_ost, Style_None);
        return *this;
    }

    StyledStream& stream_number(double x) {
        size_t size = stb_snprintf( nullptr, 0, "%g", x );
        char dest[size+1];
        stb_snprintf( dest, size + 1, "%g", x );
        _ssf(_ost, Style_Number); _ost << dest; _ssf(_ost, Style_None);
        return *this;
    }
    StyledStream& stream_number(float x) {
        return stream_number((double)x);
    }

    template<typename T>
    StyledStream& stream_number(T x) {
        _ssf(_ost, Style_Number);
        _ost << x;
        _ssf(_ost, Style_None);
        return *this;
    }
};

#define STREAM_STYLED_NUMBER(T) \
    StyledStream& operator<<(StyledStream& ss, T x) { \
        ss.stream_number(x); \
        return ss; \
    }
STREAM_STYLED_NUMBER(int8_t)
STREAM_STYLED_NUMBER(int16_t)
STREAM_STYLED_NUMBER(int32_t)
STREAM_STYLED_NUMBER(int64_t)
STREAM_STYLED_NUMBER(uint8_t)
STREAM_STYLED_NUMBER(uint16_t)
STREAM_STYLED_NUMBER(uint32_t)
STREAM_STYLED_NUMBER(uint64_t)
STREAM_STYLED_NUMBER(float)
STREAM_STYLED_NUMBER(double)

//------------------------------------------------------------------------------
// NONE
//------------------------------------------------------------------------------

struct Nothing {
};

static Nothing none;

static StyledStream& operator<<(StyledStream& ost, const Nothing &value) {
    ost << Style_Keyword << "none" << Style_None;
    return ost;
}

//------------------------------------------------------------------------------
// STRING
//------------------------------------------------------------------------------

struct String {
    struct Hash {
        std::size_t operator()(const String *s) const {
            return CityHash64(s->data, s->count);
        }
    };

    size_t count;
    char data[1];

    bool operator ==(const String &other) const {
        if (count == other.count) {
            return !memcmp(data, other.data, count);
        }
        return false;
    }

    static String *alloc(size_t count) {
        String *str = (String *)tracked_malloc(
            sizeof(size_t) + sizeof(char) * (count + 1));
        str->count = count;
        return str;
    }

    static const String *from(const char *s, size_t count) {
        String *str = (String *)tracked_malloc(
            sizeof(size_t) + sizeof(char) * (count + 1));
        str->count = count;
        memcpy(str->data, s, sizeof(char) * count);
        str->data[count] = 0;
        return str;
    }

    static const String *from_cstr(const char *s) {
        return from(s, strlen(s));
    }

    static const String *join(const String *a, const String *b) {
        size_t ac = a->count;
        size_t bc = b->count;
        size_t cc = ac + bc;
        String *str = alloc(cc);
        memcpy(str->data, a->data, sizeof(char) * ac);
        memcpy(str->data + ac, b->data, sizeof(char) * bc);
        str->data[cc] = 0;
        return str;
    }

    template<unsigned N>
    static const String *from(const char (&s)[N]) {
        return from(s, N - 1);
    }

    static const String *from_stdstring(const std::string &s) {
        return from(s.c_str(), s.size());
    }

    StyledStream& stream(StyledStream& ost, const char *escape_chars) const {
        auto c = escape_string(nullptr, data, count, escape_chars);
        char deststr[c + 1];
        escape_string(deststr, data, count, escape_chars);
        ost << deststr;
        return ost;
    }

    const String *substr(int64_t i0, int64_t i1) const {
        assert(i1 >= i0);
        return from(data + i0, (size_t)(i1 - i0));
    }
};

static StyledStream& operator<<(StyledStream& ost, const String *s) {
    ost << Style_String << "\"";
    s->stream(ost, "\"");
    ost << "\"" << Style_None;
    return ost;
}

struct StyledString {
    std::stringstream _ss;
    StyledStream out;

    StyledString() :
        out(_ss) {
    }

    StyledString(StreamStyleFunction ssf) :
        out(_ss, ssf) {
    }

    static StyledString plain() {
        return StyledString(stream_plain_style);
    }

    const String *str() const {
        return String::from_stdstring(_ss.str());
    }
};

static const String *vformat( const char *fmt, va_list va ) {
    va_list va2;
    va_copy(va2, va);
    size_t size = stb_vsnprintf( nullptr, 0, fmt, va2 );
    va_end(va2);
    String *str = String::alloc(size);
    stb_vsnprintf( str->data, size + 1, fmt, va );
    return str;
}

static const String *format( const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    const String *result = vformat(fmt, va);
    va_end(va);
    return result;
}

// computes the levenshtein distance between two strings
static size_t distance(const String *_s, const String *_t) {
    const char *s = _s->data;
    const char *t = _t->data;
    const size_t n = _s->count;
    const size_t m = _t->count;
    if (!m) return n;
    if (!n) return m;

    size_t _v0[m + 1];
    size_t _v1[m + 1];

    size_t *v0 = _v0;
    size_t *v1 = _v1;
    for (size_t i = 0; i <= m; ++i) {
        v0[i] = i;
    }

    for (size_t i = 0; i < n; ++i) {
        v1[0] = i + 1;

        for (size_t j = 0; j < m; ++j) {
            size_t cost = (s[i] == t[j])?0:1;
            v1[j + 1] = std::min(v1[j] + 1,
                std::min(v0[j + 1] + 1, v0[j] + cost));
        }

        size_t *tmp = v0;
        v0 = v1;
        v1 = tmp;
    }

    //std::cout << "lev(" << s << ", " << t << ") = " << v0[m] << std::endl;

    return v0[m];
}


//------------------------------------------------------------------------------
// SYMBOL
//------------------------------------------------------------------------------

static const char SYMBOL_ESCAPE_CHARS[] = " []{}()\"";

//------------------------------------------------------------------------------
// SYMBOL TYPE
//------------------------------------------------------------------------------

struct Symbol {
    typedef KnownSymbol EnumT;
    enum { end_value = SYM_Count };

    struct Hash {
        std::size_t operator()(const scopes::Symbol & s) const {
            return s.hash();
        }
    };

protected:
    struct StringKey {
        struct Hash {
            std::size_t operator()(const StringKey &s) const {
                return CityHash64(s.str->data, s.str->count);
            }
        };

        const String *str;

        bool operator ==(const StringKey &rhs) const {
            if (str->count == rhs.str->count) {
                return !memcmp(str->data, rhs.str->data, str->count);
            }
            return false;
        }
    };

    static std::unordered_map<Symbol, const String *, Hash> map_symbol_name;
    static std::unordered_map<StringKey, Symbol, StringKey::Hash> map_name_symbol;
    static uint64_t next_symbol_id;

    static void verify_unmapped(Symbol id, const String *name) {
        auto it = map_name_symbol.find({ name });
        if (it != map_name_symbol.end()) {
            printf("known symbols %s and %s mapped to same string.\n",
               get_known_symbol_name(id.known_value()),
               get_known_symbol_name(it->second.known_value()));
        }
    }

    static void map_symbol(Symbol id, const String *name) {
        map_name_symbol[{ name }] = id;
        map_symbol_name[id] = name;
    }

    static void map_known_symbol(Symbol id, const String *name) {
        verify_unmapped(id, name);
        map_symbol(id, name);
    }

    static Symbol get_symbol(const String *name) {
        auto it = map_name_symbol.find({ name });
        if (it != map_name_symbol.end()) {
            return it->second;
        }
        Symbol id = Symbol::wrap(++next_symbol_id);
        // make copy
        map_symbol(id, String::from(name->data, name->count));
        return id;
    }

    static const String *get_symbol_name(Symbol id) {
        auto it = map_symbol_name.find(id);
        assert (it != map_symbol_name.end());
        return it->second;
    }

    uint64_t _value;

    Symbol(uint64_t tid) :
        _value(tid) {
    }

public:
    static Symbol wrap(uint64_t value) {
        return { value };
    }

    Symbol() :
        _value(SYM_Unnamed) {}

    Symbol(EnumT id) :
        _value(id) {
    }

    template<unsigned N>
    Symbol(const char (&str)[N]) :
        _value(get_symbol(String::from(str))._value) {
    }

    Symbol(const String *str) :
        _value(get_symbol(str)._value) {
    }

    bool is_known() const {
        return _value < end_value;
    }

    EnumT known_value() const {
        assert(is_known());
        return (EnumT)_value;
    }

    // for std::map support
    bool operator < (Symbol b) const {
        return _value < b._value;
    }

    bool operator ==(Symbol b) const {
        return _value == b._value;
    }

    bool operator !=(Symbol b) const {
        return _value != b._value;
    }

    bool operator ==(EnumT b) const {
        return _value == b;
    }

    bool operator !=(EnumT b) const {
        return _value != b;
    }

    std::size_t hash() const {
        return _value;
    }

    uint64_t value() const {
        return _value;
    }

    const String *name() const {
        return get_symbol_name(*this);
    }

    static void _init_symbols() {
    #define T(sym, name) map_known_symbol(sym, String::from(name));
    #define T0 T
    #define T1 T2
    #define T2T T2
    #define T2(UNAME, LNAME, PFIX, OP) \
        map_known_symbol(FN_ ## UNAME ## PFIX, String::from(#LNAME #OP));
        B_MAP_SYMBOLS()
    #undef T
    #undef T0
    #undef T1
    #undef T2
    #undef T2T
    #define T(NAME) \
        map_known_symbol(SYM_SPIRV_StorageClass ## NAME, String::from(#NAME));
        B_SPIRV_STORAGE_CLASS()
    #undef T
    #define T(NAME) \
        map_known_symbol(SYM_SPIRV_BuiltIn ## NAME, String::from("spirv." #NAME));
        B_SPIRV_BUILTINS()
    #undef T
    #define T(NAME) \
        map_known_symbol(SYM_SPIRV_ExecutionMode ## NAME, String::from(#NAME));
        B_SPIRV_EXECUTION_MODE()
    #undef T
    #define T(NAME) \
        map_known_symbol(SYM_SPIRV_Dim ## NAME, String::from(#NAME));
        B_SPIRV_DIM()
    #undef T
    #define T(NAME) \
        map_known_symbol(SYM_SPIRV_ImageFormat ## NAME, String::from(#NAME));
        B_SPIRV_IMAGE_FORMAT()
    #undef T
    #define T(NAME) \
        map_known_symbol(SYM_SPIRV_ImageOperand ## NAME, String::from(#NAME));
        B_SPIRV_IMAGE_OPERAND()
    #undef T
    }

    StyledStream& stream(StyledStream& ost) const {
        auto s = name();
        assert(s);
        ost << Style_Symbol << "'";
        s->stream(ost, SYMBOL_ESCAPE_CHARS);
        ost << Style_None;
        return ost;
    }

};

std::unordered_map<Symbol, const String *, Symbol::Hash> Symbol::map_symbol_name;
std::unordered_map<Symbol::StringKey, Symbol, Symbol::StringKey::Hash> Symbol::map_name_symbol;
uint64_t Symbol::next_symbol_id = SYM_Count;

static StyledStream& operator<<(StyledStream& ost, Symbol sym) {
    return sym.stream(ost);
}

//------------------------------------------------------------------------------
// TIMER
//------------------------------------------------------------------------------

struct Timer {
    static std::unordered_map<Symbol, double, Symbol::Hash> timers;
    Symbol name;
    std::chrono::time_point<std::chrono::high_resolution_clock> start;
    std::chrono::time_point<std::chrono::high_resolution_clock> end;

    Timer(Symbol _name) : name(_name), start(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
        std::chrono::duration<double> diff = std::chrono::high_resolution_clock::now() - start;
        timers[name] = timers[name] + (diff.count() * 1000.0);
    }

    static void print_timers() {
        StyledStream ss;
        for (auto it = timers.begin(); it != timers.end(); ++it) {
            ss << it->first.name()->data << ": " << it->second << "ms" << std::endl;
        }
    }
};

std::unordered_map<Symbol, double, Symbol::Hash> Timer::timers;

static void on_shutdown();

//------------------------------------------------------------------------------
// SOURCE FILE
//------------------------------------------------------------------------------

struct SourceFile {
protected:
    static std::unordered_map<Symbol, SourceFile *, Symbol::Hash> file_cache;

    SourceFile(Symbol _path) :
        path(_path),
        fd(-1),
        length(0),
        ptr(MAP_FAILED),
        _str(nullptr) {
    }

public:
    Symbol path;
    int fd;
    int length;
    void *ptr;
    const String *_str;

    void close() {
        assert(!_str);
        auto it = file_cache.find(path);
        if (it != file_cache.end()) {
            file_cache.erase(it);
        }
        if (ptr != MAP_FAILED) {
            munmap(ptr, length);
            ptr = MAP_FAILED;
            length = 0;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    bool is_open() {
        return fd != -1;
    }

    const char *strptr() {
        assert(is_open() || _str);
        return (const char *)ptr;
    }

    static SourceFile *from_file(Symbol _path) {
        auto it = file_cache.find(_path);
        if (it != file_cache.end()) {
            return it->second;
        }
        SourceFile *file = new SourceFile(_path);
        file->fd = ::open(_path.name()->data, O_RDONLY);
        if (file->fd >= 0) {
            file->length = lseek(file->fd, 0, SEEK_END);
            file->ptr = mmap(nullptr,
                file->length, PROT_READ, MAP_PRIVATE, file->fd, 0);
            if (file->ptr != MAP_FAILED) {
                file_cache[_path] = file;
                return file;
            }
            file->close();
        }
        file->close();
        delete file;
        return nullptr;
    }

    static SourceFile *from_string(Symbol _path, const String *str) {
        SourceFile *file = new SourceFile(_path);
        // loading from string buffer rather than file
        file->ptr = (void *)str->data;
        file->length = str->count;
        file->_str = str;
        return file;
    }

    size_t size() const {
        return length;
    }

    StyledStream &stream(StyledStream &ost, int offset,
        const char *indent = "    ") {
        auto str = strptr();
        if (offset >= length) {
            ost << "<cannot display location in source file (offset "
                << offset << " is beyond length " << length << ")>" << std::endl;
            return ost;
        }
        auto start = offset;
        auto send = offset;
        while (start > 0) {
            if (str[start-1] == '\n') {
                break;
            }
            start = start - 1;
        }
        while (start < offset) {
            if (!isspace(str[start])) {
                break;
            }
            start = start + 1;
        }
        while (send < length) {
            if (str[send] == '\n') {
                break;
            }
            send = send + 1;
        }
        auto linelen = send - start;
        char line[linelen + 1];
        memcpy(line, str + start, linelen);
        line[linelen] = 0;
        ost << indent << line << std::endl;
        auto column = offset - start;
        if (column > 0) {
            ost << indent;
            for (int i = 0; i < column; ++i) {
                ost << " ";
            }
            ost << Style_Operator << "^" << Style_None << std::endl;
        }
        return ost;
    }
};

std::unordered_map<Symbol, SourceFile *, Symbol::Hash> SourceFile::file_cache;

//------------------------------------------------------------------------------
// ANCHOR
//------------------------------------------------------------------------------

struct Anchor {
protected:
    Anchor(SourceFile *_file, int _lineno, int _column, int _offset) :
        file(_file),
        lineno(_lineno),
        column(_column),
        offset(_offset) {}

public:
    SourceFile *file;
    int lineno;
    int column;
    int offset;

    Symbol path() const {
        return file->path;
    }

    static const Anchor *from(
        SourceFile *_file, int _lineno, int _column, int _offset = 0) {
        return new Anchor(_file, _lineno, _column, _offset);
    }

    StyledStream& stream(StyledStream& ost) const {
        ost << Style_Location;
        auto ss = StyledStream::plain(ost);
        ss << path().name()->data << ":" << lineno << ":" << column << ":";
        ost << Style_None;
        return ost;
    }

    StyledStream &stream_source_line(StyledStream &ost, const char *indent = "    ") const {
        file->stream(ost, offset, indent);
        return ost;
    }
};

static StyledStream& operator<<(StyledStream& ost, const Anchor *anchor) {
    return anchor->stream(ost);
}

//------------------------------------------------------------------------------
// TYPE
//------------------------------------------------------------------------------

static void location_error(const String *msg);

#define B_TYPE_KIND() \
    T(TK_Integer, "type-kind-integer") \
    T(TK_Real, "type-kind-real") \
    T(TK_Pointer, "type-kind-pointer") \
    T(TK_Array, "type-kind-array") \
    T(TK_Vector, "type-kind-vector") \
    T(TK_Tuple, "type-kind-tuple") \
    T(TK_Union, "type-kind-union") \
    T(TK_Typename, "type-kind-typename") \
    T(TK_ReturnLabel, "type-kind-return-label") \
    T(TK_Function, "type-kind-function") \
    T(TK_Extern, "type-kind-extern") \
    T(TK_Image, "type-kind-image") \
    T(TK_SampledImage, "type-kind-sampled-image")

enum TypeKind {
#define T(NAME, BNAME) \
    NAME,
    B_TYPE_KIND()
#undef T
};

struct Type;

static bool is_opaque(const Type *T);
static size_t size_of(const Type *T);
static size_t align_of(const Type *T);
static const Type *storage_type(const Type *T);
static StyledStream& operator<<(StyledStream& ost, const Type *type);

#define B_TYPES() \
    /* types */ \
    T(TYPE_Void, "void") \
    T(TYPE_Nothing, "Nothing") \
    T(TYPE_Any, "Any") \
    \
    T(TYPE_Type, "type") \
    T(TYPE_Unknown, "Unknown") \
    T(TYPE_Symbol, "Symbol") \
    T(TYPE_Builtin, "Builtin") \
    \
    T(TYPE_Bool, "bool") \
    \
    T(TYPE_I8, "i8") \
    T(TYPE_I16, "i16") \
    T(TYPE_I32, "i32") \
    T(TYPE_I64, "i64") \
    \
    T(TYPE_U8, "u8") \
    T(TYPE_U16, "u16") \
    T(TYPE_U32, "u32") \
    T(TYPE_U64, "u64") \
    \
    T(TYPE_F16, "f16") \
    T(TYPE_F32, "f32") \
    T(TYPE_F64, "f64") \
    T(TYPE_F80, "f80") \
    \
    T(TYPE_List, "list") \
    T(TYPE_Syntax, "Syntax") \
    T(TYPE_Anchor, "Anchor") \
    T(TYPE_String, "string") \
    \
    T(TYPE_Scope, "Scope") \
    T(TYPE_SourceFile, "SourceFile") \
    T(TYPE_Exception, "Exception") \
    \
    T(TYPE_Parameter, "Parameter") \
    T(TYPE_Label, "Label") \
    T(TYPE_Frame, "Frame") \
    T(TYPE_Closure, "Closure") \
    \
    T(TYPE_USize, "usize") \
    \
    T(TYPE_Sampler, "Sampler") \
    \
    /* supertypes */ \
    T(TYPE_Integer, "integer") \
    T(TYPE_Real, "real") \
    T(TYPE_Pointer, "pointer") \
    T(TYPE_Array, "array") \
    T(TYPE_Vector, "vector") \
    T(TYPE_Tuple, "tuple") \
    T(TYPE_Union, "union") \
    T(TYPE_Typename, "typename") \
    T(TYPE_ReturnLabel, "ReturnLabel") \
    T(TYPE_Function, "function") \
    T(TYPE_Constant, "constant") \
    T(TYPE_Extern, "extern") \
    T(TYPE_Image, "Image") \
    T(TYPE_SampledImage, "SampledImage") \
    T(TYPE_CStruct, "CStruct") \
    T(TYPE_CUnion, "CUnion") \
    T(TYPE_CEnum, "CEnum")

#define T(TYPE, TYPENAME) \
    static const Type *TYPE = nullptr;
B_TYPES()
#undef T

//------------------------------------------------------------------------------
// BUILTIN
//------------------------------------------------------------------------------

struct Builtin {
    typedef KnownSymbol EnumT;
protected:
    Symbol _name;

public:
    Builtin(EnumT name) :
        _name(name) {
    }

    EnumT value() const {
        return _name.known_value();
    }

    bool operator < (Builtin b) const { return _name < b._name; }
    bool operator ==(Builtin b) const { return _name == b._name; }
    bool operator !=(Builtin b) const { return _name != b._name; }
    bool operator ==(EnumT b) const { return _name == b; }
    bool operator !=(EnumT b) const { return _name != b; }
    std::size_t hash() const { return _name.hash(); }
    Symbol name() const { return _name; }

    StyledStream& stream(StyledStream& ost) const {
        ost << Style_Function; name().name()->stream(ost, ""); ost << Style_None;
        return ost;
    }
};

static StyledStream& operator<<(StyledStream& ost, Builtin builtin) {
    return builtin.stream(ost);
}

//------------------------------------------------------------------------------
// ANY
//------------------------------------------------------------------------------

struct Syntax;
struct List;
struct Label;
struct Parameter;
struct Scope;
struct Exception;
struct Frame;
struct Closure;

struct Any {
    struct Hash {
        std::size_t operator()(const Any & s) const {
            return s.hash();
        }
    };

    const Type *type;
    union {
        char content[8];
        bool i1;
        int8_t i8;
        int16_t i16;
        int32_t i32;
        int64_t i64;
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        size_t sizeval;
        float f32;
        double f64;
        const Type *typeref;
        const String *string;
        Symbol symbol;
        const Syntax *syntax;
        const Anchor *anchor;
        const List *list;
        Label *label;
        Parameter *parameter;
        Builtin builtin;
        Scope *scope;
        Any *ref;
        void *pointer;
        const Exception *exception;
        Frame *frame;
        const Closure *closure;
    };

    Any(Nothing x) : type(TYPE_Nothing), u64(0) {}
    Any(const Type *x) : type(TYPE_Type), typeref(x) {}
    Any(bool x) : type(TYPE_Bool), u64(0) { i1 = x; }
    Any(int8_t x) : type(TYPE_I8), u64(0) { i8 = x; }
    Any(int16_t x) : type(TYPE_I16), u64(0) { i16 = x; }
    Any(int32_t x) : type(TYPE_I32), u64(0) { i32 = x; }
    Any(int64_t x) : type(TYPE_I64), i64(x) {}
    Any(uint8_t x) : type(TYPE_U8), u64(0) { u8 = x; }
    Any(uint16_t x) : type(TYPE_U16), u64(0) { u16 = x; }
    Any(uint32_t x) : type(TYPE_U32), u64(0) { u32 = x; }
    Any(uint64_t x) : type(TYPE_U64), u64(x) {}
#ifdef SCOPES_MACOS
    Any(unsigned long x) : type(TYPE_U64), u64(x) {}
#endif
    Any(float x) : type(TYPE_F32), u64(0) { f32 = x; }
    Any(double x) : type(TYPE_F64), f64(x) {}
    Any(const String *x) : type(TYPE_String), string(x) {}
    Any(Symbol x) : type(TYPE_Symbol), symbol(x) {}
    Any(const Syntax *x) : type(TYPE_Syntax), syntax(x) {}
    Any(const Anchor *x) : type(TYPE_Anchor), anchor(x) {}
    Any(const List *x) : type(TYPE_List), list(x) {}
    Any(const Exception *x) : type(TYPE_Exception), exception(x) {}
    Any(Label *x) : type(TYPE_Label), label(x) {}
    Any(Parameter *x) : type(TYPE_Parameter), parameter(x) {}
    Any(Builtin x) : type(TYPE_Builtin), builtin(x) {}
    Any(Scope *x) : type(TYPE_Scope), scope(x) {}
    Any(Frame *x) : type(TYPE_Frame), frame(x) {}
    Any(const Closure *x) : type(TYPE_Closure), closure(x) {}
    template<unsigned N>
    Any(const char (&str)[N]) : type(TYPE_String), string(String::from(str)) {}
    // a catch-all for unsupported types
    template<typename T>
    Any(const T &x);

    Any toref() {
        return from_pointer(TYPE_Any, new Any(*this));
    }

    static Any from_opaque(const Type *type) {
        Any val = none;
        val.type = type;
        return val;
    }

    static Any from_pointer(const Type *type, void *ptr) {
        Any val = none;
        val.type = type;
        val.pointer = ptr;
        return val;
    }

    void verify(const Type *T) const;
    void verify_indirect(const Type *T) const;
    const Type *indirect_type() const;
    bool is_const() const;

    operator const Type *() const { verify(TYPE_Type); return typeref; }
    operator const List *() const { verify(TYPE_List); return list; }
    operator const Syntax *() const { verify(TYPE_Syntax); return syntax; }
    operator const Anchor *() const { verify(TYPE_Anchor); return anchor; }
    operator const String *() const { verify(TYPE_String); return string; }
    operator const Exception *() const { verify(TYPE_Exception); return exception; }
    operator Label *() const { verify(TYPE_Label); return label; }
    operator Scope *() const { verify(TYPE_Scope); return scope; }
    operator Parameter *() const { verify(TYPE_Parameter); return parameter; }
    operator const Closure *() const { verify(TYPE_Closure); return closure; }
    operator Frame *() const { verify(TYPE_Frame); return frame; }

    struct AnyStreamer {
        StyledStream& ost;
        const Type *type;
        bool annotate_type;
        AnyStreamer(StyledStream& _ost, const Type *_type, bool _annotate_type) :
            ost(_ost), type(_type), annotate_type(_annotate_type) {}
        void stream_type_suffix() const {
            if (annotate_type) {
                ost << Style_Operator << ":" << Style_None;
                ost << type;
            }
        }
        template<typename T>
        void naked(const T &x) const {
            ost << x;
        }
        template<typename T>
        void typed(const T &x) const {
            ost << x;
            stream_type_suffix();
        }
    };

    StyledStream& stream(StyledStream& ost, bool annotate_type = true) const;

    bool operator ==(const Any &other) const;

    bool operator !=(const Any &other) const {
        return !(*this == other);
    }

    size_t hash() const;
};

static StyledStream& operator<<(StyledStream& ost, Any value) {
    return value.stream(ost);
}

static bool is_unknown(const Any &value) {
    return value.type == TYPE_Unknown;
}

static bool is_typed(const Any &value) {
    return (value.type != TYPE_Unknown) || (value.typeref != TYPE_Unknown);
}

static Any unknown_of(const Type *T) {
    Any result(T);
    result.type = TYPE_Unknown;
    return result;
}

static Any untyped() {
    return unknown_of(TYPE_Unknown);
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

static const Type *superof(const Type *T);

struct Type {
    typedef std::unordered_map<Symbol, Any, Symbol::Hash> Map;

    TypeKind kind() const { return _kind; } // for this codebase

    Type(TypeKind kind) : _kind(kind), _name(Symbol(SYM_Unnamed).name()) {}
    Type(const Type &other) = delete;

    const String *name() const {
        return _name;
    }

    StyledStream& stream(StyledStream& ost) const {
        ost << Style_Type;
        ost << name()->data;
        ost << Style_None;
        return ost;
    }

    void bind(Symbol name, const Any &value) {
        auto ret = symbols.insert({ name, value });
        if (!ret.second) {
            ret.first->second = value;
        }
    }

    void del(Symbol name) {
        auto it = symbols.find(name);
        if (it != symbols.end()) {
            symbols.erase(it);
        }
    }

    bool lookup(Symbol name, Any &dest) const {
        const Type *self = this;
        do {
            auto it = self->symbols.find(name);
            if (it != self->symbols.end()) {
                dest = it->second;
                return true;
            }
            if (self == TYPE_Typename)
                break;
            self = superof(self);
        } while (self);
        return false;
    }

    bool lookup_local(Symbol name, Any &dest) const {
        auto it = symbols.find(name);
        if (it != symbols.end()) {
            dest = it->second;
            return true;
        }
        return false;
    }

    bool lookup_call_handler(Any &dest) const {
        return lookup(SYM_CallHandler, dest);
    }

    const Map &get_symbols() const {
        return symbols;
    }

private:
    const TypeKind _kind;

protected:
    const String *_name;

    Map symbols;
};

static StyledStream& operator<<(StyledStream& ost, const Type *type) {
    if (!type) {
        ost << Style_Error;
        ost << "<null type>";
        ost << Style_None;
        return ost;
    } else {
        return type->stream(ost);
    }
}

static Any wrap_pointer(const Type *type, void *ptr);

//------------------------------------------------------------------------------
// TYPE CHECK PREDICATES
//------------------------------------------------------------------------------

static void verify(const Type *typea, const Type *typeb) {
    if (typea != typeb) {
        StyledString ss;
        ss.out << "type " << typea << " expected, got " << typeb;
        location_error(ss.str());
    }
}

static void verify_integer(const Type *type) {
    if (type->kind() != TK_Integer) {
        StyledString ss;
        ss.out << "integer type expected, got " << type;
        location_error(ss.str());
    }
}

static void verify_real(const Type *type) {
    if (type->kind() != TK_Real) {
        StyledString ss;
        ss.out << "real type expected, got " << type;
        location_error(ss.str());
    }
}

static void verify_range(size_t idx, size_t count) {
    if (idx >= count) {
        StyledString ss;
        ss.out << "index out of range (" << idx
            << " >= " << count << ")";
        location_error(ss.str());
    }
}

void Any::verify(const Type *T) const {
    scopes::verify(T, type);
}

//------------------------------------------------------------------------------
// TYPE FACTORIES
//------------------------------------------------------------------------------

template<typename T>
struct TypeFactory {
    struct TypeArgs {
        std::vector<Any> args;

        TypeArgs() {}
        TypeArgs(const std::vector<Any> &_args) : args(_args) {}

        bool operator==(const TypeArgs &other) const {
            if (args.size() != other.args.size()) return false;
            for (size_t i = 0; i < args.size(); ++i) {
                auto &&a = args[i];
                auto &&b = other.args[i];
                if (a != b)
                    return false;
            }
            return true;
        }

        struct Hash {
            std::size_t operator()(const TypeArgs& s) const {
                std::size_t h = 0;
                for (auto &&arg : s.args) {
                    h = HashLen16(h, arg.hash());
                }
                return h;
            }
        };
    };

    typedef std::unordered_map<TypeArgs, T *, typename TypeArgs::Hash> ArgMap;

    ArgMap map;

    const Type *insert(const std::vector<Any> &args) {
        TypeArgs ta(args);
        typename ArgMap::iterator it = map.find(ta);
        if (it == map.end()) {
            T *t = new T(args);
            map.insert({ta, t});
            return t;
        } else {
            return it->second;
        }
    }

    template <typename... Args>
    const Type *insert(Args... args) {
        TypeArgs ta({ args... });
        typename ArgMap::iterator it = map.find(ta);
        if (it == map.end()) {
            T *t = new T(args...);
            map.insert({ta, t});
            return t;
        } else {
            return it->second;
        }
    }
};

//------------------------------------------------------------------------------
// INTEGER TYPE
//------------------------------------------------------------------------------

struct IntegerType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_Integer;
    }

    IntegerType(size_t _width, bool _issigned)
        : Type(TK_Integer), width(_width), issigned(_issigned) {
        std::stringstream ss;
        if ((_width == 1) && !_issigned) {
            ss << "bool";
        } else {
            if (issigned) {
                ss << "i";
            } else {
                ss << "u";
            }
            ss << width;
        }
        _name = String::from_stdstring(ss.str());
    }

    size_t width;
    bool issigned;
};

const Type *_Integer(size_t _width, bool _issigned) {
    return new IntegerType(_width, _issigned);
}
static auto Integer = memoize(_Integer);

//------------------------------------------------------------------------------
// INTEGER TYPE
//------------------------------------------------------------------------------

struct RealType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_Real;
    }

    RealType(size_t _width)
        : Type(TK_Real), width(_width) {
        std::stringstream ss;
        ss << "f" << width;
        _name = String::from_stdstring(ss.str());
    }

    size_t width;
};

const Type *_Real(size_t _width) {
    return new RealType(_width);
}
static auto Real = memoize(_Real);

//------------------------------------------------------------------------------
// POINTER TYPE
//------------------------------------------------------------------------------

enum PointerTypeFlags {
    PTF_NonWritable = (1 << 1),
    PTF_NonReadable = (1 << 2),
};

struct PointerType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_Pointer;
    }

    PointerType(const Type *_element_type,
        uint64_t _flags, Symbol _storage_class)
        : Type(TK_Pointer),
            element_type(_element_type),
            flags(_flags),
            storage_class(_storage_class) {
        std::stringstream ss;
        ss << element_type->name()->data;
        if (is_writable() && is_readable()) {
            ss << "*";
        } else if (is_readable()) {
            ss << "(*)";
        } else {
            ss << "*!";
        }
        if (storage_class != SYM_Unnamed) {
            ss << "[" << storage_class.name()->data << "]";
        }
        _name = String::from_stdstring(ss.str());
    }

    void *getelementptr(void *src, size_t i) const {
        size_t stride = size_of(element_type);
        return (void *)((char *)src + stride * i);
    }

    Any unpack(void *src) const {
        return wrap_pointer(element_type, src);
    }
    static size_t size() {
        return sizeof(uint64_t);
    }

    bool is_readable() const {
        return !(flags & PTF_NonReadable);
    }

    bool is_writable() const {
        return !(flags & PTF_NonWritable);
    }

    const Type *element_type;
    uint64_t flags;
    Symbol storage_class;
};

static const Type *Pointer(const Type *element_type, uint64_t flags,
    Symbol storage_class) {
    static TypeFactory<PointerType> pointers;
    assert(element_type->kind() != TK_ReturnLabel);
    return pointers.insert(element_type, flags, storage_class);
}

static const Type *NativeROPointer(const Type *element_type) {
    return Pointer(element_type, PTF_NonWritable, SYM_Unnamed);
}

static const Type *NativePointer(const Type *element_type) {
    return Pointer(element_type, 0, SYM_Unnamed);
}

static const Type *LocalROPointer(const Type *element_type) {
    return Pointer(element_type, PTF_NonWritable, SYM_SPIRV_StorageClassFunction);
}

static const Type *LocalPointer(const Type *element_type) {
    return Pointer(element_type, 0, SYM_SPIRV_StorageClassFunction);
}

static const Type *StaticPointer(const Type *element_type) {
    return Pointer(element_type, 0, SYM_SPIRV_StorageClassPrivate);
}

//------------------------------------------------------------------------------
// ARRAY TYPE
//------------------------------------------------------------------------------

struct StorageType : Type {

    StorageType(TypeKind kind) : Type(kind) {}

    size_t size;
    size_t align;
};

struct SizedStorageType : StorageType {

    SizedStorageType(TypeKind kind, const Type *_element_type, size_t _count)
        : StorageType(kind), element_type(_element_type), count(_count) {
        stride = size_of(element_type);
        size = stride * count;
        align = align_of(element_type);
    }

    void *getelementptr(void *src, size_t i) const {
        verify_range(i, count);
        return (void *)((char *)src + stride * i);
    }

    Any unpack(void *src, size_t i) const {
        return wrap_pointer(type_at_index(i), getelementptr(src, i));
    }

    const Type *type_at_index(size_t i) const {
        verify_range(i, count);
        return element_type;
    }

    const Type *element_type;
    size_t count;
    size_t stride;
};

struct ArrayType : SizedStorageType {
    static bool classof(const Type *T) {
        return T->kind() == TK_Array;
    }

    ArrayType(const Type *_element_type, size_t _count)
        : SizedStorageType(TK_Array, _element_type, _count) {
        std::stringstream ss;
        ss << "[" << element_type->name()->data << " x " << count << "]";
        _name = String::from_stdstring(ss.str());
    }
};

static const Type *Array(const Type *element_type, size_t count) {
    static TypeFactory<ArrayType> arrays;
    return arrays.insert(element_type, count);
}

//------------------------------------------------------------------------------
// VECTOR TYPE
//------------------------------------------------------------------------------

struct VectorType : SizedStorageType {
    static bool classof(const Type *T) {
        return T->kind() == TK_Vector;
    }

    VectorType(const Type *_element_type, size_t _count)
        : SizedStorageType(TK_Vector, _element_type, _count) {
        std::stringstream ss;
        ss << "<" << element_type->name()->data << " x " << count << ">";
        _name = String::from_stdstring(ss.str());
    }
};

static const Type *Vector(const Type *element_type, size_t count) {
    static TypeFactory<VectorType> vectors;
    return vectors.insert(element_type, count);
}

static void verify_integer_vector(const Type *type) {
    if (type->kind() == TK_Vector) {
        type = cast<VectorType>(type)->element_type;
    }
    if (type->kind() != TK_Integer) {
        StyledString ss;
        ss.out << "integer scalar or vector type expected, got " << type;
        location_error(ss.str());
    }
}

static void verify_real_vector(const Type *type) {
    if (type->kind() == TK_Vector) {
        type = cast<VectorType>(type)->element_type;
    }
    if (type->kind() != TK_Real) {
        StyledString ss;
        ss.out << "real scalar or vector type expected, got " << type;
        location_error(ss.str());
    }
}

static void verify_bool_vector(const Type *type) {
    if (type->kind() == TK_Vector) {
        type = cast<VectorType>(type)->element_type;
    }
    if (type != TYPE_Bool) {
        StyledString ss;
        ss.out << "bool value or vector type expected, got " << type;
        location_error(ss.str());
    }
}

static void verify_real_vector(const Type *type, size_t fixedsz) {
    if (type->kind() == TK_Vector) {
        auto T = cast<VectorType>(type);
        if (T->count == fixedsz)
            return;
    }
    StyledString ss;
    ss.out << "vector type of size " << fixedsz << " expected, got " << type;
    location_error(ss.str());
}

static void verify_vector_sizes(const Type *type1, const Type *type2) {
    bool type1v = (type1->kind() == TK_Vector);
    bool type2v = (type2->kind() == TK_Vector);
    if (type1v == type2v) {
        if (type1v) {
            if (cast<VectorType>(type1)->count
                    == cast<VectorType>(type2)->count) {
                return;
            }
        } else {
            return;
        }
    }
    StyledString ss;
    ss.out << "operands must be vector of same size or scalar";
    location_error(ss.str());
}

//------------------------------------------------------------------------------
// TUPLE TYPE
//------------------------------------------------------------------------------

struct Argument {
    Symbol key;
    Any value;

    Argument() : key(SYM_Unnamed), value(none) {}
    Argument(Any _value) : key(SYM_Unnamed), value(_value) {}
    Argument(Symbol _key, Any _value) : key(_key), value(_value) {}
    template<typename T>
    Argument(const T &x) : key(SYM_Unnamed), value(x) {}

    bool is_keyed() const {
        return key != SYM_Unnamed;
    }

    bool operator ==(const Argument &other) const {
        return (key == other.key) && (value == other.value);
    }

    bool operator !=(const Argument &other) const {
        return (key != other.key) || (value != other.value);
    }

    uint64_t hash() const {
        return HashLen16(std::hash<uint64_t>{}(key.value()), value.hash());
    }
};

static StyledStream& operator<<(StyledStream& ost, Argument value) {
    if (value.key != SYM_Unnamed) {
        ost << value.key << Style_Operator << "=" << Style_None;
    }
    ost << value.value;
    return ost;
}

typedef std::vector<Argument> Args;

struct TupleType : StorageType {
    static bool classof(const Type *T) {
        return T->kind() == TK_Tuple;
    }

    TupleType(const Args &_values, bool _packed, size_t _alignment)
        : StorageType(TK_Tuple), values(_values), packed(_packed) {
        StyledString ss = StyledString::plain();
        if (_alignment) {
            ss.out << "[align:" << _alignment << "]";
        }
        if (packed) {
            ss.out << "<";
        }
        ss.out << "{";
        size_t tcount = values.size();
        types.reserve(tcount);
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                ss.out << " ";
            }
            if (values[i].key != SYM_Unnamed) {
                ss.out << values[i].key.name()->data << "=";
            }
            if (is_unknown(values[i].value)) {
                ss.out << values[i].value.typeref->name()->data;
                types.push_back(values[i].value.typeref);
            } else {
                ss.out << "!" << values[i].value.type;
                types.push_back(values[i].value.type);
            }
        }
        ss.out << "}";
        if (packed) {
            ss.out << ">";
        }
        _name = ss.str();

        offsets.resize(types.size());
        size_t sz = 0;
        if (packed) {
            for (size_t i = 0; i < types.size(); ++i) {
                const Type *ET = types[i];
                offsets[i] = sz;
                sz += size_of(ET);
            }
            size = sz;
            align = 1;
        } else {
            size_t al = 1;
            for (size_t i = 0; i < types.size(); ++i) {
                const Type *ET = types[i];
                size_t etal = align_of(ET);
                sz = ::align(sz, etal);
                offsets[i] = sz;
                al = std::max(al, etal);
                sz += size_of(ET);
            }
            size = ::align(sz, al);
            align = al;
        }
        if (_alignment) {
            align = _alignment;
            size = ::align(sz, align);
        }
    }

    void *getelementptr(void *src, size_t i) const {
        verify_range(i, offsets.size());
        return (void *)((char *)src + offsets[i]);
    }

    Any unpack(void *src, size_t i) const {
        return wrap_pointer(type_at_index(i), getelementptr(src, i));
    }

    const Type *type_at_index(size_t i) const {
        verify_range(i, types.size());
        return types[i];
    }

    size_t field_index(Symbol name) const {
        for (size_t i = 0; i < values.size(); ++i) {
            if (name == values[i].key)
                return i;
        }
        return (size_t)-1;
    }

    Symbol field_name(size_t i) const {
        verify_range(i, values.size());
        return values[i].key;
    }

    Args values;
    bool packed;
    std::vector<const Type *> types;
    std::vector<size_t> offsets;
};

static const Type *MixedTuple(const Args &values,
    bool packed = false, size_t alignment = 0) {
    struct TypeArgs {
        Args args;
        bool packed;
        size_t alignment;

        TypeArgs() {}
        TypeArgs(const Args &_args, bool _packed, size_t _alignment)
            : args(_args), packed(_packed), alignment(_alignment) {}

        bool operator==(const TypeArgs &other) const {
            if (packed != other.packed) return false;
            if (alignment != other.alignment) return false;
            if (args.size() != other.args.size()) return false;
            for (size_t i = 0; i < args.size(); ++i) {
                auto &&a = args[i];
                auto &&b = other.args[i];
                if (a != b)
                    return false;
            }
            return true;
        }

        struct Hash {
            std::size_t operator()(const TypeArgs& s) const {
                std::size_t h = std::hash<bool>{}(s.packed);
                h = HashLen16(h, std::hash<size_t>{}(s.alignment));
                for (auto &&arg : s.args) {
                    h = HashLen16(h, arg.hash());
                }
                return h;
            }
        };
    };

    typedef std::unordered_map<TypeArgs, TupleType *, typename TypeArgs::Hash> ArgMap;

    static ArgMap map;

#ifdef SCOPES_DEBUG
    for (size_t i = 0; i < values.size(); ++i) {
        assert(values[i].value.is_const());
    }
#endif

    TypeArgs ta(values, packed, alignment);
    typename ArgMap::iterator it = map.find(ta);
    if (it == map.end()) {
        TupleType *t = new TupleType(values, packed, alignment);
        map.insert({ta, t});
        return t;
    } else {
        return it->second;
    }
}

static const Type *Tuple(const std::vector<const Type *> &types,
    bool packed = false, size_t alignment = 0) {
    Args args;
    args.reserve(types.size());
    for (size_t i = 0; i < types.size(); ++i) {
        args.push_back(unknown_of(types[i]));
    }
    return MixedTuple(args, packed, alignment);
}

//------------------------------------------------------------------------------
// UNION TYPE
//------------------------------------------------------------------------------

struct UnionType : StorageType {
    static bool classof(const Type *T) {
        return T->kind() == TK_Union;
    }

    UnionType(const Args &_values)
        : StorageType(TK_Union), values(_values) {
        StyledString ss = StyledString::plain();
        ss.out << "{";
        size_t tcount = values.size();
        types.reserve(tcount);
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                ss.out << " | ";
            }
            if (values[i].key != SYM_Unnamed) {
                ss.out << values[i].key.name()->data << "=";
            }
            if (is_unknown(values[i].value)) {
                ss.out << values[i].value.typeref->name()->data;
                types.push_back(values[i].value.typeref);
            } else {
                ss.out << "!" << values[i].value.type;
                types.push_back(values[i].value.type);
            }
        }
        ss.out << "}";
        _name = ss.str();

        size_t sz = 0;
        size_t al = 1;
        largest_field = 0;
        for (size_t i = 0; i < types.size(); ++i) {
            const Type *ET = types[i];
            auto newsz = size_of(ET);
            if (newsz > sz) {
                largest_field = i;
                sz = newsz;
            }
            al = std::max(al, align_of(ET));
        }
        size = ::align(sz, al);
        align = al;
        tuple_type = Tuple({types[largest_field]});
    }

    Any unpack(void *src, size_t i) const {
        return wrap_pointer(type_at_index(i), src);
    }

    const Type *type_at_index(size_t i) const {
        verify_range(i, types.size());
        return types[i];
    }

    size_t field_index(Symbol name) const {
        for (size_t i = 0; i < values.size(); ++i) {
            if (name == values[i].key)
                return i;
        }
        return (size_t)-1;
    }

    Symbol field_name(size_t i) const {
        verify_range(i, values.size());
        return values[i].key;
    }

    Args values;
    std::vector<const Type *> types;
    size_t largest_field;
    const Type *tuple_type;
};

static const Type *MixedUnion(const Args &values) {
    struct TypeArgs {
        Args args;

        TypeArgs() {}
        TypeArgs(const Args &_args)
            : args(_args) {}

        bool operator==(const TypeArgs &other) const {
            if (args.size() != other.args.size()) return false;
            for (size_t i = 0; i < args.size(); ++i) {
                auto &&a = args[i];
                auto &&b = other.args[i];
                if (a != b)
                    return false;
            }
            return true;
        }

        struct Hash {
            std::size_t operator()(const TypeArgs& s) const {
                std::size_t h = 0;
                for (auto &&arg : s.args) {
                    h = HashLen16(h, arg.hash());
                }
                return h;
            }
        };
    };

    typedef std::unordered_map<TypeArgs, UnionType *, typename TypeArgs::Hash> ArgMap;

    static ArgMap map;

    TypeArgs ta(values);
    typename ArgMap::iterator it = map.find(ta);
    if (it == map.end()) {
        UnionType *t = new UnionType(values);
        map.insert({ta, t});
        return t;
    } else {
        return it->second;
    }
}

static const Type *Union(const std::vector<const Type *> &types) {
    Args args;
    args.reserve(types.size());
    for (size_t i = 0; i < types.size(); ++i) {
        args.push_back(unknown_of(types[i]));
    }
    return MixedUnion(args);
}

//------------------------------------------------------------------------------
// EXTERN TYPE
//------------------------------------------------------------------------------

enum ExternFlags {
    // if storage class is 'Uniform, the value is a SSBO
    EF_BufferBlock = (1 << 0),
    EF_NonWritable = (1 << 1),
    EF_NonReadable = (1 << 2),
    EF_Volatile = (1 << 3),
    EF_Coherent = (1 << 4),
    EF_Restrict = (1 << 5),
    // if storage class is 'Uniform, the value is a UBO
    EF_Block = (1 << 6),
};

struct ExternType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_Extern;
    }

    ExternType(const Type *_type,
        size_t _flags, Symbol _storage_class, int _location, int _binding) :
        Type(TK_Extern),
        type(_type),
        flags(_flags),
        storage_class(_storage_class),
        location(_location),
        binding(_binding) {
        std::stringstream ss;
        ss << "<extern " <<  _type->name()->data;
        if (storage_class != SYM_Unnamed)
            ss << " storage=" << storage_class.name()->data;
        if (location >= 0)
            ss << " location=" << location;
        if (binding >= 0)
            ss << " binding=" << binding;
        ss << ">";
        _name = String::from_stdstring(ss.str());
        if ((_storage_class == SYM_SPIRV_StorageClassUniform)
            && !(flags & EF_BufferBlock)) {
            flags |= EF_Block;
        }
        size_t ptrflags = 0;
        if (flags & EF_NonWritable)
            ptrflags |= PTF_NonWritable;
        else if (flags & EF_NonReadable)
            ptrflags |= PTF_NonReadable;
        pointer_type = Pointer(type, ptrflags, storage_class);
    }

    const Type *type;
    size_t flags;
    Symbol storage_class;
    int location;
    int binding;
    const Type *pointer_type;
};

static const Type *Extern(const Type *type,
    size_t flags = 0,
    Symbol storage_class = SYM_Unnamed,
    int location = -1,
    int binding = -1) {
    static TypeFactory<ExternType> externs;
    return externs.insert(type, flags, storage_class, location, binding);
}

//------------------------------------------------------------------------------
// TYPED LABEL TYPE
//------------------------------------------------------------------------------

static void stream_args(StyledStream &ss, const Args &args, size_t start = 1) {
    for (size_t i = start; i < args.size(); ++i) {
        ss << " ";
        if (is_unknown(args[i].value)) {
            ss << "<unknown>:" << args[i].value.typeref;
        } else {
            ss << args[i].value;
        }
    }
    ss << std::endl;
}

static Argument first(const Args &values) {
    return values.empty()?Argument():values.front();
}

struct ReturnLabelType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_ReturnLabel;
    }

    ReturnLabelType(const Args &_values)
        : Type(TK_ReturnLabel) {
        values = _values;

        StyledString ss = StyledString::plain();
        ss.out << "λ(";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                ss.out << " ";
            }
            if (values[i].key != SYM_Unnamed) {
                ss.out << values[i].key.name()->data << "=";
            }
            if (is_unknown(values[i].value)) {
                ss.out << values[i].value.typeref->name()->data;
            } else {
                ss.out << "!" << values[i].value.type;
            }
        }
        ss.out << ")";
        _name = ss.str();

        {
            std::vector<const Type *> rettypes;
            // prune constants
            for (size_t i = 0; i < values.size(); ++i) {
                if (is_unknown(values[i].value)) {
                    rettypes.push_back(values[i].value.typeref);
                }
            }

            if (rettypes.size() == 1) {
                return_type = rettypes[0];
                has_mrv = false;
            } else if (!rettypes.empty()) {
                return_type = Tuple(rettypes);
                has_mrv = true;
            } else {
                return_type = TYPE_Void;
                has_mrv = false;
            }
        }
    }

    bool has_multiple_return_values() const {
        return has_mrv;
    }

    Args values;
    const Type *return_type;
    bool has_mrv;
};

static const Type *ReturnLabel(const Args &values) {
    struct TypeArgs {
        Args args;

        TypeArgs() {}
        TypeArgs(const Args &_args) : args(_args) {}

        bool operator==(const TypeArgs &other) const {
            if (args.size() != other.args.size()) return false;
            for (size_t i = 0; i < args.size(); ++i) {
                auto &&a = args[i];
                auto &&b = other.args[i];
                if (a != b)
                    return false;
            }
            return true;
        }

        struct Hash {
            std::size_t operator()(const TypeArgs& s) const {
                std::size_t h = 0;
                for (auto &&arg : s.args) {
                    h = HashLen16(h, arg.hash());
                }
                return h;
            }
        };
    };

    typedef std::unordered_map<TypeArgs, ReturnLabelType *, typename TypeArgs::Hash> ArgMap;

    static ArgMap map;

#ifdef SCOPES_DEBUG
    for (size_t i = 0; i < values.size(); ++i) {
        assert(values[i].value.is_const());
    }
#endif

    TypeArgs ta(values);
    typename ArgMap::iterator it = map.find(ta);
    if (it == map.end()) {
        ReturnLabelType *t = new ReturnLabelType(values);
        map.insert({ta, t});
        return t;
    } else {
        return it->second;
    }
}

//------------------------------------------------------------------------------
// FUNCTION TYPE
//------------------------------------------------------------------------------

enum {
    // takes variable number of arguments
    FF_Variadic = (1 << 0),
    // can be evaluated at compile time
    FF_Pure = (1 << 1),
    // never returns
    FF_Divergent = (1 << 2),
};

struct FunctionType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_Function;
    }

    FunctionType(
        const Type *_return_type, const Type *_argument_types, uint32_t _flags) :
        Type(TK_Function),
        return_type(_return_type),
        argument_types(llvm::cast<TupleType>(_argument_types)->types),
        flags(_flags) {

        assert(!(flags & FF_Divergent) || argument_types.empty());

        std::stringstream ss;
        if (divergent()) {
            ss << "?<-";
        } else {
            ss <<  return_type->name()->data;
            if (pure()) {
                ss << "<~";
            } else {
                ss << "<-";
            }
        }
        ss << "(";
        for (size_t i = 0; i < argument_types.size(); ++i) {
            if (i > 0) {
                ss << " ";
            }
            ss << argument_types[i]->name()->data;
        }
        if (vararg()) {
            ss << " ...";
        }
        ss << ")";
        _name = String::from_stdstring(ss.str());
    }

    bool vararg() const {
        return flags & FF_Variadic;
    }
    bool pure() const {
        return flags & FF_Pure;
    }
    bool divergent() const {
        return flags & FF_Divergent;
    }

    const Type *type_at_index(size_t i) const {
        verify_range(i, argument_types.size() + 1);
        if (i == 0)
            return cast<ReturnLabelType>(return_type)->return_type;
        else
            return argument_types[i - 1];
    }

    const Type *return_type;
    std::vector<const Type *> argument_types;
    uint32_t flags;
};

static const Type *Function(const Type *return_type,
    const std::vector<const Type *> &argument_types, uint32_t flags = 0) {
    static TypeFactory<FunctionType> functions;
    if (return_type->kind() != TK_ReturnLabel) {
        if (return_type == TYPE_Void) {
            return_type = ReturnLabel({});
        } else if (return_type->kind() == TK_Tuple) {
            auto &&types = cast<TupleType>(return_type)->types;
            Args values;
            for (auto it = types.begin(); it != types.end(); ++it) {
                values.push_back(unknown_of(*it));
            }
            return_type = ReturnLabel(values);
        } else {
            return_type = ReturnLabel({unknown_of(return_type)});
        }
    }
    return functions.insert(return_type, Tuple(argument_types), flags);
}

static bool is_function_pointer(const Type *type) {
    switch (type->kind()) {
    case TK_Pointer: {
        const PointerType *ptype = cast<PointerType>(type);
        return isa<FunctionType>(ptype->element_type);
    } break;
    case TK_Extern: {
        const ExternType *etype = cast<ExternType>(type);
        return isa<FunctionType>(etype->type);
    } break;
    default: return false;
    }
}

static bool is_pure_function_pointer(const Type *type) {
    const PointerType *ptype = dyn_cast<PointerType>(type);
    if (!ptype) return false;
    const FunctionType *ftype = dyn_cast<FunctionType>(ptype->element_type);
    if (!ftype) return false;
    return ftype->flags & FF_Pure;
}

static const FunctionType *extract_function_type(const Type *T) {
    switch(T->kind()) {
    case TK_Extern: {
        auto et = cast<ExternType>(T);
        return cast<FunctionType>(et->type);
    } break;
    case TK_Pointer: {
        auto pi = cast<PointerType>(T);
        return cast<FunctionType>(pi->element_type);
    } break;
    default: assert(false && "unexpected function type");
        return nullptr;
    }
}

//------------------------------------------------------------------------------
// TYPENAME
//------------------------------------------------------------------------------

struct TypenameType : Type {
    static std::unordered_set<Symbol, Symbol::Hash> used_names;

    static bool classof(const Type *T) {
        return T->kind() == TK_Typename;
    }

    TypenameType(const String *name)
        : Type(TK_Typename), storage_type(nullptr), super_type(nullptr) {
        auto newname = Symbol(name);
        size_t idx = 2;
        while (used_names.count(newname)) {
            // keep testing until we hit a name that's free
            auto ss = StyledString::plain();
            ss.out << name->data << "$" << idx++;
            newname = Symbol(ss.str());
        }
        used_names.insert(newname);
        _name = newname.name();
    }

    void finalize(const Type *_type) {
        if (finalized()) {
            StyledString ss;
            ss.out << "typename " << _type << " is already final";
            location_error(ss.str());
        }
        if (isa<TypenameType>(_type)) {
            StyledString ss;
            ss.out << "cannot use typename " << _type << " as storage type";
            location_error(ss.str());
        }
        storage_type = _type;
    }

    bool finalized() const { return storage_type != nullptr; }

    const Type *super() const {
        if (!super_type) return TYPE_Typename;
        return super_type;
    }

    const Type *storage_type;
    const Type *super_type;
};

std::unordered_set<Symbol, Symbol::Hash> TypenameType::used_names;

// always generates a new type
static const Type *Typename(const String *name) {
    return new TypenameType(name);
}

static const Type *storage_type(const Type *T) {
    switch(T->kind()) {
    case TK_Typename: {
        const TypenameType *tt = cast<TypenameType>(T);
        if (!tt->finalized()) {
            StyledString ss;
            ss.out << "type " << T << " is opaque";
            location_error(ss.str());
        }
        return tt->storage_type;
    } break;
    case TK_ReturnLabel: {
        const ReturnLabelType *rlt = cast<ReturnLabelType>(T);
        return storage_type(rlt->return_type);
    } break;
    default: return T;
    }
}

//------------------------------------------------------------------------------
// IMAGE TYPE
//------------------------------------------------------------------------------

struct ImageType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_Image;
    }

    ImageType(
        const Type *_type,
        Symbol _dim,
        int _depth,
        int _arrayed,
        int _multisampled,
        int _sampled,
        Symbol _format,
        Symbol _access) :
        Type(TK_Image),
        type(_type), dim(_dim), depth(_depth), arrayed(_arrayed),
        multisampled(_multisampled), sampled(_sampled),
        format(_format), access(_access) {
        auto ss = StyledString::plain();
        ss.out << "<Image " <<  _type->name()->data
            << " " << _dim;
        if (_depth == 1)
            ss.out << " depth";
        else if (_depth == 2)
            ss.out << " ?depth?";
        if (_arrayed)
            ss.out << " array";
        if (_multisampled)
            ss.out << " ms";
        if (_sampled == 0)
            ss.out << " ?sampled?";
        else if (_sampled == 1)
            ss.out << " sampled";
        ss.out << " " << _format;
        if (access != SYM_Unnamed)
            ss.out << " " << _access;
        ss.out << ">";
        _name = ss.str();
    }

    const Type *type; // sampled type
    Symbol dim; // resolved to spv::Dim
    int depth; // 0 = not a depth image, 1 = depth image, 2 = undefined
    int arrayed; // 1 = array image
    int multisampled; // 1 = multisampled content
    int sampled; // 0 = runtime dependent, 1 = sampled, 2 = storage image
    Symbol format; // resolved to spv::ImageFormat
    Symbol access; // resolved to spv::AccessQualifier
};

static const Type *Image(
    const Type *_type,
    Symbol _dim,
    int _depth,
    int _arrayed,
    int _multisampled,
    int _sampled,
    Symbol _format,
    Symbol _access) {
    static TypeFactory<ImageType> images;
    return images.insert(_type, _dim, _depth, _arrayed,
        _multisampled, _sampled, _format, _access);
}

//------------------------------------------------------------------------------
// IMAGE TYPE
//------------------------------------------------------------------------------

struct SampledImageType : Type {
    static bool classof(const Type *T) {
        return T->kind() == TK_SampledImage;
    }

    SampledImageType(const Type *_type) :
        Type(TK_SampledImage), type(_type) {
        auto ss = StyledString::plain();
        ss.out << "<SampledImage " <<  _type->name()->data << ">";
        _name = ss.str();
    }

    const Type *type; // image type
};

static const Type *SampledImage(const Type *_type) {
    static TypeFactory<SampledImageType> sampled_images;
    return sampled_images.insert(_type);
}

//------------------------------------------------------------------------------
// TYPE INQUIRIES
//------------------------------------------------------------------------------

template<TypeKind tk>
static void verify_kind(const Type *T) {
    if (T->kind() != tk) {
        StyledString ss;
        ss.out << "value of ";
        switch(tk) {
        case TK_Integer: ss.out << "integer"; break;
        case TK_Real: ss.out << "real"; break;
        case TK_Pointer: ss.out << "pointer"; break;
        case TK_Array: ss.out << "array"; break;
        case TK_Vector: ss.out << "vector"; break;
        case TK_Tuple: ss.out << "tuple"; break;
        case TK_Union: ss.out << "union"; break;
        case TK_Typename: ss.out << "typename"; break;
        case TK_ReturnLabel: ss.out << "return label"; break;
        case TK_Function: ss.out << "function"; break;
        case TK_Extern: ss.out << "extern"; break;
        case TK_Image: ss.out << "image"; break;
        case TK_SampledImage: ss.out << "sampled image"; break;
        }
        ss.out << " kind expected, got " << T;
        location_error(ss.str());
    }
}

static void verify_function_pointer(const Type *type) {
    if (!is_function_pointer(type)) {
        StyledString ss;
        ss.out << "function pointer expected, got " << type;
        location_error(ss.str());
    }
}

static bool is_opaque(const Type *T) {
    switch(T->kind()) {
    case TK_Typename: {
        const TypenameType *tt = cast<TypenameType>(T);
        if (!tt->finalized()) {
            return true;
        } else {
            return is_opaque(tt->storage_type);
        }
    } break;
    case TK_ReturnLabel: {
        const ReturnLabelType *rlt = cast<ReturnLabelType>(T);
        return is_opaque(rlt->return_type);
    } break;
    case TK_Function: return true;
    default: break;
    }
    return false;
}

static size_t size_of(const Type *T) {
    switch(T->kind()) {
    case TK_Integer: {
        const IntegerType *it = cast<IntegerType>(T);
        return (it->width + 7) / 8;
    }
    case TK_Real: {
        const RealType *rt = cast<RealType>(T);
        return (rt->width + 7) / 8;
    }
    case TK_Extern:
    case TK_Pointer: return PointerType::size();
    case TK_Array: return cast<ArrayType>(T)->size;
    case TK_Vector: return cast<VectorType>(T)->size;
    case TK_Tuple: return cast<TupleType>(T)->size;
    case TK_Union: return cast<UnionType>(T)->size;
    case TK_ReturnLabel: {
        return size_of(cast<ReturnLabelType>(T)->return_type);
    } break;
    case TK_Typename: return size_of(storage_type(cast<TypenameType>(T)));
    default: break;
    }

    StyledString ss;
    ss.out << "opaque type " << T << " has no size";
    location_error(ss.str());
    return -1;
}

static size_t align_of(const Type *T) {
    switch(T->kind()) {
    case TK_Integer: {
        const IntegerType *it = cast<IntegerType>(T);
        return (it->width + 7) / 8;
    }
    case TK_Real: {
        const RealType *rt = cast<RealType>(T);
        switch(rt->width) {
        case 16: return 2;
        case 32: return 4;
        case 64: return 8;
        case 80: return 16;
        default: break;
        }
    }
    case TK_Extern:
    case TK_Pointer: return PointerType::size();
    case TK_Array: return cast<ArrayType>(T)->align;
    case TK_Vector: return cast<VectorType>(T)->align;
    case TK_Tuple: return cast<TupleType>(T)->align;
    case TK_Union: return cast<UnionType>(T)->align;
    case TK_ReturnLabel: {
        return size_of(cast<ReturnLabelType>(T)->return_type);
    } break;
    case TK_Typename: return align_of(storage_type(cast<TypenameType>(T)));
    default: break;
    }

    StyledString ss;
    ss.out << "opaque type " << T << " has no alignment";
    location_error(ss.str());
    return 1;
}

static Any wrap_pointer(const Type *type, void *ptr) {
    Any result = none;
    result.type = type;

    type = storage_type(type);
    switch(type->kind()) {
    case TK_Integer:
    case TK_Real:
    case TK_Pointer:
        memcpy(result.content, ptr, size_of(type));
        return result;
    case TK_Array:
    case TK_Vector:
    case TK_Tuple:
    case TK_Union:
        result.pointer = ptr;
        return result;
    default: break;
    }

    StyledString ss;
    ss.out << "cannot wrap data of type " << type;
    location_error(ss.str());
    return none;
}


void *get_pointer(const Type *type, Any &value, bool create = false) {
    if (type == TYPE_Void) {
        return value.content;
    }
    switch(type->kind()) {
    case TK_Integer: {
        auto it = cast<IntegerType>(type);
        switch(it->width) {
        case 1: return (void *)&value.i1;
        case 8: return (void *)&value.u8;
        case 16: return (void *)&value.u16;
        case 32: return (void *)&value.u32;
        case 64: return (void *)&value.u64;
        default: break;
        }
    } break;
    case TK_Real: {
        auto rt = cast<RealType>(type);
        switch(rt->width) {
        case 32: return (void *)&value.f32;
        case 64: return (void *)&value.f64;
        default: break;
        }
    } break;
    case TK_Pointer: return (void *)&value.pointer;
    case TK_Typename: {
        return get_pointer(storage_type(type), value, create);
    } break;
    case TK_Array:
    case TK_Vector:
    case TK_Tuple:
    case TK_Union:
        if (create) {
            value.pointer = tracked_malloc(size_of(type));
        }
        return value.pointer;
    default: break;
    };

    StyledString ss;
    ss.out << "cannot extract pointer from type " << type;
    location_error(ss.str());
    return nullptr;
}

static const Type *superof(const Type *T) {
    switch(T->kind()) {
    case TK_Integer: return TYPE_Integer;
    case TK_Real: return TYPE_Real;
    case TK_Pointer: return TYPE_Pointer;
    case TK_Array: return TYPE_Array;
    case TK_Vector: return TYPE_Vector;
    case TK_Tuple: return TYPE_Tuple;
    case TK_Union: return TYPE_Union;
    case TK_Typename: return cast<TypenameType>(T)->super();
    case TK_ReturnLabel: return TYPE_ReturnLabel;
    case TK_Function: return TYPE_Function;
    case TK_Extern: return TYPE_Extern;
    case TK_Image: return TYPE_Image;
    case TK_SampledImage: return TYPE_SampledImage;
    }
    assert(false && "unhandled type kind; corrupt pointer?");
    return nullptr;
}

//------------------------------------------------------------------------------
// ANY METHODS
//------------------------------------------------------------------------------

StyledStream& Any::stream(StyledStream& ost, bool annotate_type) const {
    AnyStreamer as(ost, type, annotate_type);
    if (type == TYPE_Nothing) { as.naked(none); }
    else if (type == TYPE_Type) { as.naked(typeref); }
    else if (type == TYPE_Bool) { as.naked(i1); }
    else if (type == TYPE_I8) { as.typed(i8); }
    else if (type == TYPE_I16) { as.typed(i16); }
    else if (type == TYPE_I32) { as.naked(i32); }
    else if (type == TYPE_I64) { as.typed(i64); }
    else if (type == TYPE_U8) { as.typed(u8); }
    else if (type == TYPE_U16) { as.typed(u16); }
    else if (type == TYPE_U32) { as.typed(u32); }
    else if (type == TYPE_U64) { as.typed(u64); }
    else if (type == TYPE_USize) { as.typed(u64); }
    else if (type == TYPE_F32) { as.naked(f32); }
    else if (type == TYPE_F64) { as.typed(f64); }
    else if (type == TYPE_String) { as.naked(string); }
    else if (type == TYPE_Symbol) { as.naked(symbol); }
    else if (type == TYPE_Syntax) { as.naked(syntax); }
    else if (type == TYPE_Anchor) { as.typed(anchor); }
    else if (type == TYPE_List) { as.naked(list); }
    else if (type == TYPE_Builtin) { as.typed(builtin); }
    else if (type == TYPE_Label) { as.typed(label); }
    else if (type == TYPE_Parameter) { as.typed(parameter); }
    else if (type == TYPE_Scope) { as.typed(scope); }
    else if (type == TYPE_Frame) { as.typed(frame); }
    else if (type == TYPE_Closure) { as.typed(closure); }
    else if (type == TYPE_Any) {
        ost << Style_Operator << "[" << Style_None;
        ((Any *)pointer)->stream(ost);
        ost << Style_Operator << "]" << Style_None;
        as.stream_type_suffix();
    } else if (type->kind() == TK_Extern) {
        ost << symbol;
        as.stream_type_suffix();
    } else if (type->kind() == TK_Vector) {
        auto vt = cast<VectorType>(type);
        ost << Style_Operator << "<" << Style_None;
        for (size_t i = 0; i < vt->count; ++i) {
            if (i != 0) {
                ost << " ";
            }
            vt->unpack(pointer, i).stream(ost, false);
        }
        ost << Style_Operator << ">" << Style_None;
        auto ET = vt->element_type;
        if (!((ET == TYPE_Bool)
            || (ET == TYPE_I32)
            || (ET == TYPE_F32)
            ))
            as.stream_type_suffix();
    } else { as.typed(pointer); }
    return ost;
}

size_t Any::hash() const {
    if (type == TYPE_String) {
        if (!string) return 0; // can happen with nullof
        return CityHash64(string->data, string->count);
    }
    if (is_opaque(type))
        return 0;
    const Type *T = storage_type(type);
    switch(T->kind()) {
    case TK_Integer: {
        switch(cast<IntegerType>(T)->width) {
        case 1: return std::hash<bool>{}(i1);
        case 8: return std::hash<uint8_t>{}(u8);
        case 16: return std::hash<uint16_t>{}(u16);
        case 32: return std::hash<uint32_t>{}(u32);
        case 64: return std::hash<uint64_t>{}(u64);
        default: break;
        }
    } break;
    case TK_Real: {
        switch(cast<RealType>(T)->width) {
        case 32: return std::hash<float>{}(f32);
        case 64: return std::hash<double>{}(f64);
        default: break;
        }
    } break;
    case TK_Extern: {
        return std::hash<uint64_t>{}(u64);
    } break;
    case TK_Pointer: return std::hash<void *>{}(pointer);
    case TK_Array: {
        auto ai = cast<ArrayType>(T);
        size_t h = 0;
        for (size_t i = 0; i < ai->count; ++i) {
            h = HashLen16(h, ai->unpack(pointer, i).hash());
        }
        return h;
    } break;
    case TK_Vector: {
        auto vi = cast<VectorType>(T);
        size_t h = 0;
        for (size_t i = 0; i < vi->count; ++i) {
            h = HashLen16(h, vi->unpack(pointer, i).hash());
        }
        return h;
    } break;
    case TK_Tuple: {
        auto ti = cast<TupleType>(T);
        size_t h = 0;
        for (size_t i = 0; i < ti->types.size(); ++i) {
            h = HashLen16(h, ti->unpack(pointer, i).hash());
        }
        return h;
    } break;
    case TK_Union:
        return CityHash64((const char *)pointer, size_of(T));
    default: break;
    }

    StyledStream ss(std::cout);
    ss << "unhashable value: " << T << std::endl;
    assert(false && "unhashable value");
    return 0;
}

bool Any::operator ==(const Any &other) const {
    if (type != other.type) return false;
    if (type == TYPE_String) {
        if (string == other.string) return true;
        if (!string || !other.string) return false;
        if (string->count != other.string->count)
            return false;
        return !memcmp(string->data, other.string->data, string->count);
    }
    if (is_opaque(type))
        return true;
    const Type *T = storage_type(type);
    switch(T->kind()) {
    case TK_Integer: {
        switch(cast<IntegerType>(T)->width) {
        case 1: return (i1 == other.i1);
        case 8: return (u8 == other.u8);
        case 16: return (u16 == other.u16);
        case 32: return (u32 == other.u32);
        case 64: return (u64 == other.u64);
        default: break;
        }
    } break;
    case TK_Real: {
        switch(cast<RealType>(T)->width) {
        case 32: return (f32 == other.f32);
        case 64: return (f64 == other.f64);
        default: break;
        }
    } break;
    case TK_Extern: return symbol == other.symbol;
    case TK_Pointer: return pointer == other.pointer;
    case TK_Array: {
        auto ai = cast<ArrayType>(T);
        for (size_t i = 0; i < ai->count; ++i) {
            if (ai->unpack(pointer, i) != ai->unpack(other.pointer, i))
                return false;
        }
        return true;
    } break;
    case TK_Vector: {
        auto vi = cast<VectorType>(T);
        for (size_t i = 0; i < vi->count; ++i) {
            if (vi->unpack(pointer, i) != vi->unpack(other.pointer, i))
                return false;
        }
        return true;
    } break;
    case TK_Tuple: {
        auto ti = cast<TupleType>(T);
        for (size_t i = 0; i < ti->types.size(); ++i) {
            if (ti->unpack(pointer, i) != ti->unpack(other.pointer, i))
                return false;
        }
        return true;
    } break;
    case TK_Union:
        return !memcmp(pointer, other.pointer, size_of(T));
    default: break;
    }

    StyledStream ss(std::cout);
    ss << "incomparable value: " << T << std::endl;
    assert(false && "incomparable value");
    return false;
}

//------------------------------------------------------------------------------
// ERROR HANDLING
//------------------------------------------------------------------------------

static const Anchor *_active_anchor = nullptr;

static void set_active_anchor(const Anchor *anchor) {
    assert(anchor);
    _active_anchor = anchor;
}

static const Anchor *get_active_anchor() {
    return _active_anchor;
}

struct Exception {
    const Anchor *anchor;
    const String *msg;

    Exception() :
        anchor(nullptr),
        msg(nullptr) {}

    Exception(const Anchor *_anchor, const String *_msg) :
        anchor(_anchor),
        msg(_msg) {}
};

struct ExceptionPad {
    jmp_buf retaddr;
    Any value;

    ExceptionPad() : value(none) {
    }

    void invoke(const Any &value) {
        this->value = value;
        longjmp(retaddr, 1);
    }
};

#ifdef SCOPES_WIN32
#define SCOPES_TRY() \
    ExceptionPad exc_pad; \
    ExceptionPad *_last_exc_pad = _exc_pad; \
    _exc_pad = &exc_pad; \
    if (!_setjmpex(exc_pad.retaddr, nullptr)) {
#else
#define SCOPES_TRY() \
    ExceptionPad exc_pad; \
    ExceptionPad *_last_exc_pad = _exc_pad; \
    _exc_pad = &exc_pad; \
    if (!setjmp(exc_pad.retaddr)) {
#endif

#define SCOPES_CATCH(EXCNAME) \
        _exc_pad = _last_exc_pad; \
    } else { \
        _exc_pad = _last_exc_pad; \
        auto &&EXCNAME = exc_pad.value;

#define SCOPES_TRY_END() \
    }

static ExceptionPad *_exc_pad = nullptr;

static void default_exception_handler(const Any &value);

static void error(const Any &value) {
#if SCOPES_EARLY_ABORT
    default_exception_handler(value);
#else
    if (!_exc_pad) {
        default_exception_handler(value);
    } else {
        _exc_pad->invoke(value);
    }
#endif
}

static void location_error(const String *msg) {
    const Exception *exc = new Exception(_active_anchor, msg);
    error(exc);
}

//------------------------------------------------------------------------------
// SCOPE
//------------------------------------------------------------------------------

struct AnyDoc {
    Any value;
    const String *doc;
};

struct Scope {
public:
    typedef std::unordered_map<Symbol, AnyDoc, Symbol::Hash> Map;
protected:
    Scope(Scope *_parent = nullptr, Map *_map = nullptr) :
        parent(_parent),
        map(_map?_map:(new Map())),
        borrowed(_map?true:false),
        doc(nullptr),
        next_doc(nullptr) {
        if (_parent)
            doc = _parent->doc;
    }

public:
    Scope *parent;
    Map *map;
    bool borrowed;
    const String *doc;
    const String *next_doc;

    void set_doc(const String *str) {
        if (!doc) {
            doc = str;
            next_doc = nullptr;
        } else {
            next_doc = str;
        }
    }

    size_t count() const {
#if 0
        return map->size();
#else
        size_t count = 0;
        auto &&_map = *map;
        for (auto &&k : _map) {
            if (!is_typed(k.second.value))
                continue;
            count++;
        }
        return count;
#endif
    }

    size_t totalcount() const {
        const Scope *self = this;
        size_t count = 0;
        while (self) {
            count += self->count();
            self = self->parent;
        }
        return count;
    }

    size_t levelcount() const {
        const Scope *self = this;
        size_t count = 0;
        while (self) {
            count += 1;
            self = self->parent;
        }
        return count;
    }

    void ensure_not_borrowed() {
        if (!borrowed) return;
        parent = Scope::from(parent, this);
        map = new Map();
        borrowed = false;
    }

    void bind_with_doc(Symbol name, const AnyDoc &entry) {
        ensure_not_borrowed();
        auto ret = map->insert({name, entry});
        if (!ret.second) {
            ret.first->second = entry;
        }
    }

    void bind(Symbol name, const Any &value) {
        AnyDoc entry = { value, next_doc };
        bind_with_doc(name, entry);
        next_doc = nullptr;
    }

    void bind(KnownSymbol name, const Any &value) {
        AnyDoc entry = { value, nullptr };
        bind_with_doc(Symbol(name), entry);
    }

    void del(Symbol name) {
        ensure_not_borrowed();
        auto it = map->find(name);
        if (it != map->end()) {
            // if in local map, we can delete it directly
            map->erase(it);
        } else {
            // otherwise check if it's contained at all
            Any dest = none;
            if (lookup(name, dest)) {
                AnyDoc entry = { untyped(), nullptr };
                // if yes, bind to unknown unknown to mark it as deleted
                bind_with_doc(name, entry);
            }
        }
    }

    std::vector<Symbol> find_closest_match(Symbol name) const {
        const String *s = name.name();
        std::unordered_set<Symbol, Symbol::Hash> done;
        std::vector<Symbol> best_syms;
        size_t best_dist = (size_t)-1;
        const Scope *self = this;
        do {
            auto &&map = *self->map;
            for (auto &&k : map) {
                Symbol sym = k.first;
                if (done.count(sym))
                    continue;
                if (is_typed(k.second.value)) {
                    size_t dist = distance(s, sym.name());
                    if (dist == best_dist) {
                        best_syms.push_back(sym);
                    } else if (dist < best_dist) {
                        best_dist = dist;
                        best_syms = { sym };
                    }
                }
                done.insert(sym);
            }
            self = self->parent;
        } while (self);
        std::sort(best_syms.begin(), best_syms.end());
        return best_syms;
    }

    std::vector<Symbol> find_elongations(Symbol name) const {
        const String *s = name.name();

        std::unordered_set<Symbol, Symbol::Hash> done;
        std::vector<Symbol> found;
        const Scope *self = this;
        do {
            auto &&map = *self->map;
            for (auto &&k : map) {
                Symbol sym = k.first;
                if (done.count(sym))
                    continue;
                if (is_typed(k.second.value)) {
                    if (sym.name()->count >= s->count &&
                            *sym.name()->substr(0, s->count) == *s)
                        found.push_back(sym);
                }
                done.insert(sym);
            }
            self = self->parent;
        } while (self);
        std::sort(found.begin(), found.end(), [](Symbol a, Symbol b){
                  return a.name()->count < b.name()->count; });
        return found;
    }

    bool lookup(Symbol name, AnyDoc &dest, size_t depth = -1) const {
        const Scope *self = this;
        do {
            auto it = self->map->find(name);
            if (it != self->map->end()) {
                if (is_typed(it->second.value)) {
                    dest = it->second;
                    return true;
                } else {
                    return false;
                }
            }
            if (!depth)
                break;
            depth = depth - 1;
            self = self->parent;
        } while (self);
        return false;
    }

    bool lookup(Symbol name, Any &dest, size_t depth = -1) const {
        AnyDoc entry = { none, nullptr };
        if (lookup(name, entry, depth)) {
            dest = entry.value;
            return true;
        }
        return false;
    }

    bool lookup_local(Symbol name, AnyDoc &dest) const {
        return lookup(name, dest, 0);
    }

    bool lookup_local(Symbol name, Any &dest) const {
        return lookup(name, dest, 0);
    }

    StyledStream &stream(StyledStream &ss) {
        size_t totalcount = this->totalcount();
        size_t count = this->count();
        size_t levelcount = this->levelcount();
        ss << Style_Keyword << "Scope" << Style_Comment << "<" << Style_None
            << format("L:%i T:%i in %i levels", count, totalcount, levelcount)->data
            << Style_Comment << ">" << Style_None;
        return ss;
    }

    static Scope *from(Scope *_parent = nullptr, Scope *_borrow = nullptr) {
        return new Scope(_parent, _borrow?(_borrow->map):nullptr);
    }
};

static Scope *globals = Scope::from();

static StyledStream& operator<<(StyledStream& ost, Scope *scope) {
    scope->stream(ost);
    return ost;
}

//------------------------------------------------------------------------------
// LIST
//------------------------------------------------------------------------------

static const List *EOL = nullptr;


#define LIST_POOLSIZE 0x10000
struct List {
protected:
    List(const Any &_at, const List *_next, size_t _count) :
        at(_at),
        next(_next),
        count(_count) {}

public:
    Any at;
    const List *next;
    size_t count;

    Any first() const {
        if (this == EOL) {
            return none;
        } else {
            return at;
        }
    }

    static const List *from(const Any &_at, const List *_next) {
        return new List(_at, _next, (_next != EOL)?(_next->count + 1):1);
    }

    static const List *from(const Any *values, int N) {
        const List *list = EOL;
        for (int i = N - 1; i >= 0; --i) {
            list = from(values[i], list);
        }
        return list;
    }

    template<unsigned N>
    static const List *from(const Any (&values)[N]) {
        return from(values, N);
    }

    static const List *join(const List *a, const List *b);
};

// (a . (b . (c . (d . NIL)))) -> (d . (c . (b . (a . NIL))))
// this is the mutating version; input lists are modified, direction is inverted
const List *reverse_list_inplace(
    const List *l, const List *eol = EOL, const List *cat_to = EOL) {
    const List *next = cat_to;
    size_t count = 0;
    if (cat_to != EOL) {
        count = cat_to->count;
    }
    while (l != eol) {
        count = count + 1;
        const List *iternext = l->next;
        const_cast<List *>(l)->next = next;
        const_cast<List *>(l)->count = count;
        next = l;
        l = iternext;
    }
    return next;
}

const List *List::join(const List *la, const List *lb) {
    const List *l = lb;
    while (la != EOL) {
        l = List::from(la->at, l);
        la = la->next;
    }
    return reverse_list_inplace(l, lb, lb);
}

static StyledStream& operator<<(StyledStream& ost, const List *list);

//------------------------------------------------------------------------------
// SYNTAX OBJECTS
//------------------------------------------------------------------------------

struct Syntax {
protected:
    Syntax(const Anchor *_anchor, const Any &_datum, bool _quoted) :
        anchor(_anchor),
        datum(_datum),
        quoted(_quoted) {}

public:
    const Anchor *anchor;
    Any datum;
    bool quoted;

    static const Syntax *from(const Anchor *_anchor, const Any &_datum, bool quoted = false) {
        assert(_anchor);
        return new Syntax(_anchor, _datum, quoted);
    }

    static const Syntax *from_quoted(const Anchor *_anchor, const Any &_datum) {
        assert(_anchor);
        return new Syntax(_anchor, _datum, true);
    }
};

static Any unsyntax(const Any &e) {
    e.verify(TYPE_Syntax);
    return e.syntax->datum;
}

static Any maybe_unsyntax(const Any &e) {
    if (e.type == TYPE_Syntax) {
        return e.syntax->datum;
    } else {
        return e;
    }
}

static Any strip_syntax(Any e) {
    e = maybe_unsyntax(e);
    if (e.type == TYPE_List) {
        auto src = e.list;
        auto l = src;
        bool needs_unwrap = false;
        while (l != EOL) {
            if (l->at.type == TYPE_Syntax) {
                needs_unwrap = true;
                break;
            }
            l = l->next;
        }
        if (needs_unwrap) {
            l = src;
            const List *dst = EOL;
            while (l != EOL) {
                dst = List::from(strip_syntax(l->at), dst);
                l = l->next;
            }
            return reverse_list_inplace(dst);
        }
    }
    return e;
}

static Any wrap_syntax(const Anchor *anchor, Any e, bool quoted = false) {
    if (e.type == TYPE_List) {
        auto src = e.list;
        auto l = src;
        bool needs_wrap = false;
        while (l != EOL) {
            if (l->at.type != TYPE_Syntax) {
                needs_wrap = true;
                break;
            }
            l = l->next;
        }
        l = src;
        if (needs_wrap) {
            const List *dst = EOL;
            while (l != EOL) {
                dst = List::from(wrap_syntax(anchor, l->at, quoted), dst);
                l = l->next;
            }
            l = reverse_list_inplace(dst);
        }
        return Syntax::from(anchor, l, quoted);
    } else if (e.type != TYPE_Syntax) {
        return Syntax::from(anchor, e, quoted);
    }
    return e;
}

static StyledStream& operator<<(StyledStream& ost, const Syntax *value) {
    ost << value->anchor << value->datum;
    return ost;
}

//------------------------------------------------------------------------------
// S-EXPR LEXER & PARSER
//------------------------------------------------------------------------------

#define B_TOKENS() \
    T(none, -1) \
    T(eof, 0) \
    T(open, '(') \
    T(close, ')') \
    T(square_open, '[') \
    T(square_close, ']') \
    T(curly_open, '{') \
    T(curly_close, '}') \
    T(string, '"') \
    T(block_string, 'B') \
    T(quote, '\'') \
    T(symbol, 'S') \
    T(escape, '\\') \
    T(statement, ';') \
    T(number, 'N')

enum Token {
#define T(NAME, VALUE) tok_ ## NAME = VALUE,
    B_TOKENS()
#undef T
};

static const char *get_token_name(Token tok) {
    switch(tok) {
#define T(NAME, VALUE) case tok_ ## NAME: return #NAME;
    B_TOKENS()
#undef T
    }
}

static const char TOKEN_TERMINATORS[] = "()[]{}\"';#,";

struct LexerParser {
    // LEXER
    //////////////////////////////

    void verify_good_taste(char c) {
        if (c == '\t') {
            location_error(String::from("please use spaces instead of tabs."));
        }
    }

    Token token;
    int base_offset;
    SourceFile *file;
    const char *input_stream;
    const char *eof;
    const char *cursor;
    const char *next_cursor;
    int lineno;
    int next_lineno;
    const char *line;
    const char *next_line;

    const char *string;
    int string_len;

    Any value;

    LexerParser(SourceFile *_file, size_t offset = 0, size_t length = 0) :
            value(none) {
        file = _file;
        input_stream = file->strptr() + offset;
        token = tok_eof;
        base_offset = (int)offset;
        if (length) {
            eof = input_stream + length;
        } else {
            eof = file->strptr() + file->length;
        }
        cursor = next_cursor = input_stream;
        lineno = next_lineno = 1;
        line = next_line = input_stream;
    }

    int offset() {
        return base_offset + (cursor - input_stream);
    }

    int column() {
        return cursor - line + 1;
    }

    int next_column() {
        return next_cursor - next_line + 1;
    }

    const Anchor *anchor() {
        return Anchor::from(file, lineno, column(), offset());
    }

    char next() {
        char c = next_cursor[0];
        verify_good_taste(c);
        next_cursor = next_cursor + 1;
        return c;
    }

    size_t chars_left() {
        return eof - next_cursor;
    }

    bool is_eof() {
        return next_cursor == eof;
    }

    void newline() {
        next_lineno = next_lineno + 1;
        next_line = next_cursor;
    }

    void select_string() {
        string = cursor;
        string_len = next_cursor - cursor;
    }

    void read_single_symbol() {
        select_string();
    }

    void read_symbol() {
        bool escape = false;
        while (true) {
            if (is_eof()) {
                break;
            }
            char c = next();
            if (escape) {
                if (c == '\n') {
                    newline();
                }
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (isspace(c) || strchr(TOKEN_TERMINATORS, c)) {
                next_cursor = next_cursor - 1;
                break;
            }
        }
        select_string();
    }

    void read_string(char terminator) {
        bool escape = false;
        while (true) {
            if (is_eof()) {
                location_error(String::from("unterminated sequence"));
                break;
            }
            char c = next();
            if (c == '\n') {
                // 0.10
                //newline();
                // 0.11
                location_error(String::from("unexpected line break in string"));
                break;
            }
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == terminator) {
                break;
            }
        }
        select_string();
    }

    void read_block(int indent) {
        int col = column() + indent;
        while (true) {
            if (is_eof()) {
                break;
            }
            int next_col = next_column();
            char c = next();
            if (c == '\n') {
                newline();
            } else if (!isspace(c) && (next_col <= col)) {
                next_cursor = next_cursor - 1;
                break;
            }
        }
    }

    void read_block_string() {
        next();next();next();
        read_block(3);
        select_string();
    }

    void read_comment() {
        read_block(0);
    }

    template<unsigned N>
    bool is_suffix(const char (&str)[N]) {
        if (string_len != (N - 1)) {
            return false;
        }
        return !strncmp(string, str, N - 1);
    }

    enum {
        RN_Invalid = 0,
        RN_Untyped = 1,
        RN_Typed = 2,
    };

    template<typename T>
    int read_integer(void (*strton)(T *, const char*, char**)) {
        char *cend;
        errno = 0;
        T srcval;
        strton(&srcval, cursor, &cend);
        if ((cend == cursor)
            || (errno == ERANGE)
            || (cend > eof)) {
            return RN_Invalid;
        }
        value = Any(srcval);
        next_cursor = cend;
        if ((cend != eof)
            && (!isspace(*cend))
            && (!strchr(TOKEN_TERMINATORS, *cend))) {
            if (strchr(".e", *cend)) return false;
            // suffix
            auto _lineno = lineno; auto _line = line; auto _cursor = cursor;
            next_token();
            read_symbol();
            lineno = _lineno; line = _line; cursor = _cursor;
            return RN_Typed;
        } else {
            return RN_Untyped;
        }
    }

    template<typename T>
    int read_real(void (*strton)(T *, const char*, char**, int)) {
        char *cend;
        errno = 0;
        T srcval;
        strton(&srcval, cursor, &cend, 0);
        if ((cend == cursor)
            || (errno == ERANGE)
            || (cend > eof)) {
            return RN_Invalid;
        }
        value = Any(srcval);
        next_cursor = cend;
        if ((cend != eof)
            && (!isspace(*cend))
            && (!strchr(TOKEN_TERMINATORS, *cend))) {
            // suffix
            auto _lineno = lineno; auto _line = line; auto _cursor = cursor;
            next_token();
            read_symbol();
            lineno = _lineno; line = _line; cursor = _cursor;
            return RN_Typed;
        } else {
            return RN_Untyped;
        }
    }

    bool has_suffix() const {
        return (string_len >= 1) && (string[0] == ':');
    }

    bool select_integer_suffix() {
        if (!has_suffix())
            return false;
        if (is_suffix(":i8")) { value = Any(value.i8); return true; }
        else if (is_suffix(":i16")) { value = Any(value.i16); return true; }
        else if (is_suffix(":i32")) { value = Any(value.i32); return true; }
        else if (is_suffix(":i64")) { value = Any(value.i64); return true; }
        else if (is_suffix(":u8")) { value = Any(value.u8); return true; }
        else if (is_suffix(":u16")) { value = Any(value.u16); return true; }
        else if (is_suffix(":u32")) { value = Any(value.u32); return true; }
        else if (is_suffix(":u64")) { value = Any(value.u64); return true; }
        //else if (is_suffix(":isize")) { value = Any(value.i64); return true; }
        else if (is_suffix(":usize")) { value = Any(value.u64); value.type = TYPE_USize; return true; }
        else {
            StyledString ss;
            ss.out << "invalid suffix for integer literal: "
                << String::from(string, string_len);
            location_error(ss.str());
            return false;
        }
    }

    bool select_real_suffix() {
        if (!has_suffix())
            return false;
        if (is_suffix(":f32")) { value = Any((float)value.f64); return true; }
        else if (is_suffix(":f64")) { value = Any(value.f64); return true; }
        else {
            StyledString ss;
            ss.out << "invalid suffix for floating point literal: "
                << String::from(string, string_len);
            location_error(ss.str());
            return false;
        }
    }

    bool read_int64() {
        switch(read_integer(scopes_strtoll)) {
        case RN_Invalid: return false;
        case RN_Untyped:
            if ((value.i64 >= -0x80000000ll) && (value.i64 <= 0x7fffffffll)) {
                value = Any(int32_t(value.i64));
            } else if ((value.i64 >= 0x80000000ll) && (value.i64 <= 0xffffffffll)) {
                value = Any(uint32_t(value.i64));
            }
            return true;
        case RN_Typed:
            return select_integer_suffix();
        default: assert(false); return false;
        }
    }
    bool read_uint64() {
        switch(read_integer(scopes_strtoull)) {
        case RN_Invalid: return false;
        case RN_Untyped:
            return true;
        case RN_Typed:
            return select_integer_suffix();
        default: assert(false); return false;
        }
    }
    bool read_real64() {
        switch(read_real(scopes_strtod)) {
        case RN_Invalid: return false;
        case RN_Untyped:
            value = Any(float(value.f64));
            return true;
        case RN_Typed:
            return select_real_suffix();
        default: assert(false); return false;
        }
    }

    void next_token() {
        lineno = next_lineno;
        line = next_line;
        cursor = next_cursor;
        set_active_anchor(anchor());
    }

    Token read_token() {
        char c;
    skip:
        next_token();
        if (is_eof()) { token = tok_eof; goto done; }
        c = next();
        if (c == '\n') { newline(); }
        if (isspace(c)) { goto skip; }
        if (c == '#') { read_comment(); goto skip; }
        else if (c == '(') { token = tok_open; }
        else if (c == ')') { token = tok_close; }
        else if (c == '[') { token = tok_square_open; }
        else if (c == ']') { token = tok_square_close; }
        else if (c == '{') { token = tok_curly_open; }
        else if (c == '}') { token = tok_curly_close; }
        else if (c == '\\') { token = tok_escape; }
        else if (c == '"') {
            if ((chars_left() >= 3)
                && (next_cursor[0] == '"')
                && (next_cursor[1] == '"')
                && (next_cursor[2] == '"')) {
                token = tok_block_string;
                read_block_string();
            } else {
                token = tok_string;
                read_string(c);
            }
        }
        else if (c == ';') { token = tok_statement; }
        else if (c == '\'') { token = tok_quote; }
        else if (c == ',') { token = tok_symbol; read_single_symbol(); }
        else if (read_int64() || read_uint64() || read_real64()) { token = tok_number; }
        else { token = tok_symbol; read_symbol(); }
    done:
        return token;
    }

    Any get_symbol() {
        char dest[string_len + 1];
        memcpy(dest, string, string_len);
        dest[string_len] = 0;
        auto size = unescape_string(dest);
        return Symbol(String::from(dest, size));
    }
    Any get_string() {
        auto len = string_len - 2;
        char dest[len + 1];
        memcpy(dest, string + 1, len);
        dest[len] = 0;
        auto size = unescape_string(dest);
        return String::from(dest, size);
    }
    Any get_block_string() {
        int strip_col = column() + 4;
        auto len = string_len - 4;
        assert(len >= 0);
        char dest[len + 1];
        const char *start = string + 4;
        const char *end = start + len;
        // strip trailing whitespace up to the first LF after content
        const char *last_lf = end;
        while (end != start) {
            char c = *(end - 1);
            if (!isspace(c)) break;
            if (c == '\n')
                last_lf = end;
            end--;
        }
        end = last_lf;
        char *p = dest;
        while (start != end) {
            char c = *start++;
            *p++ = c;
            if (c == '\n') {
                // strip leftside column
                for (int i = 1; i < strip_col; ++i) {
                    if (start == end) break;
                    if ((*start != ' ') && (*start != '\t')) break;
                    start++;
                }
            }
        }
        return String::from(dest, p - dest);
    }
    Any get_number() {
        return value;
    }
    Any get() {
        if (token == tok_number) {
            return get_number();
        } else if (token == tok_symbol) {
            return get_symbol();
        } else if (token == tok_string) {
            return get_string();
        } else if (token == tok_block_string) {
            return get_block_string();
        } else {
            return none;
        }
    }

    // PARSER
    //////////////////////////////

    struct ListBuilder {
        LexerParser &lexer;
        const List *prev;
        const List *eol;

        ListBuilder(LexerParser &_lexer) :
            lexer(_lexer),
            prev(EOL),
            eol(EOL) {}

        void append(const Any &value) {
            assert(value.type == TYPE_Syntax);
            prev = List::from(value, prev);
        }

        bool is_empty() const {
            return (prev == EOL);
        }

        bool is_expression_empty() const {
            return (prev == EOL);
        }

        void reset_start() {
            eol = prev;
        }

        void split(const Anchor *anchor) {
            // reverse what we have, up to last split point and wrap result
            // in cell
            prev = List::from(
                Syntax::from(anchor,reverse_list_inplace(prev, eol)), eol);
            reset_start();
        }

        const List *get_result() {
            return reverse_list_inplace(prev);
        }
    };

    // parses a list to its terminator and returns a handle to the first cell
    const List *parse_list(Token end_token) {
        const Anchor *start_anchor = this->anchor();
        ListBuilder builder(*this);
        this->read_token();
        while (true) {
            if (this->token == end_token) {
                break;
            } else if (this->token == tok_escape) {
                int column = this->column();
                this->read_token();
                builder.append(parse_naked(column, end_token));
            } else if (this->token == tok_eof) {
                set_active_anchor(start_anchor);
                location_error(String::from("unclosed open bracket"));
            } else if (this->token == tok_statement) {
                builder.split(this->anchor());
                this->read_token();
            } else {
                builder.append(parse_any());
                this->read_token();
            }
        }
        return builder.get_result();
    }

    // parses the next sequence and returns it wrapped in a cell that points
    // to prev
    Any parse_any() {
        assert(this->token != tok_eof);
        const Anchor *anchor = this->anchor();
        if (this->token == tok_open) {
            return Syntax::from(anchor, parse_list(tok_close));
        } else if (this->token == tok_square_open) {
            return Syntax::from(anchor,
                List::from(Syntax::from(anchor,Symbol(SYM_SquareList)),
                    parse_list(tok_square_close)));
        } else if (this->token == tok_curly_open) {
            return Syntax::from(anchor,
                List::from(Syntax::from(anchor,Symbol(SYM_CurlyList)),
                    parse_list(tok_curly_close)));
        } else if ((this->token == tok_close)
            || (this->token == tok_square_close)
            || (this->token == tok_curly_close)) {
            location_error(String::from("stray closing bracket"));
        } else if (this->token == tok_string) {
            return Syntax::from(anchor, get_string());
        } else if (this->token == tok_block_string) {
            return Syntax::from(anchor, get_block_string());
        } else if (this->token == tok_symbol) {
            return Syntax::from(anchor, get_symbol());
        } else if (this->token == tok_number) {
            return Syntax::from(anchor, get_number());
        } else if (this->token == tok_quote) {
            this->read_token();
            if (this->token == tok_eof) {
                set_active_anchor(anchor);
                location_error(
                    String::from("unexpected end of file after quote token"));
            }
            return Syntax::from(anchor,
                List::from({
                    Any(Syntax::from(anchor, Symbol(KW_Quote))),
                    parse_any() }));
        } else {
            location_error(format("unexpected token: %c (%i)",
                this->cursor[0], (int)this->cursor[0]));
        }
        return none;
    }

    Any parse_naked(int column, Token end_token) {
        int lineno = this->lineno;

        bool escape = false;
        int subcolumn = 0;

        const Anchor *anchor = this->anchor();
        ListBuilder builder(*this);

        bool unwrap_single = true;
        while (this->token != tok_eof) {
            if (this->token == end_token) {
                break;
            } else if (this->token == tok_escape) {
                escape = true;
                this->read_token();
                if (this->lineno <= lineno) {
                    location_error(String::from(
                        "escape character is not at end of line"));
                }
                lineno = this->lineno;
            } else if (this->lineno > lineno) {
                if (subcolumn == 0) {
                    subcolumn = this->column();
                } else if (this->column() != subcolumn) {
                    location_error(String::from("indentation mismatch"));
                }
                if (column != subcolumn) {
                    if ((column + 4) != subcolumn) {
                        location_error(String::from(
                            "indentations must nest by 4 spaces."));
                    }
                }

                escape = false;
                lineno = this->lineno;
                // keep adding elements while we're in the same line
                while ((this->token != tok_eof)
                        && (this->token != end_token)
                        && (this->lineno == lineno)) {
                    builder.append(parse_naked(subcolumn, end_token));
                }
            } else if (this->token == tok_statement) {
                this->read_token();
                unwrap_single = false;
                if (!builder.is_empty()) {
                    break;
                }
            } else {
                builder.append(parse_any());
                lineno = this->next_lineno;
                this->read_token();
            }
            if ((!escape || (this->lineno > lineno))
                && (this->column() <= column)) {
                break;
            }
        }

        auto result = builder.get_result();
        if (unwrap_single && result && result->count == 1) {
            return result->at;
        } else {
            return Syntax::from(anchor, result);
        }
    }

    Any parse() {
        this->read_token();
        int lineno = 0;
        //bool escape = false;

        const Anchor *anchor = this->anchor();
        ListBuilder builder(*this);

        while (this->token != tok_eof) {
            if (this->token == tok_none) {
                break;
            } else if (this->token == tok_escape) {
                //escape = true;
                this->read_token();
                if (this->lineno <= lineno) {
                    location_error(String::from(
                        "escape character is not at end of line"));
                }
                lineno = this->lineno;
            } else if (this->lineno > lineno) {
                if (this->column() != 1) {
                    location_error(String::from(
                        "indentation mismatch"));
                }

                //escape = false;
                lineno = this->lineno;
                // keep adding elements while we're in the same line
                while ((this->token != tok_eof)
                        && (this->token != tok_none)
                        && (this->lineno == lineno)) {
                    builder.append(parse_naked(1, tok_none));
                }
            } else if (this->token == tok_statement) {
                location_error(String::from(
                    "unexpected statement token"));
            } else {
                builder.append(parse_any());
                lineno = this->next_lineno;
                this->read_token();
            }
        }
        return Syntax::from(anchor, builder.get_result());
    }

};

//------------------------------------------------------------------------------
// EXPRESSION PRINTER
//------------------------------------------------------------------------------

static const char INDENT_SEP[] = "⁞";

static Style default_symbol_styler(Symbol name) {
    if (!name.is_known())
        return Style_Symbol;
    auto val = name.known_value();
    if ((val >= KEYWORD_FIRST) && (val <= KEYWORD_LAST))
        return Style_Keyword;
    else if ((val >= FUNCTION_FIRST) && (val <= FUNCTION_LAST))
        return Style_Function;
    else if ((val >= SFXFUNCTION_FIRST) && (val <= SFXFUNCTION_LAST))
        return Style_SfxFunction;
    else if ((val >= OPERATOR_FIRST) && (val <= OPERATOR_LAST))
        return Style_Operator;
    return Style_Symbol;
}

struct StreamExprFormat {
    enum Tagging {
        All,
        Line,
        None,
    };

    bool naked;
    Tagging anchors;
    int maxdepth;
    int maxlength;
    Style (*symbol_styler)(Symbol);
    int depth;

    StreamExprFormat() :
        naked(true),
        anchors(None),
        maxdepth(1<<30),
        maxlength(1<<30),
        symbol_styler(default_symbol_styler),
        depth(0)
    {}

    static StreamExprFormat debug() {
        auto fmt = StreamExprFormat();
        fmt.naked = true;
        fmt.anchors = All;
        return fmt;
    }

    static StreamExprFormat debug_digest() {
        auto fmt = StreamExprFormat();
        fmt.naked = true;
        fmt.anchors = Line;
        fmt.maxdepth = 5;
        fmt.maxlength = 5;
        return fmt;
    }

    static StreamExprFormat debug_singleline() {
        auto fmt = StreamExprFormat();
        fmt.naked = false;
        fmt.anchors = All;
        return fmt;
    }

    static StreamExprFormat singleline() {
        auto fmt = StreamExprFormat();
        fmt.naked = false;
        return fmt;
    }

    static StreamExprFormat digest() {
        auto fmt = StreamExprFormat();
        fmt.maxdepth = 5;
        fmt.maxlength = 5;
        return fmt;
    }

    static StreamExprFormat singleline_digest() {
        auto fmt = StreamExprFormat();
        fmt.maxdepth = 5;
        fmt.maxlength = 5;
        fmt.naked = false;
        return fmt;
    }


};

struct StreamAnchors {
    StyledStream &ss;
    const Anchor *last_anchor;

    StreamAnchors(StyledStream &_ss) :
        ss(_ss), last_anchor(nullptr) {
    }

    void stream_anchor(const Anchor *anchor, bool quoted = false) {
        if (anchor) {
            ss << Style_Location;
            auto rss = StyledStream::plain(ss);
            // ss << path.name()->data << ":" << lineno << ":" << column << ":";
            if (!last_anchor || (last_anchor->path() != anchor->path())) {
                rss << anchor->path().name()->data
                    << ":" << anchor->lineno
                    << ":" << anchor->column
                    << ":";
            } else if (!last_anchor || (last_anchor->lineno != anchor->lineno)) {
                rss << ":" << anchor->lineno
                    << ":" << anchor->column
                    << ":";
            } else if (!last_anchor || (last_anchor->column != anchor->column)) {
                rss << "::" << anchor->column
                    << ":";
            } else {
                rss << ":::";
            }
            if (quoted) { rss << "'"; }
            ss << Style_None;
            last_anchor = anchor;
        }
    }
};

struct StreamExpr : StreamAnchors {
    StreamExprFormat fmt;
    bool line_anchors;
    bool atom_anchors;

    StreamExpr(StyledStream &_ss, const StreamExprFormat &_fmt) :
        StreamAnchors(_ss), fmt(_fmt) {
        line_anchors = (fmt.anchors == StreamExprFormat::Line);
        atom_anchors = (fmt.anchors == StreamExprFormat::All);
    }

    void stream_indent(int depth = 0) {
        if (depth >= 1) {
            ss << Style_Comment << "    ";
            for (int i = 2; i <= depth; ++i) {
                ss << INDENT_SEP << "   ";
            }
            ss << Style_None;
        }
    }

    static bool is_nested(const Any &_e) {
        auto e = maybe_unsyntax(_e);
        if (e.type == TYPE_List) {
            auto it = e.list;
            while (it != EOL) {
                auto q = maybe_unsyntax(it->at);
                if (q.type == TYPE_List) {
                    return true;
                }
                it = it->next;
            }
        }
        return false;
    }

    static bool is_list (const Any &_value) {
        auto value = maybe_unsyntax(_value);
        return value.type == TYPE_List;
    }

    void walk(Any e, int depth, int maxdepth, bool naked) {
        bool quoted = false;

        const Anchor *anchor = nullptr;
        if (e.type == TYPE_Syntax) {
            anchor = e.syntax->anchor;
            quoted = e.syntax->quoted;
            e = e.syntax->datum;
        }

        if (naked) {
            stream_indent(depth);
        }
        if (atom_anchors) {
            stream_anchor(anchor, quoted);
        }

        if (e.type == TYPE_List) {
            if (naked && line_anchors && !atom_anchors) {
                stream_anchor(anchor, quoted);
            }

            maxdepth = maxdepth - 1;

            auto it = e.list;
            if (it == EOL) {
                ss << Style_Operator << "()" << Style_None;
                if (naked) { ss << std::endl; }
                return;
            }
            if (maxdepth == 0) {
                ss << Style_Operator << "("
                   << Style_Comment << "<...>"
                   << Style_Operator << ")"
                   << Style_None;
                if (naked) { ss << std::endl; }
                return;
            }
            int offset = 0;
            // int numsublists = 0;
            if (naked) {
                if (is_list(it->at)) {
                    ss << ";" << std::endl;
                    goto print_sparse;
                }
            print_terse:
                walk(it->at, depth, maxdepth, false);
                it = it->next;
                offset = offset + 1;
                while (it != EOL) {
                    if (is_nested(it->at)) {
                        break;
                    }
                    ss << " ";
                    walk(it->at, depth, maxdepth, false);
                    offset = offset + 1;
                    it = it->next;
                }
                ss << std::endl;
            print_sparse:
                int subdepth = depth + 1;
                while (it != EOL) {
                    auto value = it->at;
                    if (!is_list(value) // not a list
                        && (offset >= 1)) { // not first element in list
                        stream_indent(subdepth);
                        ss << "\\ ";
                        goto print_terse;
                    }
                    if (offset >= fmt.maxlength) {
                        stream_indent(subdepth);
                        ss << "<...>" << std::endl;
                        return;
                    }
                    walk(value, subdepth, maxdepth, true);
                    offset = offset + 1;
                    it = it->next;
                }
            } else {
                depth = depth + 1;
                ss << Style_Operator << "(" << Style_None;
                while (it != EOL) {
                    if (offset > 0) {
                        ss << " ";
                    }
                    if (offset >= fmt.maxlength) {
                        ss << Style_Comment << "..." << Style_None;
                        break;
                    }
                    walk(it->at, depth, maxdepth, false);
                    offset = offset + 1;
                    it = it->next;
                }
                ss << Style_Operator << ")" << Style_None;
            }
        } else {
            if (e.type == TYPE_Symbol) {
                ss << fmt.symbol_styler(e.symbol);
                e.symbol.name()->stream(ss, SYMBOL_ESCAPE_CHARS);
                ss << Style_None;
            } else {
                ss << e;
            }
            if (naked) { ss << std::endl; }
        }
    }

    void stream(const Any &e) {
        walk(e, fmt.depth, fmt.maxdepth, fmt.naked);
    }
};

static void stream_expr(
    StyledStream &_ss, const Any &e, const StreamExprFormat &_fmt) {
    StreamExpr streamer(_ss, _fmt);
    streamer.stream(e);
}

static StyledStream& operator<<(StyledStream& ost, const List *list) {
    stream_expr(ost, list, StreamExprFormat::singleline());
    return ost;
}

//------------------------------------------------------------------------------
// IL OBJECTS
//------------------------------------------------------------------------------

// IL form inspired by and partially implemented after
// Leissa et al., Graph-Based Higher-Order Intermediate Representation
// http://compilers.cs.uni-saarland.de/papers/lkh15_cgo.pdf


enum {
    ARG_Cont = 0,
    ARG_Arg0 = 1,
    PARAM_Cont = 0,
    PARAM_Arg0 = 1,
};

typedef std::unordered_map<Parameter *, Args > MangleParamMap;
typedef std::unordered_map<Label *, Label *> MangleLabelMap;

enum ParameterKind {
    PK_Regular = 0,
    PK_Variadic = 1,
};

struct Parameter {
protected:
    Parameter(const Anchor *_anchor, Symbol _name, const Type *_type, ParameterKind _kind) :
        anchor(_anchor), name(_name), type(_type), label(nullptr), index(-1),
        kind(_kind) {}

public:
    const Anchor *anchor;
    Symbol name;
    const Type *type;
    Label *label;
    int index;
    ParameterKind kind;

    bool is_vararg() const {
        return (kind == PK_Variadic);
    }

    bool is_typed() const {
        return type != TYPE_Unknown;
    }

    bool is_none() const {
        return type == TYPE_Nothing;
    }

    StyledStream &stream_local(StyledStream &ss) const {
        if ((name != SYM_Unnamed) || !label) {
            ss << Style_Symbol;
            name.name()->stream(ss, SYMBOL_ESCAPE_CHARS);
            ss << Style_None;
        } else {
            ss << Style_Operator << "@" << Style_None << index;
        }
        if (is_vararg()) {
            ss << Style_Keyword << "…" << Style_None;
        }
        if (is_typed()) {
            ss << Style_Operator << ":" << Style_None << type;
        }
        return ss;
    }
    StyledStream &stream(StyledStream &ss) const;

    static Parameter *from(const Parameter *_param) {
        return new Parameter(
            _param->anchor, _param->name, _param->type, _param->kind);
    }

    static Parameter *from(const Anchor *_anchor, Symbol _name, const Type *_type) {
        return new Parameter(_anchor, _name, _type, PK_Regular);
    }

    static Parameter *variadic_from(const Anchor *_anchor, Symbol _name, const Type *_type) {
        return new Parameter(_anchor, _name, _type, PK_Variadic);
    }
};

void Any::verify_indirect(const Type *T) const {
    scopes::verify(T, indirect_type());
}

bool Any::is_const() const {
    return !((type == TYPE_Parameter) && parameter->label);
}

const Type *Any::indirect_type() const {
    if (!is_const()) {
        return parameter->type;
    } else {
        return type;
    }
}

static StyledStream& operator<<(StyledStream& ss, Parameter *param) {
    param->stream(ss);
    return ss;
}

//------------------------------------------------------------------------------

enum LabelBodyFlags {
    LBF_RawCall = (1 << 0),
    LBF_Complete = (1 << 1)
};

struct Body {
    const Anchor *anchor;
    Any enter;
    Args args;
    uint64_t flags;

    // if there's a scope label, the current frame will be truncated to the
    // parent frame that maps the scope label.
    Label *scope_label;

    Body() :
        anchor(nullptr), enter(none), flags(0), scope_label(nullptr) {}

    bool is_complete() const {
        return flags & LBF_Complete;
    }
    void set_complete() {
        flags |= LBF_Complete;
    }
    void unset_complete() {
        flags &= ~LBF_Complete;
    }

    bool is_rawcall() {
        return (flags & LBF_RawCall) == LBF_RawCall;
    }

    void set_rawcall(bool enable = true) {
        if (enable) {
            flags |= LBF_RawCall;
        } else {
            flags &= ~LBF_RawCall;
        }
    }

    void copy_traits_from(const Body &other) {
        flags = other.flags;
        anchor = other.anchor;
        scope_label = other.scope_label;
    }
};

static const char CONT_SEP[] = "▶";

template<typename T>
struct Tag {
    static uint64_t active_gen;
    uint64_t gen;

    Tag() :
        gen(active_gen) {}

    static void clear() {
        active_gen++;
    }
    bool visited() const {
        return gen == active_gen;
    }
    void visit() {
        gen = active_gen;
    }
};

template<typename T>
uint64_t Tag<T>::active_gen = 0;

typedef Tag<Label> LabelTag;

enum LabelFlags {
    LF_Template = (1 << 0),
    // label has been discovered to be reentrant; should not be inlined
    LF_Reentrant = (1 << 1),
    // repeatedly calling this label with the same arguments will yield
    // different results
    LF_Impure = (1 << 2),
};

struct Label {
protected:
    static uint64_t next_uid;

    Label(const Anchor *_anchor, Symbol _name, uint64_t _flags) :
        original(nullptr), docstring(nullptr),
        uid(0), next_instanceid(1), anchor(_anchor), name(_name),
        paired(nullptr), flags(_flags)
        {}

public:
    Label *original;
    const String *docstring;
    size_t uid;
    size_t next_instanceid;
    const Anchor *anchor;
    Symbol name;
    std::vector<Parameter *> params;
    Body body;
    LabelTag tag;
    Label *paired;
    uint64_t flags;

    void set_reentrant() {
        flags |= LF_Reentrant;
    }

    void set_impure() {
        flags |= LF_Impure;
    }

    bool is_impure() const {
        return flags & LF_Impure;
    }

    bool is_memoized() const {
        return !(is_impure() && is_template());
    }

    bool is_reentrant() const {
        return flags & LF_Reentrant;
    }

    bool is_template() const {
        return flags & LF_Template;
    }

    Parameter *get_param_by_name(Symbol name) {
        size_t count = params.size();
        for (size_t i = 1; i < count; ++i) {
            if (params[i]->name == name) {
                return params[i];
            }
        }
        return nullptr;
    }

    bool is_jumping() const {
        auto &&args = body.args;
        assert(!args.empty());
        return args[0].value.type == TYPE_Nothing;
    }

    bool is_calling(Label *callee) const {
        auto &&enter = body.enter;
        return (enter.type == TYPE_Label) && (enter.label == callee);
    }

    bool is_continuing_to(Label *callee) const {
        auto &&args = body.args;
        assert(!args.empty());
        return (args[0].value.type == TYPE_Label) && (args[0].value.label == callee);
    }

    bool is_basic_block_like() const {
        if (params.empty())
            return true;
        if (params[0]->type == TYPE_Nothing)
            return true;
        return false;
    }

    bool is_return_param_typed() const {
        assert(!params.empty());
        return params[0]->is_typed();
    }

    bool has_params() const {
        return params.size() > 1;
    }

    bool is_variadic() const {
        return (!params.empty() && params.back()->is_vararg());
    }

    bool is_valid() const {
        return !params.empty() && body.anchor && !body.args.empty();
    }

    void verify_valid () {
        const String *msg = nullptr;
        if (params.empty()) {
            msg = String::from("label corrupt: parameters are missing");
        } else if (!body.anchor) {
            msg = String::from("label corrupt: body anchor is missing");
        } else if (body.args.empty()) {
            msg = String::from("label corrupt: body arguments are missing");
        }
        if (msg) {
            set_active_anchor(anchor);
            location_error(msg);
        }
    }

    struct UserMap {
        std::unordered_map<Label *, std::unordered_set<Label *> > label_map;
        std::unordered_map<Parameter *, std::unordered_set<Label *> > param_map;

        void clear() {
            label_map.clear();
            param_map.clear();
        }

        void insert(Label *source, Label *dest) {
            label_map[dest].insert(source);
        }

        void insert(Label *source, Parameter *dest) {
            param_map[dest].insert(source);
        }

        void remove(Label *source, Label *dest) {
            auto it = label_map.find(dest);
            if (it != label_map.end()) {
                it->second.erase(source);
            }
        }

        void remove(Label *source, Parameter *dest) {
            auto it = param_map.find(dest);
            if (it != param_map.end()) {
                it->second.erase(source);
            }
        }

        void stream_users(const std::unordered_set<Label *> &users,
            StyledStream &ss) const {
            ss << Style_Comment << "{" << Style_None;
            size_t i = 0;
            for (auto &&kv : users) {
                if (i > 0) {
                    ss << " ";
                }
                Label *label = kv;
                label->stream_short(ss);
                i++;
            }
            ss << Style_Comment << "}" << Style_None;
        }

        void stream_users(Label *node, StyledStream &ss) const {
            auto it = label_map.find(node);
            if (it != label_map.end()) stream_users(it->second, ss);
        }

        void stream_users(Parameter *node, StyledStream &ss) const {
            auto it = param_map.find(node);
            if (it != param_map.end()) stream_users(it->second, ss);
        }
    };

    struct Args {
        Frame *frame;
        scopes::Args args;

        Args() : frame(nullptr) {}

        bool operator==(const Args &other) const {
            if (frame != other.frame) return false;
            if (args.size() != other.args.size()) return false;
            for (size_t i = 0; i < args.size(); ++i) {
                auto &&a = args[i];
                auto &&b = other.args[i];
                if (a != b)
                    return false;
            }
            return true;
        }

        struct Hash {
            std::size_t operator()(const Args& s) const {
                std::size_t h = std::hash<Frame *>{}(s.frame);
                for (auto &&arg : s.args) {
                    h = HashLen16(h, arg.hash());
                }
                return h;
            }
        };

    };

    // inlined instances of this label
    std::unordered_map<Args, Label *, Args::Hash> instances;

    Label *get_label_enter() const {
        assert(body.enter.type == TYPE_Label);
        return body.enter.label;
    }

    const Closure *get_closure_enter() const {
        assert(body.enter.type == TYPE_Closure);
        return body.enter.closure;
    }

    Builtin get_builtin_enter() const {
        assert(body.enter.type == TYPE_Builtin);
        return body.enter.builtin;
    }

    Label *get_label_cont() const {
        assert(!body.args.empty());
        assert(body.args[0].value.type == TYPE_Label);
        return body.args[0].value.label;
    }

    const ReturnLabelType *verify_return_label();

    const Type *get_return_type() const {
        assert(params.size());
        assert(!is_basic_block_like());
        if (!params[0]->is_typed())
            return TYPE_Void;
        // verify that the return type is the one we expect
        cast<ReturnLabelType>(params[0]->type);
        return params[0]->type;
    }

    void verify_compilable() const {
        if (params[0]->is_typed()
            && !params[0]->is_none()) {
            auto tl = dyn_cast<ReturnLabelType>(params[0]->type);
            if (!tl) {
                set_active_anchor(anchor);
                StyledString ss;
                ss.out << "cannot compile function with return type "
                    << params[0]->type;
                location_error(ss.str());
            }
            for (size_t i = 0; i < tl->values.size(); ++i) {
                auto &&val = tl->values[i].value;
                if (is_unknown(val)) {
                    auto T = val.typeref;
                    if (is_opaque(T)) {
                        set_active_anchor(anchor);
                        StyledString ss;
                        ss.out << "cannot compile function with opaque return argument of type "
                            << T;
                        location_error(ss.str());
                    }
                }
            }
        }

        std::vector<const Type *> argtypes;
        for (size_t i = 1; i < params.size(); ++i) {
            auto T = params[i]->type;
            if (T == TYPE_Unknown) {
                set_active_anchor(anchor);
                location_error(String::from("cannot compile function with untyped argument"));
            } else if (is_opaque(T)) {
                set_active_anchor(anchor);
                StyledString ss;
                ss.out << "cannot compile function with opaque argument of type "
                    << T;
                location_error(ss.str());
            }
        }
    }

    const Type *get_params_as_return_label_type() const {
        scopes::Args values;
        for (size_t i = 1; i < params.size(); ++i) {
            values.push_back(unknown_of(params[i]->type));
        }
        return ReturnLabel(values);
    }

    const Type *get_function_type() const {

        std::vector<const Type *> argtypes;
        for (size_t i = 1; i < params.size(); ++i) {
            argtypes.push_back(params[i]->type);
        }
        uint64_t flags = 0;
        assert(params.size());
        if (!params[0]->is_typed()) {
            flags |= FF_Divergent;
        }
        return Function(get_return_type(), argtypes, flags);
    }

    void use(UserMap &um, const Any &arg, int i) {
        if (arg.type == TYPE_Parameter && (arg.parameter->label != this)) {
            um.insert(this, arg.parameter /*, i*/);
        } else if (arg.type == TYPE_Label && (arg.label != this)) {
            um.insert(this, arg.label /*, i*/);
        }
    }

    void unuse(UserMap &um, const Any &arg, int i) {
        if (arg.type == TYPE_Parameter && (arg.parameter->label != this)) {
            um.remove(this, arg.parameter /*, i*/);
        } else if (arg.type == TYPE_Label && (arg.label != this)) {
            um.remove(this, arg.label /*, i*/);
        }
    }

    void insert_into_usermap(UserMap &um) {
        use(um, body.enter, -1);
        size_t count = body.args.size();
        for (size_t i = 0; i < count; ++i) {
            use(um, body.args[i].value, i);
        }
    }

    void remove_from_usermap(UserMap &um) {
        unuse(um, body.enter, -1);
        size_t count = body.args.size();
        for (size_t i = 0; i < count; ++i) {
            unuse(um, body.args[i].value, i);
        }
    }

    void append(Parameter *param) {
        assert(!param->label);
        param->label = this;
        param->index = (int)params.size();
        params.push_back(param);
    }

    void set_parameters(const std::vector<Parameter *> &_params) {
        assert(params.empty());
        params = _params;
        for (size_t i = 0; i < params.size(); ++i) {
            Parameter *param = params[i];
            assert(!param->label);
            param->label = this;
            param->index = (int)i;
        }
    }

    void build_reachable(std::unordered_set<Label *> &labels,
        std::vector<Label *> *ordered_labels = nullptr) {
        labels.clear();
        labels.insert(this);
        if (ordered_labels)
            ordered_labels->push_back(this);
        std::vector<Label *> stack = { this };
        while (!stack.empty()) {
            Label *parent = stack.back();
            stack.pop_back();

            int size = (int)parent->body.args.size();
            for (int i = -1; i < size; ++i) {
                Any arg = none;
                if (i == -1) {
                    arg = parent->body.enter;
                } else {
                    arg = parent->body.args[i].value;
                }

                if (arg.type == TYPE_Label) {
                    Label *label = arg.label;
                    if (!labels.count(label)) {
                        labels.insert(label);
                        if (ordered_labels)
                            ordered_labels->push_back(label);
                        stack.push_back(label);
                    }
                }
            }
        }
    }

    void build_scope(UserMap &um, std::vector<Label *> &tempscope) {
        tempscope.clear();

        std::unordered_set<Label *> visited;
        visited.clear();
        visited.insert(this);

        for (auto &&param : params) {
            auto it = um.param_map.find(param);
            if (it != um.param_map.end()) {
                auto &&users = it->second;
                // every label using one of our parameters is live in scope
                for (auto &&kv : users) {
                    Label *live_label = kv;
                    if (!visited.count(live_label)) {
                        visited.insert(live_label);
                        tempscope.push_back(live_label);
                    }
                }
            }
        }

        size_t index = 0;
        while (index < tempscope.size()) {
            Label *scope_label = tempscope[index++];

            auto it = um.label_map.find(scope_label);
            if (it != um.label_map.end()) {
                auto &&users = it->second;
                // users of scope_label are indirectly live in scope
                for (auto &&kv : users) {
                    Label *live_label = kv;
                    if (!visited.count(live_label)) {
                        visited.insert(live_label);
                        tempscope.push_back(live_label);
                    }
                }
            }

            for (auto &&param : scope_label->params) {
                auto it = um.param_map.find(param);
                if (it != um.param_map.end()) {
                    auto &&users = it->second;
                    // every label using scope_label's parameters is live in scope
                    for (auto &&kv : users) {
                        Label *live_label = kv;
                        if (!visited.count(live_label)) {
                            visited.insert(live_label);
                            tempscope.push_back(live_label);
                        }
                    }
                }
            }
        }
    }

    void build_scope(std::vector<Label *> &tempscope) {
        std::unordered_set<Label *> visited;
        std::vector<Label *> reachable;
        build_reachable(visited, &reachable);
        UserMap um;
        for (auto it = reachable.begin(); it != reachable.end(); ++it) {
            (*it)->insert_into_usermap(um);
        }

        build_scope(um, tempscope);
    }

    Label *get_original() {
        Label *l = this;
        while (l->original)
            l = l->original;
        return l;
    }

    StyledStream &stream_id(StyledStream &ss) const {
        if (original) {
            original->stream_id(ss);
        }
        ss << uid;
        if (uid >= 10) {
            ss << "x";
        }
        return ss;
    }

    StyledStream &stream_short(StyledStream &ss) const {
#if SCOPES_DEBUG_CODEGEN
        if (is_template()) {
            ss << Style_Keyword << "T:" << Style_None;
        }
#endif
        if (name != SYM_Unnamed) {
            ss << Style_Symbol;
            name.name()->stream(ss, SYMBOL_ESCAPE_CHARS);
        }
        ss << Style_Keyword << "λ" << Style_Symbol;
        {
            StyledStream ps = StyledStream::plain(ss);
            if (!original) {
                ps << uid;
            } else {
                stream_id(ps);
            }
        }
        if (is_impure()) {
            ss << Style_Keyword << "!";
        }
        ss << Style_None;
        return ss;
    }

    StyledStream &stream(StyledStream &ss, bool users = false) const {
        stream_short(ss);
        ss << Style_Operator << "(" << Style_None;
        size_t count = params.size();
        for (size_t i = 1; i < count; ++i) {
            if (i > 1) {
                ss << " ";
            }
            params[i]->stream_local(ss);
        }
        ss << Style_Operator << ")" << Style_None;
        if (count) {
            const Type *rtype = params[0]->type;
            if (rtype != TYPE_Nothing) {
                ss << Style_Comment << CONT_SEP << Style_None;
                if (rtype == TYPE_Unknown) {
                    ss << Style_Comment << "?" << Style_None;
                } else {
                    params[0]->stream_local(ss);
                }
            }
        }
        return ss;
    }

    static Label *from(const Anchor *_anchor, Symbol _name) {
        assert(_anchor);
        Label *result = new Label(_anchor, _name, LF_Template);
        result->uid = next_uid++;
        return result;
    }
    // only inherits name and anchor
    static Label *from(Label *label) {
        Label *result = new Label(label->anchor, label->name, 0);
        result->original = label;
        result->uid = label->next_instanceid++;
        result->flags |= label->flags & LF_Impure;
        return result;
    }

    // a continuation that never returns
    static Label *continuation_from(const Anchor *_anchor, Symbol _name) {
        Label *value = from(_anchor, _name);
        // first argument is present, but unused
        value->append(Parameter::from(_anchor, _name, TYPE_Nothing));
        return value;
    }

    // a function that eventually returns
    static Label *function_from(const Anchor *_anchor, Symbol _name) {
        Label *value = from(_anchor, _name);
        // continuation is always first argument
        // this argument will be called when the function is done
        value->append(
            Parameter::from(_anchor,
                Symbol(format("return-%s", _name.name()->data)),
                TYPE_Unknown));
        return value;
    }

};

uint64_t Label::next_uid = 0;

static StyledStream& operator<<(StyledStream& ss, Label *label) {
    label->stream(ss);
    return ss;
}

static StyledStream& operator<<(StyledStream& ss, const Label *label) {
    label->stream(ss);
    return ss;
}

StyledStream &Parameter::stream(StyledStream &ss) const {
    if (label) {
        label->stream_short(ss);
    } else {
        ss << Style_Comment << "<unbound>" << Style_None;
    }
    ss << Style_Comment << "." << Style_None;
    stream_local(ss);
    return ss;
}

//------------------------------------------------------------------------------

struct Closure {
protected:

    Closure(Label *_label, Frame *_frame) :
        label(_label), frame(_frame) {}

public:

    struct Hash {
        std::size_t operator()(const Closure &k) const {
            return HashLen16(
                std::hash<Label *>{}(k.label),
                std::hash<Frame *>{}(k.frame));
        }
    };

    bool operator ==(const Closure &k) const {
        return (label == k.label)
            && (frame == k.frame);
    }

    static std::unordered_map<Closure, const Closure *, Closure::Hash> map;

    Label *label;
    Frame *frame;

    static const Closure *from(Label *label, Frame *frame) {
        assert (label->is_template());
        Closure cl(label, frame);
        auto it = map.find(cl);
        if (it != map.end()) {
            return it->second;
        }
        const Closure *result = new Closure(label, frame);
        map.insert({cl, result});
        return result;
    }

    StyledStream &stream(StyledStream &ost) const {
        ost << Style_Comment << "<" << Style_None
            << frame
            << Style_Comment << "::" << Style_None;
        label->stream_short(ost);
        ost << Style_Comment << ">" << Style_None;
        return ost;
    }
};

std::unordered_map<Closure, const Closure *, Closure::Hash> Closure::map;

static StyledStream& operator<<(StyledStream& ss, const Closure *closure) {
    closure->stream(ss);
    return ss;
}

//------------------------------------------------------------------------------

struct Frame {
    Frame(Frame *_parent, Label *_label, Label *_instance, size_t _loop_count = 0) :
        parent(_parent), label(_label), instance(_instance), loop_count(_loop_count) {
        args.reserve(_label->params.size());
    }

    Args args;
    Frame *parent;
    Label *label;
    Label *instance;
    size_t loop_count;

    Frame *find_frame(Label *label) {
        Frame *top = this;
        while (top) {
            if (top->label == label) {
                return top;
            }
            top = top->parent;
        }
        return nullptr;
    }

    static Frame *from(Frame *parent, Label *label, Label *instance, size_t loop_count) {
        return new Frame(parent, label, instance, loop_count);
    }

    bool all_args_constant() const {
        for (size_t i = 1; i < args.size(); ++i) {
            if (is_unknown(args[i].value))
                return false;
            if (!args[i].value.is_const())
                return false;
        }
        return true;
    }
};

//------------------------------------------------------------------------------
// IL PRINTER
//------------------------------------------------------------------------------

struct StreamLabelFormat {
    enum Tagging {
        All,
        Line,
        Scope,
        None,
    };

    Tagging anchors;
    Tagging follow;
    bool show_users;
    bool show_scope;

    StreamLabelFormat() :
        anchors(None),
        follow(All),
        show_users(false),
        show_scope(false)
        {}

    static StreamLabelFormat debug_all() {
        StreamLabelFormat fmt;
        fmt.follow = All;
        fmt.show_users = true;
        fmt.show_scope = true;
        return fmt;
    }

    static StreamLabelFormat debug_scope() {
        StreamLabelFormat fmt;
        fmt.follow = Scope;
        fmt.show_users = true;
        return fmt;
    }

    static StreamLabelFormat debug_single() {
        StreamLabelFormat fmt;
        fmt.follow = None;
        fmt.show_users = true;
        return fmt;
    }

    static StreamLabelFormat single() {
        StreamLabelFormat fmt;
        fmt.follow = None;
        return fmt;
    }

    static StreamLabelFormat scope() {
        StreamLabelFormat fmt;
        fmt.follow = Scope;
        return fmt;
    }

};

struct StreamLabel : StreamAnchors {
    StreamLabelFormat fmt;
    bool line_anchors;
    bool atom_anchors;
    bool follow_labels;
    bool follow_scope;
    std::unordered_set<Label *> visited;

    StreamLabel(StyledStream &_ss, const StreamLabelFormat &_fmt) :
        StreamAnchors(_ss), fmt(_fmt) {
        line_anchors = (fmt.anchors == StreamLabelFormat::Line);
        atom_anchors = (fmt.anchors == StreamLabelFormat::All);
        follow_labels = (fmt.follow == StreamLabelFormat::All);
        follow_scope = (fmt.follow == StreamLabelFormat::Scope);
    }

    void stream_label_label(Label *alabel) {
        alabel->stream_short(ss);
    }

    void stream_label_label_user(Label *alabel) {
        alabel->stream_short(ss);
    }

    void stream_param_label(Parameter *param, Label *alabel) {
        if (param->label == alabel) {
            param->stream_local(ss);
        } else {
            param->stream(ss);
        }
    }

    void stream_argument(Argument arg, Label *alabel) {
        if (arg.key != SYM_Unnamed) {
            ss << arg.key << Style_Operator << "=" << Style_None;
        }
        if (arg.value.type == TYPE_Parameter) {
            stream_param_label(arg.value.parameter, alabel);
        } else if (arg.value.type == TYPE_Label) {
            stream_label_label(arg.value.label);
        } else if (arg.value.type == TYPE_List) {
            stream_expr(ss, arg.value, StreamExprFormat::singleline_digest());
        } else {
            ss << arg.value;
        }
    }

    void stream_label (Label *alabel) {
        if (visited.count(alabel)) {
            return;
        }
        visited.insert(alabel);
        if (line_anchors) {
            stream_anchor(alabel->anchor);
        }
        if (alabel->is_reentrant()) {
            ss << Style_Keyword << "R" << Style_None;
            ss << " ";
        }
        alabel->stream(ss, fmt.show_users);
        ss << Style_Operator << ":" << Style_None;
        if (fmt.show_scope && alabel->body.scope_label) {
            ss << " " << Style_Operator << "[" << Style_None;
            alabel->body.scope_label->stream_short(ss);
            ss << Style_Operator << "]" << Style_None;
        }
        //stream_scope(scopes[alabel])
        ss << std::endl;
        ss << "    ";
        if (line_anchors && alabel->body.anchor) {
            stream_anchor(alabel->body.anchor);
            ss << " ";
        }
        if (!alabel->body.is_complete()) {
            ss << Style_Keyword << "T " << Style_None;
        }
        if (alabel->body.is_rawcall()) {
            ss << Style_Keyword << "rawcall " << Style_None;
        }
        stream_argument(alabel->body.enter, alabel);
        for (size_t i=1; i < alabel->body.args.size(); ++i) {
            ss << " ";
            stream_argument(alabel->body.args[i], alabel);
        }
        if (!alabel->body.args.empty()) {
            auto &&cont = alabel->body.args[0];
            if (cont.value.type != TYPE_Nothing) {
                ss << " " << Style_Comment << CONT_SEP << Style_None << " ";
                stream_argument(cont.value, alabel);
            }
        }
        ss << std::endl;

        if (follow_labels) {
            for (size_t i=0; i < alabel->body.args.size(); ++i) {
                stream_any(alabel->body.args[i].value);
            }
            stream_any(alabel->body.enter);
        }
    }

    void stream_any(const Any &afunc) {
        if (afunc.type == TYPE_Label) {
            stream_label(afunc.label);
        }
    }

    void stream(Label *label) {
        stream_label(label);
        if (follow_scope) {
            std::vector<Label *> scope;
            label->build_scope(scope);
            size_t i = scope.size();
            while (i > 0) {
                --i;
                stream_label(scope[i]);
            }
        }
    }

};

static void stream_label(
    StyledStream &_ss, Label *label, const StreamLabelFormat &_fmt) {
    StreamLabel streamer(_ss, _fmt);
    streamer.stream(label);
}

const ReturnLabelType *Label::verify_return_label() {
    if (!params.empty()) {
        const ReturnLabelType *rt = dyn_cast<ReturnLabelType>(params[0]->type);
        if (rt)
            return rt;
    }
#if SCOPES_DEBUG_CODEGEN
    {
        StyledStream ss;
        stream_label(ss, this, StreamLabelFormat::debug_all());
    }
#endif
    set_active_anchor(anchor);
    location_error(String::from("label has no return type"));
    return nullptr;
}

//------------------------------------------------------------------------------
// FRAME PRINTER
//------------------------------------------------------------------------------

struct StreamFrameFormat {
    enum Tagging {
        All,
        None,
    };

    Tagging follow;

    StreamFrameFormat()
        : follow(All)
    {}

    static StreamFrameFormat single() {
        StreamFrameFormat fmt;
        fmt.follow = None;
        return fmt;
    }
};

struct StreamFrame : StreamAnchors {
    bool follow_all;
    StreamFrameFormat fmt;

    StreamFrame(StyledStream &_ss, const StreamFrameFormat &_fmt) :
        StreamAnchors(_ss), fmt(_fmt) {
        follow_all = (fmt.follow == StreamFrameFormat::All);
    }

    void stream_frame(const Frame *frame) {
        if (follow_all) {
            if (frame->parent)
                stream_frame(frame->parent);
        }
        ss << frame;
        if (frame->loop_count) {
            ss << " [loop=" << frame->loop_count << "]" << std::endl;
        }
        ss << std::endl;
        ss << "    instance = " << frame->instance << std::endl;
        ss << "    original label = " << frame->label << std::endl;
        auto &&args = frame->args;
        for (size_t i = 0; i < args.size(); ++i) {
            ss << "    " << i << " = " << args[i] << std::endl;
        }
    }

    void stream(const Frame *frame) {
        stream_frame(frame);
    }

};

static void stream_frame(
    StyledStream &_ss, const Frame *frame, const StreamFrameFormat &_fmt) {
    StreamFrame streamer(_ss, _fmt);
    streamer.stream(frame);
}

//------------------------------------------------------------------------------
// IL MANGLING
//------------------------------------------------------------------------------

static void mangle_remap_body(Label::UserMap &um, Label *ll, Label *entry, MangleLabelMap &lmap, MangleParamMap &pmap) {
    Any enter = entry->body.enter;
    Args &args = entry->body.args;
    Args &body = ll->body.args;
    if (enter.type == TYPE_Label) {
        auto it = lmap.find(enter.label);
        if (it != lmap.end()) {
            enter = it->second;
        }
    } else if (enter.type == TYPE_Parameter) {
        auto it = pmap.find(enter.parameter);
        if (it != pmap.end()) {
            enter = first(it->second).value;
        }
    }
    ll->flags = entry->flags & LF_Reentrant;
    ll->body.copy_traits_from(entry->body);
    ll->body.enter = enter;

    size_t lasti = (args.size() - 1);
    for (size_t i = 0; i < args.size(); ++i) {
        Argument arg = args[i];
        if (arg.value.type == TYPE_Label) {
            auto it = lmap.find(arg.value.label);
            if (it != lmap.end()) {
                arg.value = it->second;
            }
        } else if (arg.value.type == TYPE_Parameter) {
            auto it = pmap.find(arg.value.parameter);
            if (it != pmap.end()) {
                if ((i == lasti) && arg.value.parameter->is_vararg()) {
                    for (auto subit = it->second.begin(); subit != it->second.end(); ++subit) {
                        body.push_back(*subit);
                    }
                    continue;
                } else {
                    arg.value = first(it->second).value;
                }
            }
        }
        body.push_back(arg);
    }

    ll->insert_into_usermap(um);
}

void evaluate(Frame *frame, Argument arg, Args &dest, bool last_param = false) {
    if (arg.value.type == TYPE_Label) {
        // do not wrap labels in closures that have been solved
        if (arg.value.label->body.is_complete()) {
            dest.push_back(Argument(arg.key, arg.value.label));
        } else {
            Label *label = arg.value.label;
            if (frame) {
                Frame *top = frame->find_frame(label);
                if (top) {
                    frame = top;
                } else if (label->body.scope_label) {
                    top = frame->find_frame(label->body.scope_label);
                    if (top) {
                        frame = top;
                    } else {
                        #if 0
                        StyledStream ss(std::cerr);
                        stream_frame(ss, frame, StreamFrameFormat());
                        ss << "warning: can't find scope label " << label->body.scope_label << " for " << label << std::endl;
                        #endif
                    }
                }
            }
            dest.push_back(Argument(arg.key, Closure::from(label, frame)));
        }
    } else if (arg.value.type == TYPE_Parameter
        && arg.value.parameter->label) {
        auto param = arg.value.parameter;
        frame = frame->find_frame(param->label);
        if (!frame) {
            StyledString ss;
            ss.out << "parameter " << param << " is unbound";
            location_error(ss.str());
        }
        // special situation: we're forwarding varargs, but assigning
        // it to a new argument key; since keys can only be mapped to
        // individual varargs, and must not be duplicated, we have these
        // options to resolve the conflict:
        // 1. the vararg keys override the new explicit key; this was the
        //    old behavior and made it possible for implicit vararg return
        //    values to override explicit reassignments, which was
        //    always surprising and unwanted, i.e. a bug.
        // 2. re-assign only the first vararg key, keeping remaining keys
        //    as they are.
        // 3. produce a compiler error when an explicit key is set, but
        //    the vararg set is larger than 1.
        // 4. treat a keyed argument in last place like any previous argument,
        //    causing only a single vararg result to be forwarded.
        // we use option 4, as it is most consistent with existing behavior,
        // and seems to be the least surprising choice.
        if (last_param && param->is_vararg() && !arg.is_keyed()) {
            // forward as-is, with keys
            for (size_t i = (size_t)param->index; i < frame->args.size(); ++i) {
                dest.push_back(frame->args[i]);
            }
        } else if ((size_t)param->index < frame->args.size()) {
            auto &&srcarg = frame->args[param->index];
            // when forwarding a vararg and the arg is not re-keyed,
            // forward the vararg key as well.
            if (param->is_vararg() && !arg.is_keyed()) {
                dest.push_back(srcarg);
            } else {
                dest.push_back(Argument(arg.key, srcarg.value));
            }
        } else {
            if (!param->is_vararg()) {
#if SCOPES_DEBUG_CODEGEN
                {
                    StyledStream ss;
                    ss << frame << " " << frame->label;
                    for (size_t i = 0; i < frame->args.size(); ++i) {
                        ss << " " << frame->args[i];
                    }
                    ss << std::endl;
                }
#endif
                StyledString ss;
                ss.out << "parameter " << param << " is out of bounds ("
                    << param->index << " >= " << (int)frame->args.size() << ")";
                location_error(ss.str());
            }
            dest.push_back(Argument(arg.key, none));
        }
    } else {
        dest.push_back(arg);
    }
}

static void evaluate_body(Frame *frame, Label *dest, Label *source) {
    Args &args = source->body.args;
    Args &body = dest->body.args;
    Args ret;
    dest->body.copy_traits_from(source->body);
    evaluate(frame, source->body.enter, ret);
    dest->body.enter = first(ret).value;
    body.clear();

    size_t lasti = (args.size() - 1);
    for (size_t i = 0; i < args.size(); ++i) {
        evaluate(frame, args[i], body, (i == lasti));
    }
}

enum MangleFlag {
    Mangle_Verbose = (1<<0),
};

static Label *mangle(Label::UserMap &um, Label *entry,
    std::vector<Parameter *> params, MangleParamMap &pmap, int verbose = 0) {
    MangleLabelMap lmap;

    std::vector<Label *> entry_scope;
    entry->build_scope(um, entry_scope);

    // remap entry point
    Label *le = Label::from(entry);
    le->set_parameters(params);
    // create new labels and map new parameters
    for (auto &&l : entry_scope) {
        Label *ll = Label::from(l);
        l->paired = ll;
        lmap.insert({l, ll});
        ll->params.reserve(l->params.size());
        for (auto &&param : l->params) {
            Parameter *pparam = Parameter::from(param);
            pmap.insert({ param, {Argument(Any(pparam))}});
            ll->append(pparam);
        }
    }

    // remap label bodies
    for (auto &&l : entry_scope) {
        Label *ll = l->paired;
        l->paired = nullptr;
        mangle_remap_body(um, ll, l, lmap, pmap);
    }
    mangle_remap_body(um, le, entry, lmap, pmap);

    if (verbose & Mangle_Verbose) {
    StyledStream ss(std::cout);
    ss << "IN[\n";
    stream_label(ss, entry, StreamLabelFormat::debug_single());
    for (auto && l : entry_scope) {
        stream_label(ss, l, StreamLabelFormat::debug_single());
    }
    ss << "]IN\n";
    ss << "OUT[\n";
    stream_label(ss, le, StreamLabelFormat::debug_single());
    for (auto && l : entry_scope) {
        auto it = lmap.find(l);
        stream_label(ss, it->second, StreamLabelFormat::debug_single());
    }
    ss << "]OUT\n";
    }

    return le;
}

static void fold_useless_labels(Label *l) {
    auto &&enter = l->body.enter;
    if (enter.type != TYPE_Label)
        return;
    if (!enter.label->is_basic_block_like())
        return;
    Label *newl = enter.label;
    // eat as many useless labels as we can
    while (newl->body.is_complete() &&
        !newl->is_reentrant() && !newl->has_params()) {
        l->body = newl->body;
        if (enter.type != TYPE_Label)
            break;
        if (!enter.label->is_basic_block_like())
            break;
        newl = enter.label;
    }
}

// inlining the arguments of an untyped scope (including continuation)
// folds arguments and types parameters
// arguments are treated as follows:
// TYPE_Unknown = type the parameter
//      type as TYPE_Unknown = leave the parameter as-is
// any other = inline the argument and remove the parameter
static Label *fold_type_label(Label::UserMap &um, Label *label, const Args &args) {
    Label::Args la;
    la.args = args;
    auto &&instances = label->instances;
    if (label->is_memoized()) {
        auto it = instances.find(la);
        if (it != instances.end())
            return it->second;
    }
    assert(!label->params.empty());

    MangleParamMap map;
    std::vector<Parameter *> newparams;
    size_t lasti = label->params.size() - 1;
    size_t srci = 0;
    for (size_t i = 0; i < label->params.size(); ++i) {
        Parameter *param = label->params[i];
        if (param->is_vararg()) {
            assert(i == lasti);
            size_t ncount = args.size();
            if (srci < ncount) {
                ncount -= srci;
                Args vargs;
                for (size_t k = 0; k < ncount; ++k) {
                    Argument value = args[srci + k];
                    if (value.value.type == TYPE_Unknown) {
                        Parameter *newparam = Parameter::from(param);
                        newparam->kind = PK_Regular;
                        newparam->type = value.value.typeref;
                        newparam->name = Symbol(SYM_Unnamed);
                        newparams.push_back(newparam);
                        vargs.push_back(Argument(value.key, newparam));
                    } else {
                        vargs.push_back(value);
                    }
                }
                map[param] = vargs;
                srci = ncount;
            } else {
                map[param] = {};
            }
        } else if (srci < args.size()) {
            Argument value = args[srci];
            if (is_unknown(value.value)) {
                Parameter *newparam = Parameter::from(param);
                if (is_typed(value.value)) {
                    if (newparam->is_typed()
                        && (newparam->type != value.value.typeref)) {
                        StyledString ss;
                        ss.out << "attempting to retype parameter of type "
                            << newparam->type << " as " << value.value.typeref;
                        location_error(ss.str());
                    } else {
                        newparam->type = value.value.typeref;
                    }
                }
                newparams.push_back(newparam);
                map[param] = {Argument(value.key, newparam)};
            } else {
                if (!srci) {
                    Parameter *newparam = Parameter::from(param);
                    newparam->type = TYPE_Nothing;
                    newparams.push_back(newparam);
                }
                map[param] = {value};
            }
            srci++;
        } else {
            map[param] = {Argument()};
            srci++;
        }
    }
    Label *newlabel = mangle(um, label, newparams, map);//, Mangle_Verbose);
    if (label->is_memoized()) {
        instances.insert({la, newlabel});
    }
    return newlabel;
}

static void map_constant_arguments(Frame *frame, Label *label, const Args &args) {
    size_t lasti = label->params.size() - 1;
    size_t srci = 0;
    for (size_t i = 0; i < label->params.size(); ++i) {
        Parameter *param = label->params[i];
        if (param->is_vararg()) {
            assert(i == lasti);
            size_t ncount = args.size();
            while (srci < ncount) {
                Argument value = args[srci];
                assert(!is_unknown(value.value));
                frame->args.push_back(value);
                srci++;
            }
        } else if (srci < args.size()) {
            Argument value = args[srci];
            assert(!is_unknown(value.value));
            frame->args.push_back(value);
            srci++;
        } else {
            frame->args.push_back(none);
            srci++;
        }
    }
}

// inlining the arguments of an untyped scope (including continuation)
// folds arguments and types parameters
// arguments are treated as follows:
// TYPE_Unknown = type the parameter
//      type as TYPE_Unknown = leave the parameter as-is
// any other = inline the argument and remove the parameter
static Label *fold_type_label_single(Frame *parent, Label *label, const Args &args) {
    assert(!label->body.is_complete());
    size_t loop_count = 0;
    if (parent && (parent->label == label)) {
        Frame *top = parent;
        parent = top->parent;
        loop_count = top->loop_count + 1;
        if (loop_count > SCOPES_MAX_RECURSIONS) {
            StyledString ss;
            ss.out << "maximum number of recursions exceeded during"
            " compile time evaluation (" << SCOPES_MAX_RECURSIONS << ")."
            " Use 'unconst' to prevent constant propagation.";
            location_error(ss.str());
        }
    }

    Label::Args la;
    la.frame = parent;
    la.args = args;
    auto &&instances = label->instances;
    if (label->is_memoized()) {
        auto it = instances.find(la);
        if (it != instances.end())
            return it->second;
    }
    assert(!label->params.empty());

    Label *newlabel = Label::from(label);
#if SCOPES_DEBUG_CODEGEN
    {
        StyledStream ss;
        ss << "fold-type-label-single " << label << " -> " << newlabel << std::endl;
        stream_label(ss, label, StreamLabelFormat::debug_single());
    }
#endif
    if (label->is_memoized()) {
        instances.insert({la, newlabel});
    }

    Frame *frame = Frame::from(parent, label, newlabel, loop_count);

    size_t lasti = label->params.size() - 1;
    size_t srci = 0;
    for (size_t i = 0; i < label->params.size(); ++i) {
        Parameter *param = label->params[i];
        if (param->is_vararg()) {
            assert(i == lasti);
            size_t ncount = args.size();
            while (srci < ncount) {
                Argument value = args[srci];
                if (is_unknown(value.value)) {
                    Parameter *newparam = Parameter::from(param);
                    newparam->kind = PK_Regular;
                    newparam->type = value.value.typeref;
                    newparam->name = Symbol(SYM_Unnamed);
                    newlabel->append(newparam);
                    frame->args.push_back(Argument(value.key, newparam));
                } else {
                    frame->args.push_back(value);
                }
                srci++;
            }
        } else if (srci < args.size()) {
            Argument value = args[srci];
            if (is_unknown(value.value)) {
                Parameter *newparam = Parameter::from(param);
                if (is_typed(value.value)) {
                    if (newparam->is_typed()
                        && (newparam->type != value.value.typeref)) {
                        StyledString ss;
                        ss.out << "attempting to retype parameter of type "
                            << newparam->type << " as " << value.value.typeref;
                        location_error(ss.str());
                    } else {
                        newparam->type = value.value.typeref;
                    }
                }
                newlabel->append(newparam);
                frame->args.push_back(Argument(value.key, newparam));
            } else {
                if (!srci) {
                    Parameter *newparam = Parameter::from(param);
                    newparam->type = TYPE_Nothing;
                    newlabel->append(newparam);
                }
                frame->args.push_back(value);
            }
            srci++;
        } else {
            frame->args.push_back(none);
            srci++;
        }
    }

    evaluate_body(frame, newlabel, label);

    return newlabel;
}

typedef std::vector<const Type *> ArgTypes;

static Label *typify_single(Frame *frame, Label *label, const ArgTypes &argtypes) {
    assert(!label->params.empty());

    Args args;
    args.reserve(argtypes.size() + 1);
    args = { Argument(untyped()) };
    for (size_t i = 0; i < argtypes.size(); ++i) {
        args.push_back(Argument(unknown_of(argtypes[i])));
    }

    return fold_type_label_single(frame, label, args);
}

static Label *fold_typify_single(Frame *frame, Label *label, const Args &values) {
    assert(!label->params.empty());

    Args args;
    args.reserve(values.size() + 1);
    args = { Argument(untyped()) };
    for (size_t i = 0; i < values.size(); ++i) {
        args.push_back(values[i]);
    }

    return fold_type_label_single(frame, label, args);
}

//------------------------------------------------------------------------------
// C BRIDGE (CLANG)
//------------------------------------------------------------------------------

class CVisitor : public clang::RecursiveASTVisitor<CVisitor> {
public:

    typedef std::unordered_map<Symbol, const Type *, Symbol::Hash> NamespaceMap;

    Scope *dest;
    clang::ASTContext *Context;
    std::unordered_map<clang::RecordDecl *, bool> record_defined;
    std::unordered_map<clang::EnumDecl *, bool> enum_defined;
    NamespaceMap named_structs;
    NamespaceMap named_classes;
    NamespaceMap named_unions;
    NamespaceMap named_enums;
    NamespaceMap typedefs;

    CVisitor() : dest(nullptr), Context(NULL) {
        const Type *T = Typename(String::from("__builtin_va_list"));
        auto tnt = cast<TypenameType>(const_cast<Type*>(T));
        tnt->finalize(Array(TYPE_I8, sizeof(va_list)));
        typedefs.insert({Symbol("__builtin_va_list"), T });
    }

    const Anchor *anchorFromLocation(clang::SourceLocation loc) {
        auto &SM = Context->getSourceManager();

        auto PLoc = SM.getPresumedLoc(loc);

        if (PLoc.isValid()) {
            auto fname = PLoc.getFilename();
            const String *strpath = String::from_cstr(fname);
            Symbol key(strpath);
            SourceFile *sf = SourceFile::from_file(key);
            if (!sf) {
                sf = SourceFile::from_string(key, Symbol(SYM_Unnamed).name());
            }
            return Anchor::from(sf, PLoc.getLine(), PLoc.getColumn(),
                SM.getFileOffset(loc));
        }

        return get_active_anchor();
    }

    void SetContext(clang::ASTContext * ctx, Scope *_dest) {
        Context = ctx;
        dest = _dest;
    }

    void GetFields(TypenameType *tni, clang::RecordDecl * rd) {
        auto &rl = Context->getASTRecordLayout(rd);

        bool is_union = rd->isUnion();

        if (is_union) {
            tni->super_type = TYPE_CUnion;
        } else {
            tni->super_type = TYPE_CStruct;
        }

        std::vector<Argument> args;
        //auto anchors = new std::vector<Anchor>();
        //StyledStream ss;
        const Type *ST = tni;

        size_t sz = 0;
        size_t al = 1;
        bool packed = false;
        bool has_bitfield = false;
        for(clang::RecordDecl::field_iterator it = rd->field_begin(), end = rd->field_end(); it != end; ++it) {
            clang::DeclarationName declname = it->getDeclName();

            if (!it->isAnonymousStructOrUnion() && !declname) {
                continue;
            }

            clang::QualType FT = it->getType();
            const Type *fieldtype = TranslateType(FT);

            if(it->isBitField()) {
                has_bitfield = true;
                break;
            }

            //unsigned width = it->getBitWidthValue(*Context);

            Symbol name = it->isAnonymousStructOrUnion() ?
                Symbol("") : Symbol(String::from_stdstring(declname.getAsString()));

            if (!is_union) {
                //ss << "type " << ST << " field " << name << " : " << fieldtype << std::endl;

                unsigned idx = it->getFieldIndex();
                auto offset = rl.getFieldOffset(idx) / 8;
                size_t newsz = sz;
                if (!packed) {
                    size_t etal = align_of(fieldtype);
                    newsz = ::align(sz, etal);
                    al = std::max(al, etal);
                }
                if (newsz != offset) {
                    //ss << "offset mismatch " << newsz << " != " << offset << std::endl;
                    if (newsz < offset) {
                        size_t pad = offset - newsz;
                        args.push_back(Argument(SYM_Unnamed,
                            Array(TYPE_U8, pad)));
                    } else {
                        // our computed offset is later than the real one
                        // structure is likely packed
                        packed = true;
                    }
                }
                sz = offset + size_of(fieldtype);
            } else {
                sz = std::max(sz, size_of(fieldtype));
                al = std::max(al, align_of(fieldtype));
            }

            args.push_back(Argument(name, unknown_of(fieldtype)));
        }
        if (packed) {
            al = 1;
        }
        bool explicit_alignment = false;
        if (has_bitfield) {
            // ignore for now and hope that an underlying union fixes the problem
        } else {
            size_t needalign = rl.getAlignment().getQuantity();
            size_t needsize = rl.getSize().getQuantity();
            #if 0
            if (!is_union && (needalign != al)) {
                al = needalign;
                explicit_alignment = true;
            }
            #endif
            sz = ::align(sz, al);
            bool align_ok = (al == needalign);
            bool size_ok = (sz == needsize);
            if (!(align_ok && size_ok)) {
#ifdef SCOPES_DEBUG
                StyledStream ss;
                auto anchor = anchorFromLocation(rd->getSourceRange().getBegin());
                if (al != needalign) {
                    ss << anchor << " type " << ST << " alignment mismatch: " << al << " != " << needalign << std::endl;
                }
                if (sz != needsize) {
                    ss << anchor << " type " << ST << " size mismatch: " << sz << " != " << needsize << std::endl;
                }
                #if 0
                set_active_anchor(anchor);
                location_error(String::from("clang-bridge: imported record doesn't fit"));
                #endif
#endif
            }
        }

        tni->finalize(is_union?MixedUnion(args):MixedTuple(args, packed, explicit_alignment?al:0));
    }

    const Type *get_typename(Symbol name, NamespaceMap &map) {
        if (name != SYM_Unnamed) {
            auto it = map.find(name);
            if (it != map.end()) {
                return it->second;
            }
            const Type *T = Typename(name.name());
            auto ok = map.insert({name, T});
            assert(ok.second);
            return T;
        }
        return Typename(name.name());
    }

    const Type *TranslateRecord(clang::RecordDecl *rd) {
        Symbol name = SYM_Unnamed;
        if (rd->isAnonymousStructOrUnion()) {
            auto tdn = rd->getTypedefNameForAnonDecl();
            if (tdn) {
                name = Symbol(String::from_stdstring(tdn->getName().data()));
            }
        } else {
            name = Symbol(String::from_stdstring(rd->getName().data()));
        }

        const Type *struct_type = nullptr;
        if (rd->isUnion()) {
            struct_type = get_typename(name, named_unions);
        } else if (rd->isStruct()) {
            struct_type = get_typename(name, named_structs);
        } else if (rd->isClass()) {
            struct_type = get_typename(name, named_classes);
        } else {
            set_active_anchor(anchorFromLocation(rd->getSourceRange().getBegin()));
            StyledString ss;
            ss.out << "clang-bridge: can't translate record of unuspported type " << name;
            location_error(ss.str());
        }

        clang::RecordDecl * defn = rd->getDefinition();
        if (defn && !record_defined[rd]) {
            record_defined[rd] = true;

            auto tni = cast<TypenameType>(const_cast<Type *>(struct_type));
            if (tni->finalized()) {
                set_active_anchor(anchorFromLocation(rd->getSourceRange().getBegin()));
                StyledString ss;
                ss.out << "clang-bridge: duplicate body defined for type " << struct_type;
                location_error(ss.str());
            }

            GetFields(tni, defn);

            if (name != SYM_Unnamed) {
                Any target = none;
                // don't overwrite names already bound
                if (!dest->lookup(name, target)) {
                    dest->bind(name, struct_type);
                }
            }
        }

        return struct_type;
    }

    Any make_integer(const Type *T, int64_t v) {
        auto it = cast<IntegerType>(T);
        if (it->issigned) {
            switch(it->width) {
            case 8: return Any((int8_t)v);
            case 16: return Any((int16_t)v);
            case 32: return Any((int32_t)v);
            case 64: return Any((int64_t)v);
            default: assert(false); return none;
            }
        } else {
            switch(it->width) {
            case 8: return Any((uint8_t)v);
            case 16: return Any((uint16_t)v);
            case 32: return Any((uint32_t)v);
            case 64: return Any((uint64_t)v);
            default: assert(false); return none;
            }
        }
    }

    const Type *TranslateEnum(clang::EnumDecl *ed) {

        Symbol name(String::from_stdstring(ed->getName()));

        const Type *enum_type = get_typename(name, named_enums);

        //const Anchor *anchor = anchorFromLocation(ed->getIntegerTypeRange().getBegin());

        clang::EnumDecl * defn = ed->getDefinition();
        if (defn && !enum_defined[ed]) {
            enum_defined[ed] = true;

            const Type *tag_type = TranslateType(ed->getIntegerType());

            auto tni = cast<TypenameType>(const_cast<Type *>(enum_type));
            tni->super_type = TYPE_CEnum;
            tni->finalize(tag_type);

            for (auto it : ed->enumerators()) {
                //const Anchor *anchor = anchorFromLocation(it->getSourceRange().getBegin());
                auto &val = it->getInitVal();

                auto name = Symbol(String::from_stdstring(it->getName().data()));
                auto value = make_integer(tag_type, val.getExtValue());
                value.type = enum_type;

                tni->bind(name, value);
                dest->bind(name, value);
            }
        }

        return enum_type;
    }

    bool always_immutable(clang::QualType T) {
        using namespace clang;
        const clang::Type *Ty = T.getTypePtr();
        assert(Ty);
        switch (Ty->getTypeClass()) {
        case clang::Type::Elaborated: {
            const ElaboratedType *et = dyn_cast<ElaboratedType>(Ty);
            return always_immutable(et->getNamedType());
        } break;
        case clang::Type::Paren: {
            const ParenType *pt = dyn_cast<ParenType>(Ty);
            return always_immutable(pt->getInnerType());
        } break;
        case clang::Type::Typedef:
        case clang::Type::Record:
        case clang::Type::Enum:
            break;
        case clang::Type::Builtin:
            switch (cast<BuiltinType>(Ty)->getKind()) {
            case clang::BuiltinType::Void:
            case clang::BuiltinType::Bool:
            case clang::BuiltinType::Char_S:
            case clang::BuiltinType::SChar:
            case clang::BuiltinType::Char_U:
            case clang::BuiltinType::UChar:
            case clang::BuiltinType::Short:
            case clang::BuiltinType::UShort:
            case clang::BuiltinType::Int:
            case clang::BuiltinType::UInt:
            case clang::BuiltinType::Long:
            case clang::BuiltinType::ULong:
            case clang::BuiltinType::LongLong:
            case clang::BuiltinType::ULongLong:
            case clang::BuiltinType::WChar_S:
            case clang::BuiltinType::WChar_U:
            case clang::BuiltinType::Char16:
            case clang::BuiltinType::Char32:
            case clang::BuiltinType::Half:
            case clang::BuiltinType::Float:
            case clang::BuiltinType::Double:
            case clang::BuiltinType::LongDouble:
            case clang::BuiltinType::NullPtr:
            case clang::BuiltinType::UInt128:
            default:
                break;
            }
        case clang::Type::Complex:
        case clang::Type::LValueReference:
        case clang::Type::RValueReference:
            break;
        case clang::Type::Decayed: {
            const clang::DecayedType *DTy = cast<clang::DecayedType>(Ty);
            return always_immutable(DTy->getDecayedType());
        } break;
        case clang::Type::Pointer:
        case clang::Type::VariableArray:
        case clang::Type::IncompleteArray:
        case clang::Type::ConstantArray:
            break;
        case clang::Type::ExtVector:
        case clang::Type::Vector: return true;
        case clang::Type::FunctionNoProto:
        case clang::Type::FunctionProto: return true;
        case clang::Type::ObjCObject: break;
        case clang::Type::ObjCInterface: break;
        case clang::Type::ObjCObjectPointer: break;
        case clang::Type::BlockPointer:
        case clang::Type::MemberPointer:
        case clang::Type::Atomic:
        default:
            break;
        }
        if (T.isLocalConstQualified())
            return true;
        return false;
    }

    uint64_t PointerFlags(clang::QualType T) {
        uint64_t flags = 0;
        if (always_immutable(T))
            flags |= PTF_NonWritable;
        return flags;
    }

    // generate a storage type that matches alignment and size of the
    // original type; used for types that we can't translate
    const Type *TranslateStorage(clang::QualType T) {
        // retype as a tuple of aligned integer and padding byte array
        size_t sz = Context->getTypeSize(T);
        size_t al = Context->getTypeAlign(T);
        assert (sz % 8 == 0);
        assert (al % 8 == 0);
        sz = (sz + 7) / 8;
        al = (al + 7) / 8;
        assert (sz > al);
        std::vector<const Type *> fields;
        const Type *TB = Integer(al * 8, false);
        fields.push_back(TB);
        size_t pad = sz - al;
        if (pad)
            fields.push_back(Array(TYPE_U8, pad));
        return Tuple(fields);
    }

    const Type *TranslateType(clang::QualType T) {
        using namespace clang;

        const clang::Type *Ty = T.getTypePtr();
        assert(Ty);

        switch (Ty->getTypeClass()) {
        case clang::Type::Attributed: {
            const AttributedType *at = dyn_cast<AttributedType>(Ty);
            // we probably want to eventually handle some of the attributes
            // but for now, ignore any attribute
            return TranslateType(at->getEquivalentType());
        } break;
        case clang::Type::Elaborated: {
            const ElaboratedType *et = dyn_cast<ElaboratedType>(Ty);
            return TranslateType(et->getNamedType());
        } break;
        case clang::Type::Paren: {
            const ParenType *pt = dyn_cast<ParenType>(Ty);
            return TranslateType(pt->getInnerType());
        } break;
        case clang::Type::Typedef: {
            const TypedefType *tt = dyn_cast<TypedefType>(Ty);
            TypedefNameDecl * td = tt->getDecl();
            auto it = typedefs.find(
                Symbol(String::from_stdstring(td->getName().data())));
            if (it == typedefs.end()) {
                return TYPE_Void;
            }
            return it->second;
        } break;
        case clang::Type::Record: {
            const RecordType *RT = dyn_cast<RecordType>(Ty);
            RecordDecl * rd = RT->getDecl();
            return TranslateRecord(rd);
        }  break;
        case clang::Type::Enum: {
            const clang::EnumType *ET = dyn_cast<clang::EnumType>(Ty);
            EnumDecl * ed = ET->getDecl();
            return TranslateEnum(ed);
        } break;
        case clang::Type::SubstTemplateTypeParm: {
            return TranslateType(T.getCanonicalType());
        } break;
        case clang::Type::TemplateSpecialization: {
            return TranslateStorage(T);
        } break;
        case clang::Type::Builtin:
            switch (cast<BuiltinType>(Ty)->getKind()) {
            case clang::BuiltinType::Void:
                return TYPE_Void;
            case clang::BuiltinType::Bool:
                return TYPE_Bool;
            case clang::BuiltinType::Char_S:
            case clang::BuiltinType::SChar:
            case clang::BuiltinType::Char_U:
            case clang::BuiltinType::UChar:
            case clang::BuiltinType::Short:
            case clang::BuiltinType::UShort:
            case clang::BuiltinType::Int:
            case clang::BuiltinType::UInt:
            case clang::BuiltinType::Long:
            case clang::BuiltinType::ULong:
            case clang::BuiltinType::LongLong:
            case clang::BuiltinType::ULongLong:
            case clang::BuiltinType::WChar_S:
            case clang::BuiltinType::WChar_U:
            case clang::BuiltinType::Char16:
            case clang::BuiltinType::Char32: {
                int sz = Context->getTypeSize(T);
                return Integer(sz, !Ty->isUnsignedIntegerType());
            } break;
            case clang::BuiltinType::Half: return TYPE_F16;
            case clang::BuiltinType::Float:
                return TYPE_F32;
            case clang::BuiltinType::Double:
                return TYPE_F64;
            case clang::BuiltinType::LongDouble: return TYPE_F80;
            case clang::BuiltinType::NullPtr:
            case clang::BuiltinType::UInt128:
            default:
                break;
            }
        case clang::Type::Complex:
        case clang::Type::LValueReference: {
            const clang::LValueReferenceType *PTy =
                cast<clang::LValueReferenceType>(Ty);
            QualType ETy = PTy->getPointeeType();
            return Pointer(TranslateType(ETy), PointerFlags(ETy), SYM_Unnamed);
        } break;
        case clang::Type::RValueReference:
            break;
        case clang::Type::Decayed: {
            const clang::DecayedType *DTy = cast<clang::DecayedType>(Ty);
            return TranslateType(DTy->getDecayedType());
        } break;
        case clang::Type::Pointer: {
            const clang::PointerType *PTy = cast<clang::PointerType>(Ty);
            QualType ETy = PTy->getPointeeType();
            return Pointer(TranslateType(ETy), PointerFlags(ETy), SYM_Unnamed);
        } break;
        case clang::Type::VariableArray:
            break;
        case clang::Type::IncompleteArray: {
            const IncompleteArrayType *ATy = cast<IncompleteArrayType>(Ty);
            QualType ETy = ATy->getElementType();
            return Pointer(TranslateType(ETy), PointerFlags(ETy), SYM_Unnamed);
        } break;
        case clang::Type::ConstantArray: {
            const ConstantArrayType *ATy = cast<ConstantArrayType>(Ty);
            const Type *at = TranslateType(ATy->getElementType());
            uint64_t sz = ATy->getSize().getZExtValue();
            return Array(at, sz);
        } break;
        case clang::Type::ExtVector:
        case clang::Type::Vector: {
            const clang::VectorType *VT = cast<clang::VectorType>(T);
            const Type *at = TranslateType(VT->getElementType());
            uint64_t n = VT->getNumElements();
            return Vector(at, n);
        } break;
        case clang::Type::FunctionNoProto:
        case clang::Type::FunctionProto: {
            const clang::FunctionType *FT = cast<clang::FunctionType>(Ty);
            return TranslateFuncType(FT);
        } break;
        case clang::Type::ObjCObject: break;
        case clang::Type::ObjCInterface: break;
        case clang::Type::ObjCObjectPointer: break;
        case clang::Type::BlockPointer:
        case clang::Type::MemberPointer:
        case clang::Type::Atomic:
        default:
            break;
        }
        location_error(format("clang-bridge: cannot convert type: %s (%s)",
            T.getAsString().c_str(),
            Ty->getTypeClassName()));
        return TYPE_Void;
    }

    const Type *TranslateFuncType(const clang::FunctionType * f) {

        clang::QualType RT = f->getReturnType();

        const Type *returntype = TranslateType(RT);

        uint64_t flags = 0;

        std::vector<const Type *> argtypes;

        const clang::FunctionProtoType * proto = f->getAs<clang::FunctionProtoType>();
        if(proto) {
            if (proto->isVariadic()) {
                flags |= FF_Variadic;
            }
            for(size_t i = 0; i < proto->getNumParams(); i++) {
                clang::QualType PT = proto->getParamType(i);
                argtypes.push_back(TranslateType(PT));
            }
        }

        return Function(returntype, argtypes, flags);
    }

    void exportType(Symbol name, const Type *type) {
        dest->bind(name, type);
    }

    void exportExtern(Symbol name, const Type *type,
        const Anchor *anchor) {
        Any value(name);
        value.type = Extern(type);
        dest->bind(name, value);
    }

    bool TraverseRecordDecl(clang::RecordDecl *rd) {
        if (rd->isFreeStanding()) {
            TranslateRecord(rd);
        }
        return true;
    }

    bool TraverseEnumDecl(clang::EnumDecl *ed) {
        if (ed->isFreeStanding()) {
            TranslateEnum(ed);
        }
        return true;
    }

    bool TraverseVarDecl(clang::VarDecl *vd) {
        if (vd->isExternC()) {
            const Anchor *anchor = anchorFromLocation(vd->getSourceRange().getBegin());

            exportExtern(
                String::from_stdstring(vd->getName().data()),
                TranslateType(vd->getType()),
                anchor);
        }

        return true;
    }

    bool TraverseTypedefDecl(clang::TypedefDecl *td) {

        //const Anchor *anchor = anchorFromLocation(td->getSourceRange().getBegin());

        const Type *type = TranslateType(td->getUnderlyingType());

        Symbol name = Symbol(String::from_stdstring(td->getName().data()));

        typedefs.insert({name, type});
        exportType(name, type);

        return true;
    }

    bool TraverseLinkageSpecDecl(clang::LinkageSpecDecl *ct) {
        if (ct->getLanguage() == clang::LinkageSpecDecl::lang_c) {
            return clang::RecursiveASTVisitor<CVisitor>::TraverseLinkageSpecDecl(ct);
        }
        return false;
    }

    bool TraverseClassTemplateDecl(clang::ClassTemplateDecl *ct) {
        return false;
    }

    bool TraverseFunctionDecl(clang::FunctionDecl *f) {
        clang::DeclarationName DeclName = f->getNameInfo().getName();
        std::string FuncName = DeclName.getAsString();
        const clang::FunctionType * fntyp = f->getType()->getAs<clang::FunctionType>();

        if(!fntyp)
            return true;

        if(f->getStorageClass() == clang::SC_Static) {
            return true;
        }

        const Type *functype = TranslateFuncType(fntyp);

        std::string InternalName = FuncName;
        clang::AsmLabelAttr * asmlabel = f->getAttr<clang::AsmLabelAttr>();
        if(asmlabel) {
            InternalName = asmlabel->getLabel();
            #ifndef __linux__
                //In OSX and Windows LLVM mangles assembler labels by adding a '\01' prefix
                InternalName.insert(InternalName.begin(), '\01');
            #endif
        }
        const Anchor *anchor = anchorFromLocation(f->getSourceRange().getBegin());

        exportExtern(Symbol(String::from_stdstring(FuncName)),
            functype, anchor);

        return true;
    }

};

class CodeGenProxy : public clang::ASTConsumer {
public:
    Scope *dest;

    CVisitor visitor;

    CodeGenProxy(Scope *dest_) : dest(dest_) {}
    virtual ~CodeGenProxy() {}

    virtual void Initialize(clang::ASTContext &Context) {
        visitor.SetContext(&Context, dest);
    }

    virtual bool HandleTopLevelDecl(clang::DeclGroupRef D) {
        for (clang::DeclGroupRef::iterator b = D.begin(), e = D.end(); b != e; ++b)
            visitor.TraverseDecl(*b);
        return true;
    }
};

// see ASTConsumers.h for more utilities
class BangEmitLLVMOnlyAction : public clang::EmitLLVMOnlyAction {
public:
    Scope *dest;

    BangEmitLLVMOnlyAction(Scope *dest_) :
        EmitLLVMOnlyAction((llvm::LLVMContext *)LLVMGetGlobalContext()),
        dest(dest_)
    {
    }

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI,
                                                 clang::StringRef InFile) override {

        std::vector< std::unique_ptr<clang::ASTConsumer> > consumers;
        consumers.push_back(clang::EmitLLVMOnlyAction::CreateASTConsumer(CI, InFile));
        consumers.push_back(llvm::make_unique<CodeGenProxy>(dest));
        return llvm::make_unique<clang::MultiplexConsumer>(std::move(consumers));
    }
};

static LLVMExecutionEngineRef ee = nullptr;

static void init_llvm() {
    LLVMEnablePrettyStackTrace();
    LLVMLinkInMCJIT();
    //LLVMLinkInInterpreter();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmParser();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeDisassembler();
}

static std::vector<LLVMModuleRef> llvm_c_modules;

static void add_c_macro(clang::Preprocessor & PP,
    const clang::IdentifierInfo * II,
    clang::MacroDirective * MD, Scope *scope, std::list< std::pair<Symbol, Symbol> > &aliases) {
    if(!II->hasMacroDefinition())
        return;
    clang::MacroInfo * MI = MD->getMacroInfo();
    if(MI->isFunctionLike())
        return;
    bool negate = false;
    const clang::Token * Tok;
    auto numtokens = MI->getNumTokens();
    if(numtokens == 2 && MI->getReplacementToken(0).is(clang::tok::minus)) {
        negate = true;
        Tok = &MI->getReplacementToken(1);
    } else if(numtokens == 1) {
        Tok = &MI->getReplacementToken(0);
    } else {
        return;
    }

    if ((numtokens == 1) && Tok->is(clang::tok::identifier)) {
        // aliases need to be resolved once the whole namespace is known
        const String *name = String::from_cstr(II->getName().str().c_str());
        const String *value = String::from_cstr(Tok->getIdentifierInfo()->getName().str().c_str());
        aliases.push_back({ Symbol(name), Symbol(value) });
        return;
    }

    if ((numtokens == 1) && Tok->is(clang::tok::string_literal)) {
        clang::Token tokens[] = { *Tok };
        clang::StringLiteralParser Literal(tokens, PP, false);
        const String *name = String::from_cstr(II->getName().str().c_str());
        std::string svalue = Literal.GetString();
        const String *value = String::from(svalue.c_str(), svalue.size());
        scope->bind(Symbol(name), value);
        return;
    }

    if(Tok->isNot(clang::tok::numeric_constant))
        return;

    clang::SmallString<64> IntegerBuffer;
    bool NumberInvalid = false;
    clang::StringRef Spelling = PP.getSpelling(*Tok, IntegerBuffer, &NumberInvalid);
    clang::NumericLiteralParser Literal(Spelling, Tok->getLocation(), PP);
    if(Literal.hadError)
        return;
    const String *name = String::from_cstr(II->getName().str().c_str());
    std::string suffix;
    if (Literal.hasUDSuffix()) {
        suffix = Literal.getUDSuffix();
        std::cout << "TODO: macro literal suffix: " << suffix << std::endl;
    }
    if(Literal.isFloatingLiteral()) {
        llvm::APFloat Result(0.0);
        Literal.GetFloatValue(Result);
        double V = Result.convertToDouble();
        if (negate)
            V = -V;
        scope->bind(Symbol(name), V);
    } else {
        llvm::APInt Result(64,0);
        Literal.GetIntegerValue(Result);
        int64_t i = Result.getSExtValue();
        if (negate)
            i = -i;
        scope->bind(Symbol(name), i);
    }
}

static Scope *import_c_module (
    const std::string &path, const std::vector<std::string> &args,
    const char *buffer = nullptr) {
    using namespace clang;

    std::vector<const char *> aargs;
    aargs.push_back("clang");
    aargs.push_back(path.c_str());
    aargs.push_back("-I");
    aargs.push_back(scopes_clang_include_dir);
    aargs.push_back("-I");
    aargs.push_back(scopes_include_dir);
    for (size_t i = 0; i < args.size(); ++i) {
        aargs.push_back(args[i].c_str());
    }

    CompilerInstance compiler;
    compiler.setInvocation(createInvocationFromCommandLine(aargs));

    if (buffer) {
        auto &opts = compiler.getPreprocessorOpts();

        llvm::MemoryBuffer * membuffer =
            llvm::MemoryBuffer::getMemBuffer(buffer, "<buffer>").release();

        opts.addRemappedFile(path, membuffer);
    }

    // Create the compilers actual diagnostics engine.
    compiler.createDiagnostics();

    // Infer the builtin include path if unspecified.
    //~ if (compiler.getHeaderSearchOpts().UseBuiltinIncludes &&
        //~ compiler.getHeaderSearchOpts().ResourceDir.empty())
        //~ compiler.getHeaderSearchOpts().ResourceDir =
            //~ CompilerInvocation::GetResourcesPath(scopes_argv[0], MainAddr);

    LLVMModuleRef M = NULL;


    Scope *result = Scope::from();

    // Create and execute the frontend to generate an LLVM bitcode module.
    std::unique_ptr<CodeGenAction> Act(new BangEmitLLVMOnlyAction(result));
    if (compiler.ExecuteAction(*Act)) {

        clang::Preprocessor & PP = compiler.getPreprocessor();
        PP.getDiagnostics().setClient(new IgnoringDiagConsumer(), true);

        std::list< std::pair<Symbol, Symbol> > todo;
        for(Preprocessor::macro_iterator it = PP.macro_begin(false),end = PP.macro_end(false);
            it != end; ++it) {
            const IdentifierInfo * II = it->first;
            MacroDirective * MD = it->second.getLatest();

            add_c_macro(PP, II, MD, result, todo);
        }

        while (!todo.empty()) {
            auto sz = todo.size();
            for (auto it = todo.begin(); it != todo.end();) {
                Any value = none;
                if (result->lookup(it->second, value)) {
                    result->bind(it->first, value);
                    auto oldit = it++;
                    todo.erase(oldit);
                } else {
                    it++;
                }
            }
            // couldn't resolve any more keys, abort
            if (todo.size() == sz) break;
        }

        M = (LLVMModuleRef)Act->takeModule().release();
        assert(M);
        llvm_c_modules.push_back(M);
        assert(ee);
        LLVMAddModule(ee, M);
        return result;
    } else {
        location_error(String::from("compilation failed"));
    }

    return nullptr;
}

//------------------------------------------------------------------------------
// INTERPRETER
//------------------------------------------------------------------------------

static void apply_type_error(const Any &enter) {
    StyledString ss;
    ss.out << "don't know how to apply value of type " << enter.type;
    location_error(ss.str());
}

template<int mincount, int maxcount>
inline int checkargs(size_t argsize, bool allow_overshoot = false) {
    int count = (int)argsize - 1;
    if ((mincount <= 0) && (maxcount == -1)) {
        return count;
    }

    if ((maxcount >= 0) && (count > maxcount)) {
        if (allow_overshoot) {
            count = maxcount;
        } else {
            location_error(
                format("at most %i argument(s) expected, got %i", maxcount, count));
        }
    }
    if ((mincount >= 0) && (count < mincount)) {
        location_error(
            format("at least %i argument(s) expected, got %i", mincount, count));
    }
    return count;
}

static void *global_c_namespace = nullptr;

static bool signal_abort = false;
void f_abort() {
    on_shutdown();
    if (SCOPES_EARLY_ABORT || signal_abort) {
        std::abort();
    } else {
        exit(1);
    }
}


void f_exit(int c) {
    on_shutdown();
    exit(c);
}

static void print_exception(const Any &value) {
    auto cerr = StyledStream(std::cerr);
    if (value.type == TYPE_Exception) {
        const Exception *exc = value;
        if (exc->anchor) {
            cerr << exc->anchor << " ";
        }
        cerr << Style_Error << "error:" << Style_None << " "
            << exc->msg->data << std::endl;
        if (exc->anchor) {
            exc->anchor->stream_source_line(cerr);
        }
    } else {
        cerr << "exception raised: " << value << std::endl;
    }
}

static void default_exception_handler(const Any &value) {
    print_exception(value);
    f_abort();
}

static int integer_type_bit_size(const Type *T) {
    return (int)cast<IntegerType>(T)->width;
}

template<typename T>
static T cast_number(const Any &value) {
    auto ST = storage_type(value.type);
    auto it = dyn_cast<IntegerType>(ST);
    if (it) {
        if (it->issigned) {
            switch(it->width) {
            case 8: return (T)value.i8;
            case 16: return (T)value.i16;
            case 32: return (T)value.i32;
            case 64: return (T)value.i64;
            default: break;
            }
        } else {
            switch(it->width) {
            case 1: return (T)value.i1;
            case 8: return (T)value.u8;
            case 16: return (T)value.u16;
            case 32: return (T)value.u32;
            case 64: return (T)value.u64;
            default: break;
            }
        }
    }
    auto ft = dyn_cast<RealType>(ST);
    if (ft) {
        switch(ft->width) {
        case 32: return (T)value.f32;
        case 64: return (T)value.f64;
        default: break;
        }
    }
    StyledString ss;
    ss.out << "can not extract constant from ";
    if (value.is_const()) {
        ss.out << "value of type " << value.type;
    } else {
        ss.out << "variable of type " << value.indirect_type();
    }
    location_error(ss.str());
    return 0;
}

//------------------------------------------------------------------------------
// PLATFORM ABI
//------------------------------------------------------------------------------

// life is unfair, which is why we need to implement the remaining platform ABI
// support in the front-end, particularly whether an argument is passed by
// value or not.

// based on x86-64 PS ABI (SystemV)
#define DEF_ABI_CLASS_NAMES \
    /* This class consists of integral types that fit into one of the general */ \
    /* purpose registers. */ \
    T(INTEGER) \
    T(INTEGERSI) \
    /* special types for windows, not used anywhere else */ \
    T(INTEGERSI16) \
    T(INTEGERSI8) \
    /* The class consists of types that fit into a vector register. */ \
    T(SSE) \
    T(SSESF) \
    T(SSEDF) \
    /* The class consists of types that fit into a vector register and can be */ \
    /* passed and returned in the upper bytes of it. */ \
    T(SSEUP) \
    /* These classes consists of types that will be returned via the x87 FPU */ \
    T(X87) \
    T(X87UP) \
    /* This class consists of types that will be returned via the x87 FPU */ \
    T(COMPLEX_X87) \
    /* This class is used as initializer in the algorithms. It will be used for */ \
    /* padding and empty structures and unions. */ \
    T(NO_CLASS) \
    /* This class consists of types that will be passed and returned in memory */ \
    /* via the stack. */ \
    T(MEMORY)

enum ABIClass {
#define T(X) ABI_CLASS_ ## X,
    DEF_ABI_CLASS_NAMES
#undef T
};

static const char *abi_class_to_string(ABIClass class_) {
    switch(class_) {
    #define T(X) case ABI_CLASS_ ## X: return #X;
    DEF_ABI_CLASS_NAMES
    #undef T
    default: return "?";
    }
}

#undef DEF_ABI_CLASS_NAMES

const size_t MAX_ABI_CLASSES = 4;

#ifdef SCOPES_WIN32
#else
// x86-64 PS ABI based on https://www.uclibc.org/docs/psABI-x86_64.pdf

static ABIClass merge_abi_classes(ABIClass class1, ABIClass class2) {
    if (class1 == class2)
        return class1;

    if (class1 == ABI_CLASS_NO_CLASS)
        return class2;
    if (class2 == ABI_CLASS_NO_CLASS)
        return class1;

    if (class1 == ABI_CLASS_MEMORY || class2 == ABI_CLASS_MEMORY)
        return ABI_CLASS_MEMORY;

    if ((class1 == ABI_CLASS_INTEGERSI && class2 == ABI_CLASS_SSESF)
        || (class2 == ABI_CLASS_INTEGERSI && class1 == ABI_CLASS_SSESF))
        return ABI_CLASS_INTEGERSI;
    if (class1 == ABI_CLASS_INTEGER || class1 == ABI_CLASS_INTEGERSI
        || class2 == ABI_CLASS_INTEGER || class2 == ABI_CLASS_INTEGERSI)
        return ABI_CLASS_INTEGER;

    if (class1 == ABI_CLASS_X87
        || class1 == ABI_CLASS_X87UP
        || class1 == ABI_CLASS_COMPLEX_X87
        || class2 == ABI_CLASS_X87
        || class2 == ABI_CLASS_X87UP
        || class2 == ABI_CLASS_COMPLEX_X87)
        return ABI_CLASS_MEMORY;

    return ABI_CLASS_SSE;
}

static size_t classify(const Type *T, ABIClass *classes, size_t offset);

static size_t classify_array_like(size_t size,
    const Type *element_type, size_t count,
    ABIClass *classes, size_t offset) {
    const size_t UNITS_PER_WORD = 8;
    size_t words = (size + UNITS_PER_WORD - 1) / UNITS_PER_WORD;
    if (size > 32)
        return 0;
    for (size_t i = 0; i < MAX_ABI_CLASSES; i++)
        classes[i] = ABI_CLASS_NO_CLASS;
    if (!words) {
        classes[0] = ABI_CLASS_NO_CLASS;
        return 1;
    }
    auto ET = element_type;
    ABIClass subclasses[MAX_ABI_CLASSES];
    size_t alignment = align_of(ET);
    size_t esize = size_of(ET);
    for (size_t i = 0; i < count; ++i) {
        offset = align(offset, alignment);
        size_t num = classify(ET, subclasses, offset % 8);
        if (!num) return 0;
        for (size_t k = 0; k < num; ++k) {
            size_t pos = offset / 8;
            classes[k + pos] =
                merge_abi_classes (subclasses[k], classes[k + pos]);
        }
        offset += esize;
    }
    if (words > 2) {
        if (classes[0] != ABI_CLASS_SSE)
            return 0;
        for (size_t i = 1; i < words; ++i) {
            if (classes[i] != ABI_CLASS_SSEUP)
                return 0;
        }
    }
    for (size_t i = 0; i < words; i++) {
        if (classes[i] == ABI_CLASS_MEMORY)
            return 0;

        if (classes[i] == ABI_CLASS_SSEUP) {
            assert(i > 0);
            if (classes[i - 1] != ABI_CLASS_SSE
                && classes[i - 1] != ABI_CLASS_SSEUP) {
                classes[i] = ABI_CLASS_SSE;
            }
        }

        if (classes[i] == ABI_CLASS_X87UP) {
            assert(i > 0);
            if(classes[i - 1] != ABI_CLASS_X87) {
                return 0;
            }
        }
    }
    return words;
}

static size_t classify(const Type *T, ABIClass *classes, size_t offset) {
    switch(T->kind()) {
    case TK_Integer:
    case TK_Extern:
    case TK_Pointer: {
        size_t size = size_of(T) + offset;
        if (size <= 4) {
            classes[0] = ABI_CLASS_INTEGERSI;
            return 1;
        } else if (size <= 8) {
            classes[0] = ABI_CLASS_INTEGER;
            return 1;
        } else if (size <= 12) {
            classes[0] = ABI_CLASS_INTEGER;
            classes[1] = ABI_CLASS_INTEGERSI;
            return 2;
        } else if (size <= 16) {
            classes[0] = ABI_CLASS_INTEGER;
            classes[1] = ABI_CLASS_INTEGER;
            return 2;
        } else {
            assert(false && "illegal type");
        }
    } break;
    case TK_Real: {
        size_t size = size_of(T);
        if (size == 4) {
            if (!(offset % 8))
                classes[0] = ABI_CLASS_SSESF;
            else
                classes[0] = ABI_CLASS_SSE;
            return 1;
        } else if (size == 8) {
            classes[0] = ABI_CLASS_SSEDF;
            return 1;
        } else {
            assert(false && "illegal type");
        }
    } break;
    case TK_ReturnLabel:
    case TK_Typename: {
        if (is_opaque(T)) {
            classes[0] = ABI_CLASS_NO_CLASS;
            return 1;
        } else {
            return classify(storage_type(T), classes, offset);
        }
    } break;
    case TK_Vector: {
        auto tt = cast<VectorType>(T);
        return classify_array_like(size_of(T),
            tt->element_type, tt->count, classes, offset);
    } break;
    case TK_Array: {
        auto tt = cast<ArrayType>(T);
        return classify_array_like(size_of(T),
            tt->element_type, tt->count, classes, offset);
    } break;
    case TK_Union: {
        auto ut = cast<UnionType>(T);
        return classify(ut->types[ut->largest_field], classes, offset);
    } break;
    case TK_Tuple: {
        const size_t UNITS_PER_WORD = 8;
        size_t size = size_of(T);
	    size_t words = (size + UNITS_PER_WORD - 1) / UNITS_PER_WORD;
        if (size > 32)
            return 0;
        for (size_t i = 0; i < MAX_ABI_CLASSES; i++)
	        classes[i] = ABI_CLASS_NO_CLASS;
        if (!words) {
            classes[0] = ABI_CLASS_NO_CLASS;
            return 1;
        }
        auto tt = cast<TupleType>(T);
        ABIClass subclasses[MAX_ABI_CLASSES];
        for (size_t i = 0; i < tt->types.size(); ++i) {
            auto ET = tt->types[i];
            if (!tt->packed)
                offset = align(offset, align_of(ET));
            size_t num = classify (ET, subclasses, offset % 8);
            if (!num) return 0;
            for (size_t k = 0; k < num; ++k) {
                size_t pos = offset / 8;
		        classes[k + pos] =
		            merge_abi_classes (subclasses[k], classes[k + pos]);
            }
            offset += size_of(ET);
        }
        if (words > 2) {
            if (classes[0] != ABI_CLASS_SSE)
                return 0;
            for (size_t i = 1; i < words; ++i) {
                if (classes[i] != ABI_CLASS_SSEUP)
                    return 0;
            }
        }
        for (size_t i = 0; i < words; i++) {
            if (classes[i] == ABI_CLASS_MEMORY)
                return 0;

            if (classes[i] == ABI_CLASS_SSEUP) {
                assert(i > 0);
                if (classes[i - 1] != ABI_CLASS_SSE
                    && classes[i - 1] != ABI_CLASS_SSEUP) {
                    classes[i] = ABI_CLASS_SSE;
                }
            }

            if (classes[i] == ABI_CLASS_X87UP) {
                assert(i > 0);
                if(classes[i - 1] != ABI_CLASS_X87) {
                    return 0;
                }
            }
        }
        return words;
    } break;
    default: {
        assert(false && "not supported in ABI");
        return 0;
    } break;
    }
    return 0;
}
#endif // SCOPES_WIN32

static size_t abi_classify(const Type *T, ABIClass *classes) {
#ifdef SCOPES_WIN32
    if (T->kind() == TK_ReturnLabel) {
        T = cast<ReturnLabelType>(T)->return_type;
    }
    classes[0] = ABI_CLASS_NO_CLASS;
    if (is_opaque(T))
        return 1;
    T = storage_type(T);
    size_t sz = size_of(T);
    if (sz > 8)
        return 0;
    switch(T->kind()) {
    case TK_Array:
    case TK_Union:
    case TK_Tuple:
        if (sz <= 1)
            classes[0] = ABI_CLASS_INTEGERSI8;
        else if (sz <= 2)
            classes[0] = ABI_CLASS_INTEGERSI16;
        else if (sz <= 4)
            classes[0] = ABI_CLASS_INTEGERSI;
        else
            classes[0] = ABI_CLASS_INTEGER;
        return 1;
    case TK_Integer:
    case TK_Extern:
    case TK_Pointer:
    case TK_Real:
    case TK_ReturnLabel:
    case TK_Typename:
    case TK_Vector:
    default:
        return 1;
    }
#else
    size_t sz = classify(T, classes, 0);
#if 0
    if (sz) {
        StyledStream ss(std::cout);
        ss << T << " -> " << sz;
        for (int i = 0; i < sz; ++i) {
            ss << " " << abi_class_to_string(classes[i]);
        }
        ss << std::endl;
    }
#endif
    return sz;
#endif
}

static bool is_memory_class(const Type *T) {
    ABIClass classes[MAX_ABI_CLASSES];
    return !abi_classify(T, classes);
}

//------------------------------------------------------------------------------
// SCC
//------------------------------------------------------------------------------

// build strongly connected component map of label graph
// uses Dijkstra's Path-based strong component algorithm
struct SCCBuilder {
    struct Group {
        size_t index;
        std::vector<Label *> labels;
    };

    std::vector<Label *> S;
    std::vector<Label *> P;
    std::unordered_map<Label *, size_t> Cmap;
    std::vector<Group> groups;
    std::unordered_map<Label *, size_t> SCCmap;
    size_t C;

    SCCBuilder() : C(0) {}

    SCCBuilder(Label *top) :
        C(0) {
        walk(top);
    }

    void stream_group(StyledStream &ss, const Group &group) {
        ss << "group #" << group.index << " (" << group.labels.size() << " labels):" << std::endl;
        for (size_t k = 0; k < group.labels.size(); ++k) {
            stream_label(ss, group.labels[k], StreamLabelFormat::single());
        }
    }

    bool is_recursive(Label *l) {
        return group(l).labels.size() > 1;
    }

    bool contains(Label *l) {
        auto it = SCCmap.find(l);
        return it != SCCmap.end();
    }

    size_t group_id(Label *l) {
        auto it = SCCmap.find(l);
        assert(it != SCCmap.end());
        return it->second;
    }

    Group &group(Label *l) {
        return groups[group_id(l)];
    }

    void walk(Label *obj) {
        Cmap[obj] = C++;
        S.push_back(obj);
        P.push_back(obj);

        int size = (int)obj->body.args.size();
        for (int i = -1; i < size; ++i) {
            Any arg = none;
            if (i == -1) {
                arg = obj->body.enter;
            } else {
                arg = obj->body.args[i].value;
            }

            if (arg.type == TYPE_Label) {
                Label *label = arg.label;

                auto it = Cmap.find(label);
                if (it == Cmap.end()) {
                    walk(label);
                } else if (!SCCmap.count(label)) {
                    size_t Cw = it->second;
                    while (true) {
                        assert(!P.empty());
                        auto it = Cmap.find(P.back());
                        assert(it != Cmap.end());
                        if (it->second <= Cw) break;
                        P.pop_back();
                    }
                }
            }
        }

        assert(!P.empty());
        if (P.back() == obj) {
            groups.emplace_back();
            Group &scc = groups.back();
            scc.index = groups.size() - 1;
            while (true) {
                assert(!S.empty());
                Label *q = S.back();
                scc.labels.push_back(q);
                SCCmap[q] = groups.size() - 1;
                S.pop_back();
                if (q == obj) {
                    break;
                }
            }
            P.pop_back();
        }
    }
};

//------------------------------------------------------------------------------
// IL->SPIR-V GENERATOR
//------------------------------------------------------------------------------

static void disassemble_spirv(std::vector<unsigned int> &contents) {
    spv_context context = spvContextCreate(SPV_ENV_UNIVERSAL_1_2);
    uint32_t options = SPV_BINARY_TO_TEXT_OPTION_PRINT;
    options |= SPV_BINARY_TO_TEXT_OPTION_INDENT;
    options |= SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES;
    //options |= SPV_BINARY_TO_TEXT_OPTION_SHOW_BYTE_OFFSET;
    if (stream_default_style == stream_ansi_style) {
        options |= SPV_BINARY_TO_TEXT_OPTION_COLOR;
    }
    spv_diagnostic diagnostic = nullptr;
    spv_result_t error =
        spvBinaryToText(context, contents.data(), contents.size(), options,
                        nullptr, &diagnostic);
    spvContextDestroy(context);
    if (error) {
        spvDiagnosticPrint(diagnostic);
        spvDiagnosticDestroy(diagnostic);
        std::cerr << "error while pretty-printing disassembly, falling back to"
            " failsafe disassembly" << std::endl;
        spv::Disassemble(std::cerr, contents);
    }
}

static void format_spv_location(StyledStream &ss, const char* source,
    const spv_position_t& position) {
    ss << Style_Location;
    StyledStream ps = StyledStream::plain(ss);
    if (source) {
        ps << source << ":";
    }
    ps << position.line << ":" << position.column;
    ss << ":" << Style_None << " ";
}

static void verify_spirv(std::vector<unsigned int> &contents) {
    spv_target_env target_env = SPV_ENV_UNIVERSAL_1_2;
    //spvtools::ValidatorOptions options;

    StyledString ss;
    spvtools::SpirvTools tools(target_env);
    tools.SetMessageConsumer([&ss](spv_message_level_t level, const char* source,
                                const spv_position_t& position,
                                const char* message) {
        switch (level) {
        case SPV_MSG_FATAL:
        case SPV_MSG_INTERNAL_ERROR:
        case SPV_MSG_ERROR:
            format_spv_location(ss.out, source, position);
            ss.out << Style_Error << "error: " << Style_None
                << message << std::endl;
            break;
        case SPV_MSG_WARNING:
            format_spv_location(ss.out, source, position);
            ss.out << Style_Warning << "warning: " << Style_None
                << message << std::endl;
            break;
        case SPV_MSG_INFO:
            format_spv_location(ss.out, source, position);
            ss.out << Style_Comment << "info: " << Style_None
                << message << std::endl;
            break;
        default:
            break;
        }
    });

    bool succeed = tools.Validate(contents);
    if (!succeed) {
        disassemble_spirv(contents);
        std::cerr << ss._ss.str();
        location_error(String::from("SPIR-V validation found errors"));
    }
}


struct SPIRVGenerator {
    struct HashFuncLabelPair {
        size_t operator ()(const std::pair<spv::Function *, Label *> &value) const {
            return
                HashLen16(std::hash<spv::Function *>()(value.first),
                    std::hash<Label *>()(value.second));
        }
    };

    typedef std::pair<spv::Function *, Parameter *> ParamKey;
    struct HashFuncParamPair {
        size_t operator ()(const ParamKey &value) const {
            return
                HashLen16(std::hash<spv::Function *>()(value.first),
                    std::hash<Parameter *>()(value.second));
        }
    };

    typedef std::pair<const Type *, uint64_t> TypeKey;
    struct HashTypeFlagsPair {
        size_t operator ()(const TypeKey &value) const {
            return
                HashLen16(std::hash<const Type *>{}(value.first),
                    std::hash<uint64_t>{}(value.second));
        }
    };

    spv::SpvBuildLogger logger;
    spv::Builder builder;
    SCCBuilder scc;

    Label *active_function;
    spv::Function *active_function_value;
    spv::Id glsl_ext_inst;

    bool use_debug_info;

    std::unordered_map<Label *, spv::Function *> label2func;
    std::unordered_map< std::pair<spv::Function *, Label *>,
        spv::Block *, HashFuncLabelPair> label2bb;
    std::vector< std::pair<Label *, Label *> > bb_label_todo;

    //std::unordered_map<Label *, LLVMValueRef> label2md;
    //std::unordered_map<SourceFile *, LLVMValueRef> file2value;
    std::unordered_map< ParamKey, spv::Id, HashFuncParamPair> param2value;

    std::unordered_map<std::pair<const Type *, uint64_t>,
        spv::Id, HashTypeFlagsPair> type_cache;

    std::unordered_map<Any, spv::Id, Any::Hash> const_cache;

    Label::UserMap user_map;

    SPIRVGenerator() :
        builder('S' << 24 | 'C' << 16 | 'O' << 8 | 'P', &logger),
        active_function(nullptr),
        active_function_value(nullptr),
        glsl_ext_inst(0),
        use_debug_info(true) {

    }

    spv::Dim dim_from_symbol(Symbol sym) {
        switch(sym.value()) {
        #define T(NAME) \
            case SYM_SPIRV_Dim ## NAME: return spv::Dim ## NAME;
            B_SPIRV_DIM()
        #undef T
            default:
                location_error(
                    String::from(
                        "IL->SPIR: unsupported dimensionality"));
                break;
        }
        return spv::DimMax;
    }

    spv::ImageFormat image_format_from_symbol(Symbol sym) {
        switch(sym.value()) {
        #define T(NAME) \
            case SYM_SPIRV_ImageFormat ## NAME: return spv::ImageFormat ## NAME;
            B_SPIRV_IMAGE_FORMAT()
        #undef T
            default:
                location_error(
                    String::from(
                        "IL->SPIR: unsupported image format"));
                break;
        }
        return spv::ImageFormatMax;
    }

    static spv::ExecutionMode execution_mode_from_symbol(Symbol sym) {
        switch(sym.value()) {
        #define T(NAME) \
            case SYM_SPIRV_ExecutionMode ## NAME: return spv::ExecutionMode ## NAME;
            B_SPIRV_EXECUTION_MODE()
        #undef T
            default:
                location_error(
                    String::from(
                        "IL->SPIR: unsupported execution mode"));
                break;
        }
        return spv::ExecutionModeMax;
    }

    spv::StorageClass storage_class_from_extern_class(Symbol sym) {
        switch(sym.value()) {
        #define T(NAME) \
            case SYM_SPIRV_StorageClass ## NAME: return spv::StorageClass ## NAME;
            B_SPIRV_STORAGE_CLASS()
        #undef T
            case SYM_Unnamed:
                location_error(
                    String::from(
                        "IL->SPIR: pointers with C storage class"
                        " are unsupported"));
                break;
            default:
                location_error(
                    String::from(
                        "IL->SPIR: unsupported storage class for pointer"));
                break;
        }
        return spv::StorageClassMax;
    }

    spv::Id argument_to_value(Any value) {
        if (value.type == TYPE_Parameter) {
            auto it = param2value.find({active_function_value, value.parameter});
            if (it == param2value.end()) {
                assert(active_function_value);
                StyledString ss;
                ss.out << "IL->SPIR: can't translate free variable " << value.parameter;
                location_error(ss.str());
            }
            return it->second;
        }
        if (value.type != TYPE_String) {
            switch(value.type->kind()) {
            case TK_Integer: {
                auto it = cast<IntegerType>(value.type);
                if (it->issigned) {
                    switch(it->width) {
                    case 8: return builder.makeIntConstant(
                        builder.makeIntegerType(8, true), value.i8);
                    case 16: return builder.makeIntConstant(
                        builder.makeIntegerType(16, true), value.i16);
                    case 32: return builder.makeIntConstant(value.i32);
                    case 64: return builder.makeInt64Constant(value.i64);
                    default: break;
                    }
                } else {
                    switch(it->width) {
                    case 1: return builder.makeBoolConstant(value.i1);
                    case 8: return builder.makeIntConstant(
                        builder.makeIntegerType(8, false), value.i8);
                    case 16: return builder.makeIntConstant(
                        builder.makeIntegerType(16, false), value.i16);
                    case 32: return builder.makeUintConstant(value.u32);
                    case 64: return builder.makeUint64Constant(value.u64);
                    default: break;
                    }
                }
                StyledString ss;
                ss.out << "IL->SPIR: unsupported integer constant type " << value.type;
                location_error(ss.str());
            } break;
            case TK_Real: {
                auto rt = cast<RealType>(value.type);
                switch(rt->width) {
                case 32: return builder.makeFloatConstant(value.f32);
                case 64: return builder.makeDoubleConstant(value.f64);
                default: break;
                }
                StyledString ss;
                ss.out << "IL->SPIR: unsupported real constant type " << value.type;
                location_error(ss.str());
            } break;
            case TK_Pointer: {
                if (is_function_pointer(value.type)) {
                    StyledString ss;
                    ss.out << "IL->SPIR: function pointer constants are unsupported";
                    location_error(ss.str());
                }
                auto pt = cast<PointerType>(value.type);
                auto val = argument_to_value(pt->unpack(value.pointer));
                auto id = builder.createVariable(spv::StorageClassFunction,
                    builder.getTypeId(val), nullptr);
                builder.getInstruction(id)->addIdOperand(val);
                return id;
            } break;
            case TK_Typename: {
                auto tn = cast<TypenameType>(value.type);
                assert(tn->finalized());
                Any storage_value = value;
                storage_value.type = tn->storage_type;
                return argument_to_value(storage_value);
            } break;
            case TK_Array: {
                auto ai = cast<ArrayType>(value.type);
                size_t count = ai->count;
                std::vector<spv::Id> values;
                for (size_t i = 0; i < count; ++i) {
                    values.push_back(argument_to_value(ai->unpack(value.pointer, i)));
                }
                return builder.makeCompositeConstant(
                    type_to_spirv_type(value.type), values);
            } break;
            case TK_Vector: {
                auto vi = cast<VectorType>(value.type);
                size_t count = vi->count;
                std::vector<spv::Id> values;
                for (size_t i = 0; i < count; ++i) {
                    values.push_back(argument_to_value(vi->unpack(value.pointer, i)));
                }
                return builder.makeCompositeConstant(
                    type_to_spirv_type(value.type), values);
            } break;
            case TK_Tuple: {
                auto ti = cast<TupleType>(value.type);
                size_t count = ti->types.size();
                std::vector<spv::Id> values;
                for (size_t i = 0; i < count; ++i) {
                    values.push_back(argument_to_value(ti->unpack(value.pointer, i)));
                }
                return builder.makeCompositeConstant(
                    type_to_spirv_type(value.type), values);
            } break;
            case TK_Union: {
                auto ui = cast<UnionType>(value.type);
                value.type = ui->tuple_type;
                return argument_to_value(value);
            } break;
            default: {
            } break;
            }
        }
        auto it = const_cache.find(value);
        if (it != const_cache.end()) {
            return it->second;
        }
        auto id = create_spirv_value(value);
        const_cache.insert({ value, id });
        return id;
    }

    spv::Id create_spirv_value(Any value) {
        if (value.type == TYPE_String) {
            return builder.createString(value.string->data);
        }
        switch(value.type->kind()) {
        case TK_Extern: {
            auto et = cast<ExternType>(value.type);
            spv::StorageClass sc = storage_class_from_extern_class(
                et->storage_class);
            const char *name = nullptr;
            spv::BuiltIn builtin = spv::BuiltInMax;
            switch(value.symbol.value()) {
            #define T(NAME) \
            case SYM_SPIRV_BuiltIn ## NAME: \
                builtin = spv::BuiltIn ## NAME; break;
                B_SPIRV_BUILTINS()
            #undef T
                default:
                    name = value.symbol.name()->data;
                    break;
            }
            auto ty = type_to_spirv_type(et->type, et->flags);
            auto id = builder.createVariable(sc, ty, name);
            if (builtin != spv::BuiltInMax) {
                builder.addDecoration(id, spv::DecorationBuiltIn, builtin);
            }
            switch(sc) {
            case spv::StorageClassUniformConstant:
            case spv::StorageClassUniform: {
                //builder.addDecoration(id, spv::DecorationDescriptorSet, 0);
                if (et->binding >= 0) {
                    builder.addDecoration(id, spv::DecorationBinding, et->binding);
                }
                if (et->location >= 0) {
                    builder.addDecoration(id, spv::DecorationLocation, et->location);
                }
                if (builder.isImageType(ty)) {
                    auto flags = et->flags;
                    if (flags & EF_Volatile) {
                        builder.addDecoration(id, spv::DecorationVolatile);
                    }
                    if (flags & EF_Coherent) {
                        builder.addDecoration(id, spv::DecorationCoherent);
                    }
                    if (flags & EF_Restrict) {
                        builder.addDecoration(id, spv::DecorationRestrict);
                    }
                    if (flags & EF_NonWritable) {
                        builder.addDecoration(id, spv::DecorationNonWritable);
                    }
                    if (flags & EF_NonReadable) {
                        builder.addDecoration(id, spv::DecorationNonReadable);
                    }
                }
            } break;
            default: {
                if (et->location >= 0) {
                    builder.addDecoration(id, spv::DecorationLocation, et->location);
                }
            } break;
            }
            return id;
        } break;
        default: break;
        };

        StyledString ss;
        ss.out << "IL->SPIR: cannot convert argument of type " << value.type;
        location_error(ss.str());
        return 0;
    }

    bool is_bool(spv::Id value) {
        auto T = builder.getTypeId(value);
        return
            (builder.isVectorType(T)
             && builder.isBoolType(builder.getContainedTypeId(T)))
            || builder.isBoolType(T);
    }

    void write_anchor(const Anchor *anchor) {
        assert(anchor);
        assert(anchor->file);
        if (use_debug_info) {
            builder.addLine(
                argument_to_value(anchor->path().name()),
                anchor->lineno, anchor->column);
        }
    }

    // set of processed SCC groups
    std::unordered_set<size_t> handled_loops;

    bool handle_loop_label (Label *label,
        Label *&continue_label,
        Label *&break_label) {
        auto &&group = scc.group(label);
        if (group.labels.size() <= 1)
            return false;
        if (handled_loops.count(group.index))
            return false;
        handled_loops.insert(group.index);
        if (!label->is_basic_block_like()) {

        }
        auto &&labels = group.labels;
        Label *header_label = label;
        continue_label = nullptr;
        break_label = nullptr;
        size_t count = labels.size();
        for (size_t i = 0; i < count; ++i) {
            Label *l = labels[i];
            if (l->is_calling(header_label)
                || l->is_continuing_to(header_label)) {
                if (continue_label) {
                    StyledStream ss;
                    ss << header_label->anchor << " for this loop" << std::endl;
                    ss << continue_label->body.anchor << " previous continue is here" << std::endl;
                    //stream_label(ss, continue_label, StreamLabelFormat::debug_single());
                    //stream_label(ss, l, StreamLabelFormat::debug_single());
                    set_active_anchor(l->body.anchor);
                    location_error(String::from(
                        "IL->SPIR: duplicate continue label found. only one continue label is permitted per loop."));
                }
                continue_label = l;
            }
            auto &&enter = l->body.enter;
            if ((enter.type == TYPE_Builtin)
                && (enter.builtin.value() == FN_Branch)) {
                auto &&args = l->body.args;
                assert(args.size() >= 4);
                Label *then_label = args[2].value;
                Label *else_label = args[3].value;
                Label *result = nullptr;
                if (scc.group_id(then_label) != group.index) {
                    result = then_label;
                } else if (scc.group_id(else_label) != group.index) {
                    result = else_label;
                }
                if (result) {
                    if (break_label && (break_label != result)) {
                        StyledStream ss;
                        ss << header_label->anchor << " for this loop" << std::endl;
                        ss << break_label->anchor << " previous break is here" << std::endl;
                        //stream_label(ss, break_label, StreamLabelFormat::debug_single());
                        //stream_label(ss, result, StreamLabelFormat::debug_single());
                        set_active_anchor(result->anchor);
                        location_error(String::from(
                            "IL->SPIR: duplicate break label found. only one break label is permitted per loop"));
                    }
                    break_label = result;
                }
            }
        }
        assert(continue_label);
        assert(continue_label->is_basic_block_like());
        if (!break_label) {
            location_error(String::from(
                "IL->SPIR: loop is infinite"));
        }
        assert(break_label->is_basic_block_like());
        #if 0
        StyledStream ss;
        ss << "loop found:" << std::endl;
        ss << "    labels in group:";
        for (size_t i = 0; i < count; ++i) {
            ss << " " << labels[i];
        }
        ss << std::endl;
        ss << "    entry: " << label << std::endl;
        if (continue_label) {
            ss << "    continue: " << continue_label << std::endl;
        }
        if (break_label) {
            ss << "    break: " << break_label << std::endl;
        }
        #endif
        return true;
    }

    void write_label_body(Label *label) {
    repeat:
        assert(label->body.is_complete());
        bool terminated = false;
        auto &&body = label->body;
        auto &&enter = body.enter;
        auto &&args = body.args;

        set_active_anchor(label->body.anchor);

        write_anchor(label->body.anchor);

        Label *continue_label = nullptr;
        Label *break_label = nullptr;
        bool is_loop_header = handle_loop_label(label, continue_label, break_label);
        spv::Block *bb_continue = nullptr;
        spv::Block *bb_merge = nullptr;
        unsigned int control = spv::LoopControlMaskNone;
        if (is_loop_header) {
            bb_continue = label_to_basic_block(continue_label, true);
            bb_merge = label_to_basic_block(break_label, true);
        }

#define HANDLE_LOOP_MERGE() \
    if (is_loop_header) { \
        builder.createLoopMerge(bb_merge, bb_continue, control); \
    }
        assert(!args.empty());
        size_t argcount = args.size() - 1;
        size_t argn = 1;
#define READ_ANY(NAME) \
        assert(argn <= argcount); \
        Any &NAME = args[argn++].value;
#define READ_VALUE(NAME) \
        assert(argn <= argcount); \
        auto && _arg_ ## NAME = args[argn++]; \
        auto && _ ## NAME = _arg_ ## NAME .value; \
        spv::Id NAME = argument_to_value(_ ## NAME);
#define READ_LABEL_BLOCK(NAME) \
        assert(argn <= argcount); \
        spv::Block *NAME = label_to_basic_block(args[argn++].value, is_loop_header); \
        assert(NAME);
#define READ_TYPE(NAME) \
        assert(argn <= argcount); \
        assert(args[argn].value.type == TYPE_Type); \
        spv::Id NAME = type_to_spirv_type(args[argn++].value.typeref);

        spv::Id retvalue = 0;
        bool multiple_return_values = false;
        if (enter.type == TYPE_Builtin) {
            switch(enter.builtin.value()) {
            case FN_Sample: {
                READ_VALUE(sampler);
                READ_VALUE(coords);
                spv::Builder::TextureParameters params;
                memset(&params, 0, sizeof(params));
                params.sampler = sampler;
                params.coords = coords;
                auto ST = storage_type(_sampler.indirect_type());
                if (ST->kind() == TK_SampledImage) {
                    ST = storage_type(cast<SampledImageType>(ST)->type);
                }
                auto resultType = type_to_spirv_type(cast<ImageType>(ST)->type);
                bool sparse = false;
                bool fetch = false;
                bool proj = false;
                bool gather = false;
                bool explicitLod = false;
                while (argn <= argcount) {
                    READ_VALUE(value);
                    switch (_arg_value.key.value()) {
                        case SYM_SPIRV_ImageOperandLod: params.lod = value; break;
                        case SYM_SPIRV_ImageOperandBias: params.bias = value; break;
                        case SYM_SPIRV_ImageOperandDref: params.Dref = value; break;
                        case SYM_SPIRV_ImageOperandProj: proj = true; break;
                        case SYM_SPIRV_ImageOperandFetch: fetch = true; break;
                        case SYM_SPIRV_ImageOperandGradX: params.gradX = value; break;
                        case SYM_SPIRV_ImageOperandGradY: params.gradY = value; break;
                        case SYM_SPIRV_ImageOperandOffset: params.offset = value; break;
                        case SYM_SPIRV_ImageOperandConstOffsets: params.offsets = value; break;
                        case SYM_SPIRV_ImageOperandMinLod: params.lodClamp = value; break;
                        case SYM_SPIRV_ImageOperandSample: params.sample = value; break;
                        case SYM_SPIRV_ImageOperandGather: {
                            params.component = value;
                            gather = true;
                        } break;
                        case SYM_SPIRV_ImageOperandSparse: {
                            params.texelOut = value;
                            sparse = true;
                        } break;
                        default: break;
                    }
                }
                retvalue = builder.createTextureCall(
                    spv::NoPrecision, resultType, sparse, fetch, proj, gather,
                    explicitLod, params);
            } break;
            case FN_ImageRead: {
                READ_VALUE(image);
                READ_VALUE(coords);
                auto ST = _image.indirect_type();
                auto resultType = type_to_spirv_type(cast<ImageType>(ST)->type);
                retvalue = builder.createBinOp(spv::OpImageRead,
                    resultType, image, coords);
            } break;
            case FN_ImageWrite: {
                READ_VALUE(image);
                READ_VALUE(coords);
                READ_VALUE(texel);
                builder.createNoResultOp(spv::OpImageWrite, { image, coords, texel });
            } break;
            case FN_Branch: {
                READ_VALUE(cond);
                READ_LABEL_BLOCK(then_block);
                READ_LABEL_BLOCK(else_block);
                HANDLE_LOOP_MERGE();
                builder.createConditionalBranch(cond, then_block, else_block);
                terminated = true;
            } break;
            case OP_Tertiary: {
                READ_VALUE(cond);
                READ_VALUE(then_value);
                READ_VALUE(else_value);
                retvalue = builder.createTriOp(spv::OpSelect,
                    builder.getTypeId(then_value), cond,
                    then_value, else_value);
            } break;
            case FN_Length:
            case FN_Normalize:
            case OP_Sin:
            case OP_Cos:
            case OP_Tan:
            case OP_Asin:
            case OP_Acos:
            case OP_Atan:
            case OP_Trunc:
            case OP_Floor:
            case OP_FAbs:
            case OP_FSign:
            case OP_Sqrt: {
                READ_VALUE(val);
                GLSLstd450 builtin = GLSLstd450Bad;
                auto rtype = builder.getTypeId(val);
                switch (enter.symbol.value()) {
                case FN_Length:
                    rtype = builder.getContainedTypeId(rtype);
                    builtin = GLSLstd450Length; break;
                case FN_Normalize: builtin = GLSLstd450Normalize; break;
                case OP_Sin: builtin = GLSLstd450Sin; break;
                case OP_Cos: builtin = GLSLstd450Cos; break;
                case OP_Tan: builtin = GLSLstd450Tan; break;
                case OP_Asin: builtin = GLSLstd450Asin; break;
                case OP_Acos: builtin = GLSLstd450Acos; break;
                case OP_Atan: builtin = GLSLstd450Atan; break;
                case OP_Trunc: builtin = GLSLstd450Trunc; break;
                case OP_Floor: builtin = GLSLstd450Floor; break;
                case OP_FAbs: builtin = GLSLstd450FAbs; break;
                case OP_FSign: builtin = GLSLstd450FSign; break;
                case OP_Sqrt: builtin = GLSLstd450Sqrt; break;
                default: {
                    StyledString ss;
                    ss.out << "IL->SPIR: unsupported unary intrinsic " << enter << " encountered";
                    location_error(ss.str());
                } break;
                }
                retvalue = builder.createBuiltinCall(rtype, glsl_ext_inst, builtin, { val });
            } break;
            case FN_Cross:
            case OP_Step:
            case OP_Pow: {
                READ_VALUE(a);
                READ_VALUE(b);
                GLSLstd450 builtin = GLSLstd450Bad;
                auto rtype = builder.getTypeId(a);
                switch (enter.symbol.value()) {
                case OP_Step: builtin = GLSLstd450Step; break;
                case OP_Pow: builtin = GLSLstd450Pow; break;
                case FN_Cross: builtin = GLSLstd450Cross; break;
                default: {
                    StyledString ss;
                    ss.out << "IL->SPIR: unsupported binary intrinsic " << enter << " encountered";
                    location_error(ss.str());
                } break;
                }
                retvalue = builder.createBuiltinCall(rtype, glsl_ext_inst, builtin, { a, b });
            } break;
            case FN_Unconst: {
                READ_VALUE(val);
                retvalue = val;
            } break;
            case FN_ExtractValue: {
                READ_VALUE(val);
                READ_ANY(index);
                int i = cast_number<unsigned>(index);
                retvalue = builder.createCompositeExtract(val,
                    builder.getContainedTypeId(builder.getTypeId(val), i),
                    i);
            } break;
            case FN_InsertValue: {
                READ_VALUE(val);
                READ_VALUE(eltval);
                READ_ANY(index);
                retvalue = builder.createCompositeInsert(eltval, val,
                    builder.getTypeId(val),
                    cast_number<unsigned>(index));
            } break;
            case FN_ExtractElement: {
                READ_VALUE(val);
                READ_VALUE(index);
                if (_index.is_const()) {
                    int i = cast_number<unsigned>(_index);
                    retvalue = builder.createCompositeExtract(val,
                        builder.getContainedTypeId(builder.getTypeId(val), i),
                        i);
                } else {
                    retvalue = builder.createVectorExtractDynamic(val,
                        builder.getContainedTypeId(builder.getTypeId(val)),
                        index);
                }
            } break;
            case FN_InsertElement: {
                READ_VALUE(val);
                READ_VALUE(eltval);
                READ_VALUE(index);
                if (_index.is_const()) {
                    retvalue = builder.createCompositeInsert(eltval, val,
                        builder.getTypeId(val),
                        cast_number<unsigned>(_index));
                } else {
                    retvalue = builder.createVectorInsertDynamic(val,
                        builder.getTypeId(val), eltval, index);
                }
            } break;
            case FN_ShuffleVector: {
                READ_VALUE(v1);
                READ_VALUE(v2);
                READ_VALUE(mask);
                auto ET = builder.getContainedTypeId(builder.getTypeId(v1));
                auto sz = builder.getNumTypeComponents(builder.getTypeId(mask));
                auto op = new spv::Instruction(
                    builder.getUniqueId(),
                    builder.makeVectorType(ET, sz),
                    spv::OpVectorShuffle);
                op->addIdOperand(v1);
                op->addIdOperand(v2);
                auto vt = cast<VectorType>(storage_type(_mask.type));
                for (int i = 0; i < sz; ++i) {
                    op->addImmediateOperand(
                        cast_number<unsigned int>(vt->unpack(_mask.pointer, i)));
                }
                retvalue = op->getResultId();
                builder.getBuildPoint()->addInstruction(
                    std::unique_ptr<spv::Instruction>(op));
            } break;
            case FN_Undef: { READ_TYPE(ty);
                retvalue = builder.createUndefined(ty); } break;
            case FN_Alloca: { READ_TYPE(ty);
                retvalue = builder.createVariable(
                    spv::StorageClassFunction, ty); } break;
            /*
            case FN_AllocaArray: { READ_TYPE(ty); READ_VALUE(val);
                retvalue = LLVMBuildArrayAlloca(builder, ty, val, ""); } break;
            */
            case FN_AllocaOf: {
                READ_VALUE(val);
                retvalue = builder.createVariable(spv::StorageClassFunction,
                    builder.getTypeId(val));
                builder.createStore(val, retvalue);
            } break;
            /*
            case FN_Malloc: { READ_TYPE(ty);
                retvalue = LLVMBuildMalloc(builder, ty, ""); } break;
            case FN_MallocArray: { READ_TYPE(ty); READ_VALUE(val);
                retvalue = LLVMBuildArrayMalloc(builder, ty, val, ""); } break;
            case FN_Free: { READ_VALUE(val);
                retvalue = LLVMBuildFree(builder, val); } break;
            */
            case SFXFN_ExecutionMode: {
                assert(active_function_value);
                READ_ANY(mode);
                auto em = execution_mode_from_symbol(mode.symbol);
                int values[3] = { -1, -1, -1 };
                int c = 0;
                while ((c < 3) && (argn <= argcount)) {
                    READ_ANY(val);
                    values[c] = cast_number<int>(val);
                    c++;
                }
                builder.addExecutionMode(active_function_value, em,
                    values[0], values[1], values[2]);
            } break;
            case FN_GetElementPtr: {
                READ_VALUE(pointer);
                assert(argcount > 1);
                size_t count = argcount - 1;
                std::vector<spv::Id> indices;
                for (size_t i = 1; i < count; ++i) {
                    indices.push_back(argument_to_value(args[argn + i].value));
                }

                retvalue = builder.createAccessChain(
                    builder.getTypeStorageClass(builder.getTypeId(pointer)),
                    pointer, indices);
            } break;
            case FN_Bitcast:
            case FN_IntToPtr:
            case FN_PtrToInt:
            case FN_ITrunc:
            case FN_SExt:
            case FN_ZExt:
            case FN_FPTrunc:
            case FN_FPExt:
            case FN_FPToUI:
            case FN_FPToSI:
            case FN_UIToFP:
            case FN_SIToFP:
            {
                READ_VALUE(val); READ_TYPE(ty);
                spv::Op op = spv::OpMax;
                switch(enter.builtin.value()) {
                case FN_Bitcast:
                    if (builder.getTypeId(val) == ty) {
                        // do nothing
                        retvalue = val;
                    } else {
                        op = spv::OpBitcast;
                    }
                    break;
                case FN_IntToPtr: op = spv::OpConvertUToPtr; break;
                case FN_PtrToInt: op = spv::OpConvertPtrToU; break;
                case FN_SExt: op = spv::OpSConvert; break;
                case FN_ZExt: op = spv::OpUConvert; break;
                case FN_ITrunc: op = spv::OpSConvert; break;
                case FN_FPTrunc: op = spv::OpFConvert; break;
                case FN_FPExt: op = spv::OpFConvert; break;
                case FN_FPToUI: op = spv::OpConvertFToU; break;
                case FN_FPToSI: op = spv::OpConvertFToS; break;
                case FN_UIToFP: op = spv::OpConvertUToF; break;
                case FN_SIToFP: op = spv::OpConvertSToF; break;
                default: break;
                }
                if (op != spv::OpMax) {
                    retvalue = builder.createUnaryOp(op, ty, val);
                }
            } break;
            case FN_VolatileLoad:
            case FN_Load: {
                READ_VALUE(ptr);
                retvalue = builder.createLoad(ptr);
                if (enter.builtin == FN_VolatileLoad) {
                    builder.getInstruction(retvalue)->addImmediateOperand(
                        1<<spv::MemoryAccessVolatileShift);
                }
            } break;
            case FN_VolatileStore:
            case FN_Store: {
                READ_VALUE(val); READ_VALUE(ptr);
                builder.createStore(val, ptr);
                if (enter.builtin == FN_VolatileStore) {
                    builder.getInstruction(retvalue)->addImmediateOperand(
                        1<<spv::MemoryAccessVolatileShift);
                }
            } break;
            case OP_ICmpEQ:
            case OP_ICmpNE:
            case OP_ICmpUGT:
            case OP_ICmpUGE:
            case OP_ICmpULT:
            case OP_ICmpULE:
            case OP_ICmpSGT:
            case OP_ICmpSGE:
            case OP_ICmpSLT:
            case OP_ICmpSLE:
            case OP_FCmpOEQ:
            case OP_FCmpONE:
            case OP_FCmpORD:
            case OP_FCmpOGT:
            case OP_FCmpOGE:
            case OP_FCmpOLT:
            case OP_FCmpOLE:
            case OP_FCmpUEQ:
            case OP_FCmpUNE:
            case OP_FCmpUNO:
            case OP_FCmpUGT:
            case OP_FCmpUGE:
            case OP_FCmpULT:
            case OP_FCmpULE: { READ_VALUE(a); READ_VALUE(b);
                spv::Op op = spv::OpMax;
#define BOOL_OR_INT_OP(BOOL_OP, INT_OP) \
    (is_bool(a)?(BOOL_OP):(INT_OP))
                switch(enter.builtin.value()) {
                case OP_ICmpEQ: op = BOOL_OR_INT_OP(spv::OpLogicalEqual, spv::OpIEqual); break;
                case OP_ICmpNE: op = BOOL_OR_INT_OP(spv::OpLogicalNotEqual, spv::OpINotEqual); break;
                case OP_ICmpUGT: op = spv::OpUGreaterThan; break;
                case OP_ICmpUGE: op = spv::OpUGreaterThanEqual; break;
                case OP_ICmpULT: op = spv::OpULessThan; break;
                case OP_ICmpULE: op = spv::OpULessThanEqual; break;
                case OP_ICmpSGT: op = spv::OpSGreaterThan; break;
                case OP_ICmpSGE: op = spv::OpSGreaterThanEqual; break;
                case OP_ICmpSLT: op = spv::OpSLessThan; break;
                case OP_ICmpSLE: op = spv::OpSLessThanEqual; break;
                case OP_FCmpOEQ: op = spv::OpFOrdEqual; break;
                case OP_FCmpONE: op = spv::OpFOrdNotEqual; break;
                case OP_FCmpORD: op = spv::OpOrdered; break;
                case OP_FCmpOGT: op = spv::OpFOrdGreaterThan; break;
                case OP_FCmpOGE: op = spv::OpFOrdGreaterThanEqual; break;
                case OP_FCmpOLT: op = spv::OpFOrdLessThan; break;
                case OP_FCmpOLE: op = spv::OpFOrdLessThanEqual; break;
                case OP_FCmpUEQ: op = spv::OpFUnordEqual; break;
                case OP_FCmpUNE: op = spv::OpFUnordNotEqual; break;
                case OP_FCmpUNO: op = spv::OpUnordered; break;
                case OP_FCmpUGT: op = spv::OpFUnordGreaterThan; break;
                case OP_FCmpUGE: op = spv::OpFUnordGreaterThanEqual; break;
                case OP_FCmpULT: op = spv::OpFUnordLessThan; break;
                case OP_FCmpULE: op = spv::OpFUnordLessThanEqual; break;
                default: break;
                }
#undef BOOL_OR_INT_OP
                auto T = builder.getTypeId(a);
                if (builder.isVectorType(T)) {
                    T = builder.makeVectorType(builder.makeBoolType(),
                        builder.getNumTypeComponents(T));
                } else {
                    T = builder.makeBoolType();
                }
                retvalue = builder.createBinOp(op, T, a, b); } break;
            case OP_Add:
            case OP_AddNUW:
            case OP_AddNSW:
            case OP_Sub:
            case OP_SubNUW:
            case OP_SubNSW:
            case OP_Mul:
            case OP_MulNUW:
            case OP_MulNSW:
            case OP_SDiv:
            case OP_UDiv:
            case OP_SRem:
            case OP_URem:
            case OP_Shl:
            case OP_LShr:
            case OP_AShr:
            case OP_BAnd:
            case OP_BOr:
            case OP_BXor:
            case OP_FAdd:
            case OP_FSub:
            case OP_FMul:
            case OP_FDiv:
            case OP_FRem: { READ_VALUE(a); READ_VALUE(b);
                spv::Op op = spv::OpMax;
                switch(enter.builtin.value()) {
#define BOOL_OR_INT_OP(BOOL_OP, INT_OP) \
    (is_bool(a)?(BOOL_OP):(INT_OP))
                case OP_Add:
                case OP_AddNUW:
                case OP_AddNSW: op = spv::OpIAdd; break;
                case OP_Sub:
                case OP_SubNUW:
                case OP_SubNSW: op = spv::OpISub; break;
                case OP_Mul:
                case OP_MulNUW:
                case OP_MulNSW: op = spv::OpIMul; break;
                case OP_SDiv: op = spv::OpSDiv; break;
                case OP_UDiv: op = spv::OpUDiv; break;
                case OP_SRem: op = spv::OpSRem; break;
                case OP_URem: op = spv::OpUMod; break;
                case OP_Shl: op = spv::OpShiftLeftLogical; break;
                case OP_LShr: op = spv::OpShiftRightLogical; break;
                case OP_AShr: op = spv::OpShiftRightArithmetic; break;
                case OP_BAnd: op = BOOL_OR_INT_OP(spv::OpLogicalAnd, spv::OpBitwiseAnd); break;
                case OP_BOr: op = BOOL_OR_INT_OP(spv::OpLogicalOr, spv::OpBitwiseOr); break;
                case OP_BXor: op = BOOL_OR_INT_OP(spv::OpLogicalNotEqual, spv::OpBitwiseXor); break;
                case OP_FAdd: op = spv::OpFAdd; break;
                case OP_FSub: op = spv::OpFSub; break;
                case OP_FMul: op = spv::OpFMul; break;
                case OP_FDiv: op = spv::OpFDiv; break;
                case OP_FRem: op = spv::OpFRem; break;
                default: break;
                }
#undef BOOL_OR_INT_OP
                retvalue = builder.createBinOp(op,
                    builder.getTypeId(a), a, b); } break;
            case SFXFN_Unreachable:
                builder.makeUnreachable();
                terminated = true; break;
            case SFXFN_Discard:
                builder.makeDiscard();
                terminated = true; break;
            default: {
                StyledString ss;
                ss.out << "IL->SPIR: unsupported builtin " << enter.builtin << " encountered";
                location_error(ss.str());
            } break;
            }
        } else if (enter.type == TYPE_Label) {
            if (enter.label->is_basic_block_like()) {
                auto block = label_to_basic_block(enter.label, is_loop_header);
                if (!block) {
                    // no basic block was generated - just generate assignments
                    auto &&params = enter.label->params;
                    for (size_t i = 1; i < params.size(); ++i) {
                        param2value[{active_function_value, params[i]}] =
                            argument_to_value(args[i].value);
                    }
                    label = enter.label;
                    goto repeat;
                } else {
                    auto bbfrom = builder.getBuildPoint();
                    // assign phi nodes
                    auto &&params = enter.label->params;
                    for (size_t i = 1; i < params.size(); ++i) {
                        Parameter *param = params[i];
                        auto value = argument_to_value(args[i].value);
                        auto phinode = argument_to_value(param);
                        auto op = builder.getInstruction(phinode);
                        assert(op);
                        op->addIdOperand(value);
                        op->addIdOperand(bbfrom->getId());
                    }
                    HANDLE_LOOP_MERGE();
                    builder.createBranch(block);
                    terminated = true;
                }
            } else {
                /*if (use_debug_info) {
                    LLVMSetCurrentDebugLocation(builder, diloc);
                }*/
                auto func = label_to_function(enter.label);
                retvalue = build_call(
                    enter.label->get_function_type(),
                    func, args, multiple_return_values);
            }
        } else if (enter.type == TYPE_Closure) {
            StyledString ss;
            ss.out << "IL->SPIR: invalid call of compile time closure at runtime";
            location_error(ss.str());
        } else if (enter.type == TYPE_Parameter) {
            assert (enter.parameter->type != TYPE_Nothing);
            assert(enter.parameter->type != TYPE_Unknown);
            std::vector<spv::Id> values;
            for (size_t i = 0; i < argcount; ++i) {
                values.push_back(argument_to_value(args[i + 1].value));
            }
            // must be a return
            assert(enter.parameter->index == 0);
            // must be returning from this function
            assert(enter.parameter->label == active_function);

            //Label *label = enter.parameter->label;
            if (argcount > 1) {
                auto ilfunctype = cast<FunctionType>(active_function->get_function_type());
                auto rettype = type_to_spirv_type(ilfunctype->return_type);
                auto id = builder.createUndefined(rettype);
                for (size_t i = 0; i < values.size(); ++i) {
                    id = builder.createCompositeInsert(
                        values[i], id, rettype, i);
                }
                builder.makeReturn(true, id);
            } else if (argcount == 1) {
                builder.makeReturn(true, values[0]);
            } else {
                builder.makeReturn(true, 0);
            }
        } else {
            StyledString ss;
            ss.out << "IL->SPIR: cannot translate call to " << enter;
            location_error(ss.str());
        }

        Any contarg = args[0].value;
        if (terminated) {
            // write nothing
        } else if ((contarg.type == TYPE_Parameter)
            && (contarg.parameter->type != TYPE_Nothing)) {
            assert(contarg.parameter->type != TYPE_Unknown);
            assert(contarg.parameter->index == 0);
            assert(contarg.parameter->label == active_function);
            //Label *label = contarg.parameter->label;
            if (retvalue) {
                builder.makeReturn(true, retvalue);
            } else {
                builder.makeReturn(true, 0);
            }
        } else if (contarg.type == TYPE_Label) {
            auto bb = label_to_basic_block(contarg.label, is_loop_header);
            if (bb) {
                if (retvalue) {
                    auto bbfrom = builder.getBuildPoint();
                    // assign phi nodes
                    auto &&params = contarg.label->params;
                    auto rtype = builder.getTypeId(retvalue);
                    for (size_t i = 1; i < params.size(); ++i) {
                        Parameter *param = params[i];
                        auto phinode = argument_to_value(param);
                        spv::Id incoval = 0;
                        if (multiple_return_values) {
                            incoval = builder.createCompositeExtract(
                                retvalue,
                                builder.getContainedTypeId(rtype, i - 1),
                                i - 1);
                        } else {
                            assert(params.size() == 2);
                            incoval = retvalue;
                        }
                        auto op = builder.getInstruction(phinode);
                        assert(op);
                        op->addIdOperand(incoval);
                        op->addIdOperand(bbfrom->getId());
                    }
                }
                HANDLE_LOOP_MERGE();
                builder.createBranch(bb);
            } else {
                if (retvalue) {
                    // no basic block - just add assignments and continue
                    auto &&params = contarg.label->params;
                    auto rtype = builder.getTypeId(retvalue);
                    for (size_t i = 1; i < params.size(); ++i) {
                        Parameter *param = params[i];
                        spv::Id pvalue = 0;
                        if (multiple_return_values) {
                            pvalue = builder.createCompositeExtract(
                                retvalue,
                                builder.getContainedTypeId(rtype, i - 1),
                                i - 1);
                        } else {
                            assert(params.size() == 2);
                            pvalue = retvalue;
                        }
                        param2value[{active_function_value,param}] = pvalue;
                    }
                }
                label = contarg.label;
                goto repeat;
            }
        } else if (contarg.type == TYPE_Nothing) {
        } else {
            assert(false && "todo: continuing with unexpected value");
        }

        //LLVMSetCurrentDebugLocation(builder, nullptr);
    }
    #undef READ_ANY
    #undef READ_VALUE
    #undef READ_TYPE
    #undef READ_LABEL_BLOCK
    #undef HANDLE_LOOP_MERGE

    spv::Id build_call(const Type *functype, spv::Function* func, Args &args,
        bool &multiple_return_values) {
        size_t argcount = args.size() - 1;

        auto fi = cast<FunctionType>(functype);

        std::vector<spv::Id> values;
        for (size_t i = 0; i < argcount; ++i) {
            auto &&arg = args[i + 1];
            values.push_back(argument_to_value(arg.value));
        }

        size_t fargcount = fi->argument_types.size();
        assert(argcount >= fargcount);
        if (fi->flags & FF_Variadic) {
            location_error(String::from("IL->SPIR: variadic calls not supported"));
        }

        auto ret = builder.createFunctionCall(func, values);
        auto rlt = cast<ReturnLabelType>(fi->return_type);
        multiple_return_values = rlt->has_multiple_return_values();
        if (rlt->return_type == TYPE_Void) {
            return 0;
        } else {
            return ret;
        }
    }

    void set_active_function(Label *l) {
        if (active_function == l) return;
        active_function = l;
        if (l) {
            auto it = label2func.find(l);
            assert(it != label2func.end());
            active_function_value = it->second;
        } else {
            active_function_value = nullptr;
        }
    }

    void process_labels() {
        while (!bb_label_todo.empty()) {
            auto it = bb_label_todo.back();
            set_active_function(it.first);
            Label *label = it.second;
            bb_label_todo.pop_back();

            auto it2 = label2bb.find({active_function_value, label});
            assert(it2 != label2bb.end());
            spv::Block *bb = it2->second;
            builder.setBuildPoint(bb);

            write_label_body(label);
        }
    }

    bool has_single_caller(Label *l) {
        auto it = user_map.label_map.find(l);
        assert(it != user_map.label_map.end());
        auto &&users = it->second;
        if (users.size() != 1)
            return false;
        Label *userl = *users.begin();
        if (userl->body.enter == Any(l))
            return true;
        if (userl->body.args[0] == Any(l))
            return true;
        return false;
    }

    spv::Id create_struct_type(const Type *type, uint64_t flags,
        const TypenameType *tname = nullptr) {
        // todo: packed tuples
        auto ti = cast<TupleType>(type);
        size_t count = ti->types.size();
        std::vector<spv::Id> members;
        for (size_t i = 0; i < count; ++i) {
            members.push_back(type_to_spirv_type(ti->types[i]));
        }
        const char *name = "tuple";
        if (tname) {
            name = tname->name()->data;
        }
        auto id = builder.makeStructType(members, name);
        if (flags & EF_BufferBlock) {
            builder.addDecoration(id, spv::DecorationBufferBlock);
        } else if (flags & EF_Block) {
            builder.addDecoration(id, spv::DecorationBlock);
        }
        for (size_t i = 0; i < count; ++i) {
            builder.addMemberName(id, i, ti->values[i].key.name()->data);
            if (flags & EF_Volatile) {
                builder.addMemberDecoration(id, i, spv::DecorationVolatile);
            }
            if (flags & EF_Coherent) {
                builder.addMemberDecoration(id, i, spv::DecorationCoherent);
            }
            if (flags & EF_Restrict) {
                builder.addMemberDecoration(id, i, spv::DecorationRestrict);
            }
            if (flags & EF_NonWritable) {
                builder.addMemberDecoration(id, i, spv::DecorationNonWritable);
            }
            if (flags & EF_NonReadable) {
                builder.addMemberDecoration(id, i, spv::DecorationNonReadable);
            }
            builder.addMemberDecoration(id, i, spv::DecorationOffset, ti->offsets[i]);
        }
        return id;
    }

    spv::Id create_spirv_type(const Type *type, uint64_t flags) {
        switch(type->kind()) {
        case TK_Integer: {
            if (type == TYPE_Bool)
                return builder.makeBoolType();
            auto it = cast<IntegerType>(type);
            return builder.makeIntegerType(it->width, it->issigned);
        } break;
        case TK_Real: {
            auto rt = cast<RealType>(type);
            return builder.makeFloatType(rt->width);
        } break;
        case TK_Pointer: {
            auto pt = cast<PointerType>(type);
            return builder.makePointer(
                storage_class_from_extern_class(pt->storage_class),
                type_to_spirv_type(pt->element_type));
        } break;
        case TK_Array: {
            auto ai = cast<ArrayType>(type);
            auto etype = type_to_spirv_type(ai->element_type);
            spv::Id ty;
            if (!ai->count) {
                ty = builder.makeRuntimeArray(etype);
            } else {
                ty = builder.makeArrayType(etype,
                    builder.makeUintConstant(ai->count), 0);
            }
            builder.addDecoration(ty,
                spv::DecorationArrayStride,
                size_of(ai->element_type));
            return ty;
        } break;
        case TK_Vector: {
            auto vi = cast<VectorType>(type);
            return builder.makeVectorType(
                type_to_spirv_type(vi->element_type),
                vi->count);
        } break;
        case TK_Tuple: {
            return create_struct_type(type, flags);
        } break;
        case TK_Union: {
            auto ui = cast<UnionType>(type);
            return type_to_spirv_type(ui->tuple_type);
        } break;
        case TK_Extern: {
            auto et = cast<ExternType>(type);
            spv::StorageClass sc = storage_class_from_extern_class(
                et->storage_class);
            auto ty = type_to_spirv_type(et->type, et->flags);
            return builder.makePointer(sc, ty);
        } break;
        case TK_Image: {
            auto it = cast<ImageType>(type);
            auto ty = type_to_spirv_type(it->type);
            if (builder.isVectorType(ty)) {
                ty = builder.getContainedTypeId(ty);
            }
            return builder.makeImageType(ty,
                dim_from_symbol(it->dim),
                (it->depth == 1),
                (it->arrayed == 1),
                (it->multisampled == 1),
                it->sampled,
                image_format_from_symbol(it->format));
        } break;
        case TK_SampledImage: {
            auto sit = cast<SampledImageType>(type);
            return builder.makeSampledImageType(type_to_spirv_type(sit->type));
        } break;
        case TK_Typename: {
            if (type == TYPE_Void)
                return builder.makeVoidType();
            else if (type == TYPE_Sampler)
                return builder.makeSamplerType();
            auto tn = cast<TypenameType>(type);
            if (tn->finalized()) {
                if (tn->storage_type->kind() == TK_Tuple) {
                    return create_struct_type(tn->storage_type, flags, tn);
                } else {
                    return type_to_spirv_type(tn->storage_type, flags);
                }
            } else {
                location_error(String::from("IL->SPIR: opaque types are not supported"));
                return 0;
            }
        } break;
        case TK_ReturnLabel: {
            auto rlt = cast<ReturnLabelType>(type);
            return type_to_spirv_type(rlt->return_type);
        } break;
        case TK_Function: {
            auto fi = cast<FunctionType>(type);
            if (fi->vararg()) {
                location_error(String::from("IL->SPIR: vararg functions are not supported"));
            }
            size_t count = fi->argument_types.size();
            spv::Id rettype = type_to_spirv_type(fi->return_type);
            std::vector<spv::Id> elements;
            for (size_t i = 0; i < count; ++i) {
                auto AT = fi->argument_types[i];
                elements.push_back(type_to_spirv_type(AT));
            }
            return builder.makeFunctionType(rettype, elements);
        } break;
        };

        StyledString ss;
        ss.out << "IL->SPIR: cannot convert type " << type;
        location_error(ss.str());
        return 0;
    }

    spv::Id type_to_spirv_type(const Type *type, uint64_t flags = 0) {
        auto it = type_cache.find({type, flags});
        if (it == type_cache.end()) {
            spv::Id result = create_spirv_type(type, flags);
            type_cache.insert({ { type, flags }, result});
            return result;
        } else {
            return it->second;
        }
    }

    spv::Block *label_to_basic_block(Label *label, bool force = false) {
        auto old_bb = builder.getBuildPoint();
        auto func = &old_bb->getParent();
        auto it = label2bb.find({func, label});
        if (it == label2bb.end()) {
            if (has_single_caller(label) && !force) {
                // not generating basic blocks for single user labels
                return nullptr;
            }
            //const char *name = label->name.name()->data;
            auto bb = &builder.makeNewBlock();
            label2bb.insert({{func, label}, bb});
            bb_label_todo.push_back({active_function, label});
            builder.setBuildPoint(bb);

            auto &&params = label->params;
            if (!params.empty()) {
                size_t paramcount = label->params.size() - 1;
                for (size_t i = 0; i < paramcount; ++i) {
                    Parameter *param = params[i + 1];
                    auto ptype = type_to_spirv_type(param->type);
                    auto op = new spv::Instruction(
                        builder.getUniqueId(), ptype, spv::OpPhi);
                    builder.addName(op->getResultId(), param->name.name()->data);
                    param2value[{active_function_value,param}] = op->getResultId();
                    bb->addInstruction(std::unique_ptr<spv::Instruction>(op));
                }
            }

            builder.setBuildPoint(old_bb);
            return bb;
        } else {
            return it->second;
        }
    }

    spv::Function *label_to_function(Label *label,
        bool root_function = false,
        Symbol funcname = SYM_Unnamed) {
        auto it = label2func.find(label);
        if (it == label2func.end()) {

            const Anchor *old_anchor = get_active_anchor();
            set_active_anchor(label->anchor);
            Label *last_function = active_function;

            auto old_bb = builder.getBuildPoint();

            if (funcname == SYM_Unnamed) {
                funcname = label->name;
            }

            const char *name;
            if (root_function && (funcname == SYM_Unnamed)) {
                name = "unnamed";
            } else {
                name = funcname.name()->data;
            }

            label->verify_compilable();
            auto ilfunctype = cast<FunctionType>(label->get_function_type());
            //auto fi = cast<FunctionType>(ilfunctype);

            auto rettype = type_to_spirv_type(ilfunctype->return_type);

            spv::Block* bb;
            std::vector<spv::Id> paramtypes;

            auto &&argtypes = ilfunctype->argument_types;
            for (auto it = argtypes.begin(); it != argtypes.end(); ++it) {
                paramtypes.push_back(type_to_spirv_type(*it));
            }

            std::vector<std::vector<spv::Decoration>> decorations;

            auto func = builder.makeFunctionEntry(
                spv::NoPrecision, rettype, name,
                paramtypes, decorations, &bb);
            //LLVMSetLinkage(func, LLVMPrivateLinkage);

            label2func[label] = func;
            set_active_function(label);

            if (use_debug_info) {
                // LLVMSetFunctionSubprogram(func, label_to_subprogram(label));
            }

            builder.setBuildPoint(bb);
            write_anchor(label->anchor);

            auto &&params = label->params;
            size_t paramcount = params.size() - 1;
            for (size_t i = 0; i < paramcount; ++i) {
                Parameter *param = params[i + 1];
                auto val = func->getParamId(i);
                param2value[{active_function_value,param}] = val;
            }

            write_label_body(label);

            builder.setBuildPoint(old_bb);

            set_active_function(last_function);
            set_active_anchor(old_anchor);
            return func;
        } else {
            return it->second;
        }
    }

    void generate(std::vector<unsigned int> &result, Symbol target, Label *entry) {
        //assert(all_parameters_lowered(entry));
        assert(!entry->is_basic_block_like());

        builder.setSource(spv::SourceLanguageGLSL, 450);
        glsl_ext_inst = builder.import("GLSL.std.450");

        auto needfi = Function(TYPE_Void, {}, 0);
        auto hasfi = entry->get_function_type();
        if (hasfi != needfi) {
            set_active_anchor(entry->anchor);
            StyledString ss;
            ss.out << "Entry function must have type " << needfi
                << " but has type " << hasfi;
            location_error(ss.str());
        }

        {
            std::unordered_set<Label *> visited;
            std::vector<Label *> labels;
            entry->build_reachable(visited, &labels);
            for (auto it = labels.begin(); it != labels.end(); ++it) {
                (*it)->insert_into_usermap(user_map);
            }
        }

        scc.walk(entry);

        //const char *name = entry->name.name()->data;
        //module = LLVMModuleCreateWithName(name);

        if (use_debug_info) {
            /*
            const char *DebugStr = "Debug Info Version";
            LLVMValueRef DbgVer[3];
            DbgVer[0] = LLVMConstInt(i32T, 1, 0);
            DbgVer[1] = LLVMMDString(DebugStr, strlen(DebugStr));
            DbgVer[2] = LLVMConstInt(i32T, 3, 0);
            LLVMAddNamedMetadataOperand(module, "llvm.module.flags",
                LLVMMDNode(DbgVer, 3));

            LLVMDIBuilderCreateCompileUnit(di_builder,
                llvm::dwarf::DW_LANG_C99, "file", "directory", "scopes",
                false, "", 0, "", 0);*/
        }

        auto func = label_to_function(entry, true);

        switch(target.value()) {
        case SYM_TargetVertex: {
            builder.addCapability(spv::CapabilityShader);
            builder.addEntryPoint(spv::ExecutionModelVertex, func, "main");
        } break;
        case SYM_TargetFragment: {
            builder.addCapability(spv::CapabilityShader);
            builder.addEntryPoint(spv::ExecutionModelFragment, func, "main");
        } break;
        case SYM_TargetGeometry: {
            builder.addCapability(spv::CapabilityShader);
            builder.addEntryPoint(spv::ExecutionModelGeometry, func, "main");
        } break;
        case SYM_TargetCompute: {
            builder.addCapability(spv::CapabilityShader);
            builder.addEntryPoint(spv::ExecutionModelGLCompute, func, "main");
        } break;
        default: {
            StyledString ss;
            ss.out << "IL->SPIR: unsupported target: " << target << ", try one of "
                << Symbol(SYM_TargetVertex) << " "
                << Symbol(SYM_TargetFragment) << " "
                << Symbol(SYM_TargetGeometry) << " "
                << Symbol(SYM_TargetCompute);
            location_error(ss.str());
        } break;
        }

        process_labels();

        //size_t k = finalize_types();
        //assert(!k);

        builder.dump(result);

        verify_spirv(result);
    }
};

//------------------------------------------------------------------------------
// IL->LLVM IR GENERATOR
//------------------------------------------------------------------------------

static void build_and_run_opt_passes(LLVMModuleRef module, int opt_level) {
    LLVMPassManagerBuilderRef passBuilder;

    passBuilder = LLVMPassManagerBuilderCreate();
    LLVMPassManagerBuilderSetOptLevel(passBuilder, opt_level);
    LLVMPassManagerBuilderSetSizeLevel(passBuilder, 0);
    if (opt_level >= 2) {
        LLVMPassManagerBuilderUseInlinerWithThreshold(passBuilder, 225);
    }

    LLVMPassManagerRef functionPasses =
      LLVMCreateFunctionPassManagerForModule(module);
    LLVMPassManagerRef modulePasses =
      LLVMCreatePassManager();
    //LLVMAddAnalysisPasses(LLVMGetExecutionEngineTargetMachine(ee), functionPasses);

    LLVMPassManagerBuilderPopulateFunctionPassManager(passBuilder,
                                                      functionPasses);
    LLVMPassManagerBuilderPopulateModulePassManager(passBuilder, modulePasses);

    LLVMPassManagerBuilderDispose(passBuilder);

    LLVMInitializeFunctionPassManager(functionPasses);
    for (LLVMValueRef value = LLVMGetFirstFunction(module);
         value; value = LLVMGetNextFunction(value))
      LLVMRunFunctionPassManager(functionPasses, value);
    LLVMFinalizeFunctionPassManager(functionPasses);

    LLVMRunPassManager(modulePasses, module);

    LLVMDisposePassManager(functionPasses);
    LLVMDisposePassManager(modulePasses);
}

typedef llvm::DIBuilder *LLVMDIBuilderRef;

static LLVMDIBuilderRef LLVMCreateDIBuilder(LLVMModuleRef M) {
  return new llvm::DIBuilder(*llvm::unwrap(M));
}

static void LLVMDisposeDIBuilder(LLVMDIBuilderRef Builder) {
  Builder->finalize();
  delete Builder;
}

static llvm::MDNode *value_to_mdnode(LLVMValueRef value) {
    return value ? cast<llvm::MDNode>(
        llvm::unwrap<llvm::MetadataAsValue>(value)->getMetadata()) : nullptr;
}

template<typename T>
static T *value_to_DI(LLVMValueRef value) {
    return value ? cast<T>(
        llvm::unwrap<llvm::MetadataAsValue>(value)->getMetadata()) : nullptr;
}

static LLVMValueRef mdnode_to_value(llvm::MDNode *node) {
  return llvm::wrap(
    llvm::MetadataAsValue::get(*llvm::unwrap(LLVMGetGlobalContext()), node));
}

typedef llvm::DINode::DIFlags LLVMDIFlags;

static LLVMValueRef LLVMDIBuilderCreateSubroutineType(
    LLVMDIBuilderRef Builder, LLVMValueRef ParameterTypes) {
    return mdnode_to_value(
        Builder->createSubroutineType(value_to_DI<llvm::MDTuple>(ParameterTypes)));
}

static LLVMValueRef LLVMDIBuilderCreateCompileUnit(LLVMDIBuilderRef Builder,
    unsigned Lang,
    const char *File, const char *Dir, const char *Producer, bool isOptimized,
    const char *Flags, unsigned RV, const char *SplitName,
    //DICompileUnit::DebugEmissionKind Kind,
    uint64_t DWOId) {
    auto ctx = (llvm::LLVMContext *)LLVMGetGlobalContext();
    auto file = llvm::DIFile::get(*ctx, File, Dir);
    return mdnode_to_value(
        Builder->createCompileUnit(Lang, file,
                      Producer, isOptimized, Flags,
                      RV, SplitName,
                      llvm::DICompileUnit::DebugEmissionKind::FullDebug,
                      //llvm::DICompileUnit::DebugEmissionKind::LineTablesOnly,
                      DWOId));
}

static LLVMValueRef LLVMDIBuilderCreateFunction(
    LLVMDIBuilderRef Builder, LLVMValueRef Scope, const char *Name,
    const char *LinkageName, LLVMValueRef File, unsigned LineNo,
    LLVMValueRef Ty, bool IsLocalToUnit, bool IsDefinition,
    unsigned ScopeLine) {
  return mdnode_to_value(Builder->createFunction(
        cast<llvm::DIScope>(value_to_mdnode(Scope)), Name, LinkageName,
        cast<llvm::DIFile>(value_to_mdnode(File)),
        LineNo, cast<llvm::DISubroutineType>(value_to_mdnode(Ty)),
        IsLocalToUnit, IsDefinition, ScopeLine));
}

static LLVMValueRef LLVMGetFunctionSubprogram(LLVMValueRef func) {
    return mdnode_to_value(
        llvm::cast<llvm::Function>(llvm::unwrap(func))->getSubprogram());
}

static void LLVMSetFunctionSubprogram(LLVMValueRef func, LLVMValueRef subprogram) {
    llvm::cast<llvm::Function>(llvm::unwrap(func))->setSubprogram(
        value_to_DI<llvm::DISubprogram>(subprogram));
}

static LLVMValueRef LLVMDIBuilderCreateLexicalBlock(LLVMDIBuilderRef Builder,
    LLVMValueRef Scope, LLVMValueRef File, unsigned Line, unsigned Col) {
    return mdnode_to_value(Builder->createLexicalBlock(
        value_to_DI<llvm::DIScope>(Scope),
        value_to_DI<llvm::DIFile>(File), Line, Col));
}

static LLVMValueRef LLVMCreateDebugLocation(unsigned Line,
                                     unsigned Col, const LLVMValueRef Scope,
                                     const LLVMValueRef InlinedAt) {
  llvm::MDNode *SNode = value_to_mdnode(Scope);
  llvm::MDNode *INode = value_to_mdnode(InlinedAt);
  return mdnode_to_value(llvm::DebugLoc::get(Line, Col, SNode, INode).get());
}

static LLVMValueRef LLVMDIBuilderCreateFile(
    LLVMDIBuilderRef Builder, const char *Filename,
                            const char *Directory) {
  return mdnode_to_value(Builder->createFile(Filename, Directory));
}

static std::vector<void *> loaded_libs;
static void *local_aware_dlsym(const char *name) {
#if 1
    return LLVMSearchForAddressOfSymbol(name);
#else
    size_t i = loaded_libs.size();
    while (i--) {
        void *ptr = dlsym(loaded_libs[i], name);
        if (ptr) {
            LLVMAddSymbol(name, ptr);
            return ptr;
        }
    }
    return dlsym(global_c_namespace, name);
#endif
}

struct LLVMIRGenerator {
    enum Intrinsic {
        llvm_sin_f32,
        llvm_sin_f64,
        llvm_cos_f32,
        llvm_cos_f64,
        llvm_sqrt_f32,
        llvm_sqrt_f64,
        llvm_fabs_f32,
        llvm_fabs_f64,
        llvm_trunc_f32,
        llvm_trunc_f64,
        llvm_floor_f32,
        llvm_floor_f64,
        llvm_pow_f32,
        llvm_pow_f64,
        custom_fsign_f32,
        custom_fsign_f64,
        NumIntrinsics,
    };

    struct HashFuncLabelPair {
        size_t operator ()(const std::pair<LLVMValueRef, Label *> &value) const {
            return
                HashLen16(std::hash<LLVMValueRef>()(value.first),
                    std::hash<Label *>()(value.second));
        }
    };


    typedef std::pair<LLVMValueRef, Parameter *> ParamKey;
    struct HashFuncParamPair {
        size_t operator ()(const ParamKey &value) const {
            return
                HashLen16(std::hash<LLVMValueRef>()(value.first),
                    std::hash<Parameter *>()(value.second));
        }
    };

    std::unordered_map<Label *, LLVMValueRef> label2func;
    std::unordered_map< std::pair<LLVMValueRef, Label *>,
        LLVMBasicBlockRef, HashFuncLabelPair> label2bb;
    std::vector< std::pair<Label *, Label *> > bb_label_todo;

    std::unordered_map<Label *, LLVMValueRef> label2md;
    std::unordered_map<SourceFile *, LLVMValueRef> file2value;
    std::unordered_map< ParamKey, LLVMValueRef, HashFuncParamPair> param2value;
    static std::unordered_map<const Type *, LLVMTypeRef> type_cache;
    static std::vector<const Type *> type_todo;

    std::unordered_map<Any, LLVMValueRef, Any::Hash> extern2global;
    std::unordered_map<void *, LLVMValueRef> ptr2global;

    Label::UserMap user_map;

    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMDIBuilderRef di_builder;

    static LLVMTypeRef voidT;
    static LLVMTypeRef i1T;
    static LLVMTypeRef i8T;
    static LLVMTypeRef i16T;
    static LLVMTypeRef i32T;
    static LLVMTypeRef i64T;
    static LLVMTypeRef f32T;
    static LLVMTypeRef f32x2T;
    static LLVMTypeRef f64T;
    static LLVMTypeRef rawstringT;
    static LLVMTypeRef noneT;
    static LLVMValueRef noneV;
    static LLVMAttributeRef attr_byval;
    static LLVMAttributeRef attr_sret;
    static LLVMAttributeRef attr_nonnull;
    LLVMValueRef intrinsics[NumIntrinsics];

    Label *active_function;
    LLVMValueRef active_function_value;

    bool use_debug_info;
    bool inline_pointers;

    template<unsigned N>
    static LLVMAttributeRef get_attribute(const char (&s)[N]) {
        unsigned kind = LLVMGetEnumAttributeKindForName(s, N - 1);
        assert(kind);
        return LLVMCreateEnumAttribute(LLVMGetGlobalContext(), kind, 0);
    }

    LLVMIRGenerator() :
        active_function(nullptr),
        active_function_value(nullptr),
        use_debug_info(true),
        inline_pointers(true) {
        static_init();
        for (int i = 0; i < NumIntrinsics; ++i) {
            intrinsics[i] = nullptr;
        }
    }

    LLVMValueRef source_file_to_scope(SourceFile *sf) {
        assert(use_debug_info);

        auto it = file2value.find(sf);
        if (it != file2value.end())
            return it->second;

        char *dn = strdup(sf->path.name()->data);
        char *bn = strdup(dn);

        LLVMValueRef result = LLVMDIBuilderCreateFile(di_builder,
            basename(bn), dirname(dn));
        free(dn);
        free(bn);

        file2value.insert({ sf, result });

        return result;
    }

    LLVMValueRef label_to_subprogram(Label *l) {
        assert(use_debug_info);

        auto it = label2md.find(l);
        if (it != label2md.end())
            return it->second;

        const Anchor *anchor = l->anchor;

        LLVMValueRef difile = source_file_to_scope(anchor->file);

        LLVMValueRef subroutinevalues[] = {
            nullptr
        };
        LLVMValueRef disrt = LLVMDIBuilderCreateSubroutineType(di_builder,
            LLVMMDNode(subroutinevalues, 1));

        LLVMValueRef difunc = LLVMDIBuilderCreateFunction(
            di_builder, difile, l->name.name()->data, l->name.name()->data,
            difile, anchor->lineno, disrt, false, true,
            anchor->lineno);

        label2md.insert({ l, difunc });
        return difunc;
    }

    LLVMValueRef anchor_to_location(const Anchor *anchor) {
        assert(use_debug_info);

        //auto old_bb = LLVMGetInsertBlock(builder);
        //LLVMValueRef func = LLVMGetBasicBlockParent(old_bb);
        LLVMValueRef disp = LLVMGetFunctionSubprogram(active_function_value);

        LLVMValueRef result = LLVMCreateDebugLocation(
            anchor->lineno, anchor->column, disp, nullptr);

        return result;
    }

    static void diag_handler(LLVMDiagnosticInfoRef info, void *) {
        const char *severity = "Message";
        switch(LLVMGetDiagInfoSeverity(info)) {
        case LLVMDSError: severity = "Error"; break;
        case LLVMDSWarning: severity = "Warning"; break;
        case LLVMDSRemark: return;// severity = "Remark"; break;
        case LLVMDSNote: return;//severity = "Note"; break;
        default: break;
        }

        char *str = LLVMGetDiagInfoDescription(info);
        fprintf(stderr, "LLVM %s: %s\n", severity, str);
        LLVMDisposeMessage(str);
        //LLVMDiagnosticSeverity LLVMGetDiagInfoSeverity(LLVMDiagnosticInfoRef DI);
    }

    LLVMValueRef get_intrinsic(Intrinsic op) {
        if (!intrinsics[op]) {
            LLVMValueRef result = nullptr;
            switch(op) {
#define LLVM_INTRINSIC_IMPL(ENUMVAL, RETTYPE, STRNAME, ...) \
    case ENUMVAL: { \
        LLVMTypeRef argtypes[] = {__VA_ARGS__}; \
        result = LLVMAddFunction(module, STRNAME, LLVMFunctionType(RETTYPE, argtypes, sizeof(argtypes) / sizeof(LLVMTypeRef), false)); \
    } break;
#define LLVM_INTRINSIC_IMPL_BEGIN(ENUMVAL, RETTYPE, STRNAME, ...) \
    case ENUMVAL: { \
        LLVMTypeRef argtypes[] = { __VA_ARGS__ }; \
        result = LLVMAddFunction(module, STRNAME, \
            LLVMFunctionType(f32T, argtypes, sizeof(argtypes) / sizeof(LLVMTypeRef), false)); \
        LLVMSetLinkage(result, LLVMPrivateLinkage); \
        auto bb = LLVMAppendBasicBlock(result, ""); \
        auto oldbb = LLVMGetInsertBlock(builder); \
        LLVMPositionBuilderAtEnd(builder, bb);
#define LLVM_INTRINSIC_IMPL_END() \
        LLVMPositionBuilderAtEnd(builder, oldbb); \
    } break;
            LLVM_INTRINSIC_IMPL(llvm_sin_f32, f32T, "llvm.sin.f32", f32T)
            LLVM_INTRINSIC_IMPL(llvm_sin_f64, f64T, "llvm.sin.f64", f64T)
            LLVM_INTRINSIC_IMPL(llvm_cos_f32, f32T, "llvm.cos.f32", f32T)
            LLVM_INTRINSIC_IMPL(llvm_cos_f64, f64T, "llvm.cos.f64", f64T)

            LLVM_INTRINSIC_IMPL(llvm_sqrt_f32, f32T, "llvm.sqrt.f32", f32T)
            LLVM_INTRINSIC_IMPL(llvm_sqrt_f64, f64T, "llvm.sqrt.f64", f64T)
            LLVM_INTRINSIC_IMPL(llvm_fabs_f32, f32T, "llvm.fabs.f32", f32T)
            LLVM_INTRINSIC_IMPL(llvm_fabs_f64, f64T, "llvm.fabs.f64", f64T)
            LLVM_INTRINSIC_IMPL(llvm_trunc_f32, f32T, "llvm.trunc.f32", f32T)
            LLVM_INTRINSIC_IMPL(llvm_trunc_f64, f64T, "llvm.trunc.f64", f64T)
            LLVM_INTRINSIC_IMPL(llvm_floor_f32, f32T, "llvm.floor.f32", f32T)
            LLVM_INTRINSIC_IMPL(llvm_floor_f64, f64T, "llvm.floor.f64", f64T)
            LLVM_INTRINSIC_IMPL(llvm_pow_f32, f32T, "llvm.pow.f32", f32T, f32T)
            LLVM_INTRINSIC_IMPL(llvm_pow_f64, f64T, "llvm.pow.f64", f64T, f64T)
            LLVM_INTRINSIC_IMPL_BEGIN(custom_fsign_f32, f32T, "custom.fsign.f32", f32T)
                // (0 < val) - (val < 0)
                LLVMValueRef val = LLVMGetParam(result, 0);
                LLVMValueRef zero = LLVMConstReal(f32T, 0.0);
                LLVMValueRef a = LLVMBuildZExt(builder, LLVMBuildFCmp(builder, LLVMRealOLT, zero, val, ""), i8T, "");
                LLVMValueRef b = LLVMBuildZExt(builder, LLVMBuildFCmp(builder, LLVMRealOLT, val, zero, ""), i8T, "");
                val = LLVMBuildSub(builder, a, b, "");
                val = LLVMBuildSIToFP(builder, val, f32T, "");
                LLVMBuildRet(builder, val);
            LLVM_INTRINSIC_IMPL_END()
            LLVM_INTRINSIC_IMPL_BEGIN(custom_fsign_f64, f64T, "custom.fsign.f64", f64T)
                // (0 < val) - (val < 0)
                LLVMValueRef val = LLVMGetParam(result, 0);
                LLVMValueRef zero = LLVMConstReal(f64T, 0.0);
                LLVMValueRef a = LLVMBuildZExt(builder, LLVMBuildFCmp(builder, LLVMRealOLT, zero, val, ""), i8T, "");
                LLVMValueRef b = LLVMBuildZExt(builder, LLVMBuildFCmp(builder, LLVMRealOLT, val, zero, ""), i8T, "");
                val = LLVMBuildSub(builder, a, b, "");
                val = LLVMBuildSIToFP(builder, val, f64T, "");
                LLVMBuildRet(builder, val);
            LLVM_INTRINSIC_IMPL_END()
#undef LLVM_INTRINSIC_IMPL
#undef LLVM_INTRINSIC_IMPL_BEGIN
#undef LLVM_INTRINSIC_IMPL_END
            default: assert(false); break;
            }
            intrinsics[op] = result;
        }
        return intrinsics[op];
    }

    static void static_init() {
        if (voidT) return;
        voidT = LLVMVoidType();
        i1T = LLVMInt1Type();
        i8T = LLVMInt8Type();
        i16T = LLVMInt16Type();
        i32T = LLVMInt32Type();
        i64T = LLVMInt64Type();
        f32T = LLVMFloatType();
        f32x2T = LLVMVectorType(f32T, 2);
        f64T = LLVMDoubleType();
        noneV = LLVMConstStruct(nullptr, 0, false);
        noneT = LLVMTypeOf(noneV);
        rawstringT = LLVMPointerType(LLVMInt8Type(), 0);
        attr_byval = get_attribute("byval");
        attr_sret = get_attribute("sret");
        attr_nonnull = get_attribute("nonnull");

        LLVMContextSetDiagnosticHandler(LLVMGetGlobalContext(),
            diag_handler,
            nullptr);

    }

#undef DEFINE_BUILTIN

    static bool all_parameters_lowered(Label *label) {
        for (auto &&param : label->params) {
            if (param->kind != PK_Regular)
                return false;
            if ((param->type == TYPE_Type) || (param->type == TYPE_Label))
                return false;
            if (isa<ReturnLabelType>(param->type) && (param->index != 0))
                return false;
        }
        return true;
    }

    static LLVMTypeRef abi_struct_type(const ABIClass *classes, size_t sz) {
        LLVMTypeRef types[sz];
        size_t k = 0;
        for (size_t i = 0; i < sz; ++i) {
            ABIClass cls = classes[i];
            switch(cls) {
            case ABI_CLASS_SSE: {
                types[i] = f32x2T; k++;
            } break;
            case ABI_CLASS_SSESF: {
                types[i] = f32T; k++;
            } break;
            case ABI_CLASS_SSEDF: {
                types[i] = f64T; k++;
            } break;
            case ABI_CLASS_INTEGER: {
                types[i] = i64T; k++;
            } break;
            case ABI_CLASS_INTEGERSI: {
                types[i] = i32T; k++;
            } break;
            case ABI_CLASS_INTEGERSI16: {
                types[i] = i16T; k++;
            } break;
            case ABI_CLASS_INTEGERSI8: {
                types[i] = i8T; k++;
            } break;
            default: {
                // do nothing
#if 0
                StyledStream ss;
                ss << "unhandled ABI class: " <<
                    abi_class_to_string(cls) << std::endl;
#endif
            } break;
            }
        }
        if (k != sz) return nullptr;
        return LLVMStructType(types, sz, false);
    }

    LLVMValueRef abi_import_argument(Parameter *param, LLVMValueRef func, size_t &k) {
        ABIClass classes[MAX_ABI_CLASSES];
        size_t sz = abi_classify(param->type, classes);
        if (!sz) {
            LLVMValueRef val = LLVMGetParam(func, k++);
            return LLVMBuildLoad(builder, val, "");
        }
        LLVMTypeRef T = type_to_llvm_type(param->type);
        auto tk = LLVMGetTypeKind(T);
        if (tk == LLVMStructTypeKind) {
            auto ST = abi_struct_type(classes, sz);
            if (ST) {
                // reassemble from argument-sized bits
                auto ptr = safe_alloca(ST);
                auto zero = LLVMConstInt(i32T,0,false);
                for (size_t i = 0; i < sz; ++i) {
                    LLVMValueRef indices[] = {
                        zero, LLVMConstInt(i32T,i,false),
                    };
                    auto dest = LLVMBuildGEP(builder, ptr, indices, 2, "");
                    LLVMBuildStore(builder, LLVMGetParam(func, k++), dest);
                }
                ptr = LLVMBuildBitCast(builder, ptr, LLVMPointerType(T, 0), "");
                return LLVMBuildLoad(builder, ptr, "");
            }
        }
        LLVMValueRef val = LLVMGetParam(func, k++);
        return val;
    }

    void abi_export_argument(LLVMValueRef val, const Type *AT,
        std::vector<LLVMValueRef> &values, std::vector<size_t> &memptrs) {
        ABIClass classes[MAX_ABI_CLASSES];
        size_t sz = abi_classify(AT, classes);
        if (!sz) {
            LLVMValueRef ptrval = safe_alloca(type_to_llvm_type(AT));
            LLVMBuildStore(builder, val, ptrval);
            val = ptrval;
            memptrs.push_back(values.size());
            values.push_back(val);
            return;
        }
        auto tk = LLVMGetTypeKind(LLVMTypeOf(val));
        if (tk == LLVMStructTypeKind) {
            auto ST = abi_struct_type(classes, sz);
            if (ST) {
                // break into argument-sized bits
                auto ptr = safe_alloca(LLVMTypeOf(val));
                auto zero = LLVMConstInt(i32T,0,false);
                LLVMBuildStore(builder, val, ptr);
                ptr = LLVMBuildBitCast(builder, ptr, LLVMPointerType(ST, 0), "");
                for (size_t i = 0; i < sz; ++i) {
                    LLVMValueRef indices[] = {
                        zero, LLVMConstInt(i32T,i,false),
                    };
                    auto val = LLVMBuildGEP(builder, ptr, indices, 2, "");
                    val = LLVMBuildLoad(builder, val, "");
                    values.push_back(val);
                }
                return;
            }
        }
        values.push_back(val);
    }

    static void abi_transform_parameter(const Type *AT,
        std::vector<LLVMTypeRef> &params) {
        ABIClass classes[MAX_ABI_CLASSES];
        size_t sz = abi_classify(AT, classes);
        auto T = type_to_llvm_type(AT);
        if (!sz) {
            params.push_back(LLVMPointerType(T, 0));
            return;
        }
        auto tk = LLVMGetTypeKind(T);
        if (tk == LLVMStructTypeKind) {
            auto ST = abi_struct_type(classes, sz);
            if (ST) {
                for (size_t i = 0; i < sz; ++i) {
                    params.push_back(LLVMStructGetTypeAtIndex(ST, i));
                }
                return;
            }
        }
        params.push_back(T);
    }

    static LLVMTypeRef create_llvm_type(const Type *type) {
        switch(type->kind()) {
        case TK_Integer:
            return LLVMIntType(cast<IntegerType>(type)->width);
        case TK_Real:
            switch(cast<RealType>(type)->width) {
            case 32: return f32T;
            case 64: return f64T;
            default: break;
            }
            break;
        case TK_Extern: {
            return LLVMPointerType(
                _type_to_llvm_type(cast<ExternType>(type)->type), 0);
        } break;
        case TK_Pointer:
            return LLVMPointerType(
                _type_to_llvm_type(cast<PointerType>(type)->element_type), 0);
        case TK_Array: {
            auto ai = cast<ArrayType>(type);
            return LLVMArrayType(_type_to_llvm_type(ai->element_type), ai->count);
        } break;
        case TK_Vector: {
            auto vi = cast<VectorType>(type);
            return LLVMVectorType(_type_to_llvm_type(vi->element_type), vi->count);
        } break;
        case TK_Tuple: {
            auto ti = cast<TupleType>(type);
            size_t count = ti->types.size();
            LLVMTypeRef elements[count];
            for (size_t i = 0; i < count; ++i) {
                elements[i] = _type_to_llvm_type(ti->types[i]);
            }
            return LLVMStructType(elements, count, ti->packed);
        } break;
        case TK_Union: {
            auto ui = cast<UnionType>(type);
            return _type_to_llvm_type(ui->tuple_type);
        } break;
        case TK_Typename: {
            if (type == TYPE_Void)
                return LLVMVoidType();
            else if (type == TYPE_Sampler) {
                location_error(String::from(
                    "sampler type can not be used for native target"));
            }
            auto tn = cast<TypenameType>(type);
            if (tn->finalized()) {
                switch(tn->storage_type->kind()) {
                case TK_Tuple:
                case TK_Union: {
                    type_todo.push_back(type);
                } break;
                default: {
                    return create_llvm_type(tn->storage_type);
                } break;
                }
            }
            return LLVMStructCreateNamed(
                LLVMGetGlobalContext(), type->name()->data);
        } break;
        case TK_ReturnLabel: {
            auto rlt = cast<ReturnLabelType>(type);
            return _type_to_llvm_type(rlt->return_type);
        } break;
        case TK_Function: {
            auto fi = cast<FunctionType>(type);
            size_t count = fi->argument_types.size();
            bool use_sret = is_memory_class(fi->return_type);

            std::vector<LLVMTypeRef> elements;
            elements.reserve(count);
            LLVMTypeRef rettype;
            if (use_sret) {
                elements.push_back(
                    LLVMPointerType(_type_to_llvm_type(fi->return_type), 0));
                rettype = voidT;
            } else {
                rettype = _type_to_llvm_type(fi->return_type);
            }
            for (size_t i = 0; i < count; ++i) {
                auto AT = fi->argument_types[i];
                abi_transform_parameter(AT, elements);
            }
            return LLVMFunctionType(rettype,
                &elements[0], elements.size(), fi->vararg());
        } break;
        case TK_SampledImage: {
            location_error(String::from(
                "sampled image type can not be used for native target"));
        } break;
        case TK_Image: {
            location_error(String::from(
                "image type can not be used for native target"));
        } break;
        };

        StyledString ss;
        ss.out << "IL->IR: cannot convert type " << type;
        location_error(ss.str());
        return nullptr;
    }

    static size_t finalize_types() {
        size_t result = type_todo.size();
        while (!type_todo.empty()) {
            const Type *T = type_todo.back();
            type_todo.pop_back();
            auto tn = cast<TypenameType>(T);
            if (!tn->finalized())
                continue;
            LLVMTypeRef LLT = _type_to_llvm_type(T);
            const Type *ST = tn->storage_type;
            switch(ST->kind()) {
            case TK_Tuple: {
                auto ti = cast<TupleType>(ST);
                size_t count = ti->types.size();
                LLVMTypeRef elements[count];
                for (size_t i = 0; i < count; ++i) {
                    elements[i] = _type_to_llvm_type(ti->types[i]);
                }
                LLVMStructSetBody(LLT, elements, count, false);
            } break;
            case TK_Union: {
                auto ui = cast<UnionType>(ST);
                size_t count = ui->types.size();
                size_t sz = ui->size;
                size_t al = ui->align;
                // find member with the same alignment
                for (size_t i = 0; i < count; ++i) {
                    const Type *ET = ui->types[i];
                    size_t etal = align_of(ET);
                    if (etal == al) {
                        size_t remsz = sz - size_of(ET);
                        LLVMTypeRef values[2];
                        values[0] = _type_to_llvm_type(ET);
                        if (remsz) {
                            // too small, add padding
                            values[1] = LLVMArrayType(i8T, remsz);
                            LLVMStructSetBody(LLT, values, 2, false);
                        } else {
                            LLVMStructSetBody(LLT, values, 1, false);
                        }
                        break;
                    }
                }
            } break;
            default: assert(false); break;
            }
        }
        return result;
    }

    static LLVMTypeRef _type_to_llvm_type(const Type *type) {
        auto it = type_cache.find(type);
        if (it == type_cache.end()) {
            LLVMTypeRef result = create_llvm_type(type);
            type_cache.insert({type, result});
            return result;
        } else {
            return it->second;
        }
    }

    static LLVMTypeRef type_to_llvm_type(const Type *type) {
        auto typeref = _type_to_llvm_type(type);
        finalize_types();
        return typeref;
    }

    LLVMValueRef label_to_value(Label *label) {
        if (label->is_basic_block_like()) {
            auto bb = label_to_basic_block(label);
            if (!bb) return nullptr;
            else
                return LLVMBasicBlockAsValue(bb);
        } else {
            return label_to_function(label);
        }
    }

    static void fatal_error_handler(const char *Reason) {
        location_error(String::from_cstr(Reason));
    }

    LLVMValueRef argument_to_value(Any value) {
        if (value.type == TYPE_Parameter) {
            auto it = param2value.find({active_function_value, value.parameter});
            if (it == param2value.end()) {
                assert(active_function_value);
#if 0
                {
                    StyledStream ss(std::cerr);
                    ss << "function context:" << std::endl;
                    stream_label(ss, active_function, StreamLabelFormat::debug_scope());
                }
                if (value.parameter->label) {
                    StyledStream ss(std::cerr);
                    ss << "parameter context:" << std::endl;
                    stream_label(ss, value.parameter->label, StreamLabelFormat::debug_scope());
                }
#endif
                StyledString ss;
                ss.out << "IL->IR: can't translate free variable " << value.parameter;
                location_error(ss.str());
            }
            return it->second;
        }

        switch(value.type->kind()) {
        case TK_Integer: {
            auto it = cast<IntegerType>(value.type);
            if (it->issigned) {
                switch(it->width) {
                case 8: return LLVMConstInt(i8T, value.i8, true);
                case 16: return LLVMConstInt(i16T, value.i16, true);
                case 32: return LLVMConstInt(i32T, value.i32, true);
                case 64: return LLVMConstInt(i64T, value.i64, true);
                default: break;
                }
            } else {
                switch(it->width) {
                case 1: return LLVMConstInt(i1T, value.i1, false);
                case 8: return LLVMConstInt(i8T, value.u8, false);
                case 16: return LLVMConstInt(i16T, value.u16, false);
                case 32: return LLVMConstInt(i32T, value.u32, false);
                case 64: return LLVMConstInt(i64T, value.u64, false);
                default: break;
                }
            }
        } break;
        case TK_Real: {
            auto rt = cast<RealType>(value.type);
            switch(rt->width) {
            case 32: return LLVMConstReal(f32T, value.f32);
            case 64: return LLVMConstReal(f64T, value.f64);
            default: break;
            }
        } break;
        case TK_Extern: {
            auto it = extern2global.find(value);
            if (it == extern2global.end()) {
                const String *namestr = value.symbol.name();
                const char *name = namestr->data;
                assert(name);
                auto et = cast<ExternType>(value.type);
                LLVMTypeRef LLT = type_to_llvm_type(et->type);
                LLVMValueRef result = nullptr;
                if ((namestr->count > 5) && !strncmp(name, "llvm.", 5)) {
                    result = LLVMAddFunction(module, name, LLT);
                } else {
                    void *pptr = local_aware_dlsym(name);
                    uint64_t ptr = *(uint64_t*)&pptr;
                    if (!ptr) {
                        LLVMInstallFatalErrorHandler(fatal_error_handler);
                        SCOPES_TRY()
                        ptr = LLVMGetGlobalValueAddress(ee, name);
                        SCOPES_CATCH(e)
                        SCOPES_TRY_END()
                        LLVMResetFatalErrorHandler();
                    }
                    if (!ptr) {
                        StyledString ss;
                        ss.out << "could not resolve " << value;
                        location_error(ss.str());
                    }
                    result = LLVMAddGlobal(module, LLT, name);
                }
                extern2global.insert({ value, result });
                return result;
            } else {
                return it->second;
            }
        } break;
        case TK_Pointer: {
            LLVMTypeRef LLT = type_to_llvm_type(value.type);
            if (!value.pointer) {
                return LLVMConstPointerNull(LLT);
            } else if (inline_pointers) {
                return LLVMConstIntToPtr(
                    LLVMConstInt(i64T, *(uint64_t*)&value.pointer, false),
                    LLT);
            } else {
                // to serialize a pointer, we serialize the allocation range
                // of the pointer as a global binary blob
                void *baseptr;
                size_t alloc_size;
                if (!find_allocation(value.pointer, baseptr, alloc_size)) {
                    StyledString ss;
                    ss.out << "IL->IR: constant pointer of type " << value.type
                        << " points to unserializable memory";
                    location_error(ss.str());
                }
                LLVMValueRef basevalue = nullptr;
                auto it = ptr2global.find(baseptr);

                auto pi = cast<PointerType>(value.type);
                bool writable = pi->is_writable();

                if (it == ptr2global.end()) {
                    auto data = LLVMConstString((const char *)baseptr, alloc_size, true);
                    basevalue = LLVMAddGlobal(module, LLVMTypeOf(data), "");
                    ptr2global.insert({ baseptr, basevalue });
                    LLVMSetInitializer(basevalue, data);
                    if (!writable) {
                        LLVMSetGlobalConstant(basevalue, true);
                    }
                } else {
                    basevalue = it->second;
                }
                size_t offset = (uint8_t*)value.pointer - (uint8_t*)baseptr;
                LLVMValueRef indices[2];
                indices[0] = LLVMConstInt(i64T, 0, false);
                indices[1] = LLVMConstInt(i64T, offset, false);
                return LLVMConstPointerCast(
                    LLVMConstGEP(basevalue, indices, 2), LLT);
            }
        } break;
        case TK_Typename: {
            LLVMTypeRef LLT = type_to_llvm_type(value.type);
            auto tn = cast<TypenameType>(value.type);
            switch(tn->storage_type->kind()) {
            case TK_Tuple: {
                auto ti = cast<TupleType>(tn->storage_type);
                size_t count = ti->types.size();
                LLVMValueRef values[count];
                for (size_t i = 0; i < count; ++i) {
                    values[i] = argument_to_value(ti->unpack(value.pointer, i));
                }
                return LLVMConstNamedStruct(LLT, values, count);
            } break;
            default: {
                Any storage_value = value;
                storage_value.type = tn->storage_type;
                LLVMValueRef val = argument_to_value(storage_value);
                return LLVMConstBitCast(val, LLT);
            } break;
            }
        } break;
        case TK_Array: {
            auto ai = cast<ArrayType>(value.type);
            size_t count = ai->count;
            LLVMValueRef values[count];
            for (size_t i = 0; i < count; ++i) {
                values[i] = argument_to_value(ai->unpack(value.pointer, i));
            }
            return LLVMConstArray(type_to_llvm_type(ai->element_type),
                values, count);
        } break;
        case TK_Vector: {
            auto vi = cast<VectorType>(value.type);
            size_t count = vi->count;
            LLVMValueRef values[count];
            for (size_t i = 0; i < count; ++i) {
                values[i] = argument_to_value(vi->unpack(value.pointer, i));
            }
            return LLVMConstVector(values, count);
        } break;
        case TK_Tuple: {
            auto ti = cast<TupleType>(value.type);
            size_t count = ti->types.size();
            LLVMValueRef values[count];
            for (size_t i = 0; i < count; ++i) {
                values[i] = argument_to_value(ti->unpack(value.pointer, i));
            }
            return LLVMConstStruct(values, count, false);
        } break;
        case TK_Union: {
            auto ui = cast<UnionType>(value.type);
            value.type = ui->tuple_type;
            return argument_to_value(value);
        } break;
        default: break;
        };

        StyledString ss;
        ss.out << "IL->IR: cannot convert argument of type " << value.type;
        location_error(ss.str());
        return nullptr;
    }

    LLVMValueRef build_call(const Type *functype, LLVMValueRef func, Args &args,
        bool &multiple_return_values) {
        size_t argcount = args.size() - 1;

        auto fi = cast<FunctionType>(functype);

        bool use_sret = is_memory_class(fi->return_type);

        std::vector<LLVMValueRef> values;
        values.reserve(argcount + 1);

        if (use_sret) {
            values.push_back(safe_alloca(_type_to_llvm_type(fi->return_type)));
        }
        std::vector<size_t> memptrs;
        for (size_t i = 0; i < argcount; ++i) {
            auto &&arg = args[i + 1];
            LLVMValueRef val = argument_to_value(arg.value);
            auto AT = arg.value.indirect_type();
            abi_export_argument(val, AT, values, memptrs);
        }

        size_t fargcount = fi->argument_types.size();
        assert(argcount >= fargcount);
        // make variadic calls C compatible
        if (fi->flags & FF_Variadic) {
            for (size_t i = fargcount; i < argcount; ++i) {
                auto value = values[i];
                // floats need to be widened to doubles
                if (LLVMTypeOf(value) == f32T) {
                    values[i] = LLVMBuildFPExt(builder, value, f64T, "");
                }
            }
        }

        auto ret = LLVMBuildCall(builder, func, &values[0], values.size(), "");
        for (auto idx : memptrs) {
            auto i = idx + 1;
            LLVMAddCallSiteAttribute(ret, i, attr_nonnull);
        }
        auto rlt = cast<ReturnLabelType>(fi->return_type);
        multiple_return_values = rlt->has_multiple_return_values();
        if (use_sret) {
            LLVMAddCallSiteAttribute(ret, 1, attr_sret);
            return LLVMBuildLoad(builder, values[0], "");
        } else if (rlt->return_type == TYPE_Void) {
            return nullptr;
        } else {
            return ret;
        }
    }

    LLVMValueRef set_debug_location(Label *label) {
        assert(use_debug_info);
        LLVMValueRef diloc = anchor_to_location(label->body.anchor);
        LLVMSetCurrentDebugLocation(builder, diloc);
        return diloc;
    }

    LLVMValueRef build_length_op(LLVMValueRef x) {
        auto T = LLVMTypeOf(x);
        auto ET = LLVMGetElementType(T);
        LLVMValueRef func_sqrt = get_intrinsic((ET == f64T)?llvm_sqrt_f64:llvm_sqrt_f32);
        assert(func_sqrt);
        auto count = LLVMGetVectorSize(T);
        LLVMValueRef src = LLVMBuildFMul(builder, x, x, "");
        LLVMValueRef retvalue = nullptr;
        for (unsigned i = 0; i < count; ++i) {
            LLVMValueRef idx = LLVMConstInt(i32T, i, false);
            LLVMValueRef val = LLVMBuildExtractElement(builder, src, idx, "");
            if (i == 0) {
                retvalue = val;
            } else {
                retvalue = LLVMBuildFAdd(builder, retvalue, val, "");
            }
        }
        LLVMValueRef values[] = { retvalue };
        return LLVMBuildCall(builder, func_sqrt, values, 1, "");
    }

    LLVMValueRef safe_alloca(LLVMTypeRef ty, LLVMValueRef val = nullptr) {
        if (val && !LLVMIsConstant(val)) {
            // for stack arrays with dynamic size, build the array locally
            return LLVMBuildArrayAlloca(builder, ty, val, "");
        } else {
#if 0
            // add allocas at the tail
            auto oldbb = LLVMGetInsertBlock(builder);
            auto entry = LLVMGetEntryBasicBlock(active_function_value);
            auto term = LLVMGetBasicBlockTerminator(entry);
            if (term) {
                LLVMPositionBuilderBefore(builder, term);
            } else {
                LLVMPositionBuilderAtEnd(builder, entry);
            }
            LLVMValueRef result;
            if (val) {
                result = LLVMBuildArrayAlloca(builder, ty, val, "");
            } else {
                result = LLVMBuildAlloca(builder, ty, "");
            }
            LLVMPositionBuilderAtEnd(builder, oldbb);
            return result;
#elif 1
            // add allocas to the front
            auto oldbb = LLVMGetInsertBlock(builder);
            auto entry = LLVMGetEntryBasicBlock(active_function_value);
            auto instr = LLVMGetFirstInstruction(entry);
            if (instr) {
                LLVMPositionBuilderBefore(builder, instr);
            } else {
                LLVMPositionBuilderAtEnd(builder, entry);
            }
            LLVMValueRef result;
            if (val) {
                result = LLVMBuildArrayAlloca(builder, ty, val, "");
            } else {
                result = LLVMBuildAlloca(builder, ty, "");
                //LLVMSetAlignment(result, 16);
            }
            LLVMPositionBuilderAtEnd(builder, oldbb);
            return result;
#else
            // add allocas locally
            LLVMValueRef result;
            if (val) {
                result = LLVMBuildArrayAlloca(builder, ty, val, "");
            } else {
                result = LLVMBuildAlloca(builder, ty, "");
            }
            return result;
#endif
        }
    }

    void write_label_body(Label *label) {
    repeat:
        if (!label->body.is_complete()) {
            set_active_anchor(label->body.anchor);
            location_error(String::from("IL->IR: incomplete label body encountered"));
        }
        bool terminated = false;
        auto &&body = label->body;
        auto &&enter = body.enter;
        auto &&args = body.args;

        set_active_anchor(label->body.anchor);

        LLVMValueRef diloc = nullptr;
        if (use_debug_info) {
            diloc = set_debug_location(label);
        }

        assert(!args.empty());
        size_t argcount = args.size() - 1;
        size_t argn = 1;
#define READ_ANY(NAME) \
        assert(argn <= argcount); \
        Any &NAME = args[argn++].value;
#define READ_VALUE(NAME) \
        assert(argn <= argcount); \
        LLVMValueRef NAME = argument_to_value(args[argn++].value);
#define READ_LABEL_VALUE(NAME) \
        assert(argn <= argcount); \
        LLVMValueRef NAME = label_to_value(args[argn++].value); \
        assert(NAME);
#define READ_TYPE(NAME) \
        assert(argn <= argcount); \
        assert(args[argn].value.type == TYPE_Type); \
        LLVMTypeRef NAME = type_to_llvm_type(args[argn++].value.typeref);

        LLVMValueRef retvalue = nullptr;
        bool multiple_return_values = false;
        if (enter.type == TYPE_Builtin) {
            switch(enter.builtin.value()) {
            case FN_Branch: {
                READ_VALUE(cond);
                READ_LABEL_VALUE(then_block);
                READ_LABEL_VALUE(else_block);
                assert(LLVMValueIsBasicBlock(then_block));
                assert(LLVMValueIsBasicBlock(else_block));
                LLVMBuildCondBr(builder, cond,
                    LLVMValueAsBasicBlock(then_block),
                    LLVMValueAsBasicBlock(else_block));
            } break;
            case OP_Tertiary: {
                READ_VALUE(cond);
                READ_VALUE(then_value);
                READ_VALUE(else_value);
                retvalue = LLVMBuildSelect(
                    builder, cond, then_value, else_value, "");
            } break;
            case FN_Unconst: {
                READ_ANY(val);
                if (val.type == TYPE_Label) {
                    retvalue = label_to_function(val);
                } else {
                    retvalue = argument_to_value(val);
                }
            } break;
            case FN_ExtractValue: {
                READ_VALUE(val);
                READ_ANY(index);
                retvalue = LLVMBuildExtractValue(
                    builder, val, cast_number<int32_t>(index), "");
            } break;
            case FN_InsertValue: {
                READ_VALUE(val);
                READ_VALUE(eltval);
                READ_ANY(index);
                retvalue = LLVMBuildInsertValue(
                    builder, val, eltval, cast_number<int32_t>(index), "");
            } break;
            case FN_ExtractElement: {
                READ_VALUE(val);
                READ_VALUE(index);
                retvalue = LLVMBuildExtractElement(builder, val, index, "");
            } break;
            case FN_InsertElement: {
                READ_VALUE(val);
                READ_VALUE(eltval);
                READ_VALUE(index);
                retvalue = LLVMBuildInsertElement(builder, val, eltval, index, "");
            } break;
            case FN_ShuffleVector: {
                READ_VALUE(v1);
                READ_VALUE(v2);
                READ_VALUE(mask);
                retvalue = LLVMBuildShuffleVector(builder, v1, v2, mask, "");
            } break;
            case FN_Undef: { READ_TYPE(ty);
                retvalue = LLVMGetUndef(ty); } break;
            case FN_Alloca: { READ_TYPE(ty);
                retvalue = safe_alloca(ty);
            } break;
            case FN_AllocaExceptionPad: {
                LLVMTypeRef ty = type_to_llvm_type(Array(TYPE_U8, sizeof(ExceptionPad)));
#ifdef SCOPES_WIN32
                retvalue = LLVMBuildAlloca(builder, ty, "");
#else
                retvalue = safe_alloca(ty);
#endif
            } break;
            case FN_AllocaArray: { READ_TYPE(ty); READ_VALUE(val);
                retvalue = safe_alloca(ty, val); } break;
            case FN_AllocaOf: {
                READ_VALUE(val);
                retvalue = safe_alloca(LLVMTypeOf(val));
                LLVMBuildStore(builder, val, retvalue);
            } break;
            case FN_Malloc: { READ_TYPE(ty);
                retvalue = LLVMBuildMalloc(builder, ty, ""); } break;
            case FN_MallocArray: { READ_TYPE(ty); READ_VALUE(val);
                retvalue = LLVMBuildArrayMalloc(builder, ty, val, ""); } break;
            case FN_Free: { READ_VALUE(val);
                LLVMBuildFree(builder, val);
                retvalue = nullptr; } break;
            case FN_GetElementPtr: {
                READ_VALUE(pointer);
                assert(argcount > 1);
                size_t count = argcount - 1;
                LLVMValueRef indices[count];
                for (size_t i = 0; i < count; ++i) {
                    indices[i] = argument_to_value(args[argn + i].value);
                }
                retvalue = LLVMBuildGEP(builder, pointer, indices, count, "");
            } break;
            case FN_Bitcast: { READ_VALUE(val); READ_TYPE(ty);
                auto T = LLVMTypeOf(val);
                if (T == ty) {
                    retvalue = val;
                } else if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
                    // completely braindead, but what can you do
                    LLVMValueRef ptr = safe_alloca(T);
                    LLVMBuildStore(builder, val, ptr);
                    ptr = LLVMBuildBitCast(builder, ptr, LLVMPointerType(ty,0), "");
                    retvalue = LLVMBuildLoad(builder, ptr, "");
                } else {
                    retvalue = LLVMBuildBitCast(builder, val, ty, "");
                }
            } break;
            case FN_IntToPtr: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildIntToPtr(builder, val, ty, ""); } break;
            case FN_PtrToInt: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildPtrToInt(builder, val, ty, ""); } break;
            case FN_ITrunc: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildTrunc(builder, val, ty, ""); } break;
            case FN_SExt: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildSExt(builder, val, ty, ""); } break;
            case FN_ZExt: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildZExt(builder, val, ty, ""); } break;
            case FN_FPTrunc: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildFPTrunc(builder, val, ty, ""); } break;
            case FN_FPExt: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildFPExt(builder, val, ty, ""); } break;
            case FN_FPToUI: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildFPToUI(builder, val, ty, ""); } break;
            case FN_FPToSI: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildFPToSI(builder, val, ty, ""); } break;
            case FN_UIToFP: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildUIToFP(builder, val, ty, ""); } break;
            case FN_SIToFP: { READ_VALUE(val); READ_TYPE(ty);
                retvalue = LLVMBuildSIToFP(builder, val, ty, ""); } break;
            case FN_VolatileLoad:
            case FN_Load: { READ_VALUE(ptr);
                retvalue = LLVMBuildLoad(builder, ptr, "");
                if (enter.builtin.value() == FN_VolatileLoad) { LLVMSetVolatile(retvalue, true); }
            } break;
            case FN_VolatileStore:
            case FN_Store: { READ_VALUE(val); READ_VALUE(ptr);
                retvalue = LLVMBuildStore(builder, val, ptr);
                if (enter.builtin.value() == FN_VolatileStore) { LLVMSetVolatile(retvalue, true); }
                retvalue = nullptr;
            } break;
            case OP_ICmpEQ:
            case OP_ICmpNE:
            case OP_ICmpUGT:
            case OP_ICmpUGE:
            case OP_ICmpULT:
            case OP_ICmpULE:
            case OP_ICmpSGT:
            case OP_ICmpSGE:
            case OP_ICmpSLT:
            case OP_ICmpSLE: {
                READ_VALUE(a); READ_VALUE(b);
                LLVMIntPredicate pred = LLVMIntEQ;
                switch(enter.builtin.value()) {
                    case OP_ICmpEQ: pred = LLVMIntEQ; break;
                    case OP_ICmpNE: pred = LLVMIntNE; break;
                    case OP_ICmpUGT: pred = LLVMIntUGT; break;
                    case OP_ICmpUGE: pred = LLVMIntUGE; break;
                    case OP_ICmpULT: pred = LLVMIntULT; break;
                    case OP_ICmpULE: pred = LLVMIntULE; break;
                    case OP_ICmpSGT: pred = LLVMIntSGT; break;
                    case OP_ICmpSGE: pred = LLVMIntSGE; break;
                    case OP_ICmpSLT: pred = LLVMIntSLT; break;
                    case OP_ICmpSLE: pred = LLVMIntSLE; break;
                    default: assert(false); break;
                }
                retvalue = LLVMBuildICmp(builder, pred, a, b, "");
            } break;
            case OP_FCmpOEQ:
            case OP_FCmpONE:
            case OP_FCmpORD:
            case OP_FCmpOGT:
            case OP_FCmpOGE:
            case OP_FCmpOLT:
            case OP_FCmpOLE:
            case OP_FCmpUEQ:
            case OP_FCmpUNE:
            case OP_FCmpUNO:
            case OP_FCmpUGT:
            case OP_FCmpUGE:
            case OP_FCmpULT:
            case OP_FCmpULE: {
                READ_VALUE(a); READ_VALUE(b);
                LLVMRealPredicate pred = LLVMRealOEQ;
                switch(enter.builtin.value()) {
                    case OP_FCmpOEQ: pred = LLVMRealOEQ; break;
                    case OP_FCmpONE: pred = LLVMRealONE; break;
                    case OP_FCmpORD: pred = LLVMRealORD; break;
                    case OP_FCmpOGT: pred = LLVMRealOGT; break;
                    case OP_FCmpOGE: pred = LLVMRealOGE; break;
                    case OP_FCmpOLT: pred = LLVMRealOLT; break;
                    case OP_FCmpOLE: pred = LLVMRealOLE; break;
                    case OP_FCmpUEQ: pred = LLVMRealUEQ; break;
                    case OP_FCmpUNE: pred = LLVMRealUNE; break;
                    case OP_FCmpUNO: pred = LLVMRealUNO; break;
                    case OP_FCmpUGT: pred = LLVMRealUGT; break;
                    case OP_FCmpUGE: pred = LLVMRealUGE; break;
                    case OP_FCmpULT: pred = LLVMRealULT; break;
                    case OP_FCmpULE: pred = LLVMRealULE; break;
                    default: assert(false); break;
                }
                retvalue = LLVMBuildFCmp(builder, pred, a, b, "");
            } break;
            case OP_Add: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildAdd(builder, a, b, ""); } break;
            case OP_AddNUW: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildNUWAdd(builder, a, b, ""); } break;
            case OP_AddNSW: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildNSWAdd(builder, a, b, ""); } break;
            case OP_Sub: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildSub(builder, a, b, ""); } break;
            case OP_SubNUW: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildNUWSub(builder, a, b, ""); } break;
            case OP_SubNSW: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildNSWSub(builder, a, b, ""); } break;
            case OP_Mul: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildMul(builder, a, b, ""); } break;
            case OP_MulNUW: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildNUWMul(builder, a, b, ""); } break;
            case OP_MulNSW: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildNSWMul(builder, a, b, ""); } break;
            case OP_SDiv: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildSDiv(builder, a, b, ""); } break;
            case OP_UDiv: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildUDiv(builder, a, b, ""); } break;
            case OP_SRem: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildSRem(builder, a, b, ""); } break;
            case OP_URem: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildURem(builder, a, b, ""); } break;
            case OP_Shl: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildShl(builder, a, b, ""); } break;
            case OP_LShr: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildLShr(builder, a, b, ""); } break;
            case OP_AShr: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildAShr(builder, a, b, ""); } break;
            case OP_BAnd: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildAnd(builder, a, b, ""); } break;
            case OP_BOr: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildOr(builder, a, b, ""); } break;
            case OP_BXor: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildXor(builder, a, b, ""); } break;
            case OP_FAdd: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildFAdd(builder, a, b, ""); } break;
            case OP_FSub: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildFSub(builder, a, b, ""); } break;
            case OP_FMul: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildFMul(builder, a, b, ""); } break;
            case OP_FDiv: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildFDiv(builder, a, b, ""); } break;
            case OP_FRem: { READ_VALUE(a); READ_VALUE(b);
                retvalue = LLVMBuildFRem(builder, a, b, ""); } break;
            case FN_Length: {
                READ_VALUE(x);
                auto T = LLVMTypeOf(x);
                if (LLVMGetTypeKind(T) == LLVMVectorTypeKind) {
                    retvalue = build_length_op(x);
                } else {
                    LLVMValueRef func_fabs = get_intrinsic((T == f64T)?llvm_fabs_f64:llvm_fabs_f32);
                    assert(func_fabs);
                    LLVMValueRef values[] = { x };
                    retvalue = LLVMBuildCall(builder, func_fabs, values, 1, "");
                }
            } break;
            case FN_Normalize: {
                READ_VALUE(x);
                auto T = LLVMTypeOf(x);
                if (LLVMGetTypeKind(T) == LLVMVectorTypeKind) {
                    auto count = LLVMGetVectorSize(T);
                    auto ET = LLVMGetElementType(T);
                    LLVMValueRef l = build_length_op(x);
                    l = LLVMBuildInsertElement(builder,
                        LLVMGetUndef(LLVMVectorType(ET, 1)), l,
                        LLVMConstInt(i32T, 0, false),
                        "");
                    LLVMValueRef mask[count];
                    for (int i = 0; i < count; ++i) {
                        mask[i] = 0;
                    }
                    l = LLVMBuildShuffleVector(builder, l, l,
                        LLVMConstNull(LLVMVectorType(i32T, count)), "");
                    retvalue = LLVMBuildFDiv(builder, x, l, "");
                } else {
                    retvalue = LLVMConstReal(T, 1.0);
                }
            } break;
            case FN_Cross: {
                READ_VALUE(a);
                READ_VALUE(b);
                auto T = LLVMTypeOf(a);
                assert (LLVMGetTypeKind(T) == LLVMVectorTypeKind);
                LLVMValueRef i0 = LLVMConstInt(i32T, 0, false);
                LLVMValueRef i1 = LLVMConstInt(i32T, 1, false);
                LLVMValueRef i2 = LLVMConstInt(i32T, 2, false);
                LLVMValueRef i120[] = { i1, i2, i0 };
                LLVMValueRef v120 = LLVMConstVector(i120, 3);
                LLVMValueRef a120 = LLVMBuildShuffleVector(builder, a, a, v120, "");
                LLVMValueRef b120 = LLVMBuildShuffleVector(builder, b, b, v120, "");
                retvalue = LLVMBuildFSub(builder,
                    LLVMBuildFMul(builder, a, b120, ""),
                    LLVMBuildFMul(builder, b, a120, ""), "");
                retvalue = LLVMBuildShuffleVector(builder, retvalue, retvalue, v120, "");
            } break;
            // binops
            case OP_Pow: {
                READ_VALUE(a);
                READ_VALUE(b);
                auto T = LLVMTypeOf(a);
                auto ET = T;
                if (LLVMGetTypeKind(T) == LLVMVectorTypeKind) {
                    ET = LLVMGetElementType(T);
                }
                LLVMValueRef func = nullptr;
                Intrinsic op = NumIntrinsics;
                switch(enter.builtin.value()) {
                case OP_Pow: { op = (ET == f64T)?llvm_pow_f64:llvm_pow_f32; } break;
                default: break;
                }
                func = get_intrinsic(op);
                assert(func);
                if (LLVMGetTypeKind(T) == LLVMVectorTypeKind) {
                    auto count = LLVMGetVectorSize(T);
                    retvalue = LLVMGetUndef(T);
                    for (unsigned i = 0; i < count; ++i) {
                        LLVMValueRef idx = LLVMConstInt(i32T, i, false);
                        LLVMValueRef values[] = {
                            LLVMBuildExtractElement(builder, a, idx, ""),
                            LLVMBuildExtractElement(builder, b, idx, "")
                        };
                        LLVMValueRef eltval = LLVMBuildCall(builder, func, values, 2, "");
                        retvalue = LLVMBuildInsertElement(builder, retvalue, eltval, idx, "");
                    }
                } else {
                    LLVMValueRef values[] = { a, b };
                    retvalue = LLVMBuildCall(builder, func, values, 2, "");
                }
            } break;
            // unops
            case OP_Sin:
            case OP_Cos:
            case OP_Sqrt:
            case OP_FAbs:
            case OP_FSign:
            case OP_Trunc:
            case OP_Floor: { READ_VALUE(x);
                auto T = LLVMTypeOf(x);
                auto ET = T;
                if (LLVMGetTypeKind(T) == LLVMVectorTypeKind) {
                    ET = LLVMGetElementType(T);
                }
                LLVMValueRef func = nullptr;
                Intrinsic op = NumIntrinsics;
                switch(enter.builtin.value()) {
                case OP_Sin: { op = (ET == f64T)?llvm_sin_f64:llvm_sin_f32; } break;
                case OP_Cos: { op = (ET == f64T)?llvm_cos_f64:llvm_cos_f32; } break;
                case OP_Sqrt: { op = (ET == f64T)?llvm_sqrt_f64:llvm_sqrt_f32; } break;
                case OP_FAbs: { op = (ET == f64T)?llvm_fabs_f64:llvm_fabs_f32; } break;
                case OP_Trunc: { op = (ET == f64T)?llvm_trunc_f64:llvm_trunc_f32; } break;
                case OP_Floor: { op = (ET == f64T)?llvm_floor_f64:llvm_floor_f32; } break;
                case OP_FSign: { op = (ET == f64T)?custom_fsign_f64:custom_fsign_f32; } break;
                default: break;
                }
                func = get_intrinsic(op);
                assert(func);
                if (LLVMGetTypeKind(T) == LLVMVectorTypeKind) {
                    auto count = LLVMGetVectorSize(T);
                    retvalue = LLVMGetUndef(T);
                    for (unsigned i = 0; i < count; ++i) {
                        LLVMValueRef idx = LLVMConstInt(i32T, i, false);
                        LLVMValueRef values[] = { LLVMBuildExtractElement(builder, x, idx, "") };
                        LLVMValueRef eltval = LLVMBuildCall(builder, func, values, 1, "");
                        retvalue = LLVMBuildInsertElement(builder, retvalue, eltval, idx, "");
                    }
                } else {
                    LLVMValueRef values[] = { x };
                    retvalue = LLVMBuildCall(builder, func, values, 1, "");
                }
            } break;
            case SFXFN_Unreachable:
                retvalue = LLVMBuildUnreachable(builder); break;
            default: {
                StyledString ss;
                ss.out << "IL->IR: unsupported builtin " << enter.builtin << " encountered";
                location_error(ss.str());
            } break;
            }
        } else if (enter.type == TYPE_Label) {
            LLVMValueRef value = label_to_value(enter);
            if (!value) {
                // no basic block was generated - just generate assignments
                LLVMValueRef values[argcount];
                for (size_t i = 0; i < argcount; ++i) {
                    values[i] = argument_to_value(args[i + 1].value);
                }
                auto &&params = enter.label->params;
                for (size_t i = 1; i < params.size(); ++i) {
                    param2value[{active_function_value, params[i]}] = values[i - 1];
                }
                label = enter.label;
                goto repeat;
            } else if (LLVMValueIsBasicBlock(value)) {
                LLVMValueRef values[argcount];
                for (size_t i = 0; i < argcount; ++i) {
                    values[i] = argument_to_value(args[i + 1].value);
                }
                auto bbfrom = LLVMGetInsertBlock(builder);
                // assign phi nodes
                auto &&params = enter.label->params;
                LLVMBasicBlockRef incobbs[] = { bbfrom };
                for (size_t i = 1; i < params.size(); ++i) {
                    Parameter *param = params[i];
                    LLVMValueRef phinode = argument_to_value(param);
                    LLVMValueRef incovals[] = { values[i - 1] };
                    LLVMAddIncoming(phinode, incovals, incobbs, 1);
                }
                LLVMBuildBr(builder, LLVMValueAsBasicBlock(value));
                terminated = true;
            } else {
                if (use_debug_info) {
                    LLVMSetCurrentDebugLocation(builder, diloc);
                }
                retvalue = build_call(
                    enter.label->get_function_type(),
                    value, args, multiple_return_values);
            }
        } else if (enter.type == TYPE_Closure) {
            StyledString ss;
            ss.out << "IL->IR: invalid call of compile time closure at runtime";
            location_error(ss.str());
        } else if (is_function_pointer(enter.indirect_type())) {
            retvalue = build_call(extract_function_type(enter.indirect_type()),
                argument_to_value(enter), args, multiple_return_values);
        } else if (enter.type == TYPE_Parameter) {
            assert (enter.parameter->type != TYPE_Nothing);
            assert(enter.parameter->type != TYPE_Unknown);
            LLVMValueRef values[argcount];
            for (size_t i = 0; i < argcount; ++i) {
                values[i] = argument_to_value(args[i + 1].value);
            }
            // must be a return
            assert(enter.parameter->index == 0);
            // must be returning from this function
            assert(enter.parameter->label == active_function);

            Label *label = enter.parameter->label;
            bool use_sret = is_memory_class(label->get_return_type());
            if (use_sret) {
                auto it = param2value.find({active_function_value,enter.parameter});
                assert (it != param2value.end());
                if (argcount > 1) {
                    LLVMTypeRef types[argcount];
                    for (size_t i = 0; i < argcount; ++i) {
                        types[i] = LLVMTypeOf(values[i]);
                    }

                    LLVMValueRef val = LLVMGetUndef(LLVMStructType(types, argcount, false));
                    for (size_t i = 0; i < argcount; ++i) {
                        val = LLVMBuildInsertValue(builder, val, values[i], i, "");
                    }
                    LLVMBuildStore(builder, val, it->second);
                } else if (argcount == 1) {
                    LLVMBuildStore(builder, values[0], it->second);
                }
                LLVMBuildRetVoid(builder);
            } else {
                if (argcount > 1) {
                    LLVMBuildAggregateRet(builder, values, argcount);
                } else if (argcount == 1) {
                    LLVMBuildRet(builder, values[0]);
                } else {
                    LLVMBuildRetVoid(builder);
                }
            }
        } else {
            StyledString ss;
            ss.out << "IL->IR: cannot translate call to " << enter;
            location_error(ss.str());
        }

        Any contarg = args[0].value;
        if (terminated) {
            // write nothing
        } else if ((contarg.type == TYPE_Parameter)
            && (contarg.parameter->type != TYPE_Nothing)) {
            assert(contarg.parameter->type != TYPE_Unknown);
            assert(contarg.parameter->index == 0);
            assert(contarg.parameter->label == active_function);
            Label *label = contarg.parameter->label;
            bool use_sret = is_memory_class(label->get_return_type());
            if (use_sret) {
                auto it = param2value.find({active_function_value,contarg.parameter});
                assert (it != param2value.end());
                if (retvalue) {
                    LLVMBuildStore(builder, retvalue, it->second);
                }
                LLVMBuildRetVoid(builder);
            } else {
                if (retvalue) {
                    LLVMBuildRet(builder, retvalue);
                } else {
                    LLVMBuildRetVoid(builder);
                }
            }
        } else if (contarg.type == TYPE_Label) {
            auto bb = label_to_basic_block(contarg.label);
            if (bb) {
                if (retvalue) {
                    auto bbfrom = LLVMGetInsertBlock(builder);
                    // assign phi nodes
                    auto &&params = contarg.label->params;
                    LLVMBasicBlockRef incobbs[] = { bbfrom };
                    for (size_t i = 1; i < params.size(); ++i) {
                        Parameter *param = params[i];
                        LLVMValueRef phinode = argument_to_value(param);
                        LLVMValueRef incoval = nullptr;
                        if (multiple_return_values) {
                            incoval = LLVMBuildExtractValue(builder, retvalue, i - 1, "");
                        } else {
                            assert(params.size() == 2);
                            incoval = retvalue;
                        }
                        LLVMAddIncoming(phinode, &incoval, incobbs, 1);
                    }
                }

                LLVMBuildBr(builder, bb);
            } else {
                if (retvalue) {
                    // no basic block - just add assignments and continue
                    auto &&params = contarg.label->params;
                    for (size_t i = 1; i < params.size(); ++i) {
                        Parameter *param = params[i];
                        LLVMValueRef pvalue = nullptr;
                        if (multiple_return_values) {
                            pvalue = LLVMBuildExtractValue(builder, retvalue, i - 1, "");
                        } else {
                            assert(params.size() == 2);
                            pvalue = retvalue;
                        }
                        param2value[{active_function_value,param}] = pvalue;
                    }
                }
                label = contarg.label;
                goto repeat;
            }
        } else if (contarg.type == TYPE_Nothing) {
        } else {
            StyledStream ss(std::cerr);
            stream_label(ss, label, StreamLabelFormat::debug_single());
            location_error(String::from("IL->IR: continuation is of invalid type"));
            //assert(false && "todo: continuing with unexpected value");
        }

        LLVMSetCurrentDebugLocation(builder, nullptr);

    }
#undef READ_ANY
#undef READ_VALUE
#undef READ_TYPE
#undef READ_LABEL_VALUE

    void set_active_function(Label *l) {
        if (active_function == l) return;
        active_function = l;
        if (l) {
            auto it = label2func.find(l);
            assert(it != label2func.end());
            active_function_value = it->second;
        } else {
            active_function_value = nullptr;
        }
    }

    void process_labels() {
        while (!bb_label_todo.empty()) {
            auto it = bb_label_todo.back();
            set_active_function(it.first);
            Label *label = it.second;
            bb_label_todo.pop_back();

            auto it2 = label2bb.find({active_function_value, label});
            assert(it2 != label2bb.end());
            LLVMBasicBlockRef bb = it2->second;
            LLVMPositionBuilderAtEnd(builder, bb);

            write_label_body(label);
        }
    }

    bool has_single_caller(Label *l) {
        auto it = user_map.label_map.find(l);
        assert(it != user_map.label_map.end());
        auto &&users = it->second;
        if (users.size() != 1)
            return false;
        Label *userl = *users.begin();
        if (userl->body.enter == Any(l))
            return true;
        if (userl->body.args[0] == Any(l))
            return true;
        return false;
    }

    LLVMBasicBlockRef label_to_basic_block(Label *label) {
        auto old_bb = LLVMGetInsertBlock(builder);
        LLVMValueRef func = LLVMGetBasicBlockParent(old_bb);
        auto it = label2bb.find({func, label});
        if (it == label2bb.end()) {
            if (has_single_caller(label)) {
                // not generating basic blocks for single user labels
                label2bb.insert({{func, label}, nullptr});
                return nullptr;
            }
            const char *name = label->name.name()->data;
            auto bb = LLVMAppendBasicBlock(func, name);
            label2bb.insert({{func, label}, bb});
            bb_label_todo.push_back({active_function, label});
            LLVMPositionBuilderAtEnd(builder, bb);

            auto &&params = label->params;
            if (!params.empty()) {
                size_t paramcount = label->params.size() - 1;
                for (size_t i = 0; i < paramcount; ++i) {
                    Parameter *param = params[i + 1];
                    auto pvalue = LLVMBuildPhi(builder,
                        type_to_llvm_type(param->type),
                        param->name.name()->data);
                    param2value[{active_function_value,param}] = pvalue;
                }
            }

            LLVMPositionBuilderAtEnd(builder, old_bb);
            return bb;
        } else {
            return it->second;
        }
    }

    LLVMValueRef label_to_function(Label *label,
        bool root_function = false,
        Symbol funcname = SYM_Unnamed) {
        auto it = label2func.find(label);
        if (it == label2func.end()) {

            const Anchor *old_anchor = get_active_anchor();
            set_active_anchor(label->anchor);
            Label *last_function = active_function;

            auto old_bb = LLVMGetInsertBlock(builder);

            if (funcname == SYM_Unnamed) {
                funcname = label->name;
            }

            const char *name;
            if (root_function && (funcname == SYM_Unnamed)) {
                name = "unnamed";
            } else {
                name = funcname.name()->data;
            }

            label->verify_compilable();
            auto ilfunctype = label->get_function_type();
            auto fi = cast<FunctionType>(ilfunctype);
            bool use_sret = is_memory_class(fi->return_type);

            auto functype = type_to_llvm_type(ilfunctype);

            auto func = LLVMAddFunction(module, name, functype);
            if (use_debug_info) {
                LLVMSetFunctionSubprogram(func, label_to_subprogram(label));
            }
            LLVMSetLinkage(func, LLVMPrivateLinkage);
            label2func[label] = func;
            set_active_function(label);

            auto bb = LLVMAppendBasicBlock(func, "");
            LLVMPositionBuilderAtEnd(builder, bb);

            auto &&params = label->params;
            size_t offset = 0;
            if (use_sret) {
                offset++;
                Parameter *param = params[0];
                param2value[{active_function_value,param}] = LLVMGetParam(func, 0);
            }

            size_t paramcount = params.size() - 1;

            if (use_debug_info)
                set_debug_location(label);
            size_t k = offset;
            for (size_t i = 0; i < paramcount; ++i) {
                Parameter *param = params[i + 1];
                LLVMValueRef val = abi_import_argument(param, func, k);
                param2value[{active_function_value,param}] = val;
            }

            write_label_body(label);

            LLVMPositionBuilderAtEnd(builder, old_bb);

            set_active_function(last_function);
            set_active_anchor(old_anchor);
            return func;
        } else {
            return it->second;
        }
    }

    void setup_generate(const char *module_name) {
        module = LLVMModuleCreateWithName(module_name);
        builder = LLVMCreateBuilder();
        di_builder = LLVMCreateDIBuilder(module);

        if (use_debug_info) {
            const char *DebugStr = "Debug Info Version";
            LLVMValueRef DbgVer[3];
            DbgVer[0] = LLVMConstInt(i32T, 1, 0);
            DbgVer[1] = LLVMMDString(DebugStr, strlen(DebugStr));
            DbgVer[2] = LLVMConstInt(i32T, 3, 0);
            LLVMAddNamedMetadataOperand(module, "llvm.module.flags",
                LLVMMDNode(DbgVer, 3));

            LLVMDIBuilderCreateCompileUnit(di_builder,
                llvm::dwarf::DW_LANG_C99, "file", "directory", "scopes",
                false, "", 0, "", 0);
            //LLVMAddNamedMetadataOperand(module, "llvm.dbg.cu", dicu);
        }
    }

    void teardown_generate(Label *entry = nullptr) {
        process_labels();

        size_t k = finalize_types();
        assert(!k);

        LLVMDisposeBuilder(builder);
        LLVMDisposeDIBuilder(di_builder);

#if SCOPES_DEBUG_CODEGEN
        LLVMDumpModule(module);
#endif
        char *errmsg = NULL;
        if (LLVMVerifyModule(module, LLVMReturnStatusAction, &errmsg)) {
            StyledStream ss(std::cerr);
            if (entry) {
                stream_label(ss, entry, StreamLabelFormat());
            }
            LLVMDumpModule(module);
            location_error(
                String::join(
                    String::from("LLVM: "),
                    String::from_cstr(errmsg)));
        }
        LLVMDisposeMessage(errmsg);
    }

    // for generating object files
    LLVMModuleRef generate(const String *name, Scope *table) {

        {
            std::unordered_set<Label *> visited;
            std::vector<Label *> labels;
            Scope *t = table;
            while (t) {
                for (auto it = t->map->begin(); it != t->map->end(); ++it) {
                    Label *fn = it->second.value;

                    fn->verify_compilable();
                    fn->build_reachable(visited, &labels);
                }
                t = t->parent;
            }
            for (auto it = labels.begin(); it != labels.end(); ++it) {
                (*it)->insert_into_usermap(user_map);
            }
        }

        setup_generate(name->data);

        Scope *t = table;
        while (t) {
            for (auto it = t->map->begin(); it != t->map->end(); ++it) {

                Symbol name = it->first;
                Label *fn = it->second.value;

                auto func = label_to_function(fn, true, name);
                LLVMSetLinkage(func, LLVMExternalLinkage);

            }
            t = t->parent;
        }

        teardown_generate();
        return module;
    }

    std::pair<LLVMModuleRef, LLVMValueRef> generate(Label *entry) {
        assert(all_parameters_lowered(entry));
        assert(!entry->is_basic_block_like());

        {
            std::unordered_set<Label *> visited;
            std::vector<Label *> labels;
            entry->build_reachable(visited, &labels);
            for (auto it = labels.begin(); it != labels.end(); ++it) {
                (*it)->insert_into_usermap(user_map);
            }
        }

        const char *name = entry->name.name()->data;
        setup_generate(name);

        auto func = label_to_function(entry, true);
        LLVMSetLinkage(func, LLVMExternalLinkage);

        teardown_generate(entry);

        return std::pair<LLVMModuleRef, LLVMValueRef>(module, func);
    }

};

std::unordered_map<const Type *, LLVMTypeRef> LLVMIRGenerator::type_cache;
std::vector<const Type *> LLVMIRGenerator::type_todo;
LLVMTypeRef LLVMIRGenerator::voidT = nullptr;
LLVMTypeRef LLVMIRGenerator::i1T = nullptr;
LLVMTypeRef LLVMIRGenerator::i8T = nullptr;
LLVMTypeRef LLVMIRGenerator::i16T = nullptr;
LLVMTypeRef LLVMIRGenerator::i32T = nullptr;
LLVMTypeRef LLVMIRGenerator::i64T = nullptr;
LLVMTypeRef LLVMIRGenerator::f32T = nullptr;
LLVMTypeRef LLVMIRGenerator::f32x2T = nullptr;
LLVMTypeRef LLVMIRGenerator::f64T = nullptr;
LLVMTypeRef LLVMIRGenerator::rawstringT = nullptr;
LLVMTypeRef LLVMIRGenerator::noneT = nullptr;
LLVMValueRef LLVMIRGenerator::noneV = nullptr;
LLVMAttributeRef LLVMIRGenerator::attr_byval = nullptr;
LLVMAttributeRef LLVMIRGenerator::attr_sret = nullptr;
LLVMAttributeRef LLVMIRGenerator::attr_nonnull = nullptr;

//------------------------------------------------------------------------------
// IL COMPILER
//------------------------------------------------------------------------------

static void pprint(int pos, unsigned char *buf, int len, const char *disasm) {
  int i;
  printf("%04x:  ", pos);
  for (i = 0; i < 8; i++) {
    if (i < len) {
      printf("%02x ", buf[i]);
    } else {
      printf("   ");
    }
  }

  printf("   %s\n", disasm);
}

static void do_disassemble(LLVMTargetMachineRef tm, void *fptr, int siz) {

    unsigned char *buf = (unsigned char *)fptr;

  LLVMDisasmContextRef D = LLVMCreateDisasmCPUFeatures(
    LLVMGetTargetMachineTriple(tm),
    LLVMGetTargetMachineCPU(tm),
    LLVMGetTargetMachineFeatureString(tm),
    NULL, 0, NULL, NULL);
    LLVMSetDisasmOptions(D,
        LLVMDisassembler_Option_PrintImmHex);
  char outline[1024];
  int pos;

  if (!D) {
    printf("ERROR: Couldn't create disassembler\n");
    return;
  }

  pos = 0;
  while (pos < siz) {
    size_t l = LLVMDisasmInstruction(D, buf + pos, siz - pos, 0, outline,
                                     sizeof(outline));
    if (!l) {
      pprint(pos, buf + pos, 1, "\t???");
      pos++;
        break;
    } else {
      pprint(pos, buf + pos, l, outline);
      pos += l;
    }
  }

  LLVMDisasmDispose(D);
}

class DisassemblyListener : public llvm::JITEventListener {
public:
    llvm::ExecutionEngine *ee;
    DisassemblyListener(llvm::ExecutionEngine *_ee) : ee(_ee) {}

    std::unordered_map<void *, size_t> sizes;

    void InitializeDebugData(
        llvm::StringRef name,
        llvm::object::SymbolRef::Type type, uint64_t sz) {
        if(type == llvm::object::SymbolRef::ST_Function) {
            #if !defined(__arm__) && !defined(__linux__)
            name = name.substr(1);
            #endif
            void * addr = (void*)ee->getFunctionAddress(name);
            if(addr) {
                assert(addr);
                sizes[addr] = sz;
            }
        }
    }

    virtual void NotifyObjectEmitted(
        const llvm::object::ObjectFile &Obj,
        const llvm::RuntimeDyld::LoadedObjectInfo &L) {
        auto size_map = llvm::object::computeSymbolSizes(Obj);
        for(auto & S : size_map) {
            llvm::object::SymbolRef sym = S.first;
            auto name = sym.getName();
            auto type = sym.getType();
            if(name && type)
                InitializeDebugData(name.get(),type.get(),S.second);
        }
    }
};

enum {
    CF_DumpDisassembly  = (1 << 0),
    CF_DumpModule       = (1 << 1),
    CF_DumpFunction     = (1 << 2),
    CF_DumpTime         = (1 << 3),
    CF_NoDebugInfo      = (1 << 4),
    CF_O1               = (1 << 5),
    CF_O2               = (1 << 6),
    CF_O3               = CF_O1 | CF_O2,
};

static void compile_object(const String *path, Scope *scope, uint64_t flags) {
    Timer sum_compile_time(TIMER_Compile);
#if SCOPES_COMPILE_WITH_DEBUG_INFO
#else
    flags |= CF_NoDebugInfo;
#endif
#if SCOPES_OPTIMIZE_ASSEMBLY
    flags |= CF_O3;
#endif

    LLVMIRGenerator ctx;
    ctx.inline_pointers = false;
    if (flags & CF_NoDebugInfo) {
        ctx.use_debug_info = false;
    }

    LLVMModuleRef module;
    {
        Timer generate_timer(TIMER_Generate);
        module = ctx.generate(path, scope);
    }

    if (flags & CF_O3) {
        Timer optimize_timer(TIMER_Optimize);
        int level = 0;
        if ((flags & CF_O3) == CF_O1)
            level = 1;
        else if ((flags & CF_O3) == CF_O2)
            level = 2;
        else if ((flags & CF_O3) == CF_O3)
            level = 3;
        build_and_run_opt_passes(module, level);
    }
    if (flags & CF_DumpModule) {
        LLVMDumpModule(module);
    }

    assert(ee);
    auto tm = LLVMGetExecutionEngineTargetMachine(ee);

    char *errormsg = nullptr;
    char *path_cstr = strdup(path->data);
    if (LLVMTargetMachineEmitToFile(tm, module, path_cstr,
        LLVMObjectFile, &errormsg)) {
        location_error(String::from_cstr(errormsg));
    }
    free(path_cstr);
}

static DisassemblyListener *disassembly_listener = nullptr;
static Any compile(Label *fn, uint64_t flags) {
    Timer sum_compile_time(TIMER_Compile);
#if SCOPES_COMPILE_WITH_DEBUG_INFO
#else
    flags |= CF_NoDebugInfo;
#endif
#if SCOPES_OPTIMIZE_ASSEMBLY
    flags |= CF_O3;
#endif

    fn->verify_compilable();
    const Type *functype = Pointer(
        fn->get_function_type(), PTF_NonWritable, SYM_Unnamed);

    LLVMIRGenerator ctx;
    if (flags & CF_NoDebugInfo) {
        ctx.use_debug_info = false;
    }

    std::pair<LLVMModuleRef, LLVMValueRef> result;
    {
        /*
        A note on debugging "LLVM ERROR:" messages that seem to give no plausible
        point of origin: you can either set a breakpoint at llvm::report_fatal_error
        or at exit if the llvm symbols are missing, and then look at the stack trace.
        */
        Timer generate_timer(TIMER_Generate);
        result = ctx.generate(fn);
    }

    auto module = result.first;
    auto func = result.second;
    assert(func);

    if (!ee) {
        char *errormsg = nullptr;

        LLVMMCJITCompilerOptions opts;
        LLVMInitializeMCJITCompilerOptions(&opts, sizeof(opts));
        opts.OptLevel = 0;
        opts.NoFramePointerElim = true;

        if (LLVMCreateMCJITCompilerForModule(&ee, module, &opts,
            sizeof(opts), &errormsg)) {
            location_error(String::from_cstr(errormsg));
        }
    } else {
        LLVMAddModule(ee, module);
    }

    if (!disassembly_listener && (flags & CF_DumpDisassembly)) {
        llvm::ExecutionEngine *pEE = reinterpret_cast<llvm::ExecutionEngine*>(ee);
        disassembly_listener = new DisassemblyListener(pEE);
        pEE->RegisterJITEventListener(disassembly_listener);
    }

    if (flags & CF_O3) {
        Timer optimize_timer(TIMER_Optimize);
        int level = 0;
        if ((flags & CF_O3) == CF_O1)
            level = 1;
        else if ((flags & CF_O3) == CF_O2)
            level = 2;
        else if ((flags & CF_O3) == CF_O3)
            level = 3;
        build_and_run_opt_passes(module, level);
    }
    if (flags & CF_DumpModule) {
        LLVMDumpModule(module);
    } else if (flags & CF_DumpFunction) {
        LLVMDumpValue(func);
    }

    void *pfunc;
    {
        Timer mcjit_timer(TIMER_MCJIT);
        pfunc = LLVMGetPointerToGlobal(ee, func);
    }
    if (flags & CF_DumpDisassembly) {
        assert(disassembly_listener);
        //auto td = LLVMGetExecutionEngineTargetData(ee);
        auto tm = LLVMGetExecutionEngineTargetMachine(ee);
        auto it = disassembly_listener->sizes.find(pfunc);
        if (it != disassembly_listener->sizes.end()) {
            std::cout << "disassembly:\n";
            do_disassemble(tm, pfunc, it->second);
        } else {
            std::cout << "no disassembly available\n";
        }
    }

    return Any::from_pointer(functype, pfunc);
}

static void optimize_spirv(std::vector<unsigned int> &result, int opt_level) {
    spvtools::Optimizer optimizer(SPV_ENV_UNIVERSAL_1_2);
    /*
    optimizer.SetMessageConsumer([](spv_message_level_t level, const char* source,
        const spv_position_t& position,
        const char* message) {
    std::cerr << StringifyMessage(level, source, position, message)
    << std::endl;
    });*/
    StyledStream ss(std::cerr);
    optimizer.SetMessageConsumer([&ss](spv_message_level_t level, const char*,
        const spv_position_t& position,
        const char* message) {
        switch (level) {
        case SPV_MSG_FATAL:
        case SPV_MSG_INTERNAL_ERROR:
        case SPV_MSG_ERROR:
            ss << Style_Error << "error: " << Style_None
                << position.index << ": " << message << std::endl;
            break;
        case SPV_MSG_WARNING:
            ss << Style_Warning << "warning: " << Style_None
                << position.index << ": " << message << std::endl;
            break;
        case SPV_MSG_INFO:
            ss << Style_Comment << "info: " << Style_None
                << position.index << ": " << message << std::endl;
            break;
        default:
            break;
        }
    });

    if (opt_level == 3) {
        optimizer.RegisterPass(spvtools::CreateStripDebugInfoPass());
        optimizer.RegisterPass(spvtools::CreateFreezeSpecConstantValuePass());
    }

    if (opt_level == 3) {
        optimizer.RegisterPass(spvtools::CreateInlineExhaustivePass());
    }
    optimizer.RegisterPass(spvtools::CreateLocalAccessChainConvertPass());
    optimizer.RegisterPass(spvtools::CreateInsertExtractElimPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateLocalSingleStoreElimPass());
    optimizer.RegisterPass(spvtools::CreateBlockMergePass());
    optimizer.RegisterPass(spvtools::CreateEliminateDeadConstantPass());
    optimizer.RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass());
    optimizer.RegisterPass(spvtools::CreateUnifyConstantPass());

    optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
    optimizer.RegisterPass(spvtools::CreateLocalMultiStoreElimPass());
    if (opt_level == 3) {
        optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    }
    optimizer.RegisterPass(spvtools::CreateCommonUniformElimPass());

    optimizer.RegisterPass(spvtools::CreateFlattenDecorationPass());
    //optimizer.RegisterPass(spvtools::CreateCompactIdsPass());

    std::vector<unsigned int> oldresult = result;
    result.clear();
    if (!optimizer.Run(oldresult.data(), oldresult.size(), &result)) {
        location_error(String::from(
            "IL->SPIR: error while running optimization passes"));
    }

    verify_spirv(result);
}

static const String *compile_spirv(Symbol target, Label *fn, uint64_t flags) {
    Timer sum_compile_time(TIMER_CompileSPIRV);

    fn->verify_compilable();

    SPIRVGenerator ctx;
    if (flags & CF_NoDebugInfo) {
        ctx.use_debug_info = false;
    }

    std::vector<unsigned int> result;
    {
        Timer generate_timer(TIMER_GenerateSPIRV);
        ctx.generate(result, target, fn);
    }

    if (flags & CF_O3) {
        int level = 0;
        if ((flags & CF_O3) == CF_O1)
            level = 1;
        else if ((flags & CF_O3) == CF_O2)
            level = 2;
        else if ((flags & CF_O3) == CF_O3)
            level = 3;
        optimize_spirv(result, level);
    }

    if (flags & CF_DumpModule) {
    } else if (flags & CF_DumpFunction) {
    }
    if (flags & CF_DumpDisassembly) {
        disassemble_spirv(result);
    }

    size_t bytesize = sizeof(unsigned int) * result.size();

    return String::from((char *)&result[0], bytesize);
}

static const String *compile_glsl(Symbol target, Label *fn, uint64_t flags) {
    Timer sum_compile_time(TIMER_CompileSPIRV);

    fn->verify_compilable();

    SPIRVGenerator ctx;
    if (flags & CF_NoDebugInfo) {
        ctx.use_debug_info = false;
    }

    std::vector<unsigned int> result;
    {
        Timer generate_timer(TIMER_GenerateSPIRV);
        ctx.generate(result, target, fn);
    }

    if (flags & CF_O3) {
        int level = 0;
        if ((flags & CF_O3) == CF_O1)
            level = 1;
        else if ((flags & CF_O3) == CF_O2)
            level = 2;
        else if ((flags & CF_O3) == CF_O3)
            level = 3;
        optimize_spirv(result, level);
    }

    if (flags & CF_DumpDisassembly) {
        disassemble_spirv(result);
    }

	spirv_cross::CompilerGLSL glsl(std::move(result));

    /*
    // The SPIR-V is now parsed, and we can perform reflection on it.
    spirv_cross::ShaderResources resources = glsl.get_shader_resources();
    // Get all sampled images in the shader.
    for (auto &resource : resources.sampled_images)
    {
        unsigned set = glsl.get_decoration(resource.id, spv::DecorationDescriptorSet);
        unsigned binding = glsl.get_decoration(resource.id, spv::DecorationBinding);
        printf("Image %s at set = %u, binding = %u\n", resource.name.c_str(), set, binding);

        // Modify the decoration to prepare it for GLSL.
        glsl.unset_decoration(resource.id, spv::DecorationDescriptorSet);

        // Some arbitrary remapping if we want.
        glsl.set_decoration(resource.id, spv::DecorationBinding, set * 16 + binding);
    }
    */

    // Set some options.
    /*
    spirv_cross::CompilerGLSL::Options options;
    options.version = 450;
    glsl.set_options(options);*/

    // Compile to GLSL, ready to give to GL driver.
    std::string source = glsl.compile();

    if (flags & (CF_DumpModule|CF_DumpFunction)) {
        std::cout << source << std::endl;
    }

    return String::from_stdstring(source);
}

//------------------------------------------------------------------------------
// COMMON ERRORS
//------------------------------------------------------------------------------

void invalid_op2_types_error(const Type *A, const Type *B) {
    StyledString ss;
    ss.out << "invalid operand types " << A << " and " << B;
    location_error(ss.str());
}

//------------------------------------------------------------------------------
// OPERATOR TEMPLATES
//------------------------------------------------------------------------------

#define OP1_TEMPLATE(NAME, RTYPE, OP) \
    template<typename T> struct op_ ## NAME { \
        typedef RTYPE rtype; \
        static bool reductive() { return false; } \
        void operator()(void **srcptrs, void *destptr, size_t count) { \
            for (size_t i = 0; i < count; ++i) { \
                ((rtype *)destptr)[i] = op(((T *)(srcptrs[0]))[i]); \
            } \
        } \
        rtype op(T x) { \
            return OP; \
        } \
    };

#define OP2_TEMPLATE(NAME, RTYPE, OP) \
    template<typename T> struct op_ ## NAME { \
        typedef RTYPE rtype; \
        static bool reductive() { return false; } \
        void operator()(void **srcptrs, void *destptr, size_t count) { \
            for (size_t i = 0; i < count; ++i) { \
                ((rtype *)destptr)[i] = op(((T *)(srcptrs[0]))[i], ((T *)(srcptrs[1]))[i]); \
            } \
        } \
        rtype op(T a, T b) { \
            return OP; \
        } \
    };

template<typename T>
inline bool isnan(T f) {
    return f != f;
}

#define BOOL_IFXOP_TEMPLATE(NAME, OP) OP2_TEMPLATE(NAME, bool, a OP b)
#define BOOL_OF_TEMPLATE(NAME) OP2_TEMPLATE(NAME, bool, !isnan(a) && !isnan(b))
#define BOOL_UF_TEMPLATE(NAME) OP2_TEMPLATE(NAME, bool, isnan(a) || isnan(b))
#define BOOL_OF_IFXOP_TEMPLATE(NAME, OP) OP2_TEMPLATE(NAME, bool, !isnan(a) && !isnan(b) && (a OP b))
#define BOOL_UF_IFXOP_TEMPLATE(NAME, OP) OP2_TEMPLATE(NAME, bool, isnan(a) || isnan(b) || (a OP b))
#define IFXOP_TEMPLATE(NAME, OP) OP2_TEMPLATE(NAME, T, a OP b)
#define PFXOP_TEMPLATE(NAME, OP) OP2_TEMPLATE(NAME, T, OP(a, b))
#define PUNOP_TEMPLATE(NAME, OP) OP1_TEMPLATE(NAME, T, OP(x))

template<typename RType>
struct select_op_return_type {
    const Type *operator ()(const Type *T) { return T; }
};

static const Type *bool_op_return_type(const Type *T) {
    T = storage_type(T);
    if (T->kind() == TK_Vector) {
        auto vi = cast<VectorType>(T);
        return Vector(TYPE_Bool, vi->count);
    } else {
        return TYPE_Bool;
    }
}

template<>
struct select_op_return_type<bool> {
    const Type *operator ()(const Type *T) {
        return bool_op_return_type(T);
    }
};

BOOL_IFXOP_TEMPLATE(Equal, ==)
BOOL_IFXOP_TEMPLATE(NotEqual, !=)
BOOL_IFXOP_TEMPLATE(Greater, >)
BOOL_IFXOP_TEMPLATE(GreaterEqual, >=)
BOOL_IFXOP_TEMPLATE(Less, <)
BOOL_IFXOP_TEMPLATE(LessEqual, <=)

BOOL_OF_IFXOP_TEMPLATE(OEqual, ==)
BOOL_OF_IFXOP_TEMPLATE(ONotEqual, !=)
BOOL_OF_IFXOP_TEMPLATE(OGreater, >)
BOOL_OF_IFXOP_TEMPLATE(OGreaterEqual, >=)
BOOL_OF_IFXOP_TEMPLATE(OLess, <)
BOOL_OF_IFXOP_TEMPLATE(OLessEqual, <=)
BOOL_OF_TEMPLATE(Ordered)

BOOL_UF_IFXOP_TEMPLATE(UEqual, ==)
BOOL_UF_IFXOP_TEMPLATE(UNotEqual, !=)
BOOL_UF_IFXOP_TEMPLATE(UGreater, >)
BOOL_UF_IFXOP_TEMPLATE(UGreaterEqual, >=)
BOOL_UF_IFXOP_TEMPLATE(ULess, <)
BOOL_UF_IFXOP_TEMPLATE(ULessEqual, <=)
BOOL_UF_TEMPLATE(Unordered)

IFXOP_TEMPLATE(Add, +)
IFXOP_TEMPLATE(Sub, -)
IFXOP_TEMPLATE(Mul, *)

IFXOP_TEMPLATE(SDiv, /)
IFXOP_TEMPLATE(UDiv, /)
IFXOP_TEMPLATE(SRem, %)
IFXOP_TEMPLATE(URem, %)

IFXOP_TEMPLATE(BAnd, &)
IFXOP_TEMPLATE(BOr, |)
IFXOP_TEMPLATE(BXor, ^)

IFXOP_TEMPLATE(Shl, <<)
IFXOP_TEMPLATE(LShr, >>)
IFXOP_TEMPLATE(AShr, >>)

IFXOP_TEMPLATE(FAdd, +)
IFXOP_TEMPLATE(FSub, -)
IFXOP_TEMPLATE(FMul, *)
IFXOP_TEMPLATE(FDiv, /)
PFXOP_TEMPLATE(FRem, std::fmod)

} namespace std {

static bool abs(bool x) {
    return x;
}

} namespace scopes {

PUNOP_TEMPLATE(FAbs, std::abs)

template <typename T>
static T sgn(T val) {
    return T((T(0) < val) - (val < T(0)));
}

template<typename T>
static T radians(T x) {
    return x * T(M_PI / 180.0);
}

template<typename T>
static T degrees(T x) {
    return x * T(180.0 / M_PI);
}

template<typename T>
static T inversesqrt(T x) {
    return T(1.0) / std::sqrt(x);
}

template<typename T>
static T step(T edge, T x) {
    return T(x >= edge);
}

PUNOP_TEMPLATE(FSign, sgn)
PUNOP_TEMPLATE(SSign, sgn)
PUNOP_TEMPLATE(Radians, radians)
PUNOP_TEMPLATE(Degrees, degrees)
PUNOP_TEMPLATE(Sin, std::sin)
PUNOP_TEMPLATE(Cos, std::cos)
PUNOP_TEMPLATE(Tan, std::tan)
PUNOP_TEMPLATE(Asin, std::asin)
PUNOP_TEMPLATE(Acos, std::acos)
PUNOP_TEMPLATE(Atan, std::atan)
PFXOP_TEMPLATE(Atan2, std::atan2)
PUNOP_TEMPLATE(Exp, std::exp)
PUNOP_TEMPLATE(Log, std::log)
PUNOP_TEMPLATE(Exp2, std::exp2)
PUNOP_TEMPLATE(Log2, std::log2)
PUNOP_TEMPLATE(Sqrt, std::sqrt)
PUNOP_TEMPLATE(Trunc, std::trunc)
PUNOP_TEMPLATE(Floor, std::floor)
PUNOP_TEMPLATE(InverseSqrt, inversesqrt)
PFXOP_TEMPLATE(Pow, std::pow)
PFXOP_TEMPLATE(Step, step)

template<typename T> struct op_Cross {
    typedef T rtype;
    static bool reductive() { return false; }
    void operator()(void **srcptrs, void *destptr, size_t count) {
        assert(count == 3);
        auto x = (T *)(srcptrs[0]);
        auto y = (T *)(srcptrs[1]);
        auto ret = (rtype *)destptr;
        ret[0] = x[1] * y[2] - y[1] * x[2];
        ret[1] = x[2] * y[0] - y[2] * x[0];
        ret[2] = x[0] * y[1] - y[0] * x[1];
    }
};

template<typename T> struct op_Normalize {
    typedef T rtype;
    static bool reductive() { return false; }
    void operator()(void **srcptrs, void *destptr, size_t count) {
        auto x = (T *)(srcptrs[0]);
        T r = T(0);
        for (size_t i = 0; i < count; ++i) {
            r += x[i] * x[i];
        }
        r = (r == T(0))?T(1):(T(1)/std::sqrt(r));
        auto ret = (rtype *)destptr;
        for (size_t i = 0; i < count; ++i) {
            ret[i] = x[i] * r;
        }
    }
};

template<typename T> struct op_Length {
    typedef T rtype;
    static bool reductive() { return true; }
    void operator()(void **srcptrs, void *destptr, size_t count) {
        auto x = (T *)(srcptrs[0]);
        T r = T(0);
        for (size_t i = 0; i < count; ++i) {
            r += x[i] * x[i];
        }
        auto ret = (rtype *)destptr;
        ret[0] = std::sqrt(r);
    }
};

template<typename T> struct op_Distance {
    typedef T rtype;
    static bool reductive() { return true; }
    void operator()(void **srcptrs, void *destptr, size_t count) {
        auto x = (T *)(srcptrs[0]);
        auto y = (T *)(srcptrs[1]);
        T r = T(0);
        for (size_t i = 0; i < count; ++i) {
            T d = x[i] - y[i];
            r += d * d;
        }
        auto ret = (rtype *)destptr;
        ret[0] = std::sqrt(r);
    }
};

#undef BOOL_IFXOP_TEMPLATE
#undef BOOL_OF_TEMPLATE
#undef BOOL_UF_TEMPLATE
#undef BOOL_OF_IFXOP_TEMPLATE
#undef BOOL_UF_IFXOP_TEMPLATE
#undef IFXOP_TEMPLATE
#undef PFXOP_TEMPLATE

static void *aligned_alloc(size_t sz, size_t al) {
    assert(sz);
    assert(al);
    return reinterpret_cast<void *>(
        ::align(reinterpret_cast<uintptr_t>(tracked_malloc(sz + al - 1)), al));
}

static void *alloc_storage(const Type *T) {
    size_t sz = size_of(T);
    size_t al = align_of(T);
    return aligned_alloc(sz, al);
}

static void *copy_storage(const Type *T, void *ptr) {
    size_t sz = size_of(T);
    size_t al = align_of(T);
    void *destptr = aligned_alloc(sz, al);
    memcpy(destptr, ptr, sz);
    return destptr;
}

struct IntTypes_i {
    typedef bool i1;
    typedef int8_t i8;
    typedef int16_t i16;
    typedef int32_t i32;
    typedef int64_t i64;
};
struct IntTypes_u {
    typedef bool i1;
    typedef uint8_t i8;
    typedef uint16_t i16;
    typedef uint32_t i32;
    typedef uint64_t i64;
};

template<typename IT, template<typename T> class OpT>
struct DispatchInteger {
    typedef typename OpT<int8_t>::rtype rtype;
    static bool reductive() { return OpT<int8_t>::reductive(); }
    static const Type *return_type(Any *args, size_t numargs) {
        assert(numargs >= 1);
        return select_op_return_type<rtype>{}(args[0].type);
    }
    void operator ()(const Type *ET, void **srcptrs, void *destptr, size_t count,
                        Any *args, size_t numargs) {
        size_t width = cast<IntegerType>(ET)->width;
        switch(width) {
        case 1: OpT<typename IT::i1>{}(srcptrs, destptr, count); break;
        case 8: OpT<typename IT::i8>{}(srcptrs, destptr, count); break;
        case 16: OpT<typename IT::i16>{}(srcptrs, destptr, count); break;
        case 32: OpT<typename IT::i32>{}(srcptrs, destptr, count); break;
        case 64: OpT<typename IT::i64>{}(srcptrs, destptr, count); break;
        default:
            StyledString ss;
            ss.out << "unsupported bitwidth (" << width << ") for integer operation";
            location_error(ss.str());
            break;
        };
    }
};

template<template<typename T> class OpT>
struct DispatchReal {
    typedef typename OpT<float>::rtype rtype;
    static bool reductive() { return OpT<float>::reductive(); }
    static const Type *return_type(Any *args, size_t numargs) {
        assert(numargs >= 1);
        return select_op_return_type<rtype>{}(args[0].type);
    }
    void operator ()(const Type *ET, void **srcptrs, void *destptr, size_t count,
                        Any *args, size_t numargs) {
        size_t width = cast<RealType>(ET)->width;
        switch(width) {
        case 32: OpT<float>{}(srcptrs, destptr, count); break;
        case 64: OpT<double>{}(srcptrs, destptr, count); break;
        default:
            StyledString ss;
            ss.out << "unsupported bitwidth (" << width << ") for float operation";
            location_error(ss.str());
            break;
        };
    }
};

struct DispatchSelect {
    static bool reductive() { return false; }
    static const Type *return_type(Any *args, size_t numargs) {
        assert(numargs >= 1);
        return args[1].type;
    }
    void operator ()(const Type *ET, void **srcptrs, void *destptr, size_t count,
                        Any *args, size_t numargs) {
        assert(numargs == 3);
        bool *cond = (bool *)srcptrs[0];
        void *x = srcptrs[1];
        void *y = srcptrs[2];
        const Type *Tx = storage_type(args[1].type);
        if (Tx->kind() == TK_Vector) {
            auto VT = cast<VectorType>(Tx);
            auto stride = VT->stride;
            for (size_t i = 0; i < count; ++i) {
                memcpy(VT->getelementptr(destptr, i),
                    VT->getelementptr((cond[i] ? x : y), i),
                    stride);
            }
        } else {
            assert(count == 1);
            auto sz = size_of(Tx);
            memcpy(destptr, (cond[0] ? x : y), sz);
        }
    }
};

template<typename DispatchT>
static Any apply_op(Any *args, size_t numargs) {
    auto ST = storage_type(args[0].type);
    size_t count;
    void *srcptrs[numargs];
    void *destptr;
    Any result = none;
    auto RT = DispatchT::return_type(args, numargs);
    const Type *ET = nullptr;
    if (ST->kind() == TK_Vector) {
        auto vi = cast<VectorType>(ST);
        count = vi->count;
        for (size_t i = 0; i < numargs; ++i) {
            srcptrs[i] = args[i].pointer;
        }
        if (DispatchT::reductive()) {
            result.type = vi->element_type;
            destptr = get_pointer(result.type, result);
        } else {
            destptr = alloc_storage(RT);
            result = Any::from_pointer(RT, destptr);
        }
        ET = storage_type(vi->element_type);
    } else {
        count = 1;
        for (size_t i = 0; i < numargs; ++i) {
            srcptrs[i] = get_pointer(args[i].type, args[i]);
        }
        result.type = RT;
        destptr = get_pointer(result.type, result);
        ET = ST;
    }
    DispatchT{}(ET, srcptrs, destptr, count, args, numargs);
    return result;
}

template<typename IT, template<typename T> class OpT >
static Any apply_integer_op(Any x) {
    Any args[] = { x };
    return apply_op< DispatchInteger<IT, OpT> >(args, 1);
}

template<typename IT, template<typename T> class OpT >
static Any apply_integer_op(Any a, Any b) {
    Any args[] = { a, b };
    return apply_op< DispatchInteger<IT, OpT> >(args, 2);
}

template<template<typename T> class OpT>
static Any apply_real_op(Any x) {
    Any args[] = { x };
    return apply_op< DispatchReal<OpT> >(args, 1);
}

template<template<typename T> class OpT>
static Any apply_real_op(Any a, Any b) {
    Any args[] = { a, b };
    return apply_op< DispatchReal<OpT> >(args, 2);
}

//------------------------------------------------------------------------------
// NORMALIZE
//------------------------------------------------------------------------------

// assuming that value is an elementary type that can be put in a vector
static Any smear(Any value, size_t count) {
    size_t sz = size_of(value.type);
    void *psrc = get_pointer(value.type, value);
    auto VT = cast<VectorType>(Vector(value.type, count));
    void *pdest = alloc_storage(VT);
    for (size_t i = 0; i < count; ++i) {
        void *p = VT->getelementptr(pdest, i);
        memcpy(p, psrc, sz);
    }
    return Any::from_pointer(VT, pdest);
}

#define B_ARITH_OPS() \
        IARITH_NUW_NSW_OPS(Add) \
        IARITH_NUW_NSW_OPS(Sub) \
        IARITH_NUW_NSW_OPS(Mul) \
        \
        IARITH_OP(SDiv, i) \
        IARITH_OP(UDiv, u) \
        IARITH_OP(SRem, i) \
        IARITH_OP(URem, u) \
        \
        IARITH_OP(BAnd, u) \
        IARITH_OP(BOr, u) \
        IARITH_OP(BXor, u) \
        \
        IARITH_OP(Shl, u) \
        IARITH_OP(LShr, u) \
        IARITH_OP(AShr, i) \
        \
        FARITH_OP(FAdd) \
        FARITH_OP(FSub) \
        FARITH_OP(FMul) \
        FARITH_OP(FDiv) \
        FARITH_OP(FRem) \
        \
        FUN_OP(FAbs) \
        \
        IUN_OP(SSign, i) \
        FUN_OP(FSign) \
        \
        FUN_OP(Radians) FUN_OP(Degrees) \
        FUN_OP(Sin) FUN_OP(Cos) FUN_OP(Tan) \
        FUN_OP(Asin) FUN_OP(Acos) FUN_OP(Atan) FARITH_OP(Atan2) \
        FUN_OP(Exp) FUN_OP(Log) FUN_OP(Exp2) FUN_OP(Log2) \
        FUN_OP(Trunc) FUN_OP(Floor) FARITH_OP(Step) \
        FARITH_OP(Pow) FUN_OP(Sqrt) FUN_OP(InverseSqrt)

static Label *expand_module(Any expr, Scope *scope = nullptr);

struct Solver {
    StyledStream ss_cout;
    static std::vector<Label *> traceback;
    static int solve_refs;
    static bool enable_step_debugger;

    Solver()
        : ss_cout(std::cout)
    {}

    // inlining the continuation of a branch label without arguments
    void verify_branch_continuation(const Closure *closure) {
        if (!closure->label->is_basic_block_like()) {
            StyledString ss;
            ss.out << "branch destination must be label, not function" << std::endl;
            location_error(ss.str());
        }
    }

    Any fold_type_return(Any dest, const Args &values) {
        //ss_cout << "type_return: " << dest << std::endl;
#if SCOPES_TRUNCATE_FORWARDING_CONTINUATIONS
    repeat:
#endif
        if (dest.type == TYPE_Parameter) {
            Parameter *param = dest.parameter;
            if (param->is_none()) {
                location_error(String::from("attempting to call none continuation"));
            } else if (!param->is_typed()) {
                param->type = ReturnLabel(values);
                param->anchor = get_active_anchor();
            } else {
                const Type *T = ReturnLabel(values);
                if (T != param->type) {
                    {
                        StyledStream cerr(std::cerr);
                        cerr << param->anchor << " first typed here as " << param->type << std::endl;
                        param->anchor->stream_source_line(cerr);
                    }
                    {
                        StyledString ss;
                        ss.out << "return continuation retyped as " << T;
                        location_error(ss.str());
                    }
                }
            }
        } else if (dest.type == TYPE_Closure) {
            auto enter_frame = dest.closure->frame;
            auto enter_label = dest.closure->label;
            Label *newl = fold_typify_single(enter_frame, enter_label, values);
#if SCOPES_TRUNCATE_FORWARDING_CONTINUATIONS
#if 0
            bool cond1 = is_jumping(newl);
            bool cond2 = is_calling_continuation(newl);
            bool cond3 = matches_arg_count(enter_label, values.size());
            bool cond4 = is_calling_closure(newl);
            bool cond5 = forwards_all_args(enter_label);
            StyledStream ss;
            ss  << "is_jumping=" << cond1
                << " is_calling_continuation=" << cond2
                << " matches_arg_count=" << cond3
                << " is_calling_closure=" << cond4
                << " forwards_all_args=" << cond5
                << " values.size()=" << values.size()
                << std::endl;
            stream_label(ss, newl, StreamLabelFormat::single());
#endif
            if (is_jumping(newl)
                && (is_calling_continuation(newl) || is_calling_closure(newl))
                && matches_arg_count(newl, values.size())
                && forwards_all_args(newl)) {
                dest = newl->body.enter;
                goto repeat;
            } else
#endif
            {
                dest = newl;
            }
        } else if (dest.type == TYPE_Label) {
            auto TL = ReturnLabel(values);
            auto TR = dest.label->get_params_as_return_label_type();
            if (TL != TR) {
                {
                    StyledStream cerr(std::cerr);
                    cerr << dest.label->anchor << " typed as " << TR << std::endl;
                    dest.label->anchor->stream_source_line(cerr);
                }
                {
                    StyledString ss;
                    ss.out << "label retyped as " << TL;
                    location_error(ss.str());
                }
            }
        } else {
            apply_type_error(dest);
        }
        return dest;
    }

    static void verify_integer_ops(Any x) {
        verify_integer_vector(storage_type(x.indirect_type()));
    }

    static void verify_real_ops(Any x) {
        verify_real_vector(storage_type(x.indirect_type()));
    }

    static void verify_integer_ops(Any a, Any b) {
        verify_integer_vector(storage_type(a.indirect_type()));
        verify(a.indirect_type(), b.indirect_type());
    }

    static void verify_real_ops(Any a, Any b) {
        verify_real_vector(storage_type(a.indirect_type()));
        verify(a.indirect_type(), b.indirect_type());
    }

    static bool has_keyed_args(Label *l) {
        auto &&args = l->body.args;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i].key != SYM_Unnamed)
                return true;
        }
        return false;
    }

    static void verify_no_keyed_args(Label *l) {
        auto &&args = l->body.args;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i].key != SYM_Unnamed) {
                location_error(String::from("unexpected keyed argument"));
            }
        }

    }

    static bool is_jumping(Label *l) {
        auto &&args = l->body.args;
        assert(!args.empty());
        return args[0].value.type == TYPE_Nothing;
    }

    static bool is_continuing_to_label(Label *l) {
        auto &&args = l->body.args;
        assert(!args.empty());
        return args[0].value.type == TYPE_Label;
    }

    static bool is_continuing_to_parameter(Label *l) {
        auto &&args = l->body.args;
        assert(!args.empty());
        return args[0].value.type == TYPE_Parameter;
    }

    static bool is_continuing_to_closure(Label *l) {
        auto &&args = l->body.args;
        assert(!args.empty());
        return args[0].value.type == TYPE_Closure;
    }

    static bool is_calling_closure(Label *l) {
        auto &&enter = l->body.enter;
        return enter.type == TYPE_Closure;
    }

    static bool is_calling_label(Label *l) {
        auto &&enter = l->body.enter;
        return enter.type == TYPE_Label;
    }

    static bool is_return_parameter(Any val) {
        return (val.type == TYPE_Parameter) && (val.parameter->index == 0);
    }

    static bool is_calling_continuation(Label *l) {
        auto &&enter = l->body.enter;
        return (enter.type == TYPE_Parameter) && (enter.parameter->index == 0);
    }

    static bool is_calling_builtin(Label *l) {
        auto &&enter = l->body.enter;
        return enter.type == TYPE_Builtin;
    }

    static bool is_calling_callable(Label *l) {
        if (l->body.is_rawcall())
            return false;
        auto &&enter = l->body.enter;
        const Type *T = enter.indirect_type();
        Any value = none;
        return T->lookup_call_handler(value);
    }

    static bool is_calling_function(Label *l) {
        auto &&enter = l->body.enter;
        return is_function_pointer(enter.indirect_type());
    }

    static bool is_calling_pure_function(Label *l) {
        auto &&enter = l->body.enter;
        return is_pure_function_pointer(enter.type);
    }

    static bool all_params_typed(Label *l) {
        auto &&params = l->params;
        for (size_t i = 1; i < params.size(); ++i) {
            if (!params[i]->is_typed())
                return false;
        }
        return true;
    }

    static size_t find_untyped_arg(Label *l) {
        auto &&args = l->body.args;
        for (size_t i = 1; i < args.size(); ++i) {
            if ((args[i].value.type == TYPE_Parameter)
                && (args[i].value.parameter->index != 0)
                && (!args[i].value.parameter->is_typed()))
                return i;
        }
        return 0;
    }

    static bool all_args_typed(Label *l) {
        return !find_untyped_arg(l);
    }

    static bool all_args_constant(Label *l) {
        auto &&args = l->body.args;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i].value.is_const())
                return false;
        }
        return true;
    }

    static bool has_foldable_args(Label *l) {
        auto &&args = l->body.args;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i].value.is_const())
                return true;
            else if (is_return_parameter(args[i].value))
                return true;
        }
        return false;
    }

    static bool is_called_by(Label *callee, Label *caller) {
        auto &&enter = caller->body.enter;
        return (enter.type == TYPE_Label) && (enter.label == callee);
    }

    static bool is_called_by(Parameter *callee, Label *caller) {
        auto &&enter = caller->body.enter;
        return (enter.type == TYPE_Parameter) && (enter.parameter == callee);
    }

    static bool is_continuing_from(Label *callee, Label *caller) {
        auto &&args = caller->body.args;
        assert(!args.empty());
        return (args[0].value.type == TYPE_Label) && (args[0].value.label == callee);
    }

    static bool is_continuing_from(Parameter *callee, Label *caller) {
        auto &&args = caller->body.args;
        assert(!args.empty());
        return (args[0].value.type == TYPE_Parameter) && (args[0].value.parameter == callee);
    }

    void verify_function_argument_signature(const FunctionType *fi, Label *l) {
        auto &&args = l->body.args;
        verify_function_argument_count(fi, args.size() - 1);

        size_t fargcount = fi->argument_types.size();
        for (size_t i = 1; i < args.size(); ++i) {
            Argument &arg = args[i];
            size_t k = i - 1;
            const Type *argT = arg.value.indirect_type();
            if (k < fargcount) {
                const Type *ft = fi->argument_types[k];
                const Type *A = storage_type(argT);
                const Type *B = storage_type(ft);
                if (A == B)
                    continue;
                if ((A->kind() == TK_Pointer) && (B->kind() == TK_Pointer)) {
                    auto pa = cast<PointerType>(A);
                    auto pb = cast<PointerType>(B);
                    if ((pa->element_type == pb->element_type)
                        && (pa->flags == pb->flags)
                        && (pb->storage_class == SYM_Unnamed))
                        continue;
                }
                StyledString ss;
                ss.out << "argument of type " << ft << " expected, got " << argT;
                location_error(ss.str());
            }
        }
    }

    void fold_pure_function_call(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "folding pure function call in " << l << std::endl;
#endif

        auto &&enter = l->body.enter;
        auto &&args = l->body.args;

        auto pi = cast<PointerType>(enter.type);
        auto fi = cast<FunctionType>(pi->element_type);

        verify_function_argument_signature(fi, l);

        assert(!args.empty());
        Any result = none;

        if (fi->flags & FF_Variadic) {
            // convert C types
            size_t argcount = args.size() - 1;
            Args cargs;
            cargs.reserve(argcount);
            for (size_t i = 0; i < argcount; ++i) {
                Argument &srcarg = args[i + 1];
                if (i >= fi->argument_types.size()) {
                    if (srcarg.value.type == TYPE_F32) {
                        cargs.push_back(Argument(srcarg.key, (double)srcarg.value.f32));
                        continue;
                    }
                }
                cargs.push_back(srcarg);
            }
            result = run_ffi_function(enter, &cargs[0], cargs.size());
        } else {
            result = run_ffi_function(enter, &args[1], args.size() - 1);
        }

        enter = args[0].value;
        args = { Argument() };
        auto rlt = cast<ReturnLabelType>(fi->return_type);
        if (rlt->return_type != TYPE_Void) {
            if (isa<TupleType>(rlt->return_type)) {
                // unpack
                auto ti = cast<TupleType>(rlt->return_type);
                size_t count = ti->types.size();
                for (size_t i = 0; i < count; ++i) {
                    args.push_back(Argument(ti->unpack(result.pointer, i)));
                }
            } else {
                args.push_back(Argument(result));
            }
        }
    }

    void solve_keyed_args(Label *l) {
        Label *enter = l->get_closure_enter()->label;

        auto &&args = l->body.args;
        assert(!args.empty());
        Args newargs;
        newargs.reserve(args.size());
        newargs.push_back(args[0]);
        Parameter *vaparam = nullptr;
        if (!enter->params.empty() && enter->params.back()->is_vararg()) {
            vaparam = enter->params.back();
        }
        std::vector<bool> mapped;
        mapped.reserve(args.size());
        mapped.push_back(true);
        size_t next_index = 1;
        for (size_t i = 1; i < args.size(); ++i) {
            auto &&arg = args[i];
            if (arg.key == SYM_Unnamed) {
                while ((next_index < mapped.size()) && mapped[next_index])
                    next_index++;
                while (mapped.size() <= next_index) {
                    mapped.push_back(false);
                    newargs.push_back(none);
                }
                mapped[next_index] = true;
                newargs[next_index] = arg;
                next_index++;
            } else {
                auto param = enter->get_param_by_name(arg.key);
                size_t index = -1;
                if (param && (param != vaparam)) {
                    while (mapped.size() <= (size_t)param->index) {
                        mapped.push_back(false);
                        newargs.push_back(none);
                    }
                    if (mapped[param->index]) {
                        StyledString ss;
                        ss.out << "duplicate binding to parameter " << arg.key;
                        location_error(ss.str());
                    }
                    index = param->index;
                } else if (vaparam) {
                    while (mapped.size() < (size_t)vaparam->index) {
                        mapped.push_back(false);
                        newargs.push_back(none);
                    }
                    index = newargs.size();
                    mapped.push_back(false);
                    newargs.push_back(none);
                    newargs[index].key = arg.key;
                } else {
                    // no such parameter, map like regular parameter
                    while ((next_index < mapped.size()) && mapped[next_index])
                        next_index++;
                    while (mapped.size() <= next_index) {
                        mapped.push_back(false);
                        newargs.push_back(none);
                    }
                    index = next_index;
                    newargs[index].key = SYM_Unnamed;
                    next_index++;
                }
                mapped[index] = true;
                newargs[index].value = arg.value;
            }
        }
        args = newargs;
    }

    bool is_indirect_closure_type(const Type *T) {
        if (is_opaque(T)) return false;
        if (T == TYPE_Closure) return true;
        T = storage_type(T);
        const Type *ST = storage_type(TYPE_Closure);
        if (T == ST) return true;
        // TODO: detect closures in aggregate types
        return false;
    }

    bool label_returns_unreturnable_types(Label *l) {
        if (l->is_basic_block_like())
            return false;
        if (!l->is_return_param_typed())
            return false;
        const ReturnLabelType *rlt = cast<ReturnLabelType>(l->params[0]->type);
        for (size_t i = 0; i < rlt->values.size(); ++i) {
            auto &&val = rlt->values[i].value;
            if (is_unknown(val)) {
                auto T = val.typeref;
                if (!is_opaque(T)) {
                    T = storage_type(T);
                    if (T->kind() == TK_Pointer) {
                        auto pt = cast<PointerType>(T);
                        if (pt->storage_class != SYM_Unnamed) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    bool label_returns_closures(Label *l) {
        if (l->is_basic_block_like())
            return false;
        if (!l->is_return_param_typed())
            return false;
        const ReturnLabelType *rlt = cast<ReturnLabelType>(l->params[0]->type);
        for (size_t i = 0; i < rlt->values.size(); ++i) {
            auto &&val = rlt->values[i];
            if (is_indirect_closure_type(val.value.type))
                return true;
        }
        return false;
    }

    bool frame_args_match_keys(const Args &args, const Args &keys) const {
        if (args.size() != keys.size())
            return false;
        for (size_t i = 1; i < keys.size(); ++i) {
            auto &&arg = args[i].value;
            auto &&key = keys[i].value;
            if (is_unknown(key)
                && !arg.is_const()
                && (arg.parameter->type == key.typeref))
                continue;
            if (args[i].value != keys[i].value)
                return false;
        }
        return true;
    }

    enum FoldResult {
        FR_Pass,
        FR_Continue,
        FR_Break,
    };

    FoldResult fold_type_label_arguments(Label *&l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "folding & typing arguments in " << l << std::endl;
#endif

        auto &&enter = l->body.enter;
        assert(enter.type == TYPE_Closure);
        Frame *enter_frame = enter.closure->frame;
        Label *enter_label = enter.closure->label;

        // inline constant arguments
        Args callargs;
        Args keys;
        auto &&args = l->body.args;
        {
            callargs.push_back(args[0]);
            keys.push_back(Argument(untyped()));
            for (size_t i = 1; i < args.size(); ++i) {
                auto &&arg = args[i];
                if (arg.value.is_const()) {
                    keys.push_back(arg);
                } else if (is_return_parameter(arg.value)) {
                    keys.push_back(arg);
                } else {
                    keys.push_back(Argument(arg.key,
                        unknown_of(arg.value.indirect_type())));
                    callargs.push_back(arg);
                }
            }

            // we can't perform this optimization because it breaks memoization
            // of non-closure returning functions
            #if 0
            bool is_bb = enter_label->is_basic_block_like();
            if (!is_bb /* && !recursive */ && (callargs.size() == 1)) {
                // generated function will have only constant arguments, inline
                callargs[0] = none;
                keys[0] = args[0];
            }
            #endif
        }

        Label *newl = fold_type_label_single(
            enter_frame, enter_label, keys);
        bool reentrant = newl->is_reentrant();
        if (!reentrant) {
            // has this instance been used earlier in the stack?
            Frame *top_frame = enter_frame->find_frame(enter_label);
            reentrant = (top_frame && top_frame->instance == newl);
            // mark this function as reentrant for the solver further up in the stack
            if (reentrant) {
                newl->set_reentrant();
            }
        }

        if (newl == l) {
            location_error(String::from("label or function forms an infinite but empty loop"));
        }
        // we need to solve body, return type and reentrant flags for the
        // section that follows
        normalize_label(newl);
        if (newl->is_basic_block_like()) {
            enter = newl;
            args = callargs;
            clear_continuation_arg(l);
            fold_useless_labels(l);
            l->body.set_complete();
            return FR_Break;
        }

        assert(!newl->params.empty());
        bool has_return_type = newl->is_return_param_typed();
        bool returns_closures = label_returns_closures(newl);
        bool returns_unreturnable_types = label_returns_unreturnable_types(newl);

        if (has_return_type
            && (returns_closures
                || returns_unreturnable_types
                || (!reentrant
                    && is_trivial_function(newl)))) {
            // need to inline the function
            if (SCOPES_INLINE_FUNCTION_FROM_TEMPLATE
                || returns_closures /* && !recursive */) {
                callargs.clear();
                keys.clear();
                callargs.push_back(none);
                keys.push_back(args[0]);
                for (size_t i = 1; i < args.size(); ++i) {
                    keys.push_back(args[i]);
                }
                newl = fold_type_label_single(
                    enter_frame, enter_label, keys);
                enter = newl;
                args = callargs;
                return FR_Pass;
            } else {
                /*
                mangle solved function to include explicit return continuation.

                problem with this method:
                if closures escape the function, the closure's frames
                still map template parameters to labels used before the mangling.

                so for those cases, we fold the function again (see branch above)
                */
                Parameter *cont_param = newl->params[0];
                const Type *cont_type = cont_param->type;
                assert(isa<ReturnLabelType>(cont_type));
                auto tli = cast<ReturnLabelType>(cont_type);
                Any cont = fold_type_return(args[0].value, tli->values);
                assert(cont.type != TYPE_Closure);
                keys.clear();
                keys.push_back(cont);
                for (size_t i = 1; i < callargs.size(); ++i) {
                    keys.push_back(callargs[i]);
                }
                assert(!callargs.empty());
                callargs[0] = { none };
                std::unordered_set<Label *> visited;
                std::vector<Label *> labels;
                newl->build_reachable(visited, &labels);
                Label::UserMap um;
                for (auto it = labels.begin(); it != labels.end(); ++it) {
                    (*it)->insert_into_usermap(um);
                }
                Label *newll = fold_type_label(um, newl, keys);
                enter = newll;
                args = callargs;
                fold_useless_labels(l);
                l->body.set_complete();
                if (cont.type == TYPE_Label
                    /*&& !cont.label->body.is_complete()*/) {
                    l = cont.label;
                    assert(all_params_typed(l));
                    return FR_Continue;
                }
                return FR_Break;
            }
        } else {
            enter = newl;
            args = callargs;

            assert(newl->body.is_complete());
            if (has_return_type) {
                // function is now typed
                type_continuation_from_label_return_type(l);
            } else {
                if (reentrant) {
                    // possible recursion - entry label has already been
                    // processed, but not exited yet, so we don't have the
                    // continuation type yet.

                    // as long as we don't have to pass on the result,
                    // that's not a problem though
                    if (is_continuing_to_label(l) || is_continuing_to_closure(l)) {
                        location_error(String::from(
                            "attempt to continue from call to recursive function"
                            " before it has been typed; place exit condition before recursive call"));
                    }
                } else {
                    // apparently we returned from this label, but
                    // its continuation has not been typed,
                    // which means that it's functioning more like
                    // a basic block
                    // cut it
                    delete_continuation(newl);
                    clear_continuation_arg(l);
                    fold_useless_labels(l);
                }
            }
            return FR_Pass;
        }
    }

    // returns true if the builtin folds regardless of whether the arguments are
    // constant
    bool builtin_always_folds(Builtin builtin) {
        switch(builtin.value()) {
        case FN_TypeOf:
        case FN_NullOf:
        case FN_IsConstant:
        case FN_VaCountOf:
        case FN_VaKeys:
        case FN_VaKey:
        case FN_VaValues:
        case FN_VaAt:
        case FN_Location:
        case FN_Dump:
        case FN_ExternNew:
        case FN_ExternSymbol:
        case FN_ReturnLabelType:
        case FN_TupleType:
        case FN_UnionType:
        case FN_StaticAlloc:
            return true;
        default: return false;
        }
    }

    bool builtin_has_keyed_args(Builtin builtin) {
        switch(builtin.value()) {
        case FN_VaCountOf:
        case FN_VaKeys:
        case FN_VaValues:
        case FN_VaAt:
        case FN_Dump:
        case FN_ExternNew:
        case FN_ReturnLabelType:
        case FN_ScopeOf:
        case FN_TupleType:
        case FN_UnionType:
        case FN_Sample:
            return true;
        default: return false;
        }
    }

    bool builtin_never_folds(Builtin builtin) {
        switch(builtin.value()) {
        case FN_Bitcast:
        case FN_Unconst:
        case FN_Undef:
        case FN_Alloca:
        case FN_AllocaExceptionPad:
        case FN_AllocaArray:
        case FN_Malloc:
        case FN_MallocArray:
        case SFXFN_Unreachable:
        case SFXFN_Discard:
        case FN_VolatileStore:
        case FN_Store:
        case FN_VolatileLoad:
        case FN_Load:
        case FN_Sample:
        case FN_ImageRead:
        case FN_GetElementPtr:
        case SFXFN_ExecutionMode:
        case OP_Tertiary:
            return true;
        default: return false;
        }
    }

    void verify_readable(const Type *T) {
        auto pi = cast<PointerType>(T);
        if (!pi->is_readable()) {
            StyledString ss;
            ss.out << "can not load value from address of type " << T
                << " because the target is non-readable";
            location_error(ss.str());
        }
    }

    void verify_writable(const Type *T) {
        auto pi = cast<PointerType>(T);
        if (!pi->is_writable()) {
            StyledString ss;
            ss.out << "can not store value at address of type " << T
                << " because the target is non-writable";
            location_error(ss.str());
        }
    }

    // reduce typekind to compatible
    static TypeKind canonical_typekind(TypeKind k) {
        if (k == TK_Real)
            return TK_Integer;
        return k;
    }

#define CHECKARGS(MINARGS, MAXARGS) \
    checkargs<MINARGS, MAXARGS>(args.size())

#define RETARGTYPES(...) \
    { \
        const Type *retargtypes[] = { __VA_ARGS__ }; \
        size_t _count = (sizeof(retargtypes) / sizeof(const Type *)); \
        retvalues.reserve(_count); \
        for (size_t _i = 0; _i < _count; ++_i) { \
            retvalues.push_back(unknown_of(retargtypes[_i])); \
        } \
    }
#define RETARGS(...) \
    retvalues = { __VA_ARGS__ }; \
    return true;

    // returns true if the call can be eliminated
    bool values_from_builtin_call(Label *l, Args &retvalues) {
        auto &&enter = l->body.enter;
        auto &&args = l->body.args;
        assert(enter.type == TYPE_Builtin);
        switch(enter.builtin.value()) {
        case FN_Sample: {
            CHECKARGS(2, -1);
            auto ST = storage_type(args[1].value.indirect_type());
            if (ST->kind() == TK_SampledImage) {
                auto sit = cast<SampledImageType>(ST);
                ST = storage_type(sit->type);
            }
            verify_kind<TK_Image>(ST);
            auto it = cast<ImageType>(ST);
            RETARGTYPES(it->type);
        } break;
        case FN_ImageRead: {
            CHECKARGS(2, 2);
            auto ST = storage_type(args[1].value.indirect_type());
            verify_kind<TK_Image>(ST);
            auto it = cast<ImageType>(ST);
            RETARGTYPES(it->type);
        } break;
        case SFXFN_ExecutionMode: {
            CHECKARGS(1, 4);
            args[1].value.verify(TYPE_Symbol);
            SPIRVGenerator::execution_mode_from_symbol(args[1].value.symbol);
            for (size_t i = 2; i < args.size(); ++i) {
                cast_number<int>(args[i].value);
            }
            RETARGTYPES();
        } break;
        case OP_Tertiary: {
            CHECKARGS(3, 3);
            auto &&cond = args[1].value;
            if (cond.is_const() &&
                ((cond.type == TYPE_Bool)
                    || (args[2].value.is_const() && args[3].value.is_const()))) {
                if (cond.type == TYPE_Bool) {
                    if (cond.i1) {
                        RETARGS(args[2].value);
                    } else {
                        RETARGS(args[3].value);
                    }
                } else {
                    auto T1 = storage_type(cond.type);
                    auto T2 = storage_type(args[2].value.type);
                    verify_bool_vector(T1);
                    verify(args[2].value.type, args[3].value.type);
                    if (T1->kind() == TK_Vector) {
                        verify_vector_sizes(T1, T2);
                    } else if (T2->kind() == TK_Vector) {
                        cond = smear(cond, cast<VectorType>(T2)->count);
                    }
                    Any fargs[] = { cond, args[2].value, args[3].value };
                    RETARGS(apply_op< DispatchSelect >(fargs, 3));
                }
            } else {
                auto T1 = storage_type(args[1].value.indirect_type());
                auto T2 = storage_type(args[2].value.indirect_type());
                verify_bool_vector(T1);
                if (T1->kind() == TK_Vector) {
                    verify_vector_sizes(T1, T2);
                }
                verify(args[2].value.indirect_type(), args[3].value.indirect_type());
                RETARGTYPES(args[2].value.indirect_type());
            }
        } break;
        case FN_Unconst: {
            CHECKARGS(1, 1);
            if (!args[1].value.is_const()) {
                RETARGS(args[1]);
            } else {
                auto T = args[1].value.indirect_type();
                auto et = dyn_cast<ExternType>(T);
                if (et) {
                    RETARGTYPES(et->pointer_type);
                } else if (args[1].value.type == TYPE_Label) {
                    Label *fn = args[1].value;
                    fn->verify_compilable();
                    const Type *functype = Pointer(
                        fn->get_function_type(), PTF_NonWritable, SYM_Unnamed);
                    RETARGTYPES(functype);
                } else {
                    RETARGTYPES(T);
                }
            }
        } break;
        case FN_Bitcast: {
            CHECKARGS(2, 2);
            // todo: verify source and dest type are non-aggregate
            // also, both must be of same category
            args[2].value.verify(TYPE_Type);
            const Type *SrcT = args[1].value.indirect_type();
            const Type *DestT = args[2].value.typeref;
            if (SrcT == DestT) {
                RETARGS(args[1].value);
            } else {
                const Type *SSrcT = storage_type(SrcT);
                const Type *SDestT = storage_type(DestT);

                if (canonical_typekind(SSrcT->kind())
                        != canonical_typekind(SDestT->kind())) {
                    StyledString ss;
                    ss.out << "can not bitcast value of type " << SrcT
                        << " to type " << DestT
                        << " because storage types are not of compatible category";
                    location_error(ss.str());
                }
                if (SSrcT != SDestT) {
                    switch (SDestT->kind()) {
                    case TK_Array:
                    //case TK_Vector:
                    case TK_Tuple:
                    case TK_Union: {
                        StyledString ss;
                        ss.out << "can not bitcast value of type " << SrcT
                            << " to type " << DestT
                            << " with aggregate storage type " << SDestT;
                        location_error(ss.str());
                    } break;
                    default: break;
                    }
                }
                if (args[1].value.is_const()) {
                    Any result = args[1].value;
                    result.type = DestT;
                    RETARGS(result);
                } else {
                    RETARGTYPES(DestT);
                }
            }
        } break;
        case FN_IntToPtr: {
            CHECKARGS(2, 2);
            verify_integer(storage_type(args[1].value.indirect_type()));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_kind<TK_Pointer>(storage_type(DestT));
            RETARGTYPES(DestT);
        } break;
        case FN_PtrToInt: {
            CHECKARGS(2, 2);
            verify_kind<TK_Pointer>(
                storage_type(args[1].value.indirect_type()));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(storage_type(DestT));
            RETARGTYPES(DestT);
        } break;
        case FN_ITrunc: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_integer(storage_type(T));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(storage_type(DestT));
            RETARGTYPES(DestT);
        } break;
        case FN_FPTrunc: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            if (cast<RealType>(T)->width >= cast<RealType>(DestT)->width) {
            } else { invalid_op2_types_error(T, DestT); }
            RETARGTYPES(DestT);
        } break;
        case FN_FPExt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            if (cast<RealType>(T)->width <= cast<RealType>(DestT)->width) {
            } else { invalid_op2_types_error(T, DestT); }
            RETARGTYPES(DestT);
        } break;
        case FN_FPToUI: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(DestT);
            if ((T == TYPE_F32) || (T == TYPE_F64)) {
            } else {
                invalid_op2_types_error(T, DestT);
            }
            RETARGTYPES(DestT);
        } break;
        case FN_FPToSI: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(DestT);
            if ((T == TYPE_F32) || (T == TYPE_F64)) {
            } else {
                invalid_op2_types_error(T, DestT);
            }
            RETARGTYPES(DestT);
        } break;
        case FN_UIToFP: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_integer(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            if ((DestT == TYPE_F32) || (DestT == TYPE_F64)) {
            } else {
                invalid_op2_types_error(T, DestT);
            }
            RETARGTYPES(DestT);
        } break;
        case FN_SIToFP: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_integer(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            if ((DestT == TYPE_F32) || (DestT == TYPE_F64)) {
            } else {
                invalid_op2_types_error(T, DestT);
            }
            RETARGTYPES(DestT);
        } break;
        case FN_ZExt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_integer(storage_type(T));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(storage_type(DestT));
            RETARGTYPES(DestT);
        } break;
        case FN_SExt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.indirect_type();
            verify_integer(storage_type(T));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(storage_type(DestT));
            RETARGTYPES(DestT);
        } break;
        case FN_ExtractElement: {
            CHECKARGS(2, 2);
            const Type *T = storage_type(args[1].value.indirect_type());
            verify_kind<TK_Vector>(T);
            auto vi = cast<VectorType>(T);
            verify_integer(storage_type(args[2].value.indirect_type()));
            RETARGTYPES(vi->element_type);
        } break;
        case FN_InsertElement: {
            CHECKARGS(3, 3);
            const Type *T = storage_type(args[1].value.indirect_type());
            const Type *ET = storage_type(args[2].value.indirect_type());
            verify_integer(storage_type(args[3].value.indirect_type()));
            verify_kind<TK_Vector>(T);
            auto vi = cast<VectorType>(T);
            verify(storage_type(vi->element_type), ET);
            RETARGTYPES(args[1].value.indirect_type());
        } break;
        case FN_ShuffleVector: {
            CHECKARGS(3, 3);
            const Type *TV1 = storage_type(args[1].value.indirect_type());
            const Type *TV2 = storage_type(args[2].value.indirect_type());
            const Type *TMask = storage_type(args[3].value.type);
            verify_kind<TK_Vector>(TV1);
            verify_kind<TK_Vector>(TV2);
            verify_kind<TK_Vector>(TMask);
            verify(TV1, TV2);
            auto vi = cast<VectorType>(TV1);
            auto mask_vi = cast<VectorType>(TMask);
            verify(TYPE_I32, mask_vi->element_type);
            size_t incount = vi->count * 2;
            size_t outcount = mask_vi->count;
            for (size_t i = 0; i < outcount; ++i) {
                verify_range(
                    (size_t)mask_vi->unpack(args[3].value.pointer, i).i32,
                    incount);
            }
            RETARGTYPES(Vector(vi->element_type, outcount));
        } break;
        case FN_ExtractValue: {
            CHECKARGS(2, 2);
            size_t idx = cast_number<size_t>(args[2].value);
            const Type *T = storage_type(args[1].value.indirect_type());
            switch(T->kind()) {
            case TK_Array: {
                auto ai = cast<ArrayType>(T);
                RETARGTYPES(ai->type_at_index(idx));
            } break;
            case TK_Tuple: {
                auto ti = cast<TupleType>(T);
                RETARGTYPES(ti->type_at_index(idx));
            } break;
            case TK_Union: {
                auto ui = cast<UnionType>(T);
                RETARGTYPES(ui->type_at_index(idx));
            } break;
            default: {
                StyledString ss;
                ss.out << "can not extract value from type " << T;
                location_error(ss.str());
            } break;
            }
        } break;
        case FN_InsertValue: {
            CHECKARGS(3, 3);
            const Type *T = storage_type(args[1].value.indirect_type());
            const Type *ET = storage_type(args[2].value.indirect_type());
            size_t idx = cast_number<size_t>(args[3].value);
            switch(T->kind()) {
            case TK_Array: {
                auto ai = cast<ArrayType>(T);
                verify(storage_type(ai->type_at_index(idx)), ET);
            } break;
            case TK_Tuple: {
                auto ti = cast<TupleType>(T);
                verify(storage_type(ti->type_at_index(idx)), ET);
            } break;
            case TK_Union: {
                auto ui = cast<UnionType>(T);
                verify(storage_type(ui->type_at_index(idx)), ET);
            } break;
            default: {
                StyledString ss;
                ss.out << "can not insert value into type " << T;
                location_error(ss.str());
            } break;
            }
            RETARGTYPES(args[1].value.indirect_type());
        } break;
        case FN_Undef: {
            CHECKARGS(1, 1);
            args[1].value.verify(TYPE_Type);
            RETARGTYPES(args[1].value.typeref);
        } break;
        case FN_Malloc: {
            CHECKARGS(1, 1);
            args[1].value.verify(TYPE_Type);
            RETARGTYPES(NativePointer(args[1].value.typeref));
        } break;
        case FN_Alloca: {
            CHECKARGS(1, 1);
            args[1].value.verify(TYPE_Type);
            RETARGTYPES(LocalPointer(args[1].value.typeref));
        } break;
        case FN_AllocaExceptionPad: {
            CHECKARGS(0, 0);
            RETARGTYPES(LocalPointer(Array(TYPE_U8, sizeof(ExceptionPad))));
        } break;
        case FN_AllocaOf: {
            CHECKARGS(1, 1);
            RETARGTYPES(LocalROPointer(args[1].value.indirect_type()));
        } break;
        case FN_MallocArray: {
            CHECKARGS(2, 2);
            args[1].value.verify(TYPE_Type);
            verify_integer(storage_type(args[2].value.indirect_type()));
            RETARGTYPES(NativePointer(args[1].value.typeref));
        } break;
        case FN_AllocaArray: {
            CHECKARGS(2, 2);
            args[1].value.verify(TYPE_Type);
            verify_integer(storage_type(args[2].value.indirect_type()));
            RETARGTYPES(LocalPointer(args[1].value.typeref));
        } break;
        case FN_Free: {
            CHECKARGS(1, 1);
            const Type *T = storage_type(args[1].value.indirect_type());
            verify_kind<TK_Pointer>(T);
            verify_writable(T);
            if (cast<PointerType>(T)->storage_class != SYM_Unnamed) {
                location_error(String::from(
                    "pointer is not a heap pointer"));
            }
            RETARGTYPES();
        } break;
        case FN_GetElementPtr: {
            CHECKARGS(2, -1);
            const Type *T = storage_type(args[1].value.indirect_type());
            bool is_extern = (T->kind() == TK_Extern);
            if (is_extern) {
                T = cast<ExternType>(T)->pointer_type;
            }
            verify_kind<TK_Pointer>(T);
            auto pi = cast<PointerType>(T);
            T = pi->element_type;
            bool all_const = args[1].value.is_const();
            if (all_const) {
                for (size_t i = 2; i < args.size(); ++i) {
                    if (!args[i].value.is_const()) {
                        all_const = false;
                        break;
                    }
                }
            }
            if (!is_extern && all_const) {
                void *ptr = args[1].value.pointer;
                size_t idx = cast_number<size_t>(args[2].value);
                ptr = pi->getelementptr(ptr, idx);

                for (size_t i = 3; i < args.size(); ++i) {
                    const Type *ST = storage_type(T);
                    auto &&arg = args[i].value;
                    switch(ST->kind()) {
                    case TK_Array: {
                        auto ai = cast<ArrayType>(ST);
                        T = ai->element_type;
                        size_t idx = cast_number<size_t>(arg);
                        ptr = ai->getelementptr(ptr, idx);
                    } break;
                    case TK_Tuple: {
                        auto ti = cast<TupleType>(ST);
                        size_t idx = 0;
                        if (arg.type == TYPE_Symbol) {
                            idx = ti->field_index(arg.symbol);
                            if (idx == (size_t)-1) {
                                StyledString ss;
                                ss.out << "no such field " << arg.symbol << " in storage type " << ST;
                                location_error(ss.str());
                            }
                            // rewrite field
                            arg = (int)idx;
                        } else {
                            idx = cast_number<size_t>(arg);
                        }
                        T = ti->type_at_index(idx);
                        ptr = ti->getelementptr(ptr, idx);
                    } break;
                    default: {
                        StyledString ss;
                        ss.out << "can not get element pointer from type " << T;
                        location_error(ss.str());
                    } break;
                    }
                }
                T = Pointer(T, pi->flags, pi->storage_class);
                RETARGS(Any::from_pointer(T, ptr));
            } else {
                verify_integer(storage_type(args[2].value.indirect_type()));
                for (size_t i = 3; i < args.size(); ++i) {

                    const Type *ST = storage_type(T);
                    auto &&arg = args[i];
                    switch(ST->kind()) {
                    case TK_Array: {
                        auto ai = cast<ArrayType>(ST);
                        T = ai->element_type;
                        verify_integer(storage_type(arg.value.indirect_type()));
                    } break;
                    case TK_Tuple: {
                        auto ti = cast<TupleType>(ST);
                        size_t idx = 0;
                        if (arg.value.type == TYPE_Symbol) {
                            idx = ti->field_index(arg.value.symbol);
                            if (idx == (size_t)-1) {
                                StyledString ss;
                                ss.out << "no such field " << arg.value.symbol << " in storage type " << ST;
                                location_error(ss.str());
                            }
                            // rewrite field
                            arg = Argument(arg.key, Any((int)idx));
                        } else {
                            idx = cast_number<size_t>(arg.value);
                        }
                        T = ti->type_at_index(idx);
                    } break;
                    default: {
                        StyledString ss;
                        ss.out << "can not get element pointer from type " << T;
                        location_error(ss.str());
                    } break;
                    }
                }
                T = Pointer(T, pi->flags, pi->storage_class);
                RETARGTYPES(T);
            }
        } break;
        case FN_VolatileLoad:
        case FN_Load: {
            CHECKARGS(1, 1);
            const Type *T = storage_type(args[1].value.indirect_type());
            bool is_extern = (T->kind() == TK_Extern);
            if (is_extern) {
                T = cast<ExternType>(T)->pointer_type;
            }
            verify_kind<TK_Pointer>(T);
            verify_readable(T);
            auto pi = cast<PointerType>(T);
            if (!is_extern && args[1].value.is_const()
                && !pi->is_writable()) {
                RETARGS(pi->unpack(args[1].value.pointer));
            } else {
                RETARGTYPES(pi->element_type);
            }
        } break;
        case FN_VolatileStore:
        case FN_Store: {
            CHECKARGS(2, 2);
            const Type *T = storage_type(args[2].value.indirect_type());
            bool is_extern = (T->kind() == TK_Extern);
            if (is_extern) {
                T = cast<ExternType>(T)->pointer_type;
            }
            verify_kind<TK_Pointer>(T);
            verify_writable(T);
            auto pi = cast<PointerType>(T);
            verify(storage_type(pi->element_type),
                storage_type(args[1].value.indirect_type()));
            RETARGTYPES();
        } break;
        case FN_Cross: {
            CHECKARGS(2, 2);
            const Type *Ta = storage_type(args[1].value.indirect_type());
            const Type *Tb = storage_type(args[2].value.indirect_type());
            verify_real_vector(Ta, 3);
            verify(Ta, Tb);
            RETARGTYPES(args[1].value.indirect_type());
        } break;
        case FN_Normalize: {
            CHECKARGS(1, 1);
            verify_real_ops(args[1].value);
            RETARGTYPES(args[1].value.indirect_type());
        } break;
        case FN_Length: {
            CHECKARGS(1, 1);
            const Type *T = storage_type(args[1].value.indirect_type());
            verify_real_vector(T);
            if (T->kind() == TK_Vector) {
                RETARGTYPES(cast<VectorType>(T)->element_type);
            } else {
                RETARGTYPES(args[1].value.indirect_type());
            }
        } break;
        case FN_Distance: {
            CHECKARGS(2, 2);
            verify_real_ops(args[1].value, args[2].value);
            const Type *T = storage_type(args[1].value.indirect_type());
            if (T->kind() == TK_Vector) {
                RETARGTYPES(cast<VectorType>(T)->element_type);
            } else {
                RETARGTYPES(args[1].value.indirect_type());
            }
        } break;
        case OP_ICmpEQ:
        case OP_ICmpNE:
        case OP_ICmpUGT:
        case OP_ICmpUGE:
        case OP_ICmpULT:
        case OP_ICmpULE:
        case OP_ICmpSGT:
        case OP_ICmpSGE:
        case OP_ICmpSLT:
        case OP_ICmpSLE: {
            CHECKARGS(2, 2);
            verify_integer_ops(args[1].value, args[2].value);
            RETARGTYPES(
                bool_op_return_type(args[1].value.indirect_type()));
        } break;
        case OP_FCmpOEQ:
        case OP_FCmpONE:
        case OP_FCmpORD:
        case OP_FCmpOGT:
        case OP_FCmpOGE:
        case OP_FCmpOLT:
        case OP_FCmpOLE:
        case OP_FCmpUEQ:
        case OP_FCmpUNE:
        case OP_FCmpUNO:
        case OP_FCmpUGT:
        case OP_FCmpUGE:
        case OP_FCmpULT:
        case OP_FCmpULE: {
            CHECKARGS(2, 2);
            verify_real_ops(args[1].value, args[2].value);
            RETARGTYPES(
                bool_op_return_type(args[1].value.indirect_type()));
        } break;
#define IARITH_NUW_NSW_OPS(NAME) \
    case OP_ ## NAME: \
    case OP_ ## NAME ## NUW: \
    case OP_ ## NAME ## NSW: { \
        CHECKARGS(2, 2); \
        verify_integer_ops(args[1].value, args[2].value); \
        RETARGTYPES(args[1].value.indirect_type()); \
    } break;
#define IARITH_OP(NAME, PFX) \
    case OP_ ## NAME: { \
        CHECKARGS(2, 2); \
        verify_integer_ops(args[1].value, args[2].value); \
        RETARGTYPES(args[1].value.indirect_type()); \
    } break;
#define FARITH_OP(NAME) \
    case OP_ ## NAME: { \
        CHECKARGS(2, 2); \
        verify_real_ops(args[1].value, args[2].value); \
        RETARGTYPES(args[1].value.indirect_type()); \
    } break;
#define IUN_OP(NAME, PFX) \
    case OP_ ## NAME: { \
        CHECKARGS(1, 1); \
        verify_integer_ops(args[1].value); \
        RETARGTYPES(args[1].value.indirect_type()); \
    } break;
#define FUN_OP(NAME) \
    case OP_ ## NAME: { \
        CHECKARGS(1, 1); \
        verify_real_ops(args[1].value); \
        RETARGTYPES(args[1].value.indirect_type()); \
    } break;
            B_ARITH_OPS()

#undef IARITH_NUW_NSW_OPS
#undef IARITH_OP
#undef FARITH_OP
#undef IUN_OP
#undef FUN_OP
        default: {
            StyledString ss;
            ss.out << "can not type builtin " << enter.builtin;
            location_error(ss.str());
        } break;
        }

        return false;
    }
#undef RETARGS
#undef RETARGTYPES

#define RETARGS(...) \
    enter = args[0].value; \
    args = { none, __VA_ARGS__ };

    static void print_traceback_entry(StyledStream &ss, Label *last_head, Label *last_loc) {
        if (!last_head) return;
        assert(last_loc);
        ss << last_loc->body.anchor << " in ";
        if (last_head->name == SYM_Unnamed) {
            if (last_head->is_basic_block_like()) {
                ss << "unnamed label";
            } else {
                ss << "unnamed function";
            }
        } else {
            ss << Style_Function << last_head->name.name()->data << Style_None;
        }
        ss << std::endl;
        last_loc->body.anchor->stream_source_line(ss);
        //stream_label(ss, last_head->get_original(), StreamLabelFormat::debug_single());
        //stream_label(ss, last_loc->get_original(), StreamLabelFormat::debug_single());
    }

    static void print_traceback() {
        if (traceback.empty()) return;
        StyledStream ss(std::cerr);
        ss << "Traceback (most recent call last):" << std::endl;

        size_t sz = traceback.size();
        size_t lasti = sz - 1;
        size_t i = 0;
        Label *l = traceback[lasti - i];
        Label *last_head = l;
        Label *last_loc = l;
        i++;
        while (i < sz) {
            l = traceback[lasti - i++];
            Label *orig = l->get_original();
            if (!orig->is_basic_block_like()) {
                print_traceback_entry(ss, last_head, last_loc);
                last_head = l;
            }
            last_loc = l;
            if (is_calling_label(orig)
                && !orig->get_label_enter()->is_basic_block_like()) {
                if (!last_head)
                    last_head = last_loc;
                print_traceback_entry(ss, last_head, last_loc);
                last_head = nullptr;
            }
        }
        print_traceback_entry(ss, last_head, last_loc);
    }

    void fold_callable_call(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "folding callable call in " << l << std::endl;
#endif

        auto &&enter = l->body.enter;
        auto &&args = l->body.args;
        const Type *T = enter.indirect_type();

        Any value = none;
        bool result = T->lookup_call_handler(value);
        assert(result);
        args.insert(args.begin() + 1, Argument(enter));
        enter = value;
    }

    bool fold_builtin_call(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "folding builtin call in " << l << std::endl;
#endif

        auto &&enter = l->body.enter;
        auto &&args = l->body.args;
        assert(enter.type == TYPE_Builtin);
        switch(enter.builtin.value()) {
        case KW_SyntaxExtend: {
            CHECKARGS(3, 3);
            const Closure *cl = args[1].value;
            const Syntax *sx = args[2].value;
            Scope *env = args[3].value;
            Label *metafunc = fold_type_label_single(cl->frame, cl->label,
                { untyped(), env });
            Solver solver;
            solver.solve(metafunc);
            auto rlt = metafunc->verify_return_label();
            //const Type *functype = metafunc->get_function_type();
            if (rlt->values.size() != 1)
                goto failed;
            {
                Scope *scope = nullptr;
                Any compiled = compile(metafunc, 0);
                if (rlt->values[0].value.type == TYPE_Scope) {
                    // returns a constant scope
                    typedef void (*FuncType)();
                    FuncType fptr = (FuncType)compiled.pointer;
                    fptr();
                    scope = rlt->values[0].value.scope;
                } else if ((rlt->values[0].value.type == TYPE_Unknown)
                    && (rlt->values[0].value.typeref == TYPE_Scope)) {
                    // returns a variable scope
                    typedef Scope *(*FuncType)();
                    FuncType fptr = (FuncType)compiled.pointer;
                    scope = fptr();
                } else {
                    goto failed;
                }
                enter = fold_type_label_single(cl->frame,
                    expand_module(sx, scope), { args[0] });
                args = { none };
                return false;
            }
        failed:
            set_active_anchor(sx->anchor);
            StyledString ss;
            const Type *T = rlt;
            ss.out << "syntax-extend has wrong return type (expected "
                << ReturnLabel({unknown_of(TYPE_Scope)}) << ", got "
                << T << ")";
            location_error(ss.str());
        } break;
        case FN_ScopeOf: {
            CHECKARGS(0, -1);
            Scope *scope = nullptr;
            size_t start = 1;
            if ((args.size() > 1) && (args[1].key == SYM_Unnamed)) {
                start = 2;
                scope = Scope::from(args[1].value);
            } else {
                scope = Scope::from();
            }
            for (size_t i = start; i < args.size(); ++i) {
                auto &&arg = args[i];
                if (arg.key == SYM_Unnamed) {
                    scope = Scope::from(scope, arg.value);
                } else {
                    scope->bind(arg.key, arg.value);
                }
            }
            RETARGS(scope);
        } break;
        case FN_AllocaOf: {
            CHECKARGS(1, 1);
            const Type *T = args[1].value.type;
            void *src = get_pointer(T, args[1].value);
            void *dst = tracked_malloc(size_of(T));
            memcpy(dst, src, size_of(T));
            RETARGS(Any::from_pointer(NativeROPointer(T), dst));
        } break;
        case FN_StaticAlloc: {
            CHECKARGS(1, 1);
            const Type *T = args[1].value;
            void *dst = tracked_malloc(size_of(T));
            RETARGS(Any::from_pointer(StaticPointer(T), dst));
        } break;
        case FN_NullOf: {
            CHECKARGS(1, 1);
            const Type *T = args[1].value;
            Any value = none;
            value.type = T;
            if (!is_opaque(T)) {
                void *ptr = get_pointer(T, value, true);
                memset(ptr, 0, size_of(T));
            }
            RETARGS(value);
        } break;
        case FN_ExternSymbol: {
            CHECKARGS(1, 1);
            verify_kind<TK_Extern>(args[1].value);
            RETARGS(args[1].value.symbol);
        } break;
        case FN_ExternNew: {
            CHECKARGS(2, -1);
            args[1].value.verify(TYPE_Symbol);
            const Type *T = args[2].value;
            Any value(args[1].value.symbol);
            Symbol extern_storage_class = SYM_Unnamed;
            size_t flags = 0;
            int location = -1;
            int binding = -1;
            if (args.size() > 3) {
                size_t i = 3;
                while (i < args.size()) {
                    auto &&arg = args[i];
                    switch(arg.key.value()) {
                    case SYM_Location: {
                        if (location == -1) {
                            location = cast_number<int>(arg.value);
                        } else {
                            location_error(String::from("duplicate location"));
                        }
                    } break;
                    case SYM_Binding: {
                        if (binding == -1) {
                            binding = cast_number<int>(arg.value);
                        } else {
                            location_error(String::from("duplicate binding"));
                        }
                    } break;
                    case SYM_Storage: {
                        arg.value.verify(TYPE_Symbol);

                        if (extern_storage_class == SYM_Unnamed) {
                            switch(arg.value.symbol.value()) {
                            #define T(NAME) \
                                case SYM_SPIRV_StorageClass ## NAME:
                                B_SPIRV_STORAGE_CLASS()
                            #undef T
                                extern_storage_class = arg.value.symbol; break;
                            default: {
                                location_error(String::from("illegal storage class"));
                            } break;
                            }
                        } else {
                            location_error(String::from("duplicate storage class"));
                        }
                    } break;
                    case SYM_Unnamed: {
                        arg.value.verify(TYPE_Symbol);

                        switch(arg.value.symbol.value()) {
                        case SYM_Buffer: flags |= EF_BufferBlock; break;
                        case SYM_ReadOnly: flags |= EF_NonWritable; break;
                        case SYM_WriteOnly: flags |= EF_NonReadable; break;
                        case SYM_Coherent: flags |= EF_Coherent; break;
                        case SYM_Restrict: flags |= EF_Restrict; break;
                        case SYM_Volatile: flags |= EF_Volatile; break;
                        default: {
                            location_error(String::from("unknown flag"));
                        } break;
                        }
                    } break;
                    default: {
                        StyledString ss;
                        ss.out << "unexpected key: " << arg.key;
                        location_error(ss.str());
                    } break;
                    }

                    i++;
                }
            }
            value.type = Extern(T, flags, extern_storage_class, location, binding);
            RETARGS(value);
        } break;
        case FN_FunctionType: {
            CHECKARGS(1, -1);
            std::vector<const Type *> types;
            size_t k = 2;
            while (k < args.size()) {
                if (args[k].value.type != TYPE_Type)
                    break;
                types.push_back(args[k].value);
                k++;
            }
            uint32_t flags = 0;

            while (k < args.size()) {
                args[k].value.verify(TYPE_Symbol);
                Symbol sym = args[k].value.symbol;
                uint64_t flag = 0;
                switch(sym.value()) {
                case SYM_Variadic: flag = FF_Variadic; break;
                case SYM_Pure: flag = FF_Pure; break;
                default: {
                    StyledString ss;
                    ss.out << "illegal option: " << sym;
                    location_error(ss.str());
                } break;
                }
                flags |= flag;
                k++;
            }
            RETARGS(Function(args[1].value, types, flags));
        } break;
        case FN_TupleType: {
            CHECKARGS(0, -1);
            Args values;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i].value.is_const()) {
                    values.push_back(args[i]);
                } else {
                    values.push_back(
                        Argument(args[i].key,
                            unknown_of(args[i].value.indirect_type())));
                }
            }
            RETARGS(MixedTuple(values));
        } break;
        case FN_UnionType: {
            CHECKARGS(0, -1);
            Args values;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i].value.is_const()) {
                    location_error(String::from("all union type arguments must be non-constant"));
                    //values.push_back(args[i]);
                } else {
                    values.push_back(
                        Argument(args[i].key,
                            unknown_of(args[i].value.indirect_type())));
                }
            }
            RETARGS(MixedUnion(values));
        } break;
        case FN_ReturnLabelType: {
            CHECKARGS(0, -1);
            Args values;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i].value.is_const()) {
                    values.push_back(args[i]);
                } else {
                    values.push_back(
                        Argument(args[i].key,
                            unknown_of(args[i].value.indirect_type())));
                }
            }
            RETARGS(ReturnLabel(values));
        } break;
        case FN_Location: {
            CHECKARGS(0, 0);
            RETARGS(l->body.anchor);
        } break;
        case SFXFN_SetTypenameStorage: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value;
            const Type *T2 = args[2].value;
            verify_kind<TK_Typename>(T);
            cast<TypenameType>(const_cast<Type *>(T))->finalize(T2);
            RETARGS();
        } break;
        case SFXFN_SetTypeSymbol: {
            CHECKARGS(3, 3);
            const Type *T = args[1].value;
            args[2].value.verify(TYPE_Symbol);
            const_cast<Type *>(T)->bind(args[2].value.symbol, args[3].value);
            RETARGS();
        } break;
        case SFXFN_DelTypeSymbol: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value;
            args[2].value.verify(TYPE_Symbol);
            const_cast<Type *>(T)->del(args[2].value.symbol);
            RETARGS();
        } break;
        case FN_TypeAt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value;
            args[2].value.verify(TYPE_Symbol);
            Any result = none;
            if (!T->lookup(args[2].value.symbol, result)) {
                RETARGS(none, false);
            } else {
                RETARGS(result, true);
            }
        } break;
        case FN_TypeLocalAt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value;
            args[2].value.verify(TYPE_Symbol);
            Any result = none;
            if (!T->lookup_local(args[2].value.symbol, result)) {
                RETARGS(none, false);
            } else {
                RETARGS(result, true);
            }
        } break;
        case FN_IsConstant: {
            CHECKARGS(1, 1);
            RETARGS(args[1].value.is_const());
        } break;
        case FN_VaCountOf: {
            RETARGS((int)(args.size()-1));
        } break;
        case FN_VaKeys: {
            CHECKARGS(0, -1);
            Args result = { none };
            for (size_t i = 1; i < args.size(); ++i) {
                result.push_back(args[i].key);
            }
            enter = args[0].value;
            args = result;
        } break;
        case FN_VaValues: {
            CHECKARGS(0, -1);
            Args result = { none };
            for (size_t i = 1; i < args.size(); ++i) {
                result.push_back(args[i].value);
            }
            enter = args[0].value;
            args = result;
        } break;
        case FN_VaKey: {
            CHECKARGS(2, 2);
            args[1].value.verify(TYPE_Symbol);
            enter = args[0].value;
            args = { none, Argument(args[1].value.symbol, args[2].value) };
        } break;
        case FN_VaAt: {
            CHECKARGS(1, -1);
            Args result = { none };
            if (args[1].value.type == TYPE_Symbol) {
                auto key = args[1].value.symbol;
                for (size_t i = 2; i < args.size(); ++i) {
                    if (args[i].key == key) {
                        result.push_back(args[i]);
                    }
                }
            } else {
                size_t idx = cast_number<size_t>(args[1].value);
                for (size_t i = (idx + 2); i < args.size(); ++i) {
                    result.push_back(args[i]);
                }
            }
            enter = args[0].value;
            args = result;
        } break;
        case FN_OffsetOf: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value;
            auto &&arg = args[2].value;
            size_t idx = 0;
            T = storage_type(T);
            verify_kind<TK_Tuple>(T);
            auto ti = cast<TupleType>(T);
            if (arg.type == TYPE_Symbol) {
                idx = ti->field_index(arg.symbol);
                if (idx == (size_t)-1) {
                    StyledString ss;
                    ss.out << "no such field " << arg.symbol << " in storage type " << T;
                    location_error(ss.str());
                }
                // rewrite field
                arg = (int)idx;
            } else {
                idx = cast_number<size_t>(arg);
            }
            verify_range(idx, ti->offsets.size());
            RETARGS(ti->offsets[idx]);
        } break;
        case FN_Branch: {
            CHECKARGS(3, 3);
            args[1].value.verify(TYPE_Bool);
            // either branch label is typed and binds no parameters,
            // so we can directly inline it
            const Closure *newl = nullptr;
            if (args[1].value.i1) {
                newl = args[2].value;
            } else {
                newl = args[3].value;
            }
            verify_branch_continuation(newl);
            evaluate_body(newl->frame, l, newl->label);
        } break;
        case FN_IntToPtr: {
            CHECKARGS(2, 2);
            verify_integer(storage_type(args[1].value.type));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_kind<TK_Pointer>(storage_type(DestT));
            Any result = args[1].value;
            result.type = DestT;
            RETARGS(result);
        } break;
        case FN_PtrToInt: {
            CHECKARGS(2, 2);
            verify_kind<TK_Pointer>(storage_type(args[1].value.type));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(storage_type(DestT));
            Any result = args[1].value;
            result.type = DestT;
            RETARGS(result);
        } break;
        case FN_ITrunc: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            verify_integer(storage_type(T));
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(storage_type(DestT));
            Any result = args[1].value;
            result.type = DestT;
            RETARGS(result);
        } break;
        case FN_FPTrunc: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            if ((T == TYPE_F64) && (DestT == TYPE_F32)) {
                RETARGS((float)args[1].value.f64);
            } else { invalid_op2_types_error(T, DestT); }
        } break;
        case FN_FPExt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            if ((T == TYPE_F32) && (DestT == TYPE_F64)) {
                RETARGS((double)args[1].value.f32);
            } else { invalid_op2_types_error(T, DestT); }
        } break;
        case FN_FPToUI: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(DestT);
            uint64_t val = 0;
            if (T == TYPE_F32) {
                val = (uint64_t)args[1].value.f32;
            } else if (T == TYPE_F64) {
                val = (uint64_t)args[1].value.f64;
            } else {
                invalid_op2_types_error(T, DestT);
            }
            Any result = val;
            result.type = DestT;
            RETARGS(result);
        } break;
        case FN_FPToSI: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            verify_real(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_integer(DestT);
            int64_t val = 0;
            if (T == TYPE_F32) {
                val = (int64_t)args[1].value.f32;
            } else if (T == TYPE_F64) {
                val = (int64_t)args[1].value.f64;
            } else {
                invalid_op2_types_error(T, DestT);
            }
            Any result = val;
            result.type = DestT;
            RETARGS(result);
        } break;
        case FN_UIToFP: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            verify_integer(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            uint64_t src = cast_number<uint64_t>(args[1].value);
            Any result = none;
            if (DestT == TYPE_F32) {
                result = (float)src;
            } else if (DestT == TYPE_F64) {
                result = (double)src;
            } else {
                invalid_op2_types_error(T, DestT);
            }
            RETARGS(result);
        } break;
        case FN_SIToFP: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            verify_integer(T);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            verify_real(DestT);
            int64_t src = cast_number<int64_t>(args[1].value);
            Any result = none;
            if (DestT == TYPE_F32) {
                result = (float)src;
            } else if (DestT == TYPE_F64) {
                result = (double)src;
            } else {
                invalid_op2_types_error(T, DestT);
            }
            RETARGS(result);
        } break;
        case FN_ZExt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            auto ST = storage_type(T);
            verify_integer(ST);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            auto DestST = storage_type(DestT);
            verify_integer(DestST);
            Any result = args[1].value;
            result.type = DestT;
            int oldbitnum = integer_type_bit_size(ST);
            int newbitnum = integer_type_bit_size(DestST);
            for (int i = oldbitnum; i < newbitnum; ++i) {
                result.u64 &= ~(1ull << i);
            }
            RETARGS(result);
        } break;
        case FN_SExt: {
            CHECKARGS(2, 2);
            const Type *T = args[1].value.type;
            auto ST = storage_type(T);
            verify_integer(ST);
            args[2].value.verify(TYPE_Type);
            const Type *DestT = args[2].value.typeref;
            auto DestST = storage_type(DestT);
            verify_integer(DestST);
            Any result = args[1].value;
            result.type = DestT;
            int oldbitnum = integer_type_bit_size(ST);
            int newbitnum = integer_type_bit_size(DestST);
            uint64_t bit = (result.u64 >> (oldbitnum - 1)) & 1ull;
            for (int i = oldbitnum; i < newbitnum; ++i) {
                result.u64 &= ~(1ull << i);
                result.u64 |= bit << i;
            }
            RETARGS(result);
        } break;
        case FN_TypeOf: {
            CHECKARGS(1, 1);
            RETARGS(args[1].value.indirect_type());
        } break;
        case FN_ExtractElement: {
            CHECKARGS(2, 2);
            const Type *T = storage_type(args[1].value.type);
            verify_kind<TK_Vector>(T);
            auto vi = cast<VectorType>(T);
            size_t idx = cast_number<size_t>(args[2].value);
            RETARGS(vi->unpack(args[1].value.pointer, idx));
        } break;
        case FN_InsertElement: {
            CHECKARGS(3, 3);
            const Type *T = storage_type(args[1].value.type);
            const Type *ET = storage_type(args[2].value.type);
            size_t idx = cast_number<size_t>(args[3].value);
            void *destptr = args[1].value.pointer;
            void *offsetptr = nullptr;
            destptr = copy_storage(T, destptr);
            auto vi = cast<VectorType>(T);
            verify(storage_type(vi->type_at_index(idx)), ET);
            offsetptr = vi->getelementptr(destptr, idx);
            void *srcptr = get_pointer(ET, args[2].value);
            memcpy(offsetptr, srcptr, size_of(ET));
            RETARGS(Any::from_pointer(args[1].value.type, destptr));
        } break;
        case FN_ShuffleVector: {
            CHECKARGS(3, 3);
            const Type *TV1 = storage_type(args[1].value.type);
            const Type *TV2 = storage_type(args[2].value.type);
            const Type *TMask = storage_type(args[3].value.type);
            verify_kind<TK_Vector>(TV1);
            verify_kind<TK_Vector>(TV2);
            verify_kind<TK_Vector>(TMask);
            verify(TV1, TV2);
            auto vi = cast<VectorType>(TV1);
            auto mask_vi = cast<VectorType>(TMask);
            verify(TYPE_I32, storage_type(mask_vi->element_type));
            size_t halfcount = vi->count;
            size_t incount = halfcount * 2;
            size_t outcount = mask_vi->count;
            const Type *T = Vector(vi->element_type, outcount);
            void *srcptr_a = get_pointer(TV1, args[1].value);
            void *srcptr_b = get_pointer(TV1, args[2].value);
            void *destptr = alloc_storage(T);
            auto out_vi = cast<VectorType>(T);
            size_t esize = size_of(vi->element_type);
            for (size_t i = 0; i < outcount; ++i) {
                size_t idx = (size_t)mask_vi->unpack(args[3].value.pointer, i).i32;
                verify_range(idx, incount);
                void *srcptr;
                if (idx < halfcount) {
                    srcptr = srcptr_a;
                } else {
                    srcptr = srcptr_b;
                    idx -= halfcount;
                }
                void *inp = vi->getelementptr(srcptr, idx);
                void *outp = out_vi->getelementptr(destptr, i);
                memcpy(outp, inp, esize);
            }
            RETARGS(Any::from_pointer(T, destptr));
        } break;
        case FN_ExtractValue: {
            CHECKARGS(2, 2);
            size_t idx = cast_number<size_t>(args[2].value);
            const Type *T = storage_type(args[1].value.type);
            switch(T->kind()) {
            case TK_Array: {
                auto ai = cast<ArrayType>(T);
                RETARGS(ai->unpack(args[1].value.pointer, idx));
            } break;
            case TK_Tuple: {
                auto ti = cast<TupleType>(T);
                RETARGS(ti->unpack(args[1].value.pointer, idx));
            } break;
            case TK_Union: {
                auto ui = cast<UnionType>(T);
                RETARGS(ui->unpack(args[1].value.pointer, idx));
            } break;
            default: {
                StyledString ss;
                ss.out << "can not extract value from type " << T;
                location_error(ss.str());
            } break;
            }
        } break;
        case FN_InsertValue: {
            CHECKARGS(3, 3);
            const Type *T = storage_type(args[1].value.type);
            const Type *ET = storage_type(args[2].value.type);
            size_t idx = cast_number<size_t>(args[3].value);

            void *destptr = args[1].value.pointer;
            void *offsetptr = nullptr;
            switch(T->kind()) {
            case TK_Array: {
                destptr = copy_storage(T, destptr);
                auto ai = cast<ArrayType>(T);
                verify(storage_type(ai->type_at_index(idx)), ET);
                offsetptr = ai->getelementptr(destptr, idx);
            } break;
            case TK_Tuple: {
                destptr = copy_storage(T, destptr);
                auto ti = cast<TupleType>(T);
                verify(storage_type(ti->type_at_index(idx)), ET);
                offsetptr = ti->getelementptr(destptr, idx);
            } break;
            case TK_Union: {
                destptr = copy_storage(T, destptr);
                auto ui = cast<UnionType>(T);
                verify(storage_type(ui->type_at_index(idx)), ET);
                offsetptr = destptr;
            } break;
            default: {
                StyledString ss;
                ss.out << "can not extract value from type " << T;
                location_error(ss.str());
            } break;
            }
            void *srcptr = get_pointer(ET, args[2].value);
            memcpy(offsetptr, srcptr, size_of(ET));
            RETARGS(Any::from_pointer(args[1].value.type, destptr));
        } break;
        case FN_AnyExtract: {
            CHECKARGS(1, 1);
            args[1].value.verify(TYPE_Any);
            Any arg = *args[1].value.ref;
            RETARGS(arg);
        } break;
        case FN_AnyWrap: {
            CHECKARGS(1, 1);
            RETARGS(args[1].value.toref());
        } break;
        case FN_Purify: {
            CHECKARGS(1, 1);
            Any arg = args[1].value;
            verify_function_pointer(arg.type);
            auto pi = cast<PointerType>(arg.type);
            auto fi = cast<FunctionType>(pi->element_type);
            if (fi->flags & FF_Pure) {
                RETARGS(args[1]);
            } else {
                arg.type = Pointer(Function(
                    fi->return_type, fi->argument_types, fi->flags | FF_Pure),
                    pi->flags, pi->storage_class);
                RETARGS(arg);
            }
        } break;
        case SFXFN_CompilerError: {
            CHECKARGS(1, 1);
            location_error(args[1].value);
            RETARGS();
        } break;
        case FN_CompilerMessage: {
            CHECKARGS(1, 1);
            args[1].value.verify(TYPE_String);
            StyledString ss;
            ss.out << l->body.anchor << " message: " << args[1].value.string->data << std::endl;
            std::cout << ss.str()->data;
            RETARGS();
        } break;
        case FN_Dump: {
            CHECKARGS(0, -1);
            StyledStream ss(std::cerr);
            ss << l->body.anchor << " dump:";
            for (size_t i = 1; i < args.size(); ++i) {
                ss << " ";
                if (args[i].key != SYM_Unnamed) {
                    ss << args[i].key << " " << Style_Operator << "=" << Style_None << " ";
                }

                if (args[i].value.is_const()) {
                    stream_expr(ss, args[i].value, StreamExprFormat::singleline());
                } else {
                    /*
                    ss << "<unknown>"
                        << Style_Operator << ":" << Style_None
                        << args[i].value.indirect_type() << std::endl;*/
                    args[i].value.stream(ss, false);
                }
            }
            ss << std::endl;
            enter = args[0].value;
            args[0].value = none;
        } break;
        case FN_Cross: {
            CHECKARGS(2, 2);
            const Type *Ta = storage_type(args[1].value.type);
            const Type *Tb = storage_type(args[2].value.type);
            verify_real_vector(Ta, 3);
            verify(Ta, Tb);
            RETARGS(apply_real_op<op_Cross>(args[1].value, args[2].value));
        } break;
        case FN_Normalize: {
            CHECKARGS(1, 1);
            verify_real_ops(args[1].value);
            RETARGS(apply_real_op<op_Normalize>(args[1].value));
        } break;
        case FN_Length: {
            CHECKARGS(1, 1);
            verify_real_ops(args[1].value);
            RETARGS(apply_real_op<op_Length>(args[1].value));
        } break;
        case FN_Distance: {
            CHECKARGS(2, 2);
            verify_real_ops(args[1].value, args[2].value);
            RETARGS(apply_real_op<op_Distance>(args[1].value, args[2].value));
        } break;
        case OP_ICmpEQ:
        case OP_ICmpNE:
        case OP_ICmpUGT:
        case OP_ICmpUGE:
        case OP_ICmpULT:
        case OP_ICmpULE:
        case OP_ICmpSGT:
        case OP_ICmpSGE:
        case OP_ICmpSLT:
        case OP_ICmpSLE: {
            CHECKARGS(2, 2);
            verify_integer_ops(args[1].value, args[2].value);
#define B_INT_OP1(OP, N) \
    result = apply_integer_op<IntTypes_ ## N, op_ ## OP>(args[1].value);
#define B_INT_OP2(OP, N) \
    result = apply_integer_op<IntTypes_ ## N, op_ ## OP>(args[1].value, args[2].value);
            Any result = false;
            switch(enter.builtin.value()) {
            case OP_ICmpEQ: B_INT_OP2(Equal, u); break;
            case OP_ICmpNE: B_INT_OP2(NotEqual, u); break;
            case OP_ICmpUGT: B_INT_OP2(Greater, u); break;
            case OP_ICmpUGE: B_INT_OP2(GreaterEqual, u); break;
            case OP_ICmpULT: B_INT_OP2(Less, u); break;
            case OP_ICmpULE: B_INT_OP2(LessEqual, u); break;
            case OP_ICmpSGT: B_INT_OP2(Greater, i); break;
            case OP_ICmpSGE: B_INT_OP2(GreaterEqual, i); break;
            case OP_ICmpSLT: B_INT_OP2(Less, i); break;
            case OP_ICmpSLE: B_INT_OP2(LessEqual, i); break;
            default: assert(false); break;
            }
            RETARGS(result);
        } break;
        case OP_FCmpOEQ:
        case OP_FCmpONE:
        case OP_FCmpORD:
        case OP_FCmpOGT:
        case OP_FCmpOGE:
        case OP_FCmpOLT:
        case OP_FCmpOLE:
        case OP_FCmpUEQ:
        case OP_FCmpUNE:
        case OP_FCmpUNO:
        case OP_FCmpUGT:
        case OP_FCmpUGE:
        case OP_FCmpULT:
        case OP_FCmpULE: {
            CHECKARGS(2, 2);
            verify_real_ops(args[1].value, args[2].value);
#define B_FLOAT_OP1(OP) \
    result = apply_real_op<op_ ## OP>(args[1].value);
#define B_FLOAT_OP2(OP) \
    result = apply_real_op<op_ ## OP>(args[1].value, args[2].value);
            Any result = false;
            switch(enter.builtin.value()) {
            case OP_FCmpOEQ: B_FLOAT_OP2(OEqual); break;
            case OP_FCmpONE: B_FLOAT_OP2(ONotEqual); break;
            case OP_FCmpORD: B_FLOAT_OP2(Ordered); break;
            case OP_FCmpOGT: B_FLOAT_OP2(OGreater); break;
            case OP_FCmpOGE: B_FLOAT_OP2(OGreaterEqual); break;
            case OP_FCmpOLT: B_FLOAT_OP2(OLess); break;
            case OP_FCmpOLE: B_FLOAT_OP2(OLessEqual); break;
            case OP_FCmpUEQ: B_FLOAT_OP2(UEqual); break;
            case OP_FCmpUNE: B_FLOAT_OP2(UNotEqual); break;
            case OP_FCmpUNO: B_FLOAT_OP2(Unordered); break;
            case OP_FCmpUGT: B_FLOAT_OP2(UGreater); break;
            case OP_FCmpUGE: B_FLOAT_OP2(UGreaterEqual); break;
            case OP_FCmpULT: B_FLOAT_OP2(ULess); break;
            case OP_FCmpULE: B_FLOAT_OP2(ULessEqual); break;
            default: assert(false); break;
            }
            RETARGS(result);
        } break;
#define IARITH_NUW_NSW_OPS(NAME) \
    case OP_ ## NAME: \
    case OP_ ## NAME ## NUW: \
    case OP_ ## NAME ## NSW: { \
        CHECKARGS(2, 2); \
        verify_integer_ops(args[1].value, args[2].value); \
        Any result = none; \
        switch(enter.builtin.value()) { \
        case OP_ ## NAME: B_INT_OP2(NAME, u); break; \
        case OP_ ## NAME ## NUW: B_INT_OP2(NAME, u); break; \
        case OP_ ## NAME ## NSW: B_INT_OP2(NAME, i); break; \
        default: assert(false); break; \
        } \
        result.type = args[1].value.type; \
        RETARGS(result); \
    } break;
#define IARITH_OP(NAME, PFX) \
    case OP_ ## NAME: { \
        CHECKARGS(2, 2); \
        verify_integer_ops(args[1].value, args[2].value); \
        Any result = none; \
        B_INT_OP2(NAME, PFX); \
        result.type = args[1].value.type; \
        RETARGS(result); \
    } break;
#define FARITH_OP(NAME) \
    case OP_ ## NAME: { \
        CHECKARGS(2, 2); \
        verify_real_ops(args[1].value, args[2].value); \
        Any result = none; \
        B_FLOAT_OP2(NAME); \
        RETARGS(result); \
    } break;
#define IUN_OP(NAME, PFX) \
    case OP_ ## NAME: { \
        CHECKARGS(1, 1); \
        verify_integer_ops(args[1].value); \
        Any result = none; \
        B_INT_OP1(NAME, PFX); \
        result.type = args[1].value.type; \
        RETARGS(result); \
    } break;
#define FUN_OP(NAME) \
    case OP_ ## NAME: { \
        CHECKARGS(1, 1); \
        verify_real_ops(args[1].value); \
        Any result = none; \
        B_FLOAT_OP1(NAME); \
        RETARGS(result); \
    } break;

        B_ARITH_OPS()

        default: {
            StyledString ss;
            ss.out << "can not fold constant expression using builtin " << enter.builtin;
            location_error(ss.str());
        } break;
        }
        return true;
    }

    void inline_branch_continuations(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "inlining branch continuations in " << l << std::endl;
#endif

        auto &&args = l->body.args;
        CHECKARGS(3, 3);
        args[1].value.verify_indirect(TYPE_Bool);
        const Closure *then_br = args[2].value;
        const Closure *else_br = args[3].value;
        verify_branch_continuation(then_br);
        verify_branch_continuation(else_br);
        args[0].value = none;
        args[2].value = typify_single(then_br->frame, then_br->label, {});
        args[3].value = typify_single(else_br->frame, else_br->label, {});
    }

#undef IARITH_NUW_NSW_OPS
#undef IARITH_OP
#undef FARITH_OP
#undef FARITH_OPF
#undef B_INT_OP2
#undef B_INT_OP1
#undef B_FLOAT_OP2
#undef B_FLOAT_OP1
#undef CHECKARGS
#undef RETARGS
#undef IUN_OP
#undef FUN_OP

    /*
    void inline_single_label(Label *dest, Label *source) {
        Frame frame(nullptr, source);
        map_constant_arguments(&frame, source, dest->body.args);
        evaluate_body(&frame, dest, source);
    }*/

    static Label *skip_jumps(Label *l) {
        size_t counter = 0;
        while (jumps_immediately(l)) {
            l = l->body.enter.label;
            counter++;
            if (counter == SCOPES_MAX_SKIP_JUMPS) {
                std::cerr
                    << "internal warning: max iterations exceeded"
                        " during jump skip check" << std::endl;
                break;
            }
        }
        return l;
    }

    static bool is_exiting(Label *l) {
        return is_calling_continuation(l) || is_continuing_to_parameter(l);
    }

    // label targets count as calls, all other as ops
    static void count_instructions(Label *l, int &callcount, int &opcount) {
        callcount = 0;
        opcount = 0;
        assert(!l->params.empty());
        std::unordered_set<Label *> visited;
        while (!is_exiting(l) && !visited.count(l)) {
            visited.insert(l);
            if (jumps_immediately(l)) {
                l = l->body.enter.label;
                continue;
            }
            if (is_calling_label(l) || is_calling_closure(l)) {
                callcount++;
            } else {
                opcount++;
            }
            if (is_continuing_to_label(l)) {
                l = l->body.args[0].value.label;
                continue;
            } else if (l->body.args[0].value.type == TYPE_Nothing) {
                // branch, unreachable, etc.
                break;
            } else {
                StyledStream ss(std::cerr);
                ss << "internal warning: unexpected continuation type "
                    << l->body.args[0].value.type
                    << " encountered while counting instructions" << std::endl;
                break;
            }
        }
    }

    static bool is_trivial_function(Label *l) {
        assert(!l->params.empty());
        if (!isa<ReturnLabelType>(l->params[0]->type))
            return false;
        if (l->params.size() == 1) {
            // doesn't take any arguments
            return true;
        }
#if 1
        int callcount, opcount;
        count_instructions(l, callcount, opcount);
        return (callcount <= 4) && (opcount <= 1);
#else
        assert(!l->params.empty());
        l = skip_jumps(l);
        if (is_exiting(l))
            return true;
        if (is_continuing_to_label(l)) {
            l = l->body.args[0].value.label;
            l = skip_jumps(l);
            if (is_exiting(l))
                return true;
        }
#endif
        return false;
    }

    static bool matches_arg_count(Label *l, size_t inargs) {
        // works only on instantiated labels, as we're
        // assuming no parameter is variadic at this point
        auto &&params = l->params;
        return ((inargs + 1) == std::max(size_t(1), params.size()));
    }

    static bool forwards_all_args(Label *l) {
        assert(!l->params.empty());
        auto &&args = l->body.args;
        auto &&params = l->params;
        if (args.size() != params.size())
            return false;
        for (size_t i = 1; i < args.size(); ++i) {
            auto &&arg = args[i];
            if (arg.value.type != TYPE_Parameter)
                return false;
            if (arg.value.parameter != params[i])
                return false;
        }
        return true;
    }

    // a label just jumps to the next label
    static bool jumps_immediately(Label *l) {
        return is_calling_label(l)
            && l->get_label_enter()->is_basic_block_like();
    }

    void clear_continuation_arg(Label *l) {
        auto &&args = l->body.args;
        args[0] = none;
    }

    // clear continuation argument and clear it for labels that use it
    void delete_continuation(Label *owner) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "deleting continuation of " << owner << std::endl;
#endif

        assert(!owner->params.empty());
        Parameter *param = owner->params[0];
        param->type = TYPE_Nothing;

        assert(!is_called_by(param, owner));
        if (is_continuing_from(param, owner)) {
            clear_continuation_arg(owner);
        }
    }

    void type_continuation_from_label_return_type(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "typing continuation from label return type in " << l << std::endl;
#endif
        auto &&enter = l->body.enter;
        auto &&args = l->body.args;
        assert(enter.type == TYPE_Label);
        Label *enter_label = enter.label;
        assert(!args.empty());
        assert(!enter_label->params.empty());
        Parameter *cont_param = enter_label->params[0];
        const Type *cont_type = cont_param->type;

        if (isa<ReturnLabelType>(cont_type)) {
            auto tli = cast<ReturnLabelType>(cont_type);
            args[0] = fold_type_return(args[0].value, tli->values);
        } else {
#if SCOPES_DEBUG_CODEGEN
            ss_cout << "unexpected return type: " << cont_type << std::endl;
#endif
            assert(false && "todo: unexpected return type");
        }
    }

    bool type_continuation_from_builtin_call(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "typing continuation from builtin call in " << l << std::endl;
#endif
        auto &&enter = l->body.enter;
        assert(enter.type == TYPE_Builtin);
        auto &&args = l->body.args;
        if ((enter.builtin == SFXFN_Unreachable)
            || (enter.builtin == SFXFN_Discard)) {
            args[0] = none;
        } else {
            Args values;
            bool fold = values_from_builtin_call(l, values);
            if (fold) {
                enter = args[0].value;
                args = { none };
                for (size_t i = 0; i < values.size(); ++i) {
                    args.push_back(values[i]);
                }
                return true;
            } else {
                args[0] = fold_type_return(args[0].value, values);
            }
        }

        return false;
    }

    void type_continuation_from_function_call(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "typing continuation from function call in " << l << std::endl;
#endif
        auto &&enter = l->body.enter;

        const FunctionType *fi = extract_function_type(enter.indirect_type());
        verify_function_argument_signature(fi, l);
        auto &&args = l->body.args;
        auto rlt = cast<ReturnLabelType>(fi->return_type);
        args[0] = fold_type_return(args[0].value, rlt->values);
    }

    void type_continuation_call(Label *l) {
#if SCOPES_DEBUG_CODEGEN
        ss_cout << "typing continuation call in " << l << std::endl;
#endif
        auto &&args = l->body.args;
        Args newargs;
        Args values;
        values.reserve(args.size());
        newargs.reserve(args.size());
        newargs.push_back(none);
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i].value.is_const()) {
                values.push_back(args[i]);
            } else {
                values.push_back(Argument(
                    args[i].key,
                    unknown_of(args[i].value.indirect_type())));
                newargs.push_back(args[i]);
            }
        }
        fold_type_return(l->body.enter, values);
        args = newargs;
    }

    static void inc_solve_ref() {
        solve_refs++;
    }

    static bool dec_solve_ref() {
        solve_refs--;
        assert(solve_refs >= 0);
        return solve_refs == 0;
    }

    Label *solve(Label *entry) {
        inc_solve_ref();
        SCOPES_TRY()

        normalize_label(entry);

        SCOPES_CATCH(exc)
            if (dec_solve_ref()) {
                print_traceback();
                traceback.clear();
            }
            error(exc);
        SCOPES_TRY_END()

        if (dec_solve_ref()) {
            traceback.clear();
        }
        lower2cff(entry);
#if SCOPES_CLEANUP_LABELS
        cleanup_labels(entry);
#endif
        return entry;
    }

    void complete_existing_label_continuation (Label *l) {
        Label *enter_label = l->get_label_enter();
        if (!enter_label->is_basic_block_like()) {
            assert(enter_label->body.is_complete());
            assert(enter_label->is_return_param_typed());
            type_continuation_from_label_return_type(l);
        }
    }

    enum CLICmd {
        CmdNone,
        CmdSkip,
    };

    CLICmd on_label_processing(Label *l, const char *task = nullptr) {
        if (!enable_step_debugger)
            return CmdNone;
        CLICmd clicmd = CmdNone;
        auto slfmt = StreamLabelFormat::debug_single();
        slfmt.anchors = StreamLabelFormat::Line;
        if (task) {
            ss_cout << task << std::endl;
        }
        stream_label(ss_cout, l, slfmt);
        bool skip = false;
        while (!skip) {
            set_active_anchor(l->body.anchor);
            char *r = linenoise("solver> ");
            if (!r) {
                location_error(String::from("aborted"));
            }

            linenoiseHistoryAdd(r);
            SCOPES_TRY()
                auto file = SourceFile::from_string(Symbol("<string>"),
                    String::from_cstr(r));
                LexerParser parser(file);
                auto expr = parser.parse();
                //stream_expr(ss_cout, expr, StreamExprFormat());
                const List *stmts = unsyntax(expr);
                if (stmts != EOL) {
                    while (stmts != EOL) {
                        set_active_anchor(stmts->at.syntax->anchor);
                        auto cmd = unsyntax(stmts->at);
                        Symbol head = SYM_Unnamed;
                        const List *arglist = nullptr;
                        if (cmd.type == TYPE_Symbol) {
                            head = cmd.symbol;
                        } else if (cmd.type == TYPE_List) {
                            arglist = cmd.list;
                            if (arglist != EOL) {
                                cmd = unsyntax(arglist->at);
                                if (cmd.type == TYPE_Symbol) {
                                    head = cmd.symbol;
                                }
                                arglist = arglist->next;
                            }
                        }
                        if (head == SYM_Unnamed) {
                            location_error(String::from("syntax error"));
                        }
                        switch(head.value()) {
                        case SYM_C:
                        case KW_Continue: {
                            skip = true;
                            enable_step_debugger = false;
                        } break;
                        case SYM_Skip: {
                            skip = true;
                            clicmd = CmdSkip;
                            enable_step_debugger = false;
                        } break;
                        case SYM_Original: {
                            Label *o = l->original;
                            while (o) {
                                stream_label(ss_cout, o, slfmt);
                                o = o->original;
                            }
                        } break;
                        case SYM_Help: {
                            ss_cout << "Available commands:" << std::endl;
                            ss_cout << "c(ontinue) help original skip" << std::endl;
                            ss_cout << "An empty line continues to the next label." << std::endl;
                        } break;
                        default: {
                            location_error(String::from("unknown command. try 'help'."));
                        }break;
                        }
                        stmts = stmts->next;
                    }
                } else {
                    skip = true;
                }
            SCOPES_CATCH(exc)
                print_exception(exc);
            SCOPES_TRY_END()
        }
        return clicmd;
    }

    void normalize_label(Label *l) {
        if (l->body.is_complete())
            return;
        SCOPES_TRY()

        size_t ssz = memory_stack_size();
        if (ssz >= SCOPES_MAX_STACK_SIZE) {
            location_error(String::from("stack overflow during partial evaluation"));
        }

        CLICmd clicmd = CmdNone;
        while (!l->body.is_complete()) {
            assert(!l->is_template());
            if (clicmd == CmdSkip) {
                enable_step_debugger = true;
            }
            clicmd = on_label_processing(l);

            l->verify_valid();

            assert(all_params_typed(l));

            set_active_anchor(l->body.anchor);

            if (!all_args_typed(l)) {
                size_t idx = find_untyped_arg(l);
                StyledString ss;
                ss.out << "parameter " << l->body.args[idx].value.parameter
                    << " passed as argument " << idx << " has not been typed yet";
                location_error(ss.str());
            }

            if (is_calling_callable(l)) {
                fold_callable_call(l);
                continue;
            } else if (is_calling_function(l)) {
                verify_no_keyed_args(l);
                if (is_calling_pure_function(l)
                    && all_args_constant(l)) {
                    fold_pure_function_call(l);
                    continue;
                } else {
                    type_continuation_from_function_call(l);
                }
            } else if (is_calling_builtin(l)) {
                if (!builtin_has_keyed_args(l->get_builtin_enter()))
                    verify_no_keyed_args(l);
                if ((all_args_constant(l)
                    && !builtin_never_folds(l->get_builtin_enter()))
                    || builtin_always_folds(l->get_builtin_enter())) {
                    if (fold_builtin_call(l))
                        continue;
                } else if (l->body.enter.builtin == FN_Branch) {
                    inline_branch_continuations(l);
                    auto &&args = l->body.args;
                    l->body.set_complete();
                    normalize_label(args[2].value);
                    l = args[3].value;
                    continue;
                } else {
                    if (type_continuation_from_builtin_call(l))
                        continue;
                }
            } else if (is_calling_closure(l)) {
                if (has_keyed_args(l)) {
                    solve_keyed_args(l);
                }

                auto result = fold_type_label_arguments(l);
                if (result == FR_Continue)
                    continue;
                else if (result == FR_Break)
                    break;
            } else if (is_calling_continuation(l)) {
                type_continuation_call(l);
            } else if (is_calling_label(l)) {
                if (!l->get_label_enter()->body.is_complete()) {
                    location_error(String::from("failed to propagate return type from untyped label"));
                }
                complete_existing_label_continuation(l);
            } else {
                StyledString ss;
                auto &&enter = l->body.enter;
                if (!enter.is_const()) {
                    ss.out << "unable to call variable of type " << enter.indirect_type();
                } else {
                    ss.out << "unable to call constant of type " << enter.type;
                }
                location_error(ss.str());
            }

#if SCOPES_DEBUG_CODEGEN
            Label *oldl = l;
#endif
            l->body.set_complete();
            if (jumps_immediately(l)) {
                Label *enter_label = l->get_label_enter();
                /*
                if (!enter_label->has_params()) {
#if SCOPES_DEBUG_CODEGEN
                    stream_label(ss_cout, l, StreamLabelFormat::debug_single());
                    stream_label(ss_cout, enter_label, StreamLabelFormat::debug_single());
                    ss_cout << "folding jump to label in " << l << std::endl;
#endif
                    l->body = enter_label->body;
                    continue;
                } else */ {
                    if (!is_jumping(l)) {
                        clear_continuation_arg(l);
                    }
                    l = enter_label;
                    assert(all_params_typed(l));
                }
            } else if (is_continuing_to_label(l)) {
                Label *nextl = l->body.args[0].value.label;
                if (!all_params_typed(nextl)) {
                    StyledStream ss(std::cerr);
                    stream_label(ss, l, StreamLabelFormat::debug_single());
                    location_error(String::from("failed to type continuation"));
                }
                l = nextl;
            }
#if SCOPES_DEBUG_CODEGEN
            ss_cout << "done: ";
            stream_label(ss_cout, oldl, StreamLabelFormat::debug_single());
#endif

        }

        SCOPES_CATCH(exc)
            traceback.push_back(l);
            error(exc);
        SCOPES_TRY_END()
    }

    // eliminate single user labels
    void cleanup_labels(Label *entry) {
        Timer cleanup_timer(TIMER_CleanupLabels);

        size_t count = 0;
        size_t total_processed = 0;
        while (true) {
            std::unordered_set<Label *> visited;
            std::vector<Label *> labels;
            entry->build_reachable(visited, &labels);

            Label::UserMap um;
            for (auto it = labels.begin(); it != labels.end(); ++it) {
                (*it)->insert_into_usermap(um);
            }

            std::unordered_set<Label *> deleted;
            size_t processed = 0;
            for (auto it = labels.begin(); it != labels.end(); ++it) {
                Label *l = *it;
                if (!l->is_basic_block_like())
                    continue;
                auto umit = um.label_map.find(l);
                if (umit == um.label_map.end())
                    continue;
                auto &&users = umit->second;
                if (users.size() != 1)
                    continue;
                Label *user = *users.begin();
                if (deleted.count(user))
                    continue;
                if (user->body.enter.type != TYPE_Label)
                    continue;
                if (user->body.enter.label != l)
                    continue;
                auto &&args = user->body.args;
                bool ok = true;
                for (size_t i = 1; i < user->body.args.size(); ++i) {
                    if (is_unknown(args[i].value)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;
                processed++;
                deleted.insert(l);
                Label *newl = l;
                StyledStream ss;
                if (l->params.size() > 1) {
                    // inline parameters into scope
                    Args newargs = { none };
                    for (size_t i = 1; i < user->body.args.size(); ++i) {
                        newargs.push_back(args[i]);
                    }
                    newl = fold_type_label(um, l, newargs);
                }
                l->remove_from_usermap(um);
                user->remove_from_usermap(um);
                user->body = newl->body;
                user->insert_into_usermap(um);
            }

            if (!processed) break;

            total_processed += processed;
            count++;
        }
#if 0
        if (total_processed)
            std::cout << "eliminated "
                << total_processed << " labels in "
                << count << " passes" << std::endl;
#endif
    }

    Label *lower2cff(Label *entry) {
        Timer lower2cff_timer(TIMER_Lower2CFF);

        size_t numchanges = 0;
        size_t iterations = 0;
        do {
            numchanges = 0;
            iterations++;
            if (iterations > 256) {
                location_error(String::from(
                    "free variable elimination not "
                    "terminated after 256 iterations"));
            }

            std::unordered_set<Label *> visited;
            std::vector<Label *> labels;
            entry->build_reachable(visited, &labels);

            Label::UserMap um;
            for (auto it = labels.begin(); it != labels.end(); ++it) {
                (*it)->insert_into_usermap(um);
            }

            std::unordered_set<Label *> illegal;
            std::unordered_set<Label *> has_illegals;
            for (auto it = labels.begin(); it != labels.end(); ++it) {
                Label *l = *it;
                if (l->is_basic_block_like()) {
                    continue;
                }
                std::vector<Label *> scope;
                l->build_scope(um, scope);
                bool found = false;
                for (size_t i = 0; i < scope.size(); ++i) {
                    Label *subl = scope[i];
                    if (!subl->is_basic_block_like()) {
                        illegal.insert(subl);
                        found = true;
                    }
                }
                if (found) {
                    has_illegals.insert(l);
                }
            }

            for (auto it = illegal.begin(); it != illegal.end(); ++it) {
                Label *l = *it;
                // always process deepest illegal labels
                if (has_illegals.count(l))
                    continue;
#if SCOPES_DEBUG_CODEGEN
                ss_cout << "invalid: ";
                stream_label(ss_cout, l, StreamLabelFormat::debug_single());
#endif

                auto umit = um.label_map.find(l);
                if (umit != um.label_map.end()) {
                    auto users = umit->second;
                    // continuation must be eliminated
                    for (auto kv = users.begin(); kv != users.end(); ++kv) {
                        Label *user = *kv;
                        auto &&enter = user->body.enter;
                        auto &&args = user->body.args;
                        if ((enter.type == TYPE_Label) && (enter.label == l)) {
                            assert(!args.empty());

                            auto &&cont = args[0];
                            if ((cont.value.type == TYPE_Parameter)
                                && (cont.value.parameter->label == l)) {
#if SCOPES_DEBUG_CODEGEN
                                ss_cout << "skipping recursive call" << std::endl;
#endif
                            } else {
                                Args newargs = { cont };
                                for (size_t i = 1; i < l->params.size(); ++i) {
                                    newargs.push_back(untyped());
                                }
                                Label *newl = fold_type_label(um, l, newargs);

#if SCOPES_DEBUG_CODEGEN
                                ss_cout << l << "(" << cont.value << ") -> " << newl << std::endl;
#endif
                                user->remove_from_usermap(um);
                                cont = none;
                                enter = newl;
                                user->insert_into_usermap(um);
                                numchanges++;
                            }
                        } else {
#if SCOPES_DEBUG_CODEGEN
                            ss_cout << "warning: invalidated user encountered" << std::endl;
#endif
                        }
                    }
                }
            }

            if (!numchanges) {
                if (!has_illegals.empty() || !illegal.empty()) {
                    StyledStream ss(std::cerr);
                    ss << "could not eliminate closures:" << std::endl;
                    for (auto it = illegal.begin(); it != illegal.end(); ++it) {
                        stream_label(ss, *it, StreamLabelFormat::debug_single());
                    }
                    ss << "within these functions:" << std::endl;
                    for (auto it = has_illegals.begin(); it != has_illegals.end(); ++it) {
                        stream_label(ss, *it, StreamLabelFormat::debug_scope());
                        ss << "----" << std::endl;
                    }
                    location_error(String::from("closure elimination failed"));

                }
            }

        } while (numchanges);

#if SCOPES_DEBUG_CODEGEN
        ss_cout << "lowered to CFF in " << iterations << " steps" << std::endl;
#endif

        return entry;
    }

    std::unordered_map<const Type *, ffi_type *> ffi_types;

    ffi_type *new_type() {
        ffi_type *result = (ffi_type *)malloc(sizeof(ffi_type));
        memset(result, 0, sizeof(ffi_type));
        return result;
    }

    ffi_type *create_ffi_type(const Type *type) {
        if (type == TYPE_Void) return &ffi_type_void;
        if (type == TYPE_Nothing) return &ffi_type_void;

        switch(type->kind()) {
        case TK_Integer: {
            auto it = cast<IntegerType>(type);
            if (it->issigned) {
                switch (it->width) {
                case 8: return &ffi_type_sint8;
                case 16: return &ffi_type_sint16;
                case 32: return &ffi_type_sint32;
                case 64: return &ffi_type_sint64;
                default: break;
                }
            } else {
                switch (it->width) {
                case 1: return &ffi_type_uint8;
                case 8: return &ffi_type_uint8;
                case 16: return &ffi_type_uint16;
                case 32: return &ffi_type_uint32;
                case 64: return &ffi_type_uint64;
                default: break;
                }
            }
        } break;
        case TK_Real: {
            switch(cast<RealType>(type)->width) {
            case 32: return &ffi_type_float;
            case 64: return &ffi_type_double;
            default: break;
            }
        } break;
        case TK_Pointer: return &ffi_type_pointer;
        case TK_Typename: {
            return get_ffi_type(storage_type(type));
        } break;
        case TK_Array: {
            auto ai = cast<ArrayType>(type);
            size_t count = ai->count;
            ffi_type *ty = (ffi_type *)malloc(sizeof(ffi_type));
            ty->size = 0;
            ty->alignment = 0;
            ty->type = FFI_TYPE_STRUCT;
            ty->elements = (ffi_type **)malloc(sizeof(ffi_type*) * (count + 1));
            ffi_type *element_type = get_ffi_type(ai->element_type);
            for (size_t i = 0; i < count; ++i) {
                ty->elements[i] = element_type;
            }
            ty->elements[count] = nullptr;
            return ty;
        } break;
        case TK_Tuple: {
            auto ti = cast<TupleType>(type);
            size_t count = ti->types.size();
            ffi_type *ty = (ffi_type *)malloc(sizeof(ffi_type));
            ty->size = 0;
            ty->alignment = 0;
            ty->type = FFI_TYPE_STRUCT;
            ty->elements = (ffi_type **)malloc(sizeof(ffi_type*) * (count + 1));
            for (size_t i = 0; i < count; ++i) {
                ty->elements[i] = get_ffi_type(ti->types[i]);
            }
            ty->elements[count] = nullptr;
            return ty;
        } break;
        case TK_Union: {
            auto ui = cast<UnionType>(type);
            size_t count = ui->types.size();
            size_t sz = ui->size;
            size_t al = ui->align;
            ffi_type *ty = (ffi_type *)malloc(sizeof(ffi_type));
            ty->size = 0;
            ty->alignment = 0;
            ty->type = FFI_TYPE_STRUCT;
            // find member with the same alignment
            for (size_t i = 0; i < count; ++i) {
                const Type *ET = ui->types[i];
                size_t etal = align_of(ET);
                if (etal == al) {
                    size_t remsz = sz - size_of(ET);
                    ffi_type *tvalue = get_ffi_type(ET);
                    if (remsz) {
                        ty->elements = (ffi_type **)malloc(sizeof(ffi_type*) * 3);
                        ty->elements[0] = tvalue;
                        ty->elements[1] = get_ffi_type(Array(TYPE_I8, remsz));
                        ty->elements[2] = nullptr;
                    } else {
                        ty->elements = (ffi_type **)malloc(sizeof(ffi_type*) * 2);
                        ty->elements[0] = tvalue;
                        ty->elements[1] = nullptr;
                    }
                    return ty;
                }
            }
            // should never get here
            assert(false);
        } break;
        default: break;
        };

        StyledString ss;
        ss.out << "FFI: cannot convert argument of type " << type;
        location_error(ss.str());
        return nullptr;
    }

    ffi_type *get_ffi_type(const Type *type) {
        auto it = ffi_types.find(type);
        if (it == ffi_types.end()) {
            auto result = create_ffi_type(type);
            ffi_types[type] = result;
            return result;
        } else {
            return it->second;
        }
    }

    void verify_function_argument_count(const FunctionType *fi, size_t argcount) {

        size_t fargcount = fi->argument_types.size();
        if (fi->flags & FF_Variadic) {
            if (argcount < fargcount) {
                StyledString ss;
                ss.out << "argument count mismatch (need at least "
                    << fargcount << ", got " << argcount << ")";
                location_error(ss.str());
            }
        } else {
            if (argcount != fargcount) {
                StyledString ss;
                ss.out << "argument count mismatch (need "
                    << fargcount << ", got " << argcount << ")";
                location_error(ss.str());
            }
        }
    }

    Any run_ffi_function(Any enter, Argument *args, size_t argcount) {
        auto pi = cast<PointerType>(enter.type);
        auto fi = cast<FunctionType>(pi->element_type);

        size_t fargcount = fi->argument_types.size();

        const Type *rettype = cast<ReturnLabelType>(fi->return_type)->return_type;

        ffi_cif cif;
        ffi_type *argtypes[argcount];
        void *avalues[argcount];
        for (size_t i = 0; i < argcount; ++i) {
            Argument &arg = args[i];
            argtypes[i] = get_ffi_type(arg.value.type);
            avalues[i] = get_pointer(arg.value.type, arg.value);
        }
        ffi_status prep_result;
        if (fi->flags & FF_Variadic) {
            prep_result = ffi_prep_cif_var(
                &cif, FFI_DEFAULT_ABI, fargcount, argcount, get_ffi_type(rettype), argtypes);
        } else {
            prep_result = ffi_prep_cif(
                &cif, FFI_DEFAULT_ABI, argcount, get_ffi_type(rettype), argtypes);
        }
        assert(prep_result == FFI_OK);

        Any result = Any::from_pointer(rettype, nullptr);
        ffi_call(&cif, FFI_FN(enter.pointer),
            get_pointer(result.type, result, true), avalues);
        return result;
    }

};

std::vector<Label *> Solver::traceback;
int Solver::solve_refs = 0;
bool Solver::enable_step_debugger = false;

//------------------------------------------------------------------------------
// MACRO EXPANDER
//------------------------------------------------------------------------------
// expands macros and generates the IL

static bool verify_list_parameter_count(const List *expr, int mincount, int maxcount) {
    assert(expr != EOL);
    if ((mincount <= 0) && (maxcount == -1)) {
        return true;
    }
    int argcount = (int)expr->count - 1;

    if ((maxcount >= 0) && (argcount > maxcount)) {
        location_error(
            format("excess argument. At most %i arguments expected", maxcount));
        return false;
    }
    if ((mincount >= 0) && (argcount < mincount)) {
        location_error(
            format("at least %i arguments expected, got %i", mincount, argcount));
        return false;
    }
    return true;
}

static void verify_at_parameter_count(const List *topit, int mincount, int maxcount) {
    assert(topit != EOL);
    verify_list_parameter_count(unsyntax(topit->at), mincount, maxcount);
}

//------------------------------------------------------------------------------

static bool ends_with_parenthesis(Symbol sym) {
    if (sym == SYM_Parenthesis)
        return true;
    const String *str = sym.name();
    if (str->count < 3)
        return false;
    const char *dot = str->data + str->count - 3;
    return !strcmp(dot, "...");
}

struct Expander {
    Label *state;
    Scope *env;
    const List *next;
    static bool verbose;

    const Type *list_expander_func_type;

    Expander(Label *_state, Scope *_env, const List *_next = EOL) :
        state(_state),
        env(_env),
        next(_next),
        list_expander_func_type(nullptr) {
        list_expander_func_type = Pointer(Function(
            ReturnLabel({unknown_of(TYPE_List), unknown_of(TYPE_Scope)}),
            {TYPE_List, TYPE_Scope}), PTF_NonWritable, SYM_Unnamed);
    }

    ~Expander() {}

    bool is_goto_label(Any enter) {
        return (enter.type == TYPE_Label)
            && (enter.label->params[0]->type == TYPE_Nothing);
    }

    // arguments must include continuation
    // enter and args must be passed with syntax object removed
    void br(Any enter, const Args &args, uint64_t flags = 0) {
        assert(!args.empty());
        const Anchor *anchor = get_active_anchor();
        assert(anchor);
        if (!state) {
            set_active_anchor(anchor);
            location_error(String::from("can not define body: continuation already exited."));
            return;
        }
        assert(!is_goto_label(enter) || (args[0].value.type == TYPE_Nothing));
        assert(state->body.enter.type == TYPE_Nothing);
        assert(state->body.args.empty());
        state->body.flags = flags;
        state->body.enter = enter;
        state->body.args = args;
        state->body.anchor = anchor;
        state = nullptr;
    }

    bool is_parameter_or_label(Any val) {
        return (val.type == TYPE_Parameter) || (val.type == TYPE_Label);
    }
    bool is_parameter_or_label_or_none(Any val) {
        return is_parameter_or_label(val) || (val.type == TYPE_Nothing);
    }

    void verify_dest_not_none(Any dest) {
        if (dest.type == TYPE_Nothing) {
            location_error(String::from("attempting to implicitly return from label"));
        }
    }

    Any write_dest(const Any &dest) {
        if (dest.type == TYPE_Symbol) {
            return none;
        } else if (is_parameter_or_label_or_none(dest)) {
            if (last_expression()) {
                verify_dest_not_none(dest);
                br(dest, { none });
            }
            return none;
        } else {
            assert(false && "illegal dest type");
        }
        return none;
    }

    Any write_dest(const Any &dest, const Any &value) {
        if (dest.type == TYPE_Symbol) {
            return value;
        } else if (is_parameter_or_label_or_none(dest)) {
            if (last_expression()) {
                verify_dest_not_none(dest);
                br(dest, { none, value });
            }
            return value;
        } else {
            assert(false && "illegal dest type");
        }
        return none;
    }

    void expand_block(const List *it, const Any &dest) {
        assert(is_parameter_or_label_or_none(dest));
        if (it == EOL) {
            br(dest, { none });
        } else {
            while (it) {
                next = it->next;
                const Syntax *sx = it->at;
                Any expr = sx->datum;
                if (!last_expression() && (expr.type == TYPE_String)) {
                    env->set_doc(expr);
                }
                expand(it->at, dest);
                it = next;
            }
        }
    }

    Any expand_syntax_extend(const List *it, const Any &dest) {
        auto _anchor = get_active_anchor();

        verify_list_parameter_count(it, 1, -1);

        // skip head
        it = it->next;

        Label *func = Label::from(_anchor, Symbol(KW_SyntaxExtend));

        auto retparam = Parameter::from(_anchor, Symbol(SYM_Unnamed), TYPE_Unknown);
        auto scopeparam = Parameter::from(_anchor, Symbol(SYM_SyntaxScope), TYPE_Unknown);

        func->append(retparam);
        func->append(scopeparam);

        Scope *subenv = Scope::from(env);
        subenv->bind(Symbol(SYM_SyntaxScope), scopeparam);

        Expander subexpr(func, subenv);

        subexpr.expand_block(it, retparam);

        set_active_anchor(_anchor);

        Args args;
        args.reserve(4);
        Label *nextstate = nullptr;
        Any result = none;
        if (dest.type == TYPE_Symbol) {
            nextstate = Label::continuation_from(_anchor, Symbol(SYM_Unnamed));
            Parameter *param = Parameter::variadic_from(_anchor, Symbol(SYM_Unnamed), TYPE_Unknown);
            nextstate->append(param);
            args.push_back(nextstate);
            result = param;
        } else if (is_parameter_or_label_or_none(dest)) {
            args.push_back(dest);
        } else {
            assert(false && "syntax extend: illegal dest type");
        }
        args.push_back(func);
        args.push_back(Syntax::from(_anchor, next));
        args.push_back(env);
        //state = subexp.state;
        set_active_anchor(_anchor);
        br(Builtin(KW_SyntaxExtend), args);
        state = nextstate;
        next = EOL;
        return result;
    }

    Parameter *expand_parameter(Any value) {
        const Syntax *sxvalue = value;
        const Anchor *anchor = sxvalue->anchor;
        Any _value = sxvalue->datum;
        if (_value.type == TYPE_Parameter) {
            return _value.parameter;
        } else if (_value.type == TYPE_List && _value.list == EOL) {
            return Parameter::from(anchor, Symbol(SYM_Unnamed), TYPE_Nothing);
        } else {
            _value.verify(TYPE_Symbol);
            Parameter *param = nullptr;
            if (ends_with_parenthesis(_value.symbol)) {
                param = Parameter::variadic_from(anchor, _value.symbol, TYPE_Unknown);
            } else {
                param = Parameter::from(anchor, _value.symbol, TYPE_Unknown);
            }
            env->bind(_value.symbol, param);
            return param;
        }
    }

    Any expand_fn(const List *it, const Any &dest, bool label, bool impure) {
        auto _anchor = get_active_anchor();

        verify_list_parameter_count(it, 1, -1);

        // skip head
        it = it->next;

        assert(it != EOL);

        bool continuing = false;
        Label *func = nullptr;
        Any tryfunc_name = unsyntax(it->at);
        if (tryfunc_name.type == TYPE_Symbol) {
            // named self-binding
            // see if we can find a forward declaration in the local scope
            Any result = none;
            if (env->lookup_local(tryfunc_name.symbol, result)
                && (result.type == TYPE_Label)
                && !result.label->is_valid()) {
                func = result.label;
                continuing = true;
            } else {
                func = Label::from(_anchor, tryfunc_name.symbol);
                env->bind(tryfunc_name.symbol, func);
            }
            it = it->next;
        } else if (tryfunc_name.type == TYPE_String) {
            // named lambda
            func = Label::from(_anchor, Symbol(tryfunc_name.string));
            it = it->next;
        } else {
            // unnamed lambda
            func = Label::from(_anchor, Symbol(SYM_Unnamed));
        }
        if (impure)
            func->set_impure();

        Parameter *retparam = nullptr;
        if (continuing) {
            assert(!func->params.empty());
            retparam = func->params[0];
        } else {
            retparam = Parameter::from(_anchor, Symbol(SYM_Unnamed), label?TYPE_Nothing:TYPE_Unknown);
            func->append(retparam);
        }

        if (it == EOL) {
            // forward declaration
            if (tryfunc_name.type != TYPE_Symbol) {
                location_error(label?
                    String::from("forward declared label must be named")
                    :String::from("forward declared function must be named"));
            }

            return write_dest(dest);
        }

        const Syntax *sxplist = it->at;
        const List *params = sxplist->datum;

        it = it->next;

        Scope *subenv = Scope::from(env);
        // hidden self-binding for subsequent macros
        subenv->bind(SYM_ThisFnCC, func);
        Any subdest = none;
        if (!label) {
            subenv->bind(KW_Recur, func);
            subenv->bind(KW_Return, retparam);
            subdest = retparam;
        }
        // ensure the local scope does not contain special symbols
        subenv = Scope::from(subenv);

        Expander subexpr(func, subenv);

        while (params != EOL) {
            func->append(subexpr.expand_parameter(params->at));
            params = params->next;
        }

        if ((it != EOL) && (it->next != EOL)) {
            Any val = unsyntax(it->at);
            if (val.type == TYPE_String) {
                func->docstring = val.string;
                it = it->next;
            }
        }

        subexpr.expand_block(it, subdest);

        if (state) {
            func->body.scope_label = state;
        }

        set_active_anchor(_anchor);
        return write_dest(dest, func);
    }

    bool is_return_parameter(Any val) {
        return (val.type == TYPE_Parameter) && (val.parameter->index == 0);
    }

    bool last_expression() {
        return next == EOL;
    }

    Label *make_nextstate(const Any &dest, Any &result, Any &subdest) {
        auto _anchor = get_active_anchor();
        Label *nextstate = nullptr;
        subdest = dest;
        if (dest.type == TYPE_Symbol) {
            nextstate = Label::continuation_from(_anchor, Symbol(SYM_Unnamed));
            Parameter *param = Parameter::variadic_from(_anchor,
                Symbol(SYM_Unnamed), TYPE_Unknown);
            nextstate->append(param);
            if (state) {
                nextstate->body.scope_label = state;
            }
            subdest = nextstate;
            result = param;
        } else if (is_parameter_or_label_or_none(dest)) {
            if (dest.type == TYPE_Parameter) {
                assert(dest.parameter->type != TYPE_Nothing);
            }
            if (!last_expression()) {
                nextstate = Label::continuation_from(_anchor, Symbol(SYM_Unnamed));
                if (state) {
                    nextstate->body.scope_label = state;
                }
                subdest = nextstate;
            }
        } else {
            assert(false && "illegal dest type");
        }
        return nextstate;
    }

    Any expand_defer(const List *it, const Any &dest) {
        auto _anchor = get_active_anchor();

        it = it->next;
        const List *body = it;
        const List *block = next;
        next = EOL;

        Label *nextstate = Label::continuation_from(_anchor, Symbol(SYM_Unnamed));

        expand_block(block, nextstate);

        state = nextstate;
        // read parameter names
        it = unsyntax(it->at);
        while (it != EOL) {
            nextstate->append(expand_parameter(it->at));
            it = it->next;
        }
        return expand_do(body, dest, false);
    }

    Any expand_do(const List *it, const Any &dest, bool new_scope) {
        auto _anchor = get_active_anchor();

        it = it->next;

        Any result = none;
        Any subdest = none;
        Label *nextstate = make_nextstate(dest, result, subdest);

        Label *func = Label::continuation_from(_anchor, Symbol(SYM_Unnamed));
        Scope *subenv = env;
        if (new_scope) {
            subenv = Scope::from(env);
        }
        Expander subexpr(func, subenv);
        subexpr.expand_block(it, subdest);

        set_active_anchor(_anchor);
        br(func, { none });
        state = nextstate;
        return result;
    }

    bool is_equal_token(const Any &name) {
        return (name.type == TYPE_Symbol) && (name.symbol == OP_Set);
    }

    void print_name_suggestions(Symbol name, StyledStream &ss) {
        auto syms = env->find_closest_match(name);
        if (!syms.empty()) {
            ss << "Did you mean '" << syms[0].name()->data << "'";
            for (size_t i = 1; i < syms.size(); ++i) {
                if ((i + 1) == syms.size()) {
                    ss << " or ";
                } else {
                    ss << ", ";
                }
                ss << "'" << syms[i].name()->data << "'";
            }
            ss << "?";
        }
    }

    // (let x ... [= args ...])
    // (let name ([x ...]) [= args ...])
    // ...
    Any expand_let(const List *it, const Any &dest) {

        verify_list_parameter_count(it, 1, -1);
        it = it->next;

        auto _anchor = get_active_anchor();

        Symbol labelname = Symbol(SYM_Unnamed);
        const List *params = nullptr;
        const List *values = nullptr;

        if (it) {
            auto name = unsyntax(it->at);
            auto nextit = it->next;
            if ((name.type == TYPE_Symbol) && nextit) {
                auto val = unsyntax(nextit->at);
                if (val.type == TYPE_List) {
                    labelname = name.symbol;
                    params = val.list;
                    nextit = nextit->next;
                    it = params;
                    if (nextit != EOL) {
                        if (!is_equal_token(unsyntax(nextit->at))) {
                            location_error(String::from("equal sign (=) expected"));
                        }
                        values = nextit;
                    }
                }
            }
        }

        auto endit = EOL;
        if (!params) {
            endit = it;
            // read parameter names
            while (endit) {
                auto name = unsyntax(endit->at);
                if (is_equal_token(name))
                    break;
                endit = endit->next;
            }
            if (endit != EOL)
                values = endit;
        }

        Label *nextstate = nullptr;
        if (!values) {
            // no assignments, reimport parameter names into local scope
            if (labelname != SYM_Unnamed) {
                nextstate = Label::continuation_from(_anchor, labelname);
                env->bind(labelname, nextstate);
            }

            while (it != endit) {
                auto name = unsyntax(it->at);
                name.verify(TYPE_Symbol);
                AnyDoc entry = { none, nullptr };
                if (!env->lookup(name.symbol, entry)) {
                    StyledString ss;
                    ss.out << "no such name bound in parent scope: '"
                        << name.symbol.name()->data << "'. ";
                    print_name_suggestions(name.symbol, ss.out);
                    location_error(ss.str());
                }
                env->bind_with_doc(name.symbol, entry);
                it = it->next;
            }

            if (nextstate) {
                br(nextstate, { none });
                state = nextstate;
            }

            return write_dest(dest);
        }

        nextstate = Label::continuation_from(_anchor, labelname);
        if (state) {
            nextstate->body.scope_label = state;
        }
        if (labelname != SYM_Unnamed) {
            env->bind(labelname, nextstate);
        }

        size_t numparams = 0;
        // bind to fresh env so the rhs expressions don't see the symbols yet
        Scope *orig_env = env;
        env = Scope::from();
        // read parameter names
        while (it != endit) {
            nextstate->append(expand_parameter(it->at));
            numparams++;
            it = it->next;
        }

        if (nextstate->is_variadic()) {
            // accepts maximum number of arguments
            numparams = (size_t)-1;
        }

        it = values;

        Args args;
        args.reserve(it->count);
        args.push_back(none);

        it = it->next;

        // read init values
        Expander subexp(state, orig_env);
        size_t numvalues = 0;
        while (it) {
            numvalues++;
            if (numvalues > numparams) {
                set_active_anchor(((const Syntax *)it->at)->anchor);
                StyledString ss;
                ss.out << "number of arguments exceeds number of defined names ("
                    << numvalues << " > " << numparams << ")";
                location_error(ss.str());
            }
            subexp.next = it->next;
            args.push_back(subexp.expand(it->at, Symbol(SYM_Unnamed)));
            it = subexp.next;
        }

        //
        for (auto kv = env->map->begin(); kv != env->map->end(); ++kv) {
            orig_env->bind(kv->first, kv->second.value);
        }
        env = orig_env;

        set_active_anchor(_anchor);
        state = subexp.state;
        br(nextstate, args);
        state = nextstate;

        return write_dest(dest);
    }

    // quote <value> ...
    Any expand_quote(const List *it, const Any &dest) {
        //auto _anchor = get_active_anchor();

        verify_list_parameter_count(it, 1, -1);
        it = it->next;

        Any result = none;
        if (it->count == 1) {
            result = it->at;
        } else {
            result = it;
        }
        return write_dest(dest, strip_syntax(result));
    }

    Any expand_syntax_log(const List *it, const Any &dest) {
        //auto _anchor = get_active_anchor();

        verify_list_parameter_count(it, 1, 1);
        it = it->next;

        Any val = unsyntax(it->at);
        val.verify(TYPE_Symbol);

        auto sym = val.symbol;
        if (sym == KW_True) {
            this->verbose = true;
        } else if (sym == KW_False) {
            this->verbose = false;
        } else {
            // ignore
        }

        return write_dest(dest);
    }

    // (if cond body ...)
    // [(elseif cond body ...)]
    // [(else body ...)]
    Any expand_if(const List *it, const Any &dest) {
        auto _anchor = get_active_anchor();

        std::vector<const List *> branches;

    collect_branch:
        verify_list_parameter_count(it, 1, -1);
        branches.push_back(it);

        it = next;
        if (it != EOL) {
            auto itnext = it->next;
            const Syntax *sx = it->at;
            if (sx->datum.type == TYPE_List) {
                it = sx->datum;
                if (it != EOL) {
                    auto head = unsyntax(it->at);
                    if (head == Symbol(KW_ElseIf)) {
                        next = itnext;
                        goto collect_branch;
                    } else if (head == Symbol(KW_Else)) {
                        next = itnext;
                        branches.push_back(it);
                    } else {
                        branches.push_back(EOL);
                    }
                } else {
                    branches.push_back(EOL);
                }
            } else {
                branches.push_back(EOL);
            }
        } else {
            branches.push_back(EOL);
        }

        Any result = none;
        Any subdest = none;
        Label *nextstate = make_nextstate(dest, result, subdest);

        int lastidx = (int)branches.size() - 1;
        for (int idx = 0; idx < lastidx; ++idx) {
            it = branches[idx];
            it = it->next;

            Expander subexp(state, env);
            subexp.next = it->next;
            Any cond = subexp.expand(it->at, Symbol(SYM_Unnamed));
            it = subexp.next;

            Label *thenstate = Label::continuation_from(_anchor, Symbol(SYM_Unnamed));
            Label *elsestate = Label::continuation_from(_anchor, Symbol(SYM_Unnamed));

            set_active_anchor(_anchor);
            state = subexp.state;
            br(Builtin(FN_Branch), { none, cond, thenstate, elsestate });

            subexp.env = Scope::from(env);
            subexp.state = thenstate;
            subexp.expand_block(it, subdest);

            state = elsestate;
        }

        it = branches[lastidx];
        if (it != EOL) {
            it = it->next;
            Expander subexp(state, Scope::from(env));
            subexp.expand_block(it, subdest);
        } else {
            br(subdest, { none });
        }

        state = nextstate;
        return result;
    }

    static bool get_kwargs(Any it, Argument &value) {
        it = unsyntax(it);
        if (it.type != TYPE_List) return false;
        auto l = it.list;
        if (l == EOL) return false;
        if (l->count != 3) return false;
        it = unsyntax(l->at);
        if (it.type != TYPE_Symbol) return false;
        value.key = it.symbol;
        l = l->next;
        it = unsyntax(l->at);
        if (it.type != TYPE_Symbol) return false;
        if (it.symbol != OP_Set) return false;
        l = l->next;
        value.value = l->at;
        return true;
    }

    Any expand_call(const List *it, const Any &dest, bool rawcall = false) {
        if (it == EOL)
            return write_dest(dest, it);
        auto _anchor = get_active_anchor();
        Expander subexp(state, env, it->next);

        Args args;
        args.reserve(it->count);

        Any result = none;
        Any subdest = none;
        Label *nextstate = make_nextstate(dest, result, subdest);
        args.push_back(subdest);

        Any enter = subexp.expand(it->at, Symbol(SYM_Unnamed));
        if (is_return_parameter(enter)) {
            assert(enter.parameter->type != TYPE_Nothing);
            args[0] = none;
            if (!last_expression()) {
                location_error(
                    String::from("return call must be last in statement list"));
            }
        } else if (is_goto_label(enter)) {
            args[0] = none;
        }

        it = subexp.next;
        while (it) {
            subexp.next = it->next;
            Argument value;
            set_active_anchor(((const Syntax *)it->at)->anchor);
            if (get_kwargs(it->at, value)) {
                value.value = subexp.expand(
                    value.value, Symbol(SYM_Unnamed));
            } else {
                value = subexp.expand(it->at, Symbol(SYM_Unnamed));
            }
            args.push_back(value);
            it = subexp.next;
        }

        state = subexp.state;
        set_active_anchor(_anchor);
        br(enter, args, rawcall?LBF_RawCall:0);
        state = nextstate;
        return result;
    }

    Any expand(const Syntax *sx, const Any &dest) {
    expand_again:
        set_active_anchor(sx->anchor);
        if (sx->quoted) {
            if (verbose) {
                StyledStream ss(std::cerr);
                ss << "quoting ";
                stream_expr(ss, sx, StreamExprFormat::debug_digest());
            }
            // return as-is
            return write_dest(dest, sx->datum);
        }
        Any expr = sx->datum;
        if (expr.type == TYPE_List) {
            if (verbose) {
                StyledStream ss(std::cerr);
                ss << "expanding list ";
                stream_expr(ss, sx, StreamExprFormat::debug_digest());
            }

            const List *list = expr.list;
            if (list == EOL) {
                location_error(String::from("expression is empty"));
            }

            Any head = unsyntax(list->at);

            // resolve symbol
            if (head.type == TYPE_Symbol) {
                env->lookup(head.symbol, head);
            }

            if (head.type == TYPE_Builtin) {
                Builtin func = head.builtin;
                switch(func.value()) {
                case KW_SyntaxLog: return expand_syntax_log(list, dest);
                case KW_Fn: return expand_fn(list, dest, false, false);
                case KW_ImpureFn: return expand_fn(list, dest, false, true);
                case KW_Label: return expand_fn(list, dest, true, false);
                case KW_SyntaxExtend: return expand_syntax_extend(list, dest);
                case KW_Let: return expand_let(list, dest);
                case KW_If: return expand_if(list, dest);
                case KW_Quote: return expand_quote(list, dest);
                case KW_Defer: return expand_defer(list, dest);
                case KW_Do: return expand_do(list, dest, true);
                case KW_DoIn: return expand_do(list, dest, false);
                case KW_RawCall:
                case KW_Call: {
                    verify_list_parameter_count(list, 1, -1);
                    list = list->next;
                    assert(list != EOL);
                    return expand_call(list, dest, func.value() == KW_RawCall);
                } break;
                default: break;
                }
            }

            Any list_handler = none;
            if (env->lookup(Symbol(SYM_ListWildcard), list_handler)) {
                if (list_handler.type != list_expander_func_type) {
                    StyledString ss;
                    ss.out << "custom list expander has wrong type "
                        << list_handler.type << ", must be "
                        << list_expander_func_type;
                    location_error(ss.str());
                }
                struct ListScopePair { const List *topit; Scope *env; };
                typedef ListScopePair (*HandlerFuncType)(const List *, Scope *);
                HandlerFuncType f = (HandlerFuncType)list_handler.pointer;
                auto result = f(List::from(sx, next), env);
                const Syntax *newsx = result.topit->at;
                if (newsx != sx) {
                    sx = newsx;
                    next = result.topit->next;
                    env = result.env;
                    goto expand_again;
                } else if (verbose) {
                    StyledStream ss(std::cerr);
                    ss << "ignored by list handler" << std::endl;
                }
            }
            return expand_call(list, dest);
        } else if (expr.type == TYPE_Symbol) {
            if (verbose) {
                StyledStream ss(std::cerr);
                ss << "expanding symbol ";
                stream_expr(ss, sx, StreamExprFormat::debug_digest());
            }

            Symbol name = expr.symbol;

            Any result = none;
            if (!env->lookup(name, result)) {
                Any symbol_handler = none;
                if (env->lookup(Symbol(SYM_SymbolWildcard), symbol_handler)) {
                    if (symbol_handler.type != list_expander_func_type) {
                        StyledString ss;
                        ss.out << "custom symbol expander has wrong type "
                            << symbol_handler.type << ", must be "
                            << list_expander_func_type;
                        location_error(ss.str());
                    }
                    struct ListScopePair { const List *topit; Scope *env; };
                    typedef ListScopePair (*HandlerFuncType)(const List *, Scope *);
                    HandlerFuncType f = (HandlerFuncType)symbol_handler.pointer;
                    auto result = f(List::from(sx, next), env);
                    const Syntax *newsx = result.topit->at;
                    if (newsx != sx) {
                        sx = newsx;
                        next = result.topit->next;
                        env = result.env;
                        goto expand_again;
                    }
                }

                StyledString ss;
                ss.out << "use of undeclared identifier '" << name.name()->data << "'. ";
                print_name_suggestions(name, ss.out);
                location_error(ss.str());
            }
            return write_dest(dest, result);
        } else {
            if (verbose) {
                StyledStream ss(std::cerr);
                ss << "ignoring ";
                stream_expr(ss, sx, StreamExprFormat::debug_digest());
            }
            return write_dest(dest, expr);
        }
    }

};

bool Expander::verbose = false;

static Label *expand_module(Any expr, Scope *scope) {
    const Anchor *anchor = get_active_anchor();
    if (expr.type == TYPE_Syntax) {
        anchor = expr.syntax->anchor;
        set_active_anchor(anchor);
        expr = expr.syntax->datum;
    }
    expr.verify(TYPE_List);
    assert(anchor);
    Label *mainfunc = Label::function_from(anchor, anchor->path());

    Expander subexpr(mainfunc, scope?scope:globals);
    subexpr.expand_block(expr, mainfunc->params[0]);

    return mainfunc;
}

//------------------------------------------------------------------------------
// GLOBALS
//------------------------------------------------------------------------------

#define DEFINE_TYPENAME(NAME, T) \
    T = Typename(String::from(NAME));

#define DEFINE_BASIC_TYPE(NAME, CT, T, BODY) { \
        T = Typename(String::from(NAME)); \
        auto tn = cast<TypenameType>(const_cast<Type *>(T)); \
        tn->finalize(BODY); \
        assert(sizeof(CT) == size_of(T)); \
    }

#define DEFINE_STRUCT_TYPE(NAME, CT, T, ...) { \
        T = Typename(String::from(NAME)); \
        auto tn = cast<TypenameType>(const_cast<Type *>(T)); \
        tn->finalize(Tuple({ __VA_ARGS__ })); \
        assert(sizeof(CT) == size_of(T)); \
    }

#define DEFINE_STRUCT_HANDLE_TYPE(NAME, CT, T, ...) { \
        T = Typename(String::from(NAME)); \
        auto tn = cast<TypenameType>(const_cast<Type *>(T)); \
        auto ET = Tuple({ __VA_ARGS__ }); \
        assert(sizeof(CT) == size_of(ET)); \
        tn->finalize(NativeROPointer(ET)); \
    }

#define DEFINE_OPAQUE_HANDLE_TYPE(NAME, CT, T) { \
        T = Typename(String::from(NAME)); \
        auto tn = cast<TypenameType>(const_cast<Type *>(T)); \
        tn->finalize(NativeROPointer(Typename(String::from("_" NAME)))); \
    }

static void init_types() {
    DEFINE_TYPENAME("typename", TYPE_Typename);

    DEFINE_TYPENAME("void", TYPE_Void);
    DEFINE_TYPENAME("Nothing", TYPE_Nothing);

    DEFINE_TYPENAME("Sampler", TYPE_Sampler);

    DEFINE_TYPENAME("integer", TYPE_Integer);
    DEFINE_TYPENAME("real", TYPE_Real);
    DEFINE_TYPENAME("pointer", TYPE_Pointer);
    DEFINE_TYPENAME("array", TYPE_Array);
    DEFINE_TYPENAME("vector", TYPE_Vector);
    DEFINE_TYPENAME("tuple", TYPE_Tuple);
    DEFINE_TYPENAME("union", TYPE_Union);
    DEFINE_TYPENAME("ReturnLabel", TYPE_ReturnLabel);
    DEFINE_TYPENAME("constant", TYPE_Constant);
    DEFINE_TYPENAME("function", TYPE_Function);
    DEFINE_TYPENAME("extern", TYPE_Extern);
    DEFINE_TYPENAME("Image", TYPE_Image);
    DEFINE_TYPENAME("SampledImage", TYPE_SampledImage);
    DEFINE_TYPENAME("CStruct", TYPE_CStruct);
    DEFINE_TYPENAME("CUnion", TYPE_CUnion);
    DEFINE_TYPENAME("CEnum", TYPE_CEnum);

    TYPE_Bool = Integer(1, false);

    TYPE_I8 = Integer(8, true);
    TYPE_I16 = Integer(16, true);
    TYPE_I32 = Integer(32, true);
    TYPE_I64 = Integer(64, true);

    TYPE_U8 = Integer(8, false);
    TYPE_U16 = Integer(16, false);
    TYPE_U32 = Integer(32, false);
    TYPE_U64 = Integer(64, false);

    TYPE_F16 = Real(16);
    TYPE_F32 = Real(32);
    TYPE_F64 = Real(64);
    TYPE_F80 = Real(80);

    DEFINE_BASIC_TYPE("usize", size_t, TYPE_USize, TYPE_U64);

    TYPE_Type = Typename(String::from("type"));
    TYPE_Unknown = Typename(String::from("Unknown"));
    const Type *_TypePtr = NativeROPointer(Typename(String::from("_type")));
    cast<TypenameType>(const_cast<Type *>(TYPE_Type))->finalize(_TypePtr);
    cast<TypenameType>(const_cast<Type *>(TYPE_Unknown))->finalize(_TypePtr);

    cast<TypenameType>(const_cast<Type *>(TYPE_Nothing))->finalize(Tuple({}));

    DEFINE_BASIC_TYPE("Symbol", Symbol, TYPE_Symbol, TYPE_U64);
    DEFINE_BASIC_TYPE("Builtin", Builtin, TYPE_Builtin, TYPE_U64);

    DEFINE_STRUCT_TYPE("Any", Any, TYPE_Any,
        TYPE_Type,
        TYPE_U64
    );

    DEFINE_OPAQUE_HANDLE_TYPE("SourceFile", SourceFile, TYPE_SourceFile);
    DEFINE_OPAQUE_HANDLE_TYPE("Label", Label, TYPE_Label);
    DEFINE_OPAQUE_HANDLE_TYPE("Parameter", Parameter, TYPE_Parameter);
    DEFINE_OPAQUE_HANDLE_TYPE("Scope", Scope, TYPE_Scope);
    DEFINE_OPAQUE_HANDLE_TYPE("Frame", Frame, TYPE_Frame);
    DEFINE_OPAQUE_HANDLE_TYPE("Closure", Closure, TYPE_Closure);

    DEFINE_STRUCT_HANDLE_TYPE("Anchor", Anchor, TYPE_Anchor,
        NativeROPointer(TYPE_SourceFile),
        TYPE_I32,
        TYPE_I32,
        TYPE_I32
    );

    {
        TYPE_List = Typename(String::from("list"));

        const Type *cellT = Typename(String::from("_list"));
        auto tn = cast<TypenameType>(const_cast<Type *>(cellT));
        auto ET = Tuple({ TYPE_Any,
            NativeROPointer(cellT), TYPE_USize });
        assert(sizeof(List) == size_of(ET));
        tn->finalize(ET);

        cast<TypenameType>(const_cast<Type *>(TYPE_List))
            ->finalize(NativeROPointer(cellT));
    }

    DEFINE_STRUCT_HANDLE_TYPE("Syntax", Syntax, TYPE_Syntax,
        TYPE_Anchor,
        TYPE_Any,
        TYPE_Bool);

    DEFINE_STRUCT_HANDLE_TYPE("string", String, TYPE_String,
        TYPE_USize,
        Array(TYPE_I8, 1)
    );

    DEFINE_STRUCT_HANDLE_TYPE("Exception", Exception, TYPE_Exception,
        TYPE_Anchor,
        TYPE_String);

#define T(TYPE, TYPENAME) \
    assert(TYPE);
    B_TYPES()
#undef T
}

#undef DEFINE_TYPENAME
#undef DEFINE_BASIC_TYPE
#undef DEFINE_STRUCT_TYPE
#undef DEFINE_STRUCT_HANDLE_TYPE
#undef DEFINE_OPAQUE_HANDLE_TYPE
#undef DEFINE_STRUCT_TYPE

typedef struct { int x,y; } I2;
typedef struct { int x,y,z; } I3;

static const String *f_repr(Any value) {
    StyledString ss;
    value.stream(ss.out, false);
    return ss.str();
}

static const String *f_any_string(Any value) {
    auto ss = StyledString::plain();
    ss.out << value;
    return ss.str();
}

static void f_write(const String *value) {
    fputs(value->data, stdout);
}

static Scope *f_import_c(const String *path,
    const String *content, const List *arglist) {
    std::vector<std::string> args;
    while (arglist) {
        auto &&at = arglist->at;
        if (at.type == TYPE_String) {
            args.push_back(at.string->data);
        }
        arglist = arglist->next;
    }
    return import_c_module(path->data, args, content->data);
}

static void f_dump_label(Label *label) {
    StyledStream ss(std::cerr);
    stream_label(ss, label, StreamLabelFormat::debug_all());
}

static void f_dump_frame(Frame *frame) {
    StyledStream ss(std::cerr);
    stream_frame(ss, frame, StreamFrameFormat::single());
}

static const List *f_dump_list(const List *l) {
    StyledStream ss(std::cerr);
    stream_expr(ss, l, StreamExprFormat());
    return l;
}

typedef struct { Any result; bool ok; } AnyBoolPair;
static AnyBoolPair f_scope_at(Scope *scope, Symbol key) {
    Any result = none;
    bool ok = scope->lookup(key, result);
    return { result, ok };
}

static AnyBoolPair f_scope_local_at(Scope *scope, Symbol key) {
    Any result = none;
    bool ok = scope->lookup_local(key, result);
    return { result, ok };
}

static AnyBoolPair f_type_at(const Type *T, Symbol key) {
    Any result = none;
    bool ok = T->lookup(key, result);
    return { result, ok };
}

static const String *f_scope_docstring(Scope *scope, Symbol key) {
    if (key == SYM_Unnamed) {
        if (scope->doc) return scope->doc;
    } else {
        AnyDoc entry = { none, nullptr };
        if (scope->lookup(key, entry) && entry.doc) {
            return entry.doc;
        }
    }
    return Symbol(SYM_Unnamed).name();
}

static void f_scope_set_docstring(Scope *scope, Symbol key, const String *str) {
    if (key == SYM_Unnamed) {
        scope->doc = str;
    } else {
        AnyDoc entry = { none, nullptr };
        if (!scope->lookup_local(key, entry)) {
            location_error(
                String::from("attempting to set a docstring for a non-local name"));
        }
        entry.doc = str;
        scope->bind_with_doc(key, entry);
    }
}

static Symbol f_symbol_new(const String *str) {
    return Symbol(str);
}

static const String *f_string_join(const String *a, const String *b) {
    return String::join(a,b);
}

static size_t f_sizeof(const Type *T) {
    return size_of(T);
}

static size_t f_alignof(const Type *T) {
    return align_of(T);
}

int f_type_countof(const Type *T) {
    T = storage_type(T);
    switch(T->kind()) {
    case TK_Pointer:
    case TK_Extern:
    case TK_Image:
    case TK_SampledImage:
        return 1;
    case TK_Array: return cast<ArrayType>(T)->count;
    case TK_Vector: return cast<VectorType>(T)->count;
    case TK_Tuple: return cast<TupleType>(T)->types.size();
    case TK_Union: return cast<UnionType>(T)->types.size();
    case TK_Function:  return cast<FunctionType>(T)->argument_types.size() + 1;
    default:  break;
    }
    return 0;
}

static const Type *f_elementtype(const Type *T, int i) {
    T = storage_type(T);
    switch(T->kind()) {
    case TK_Pointer: return cast<PointerType>(T)->element_type;
    case TK_Array: return cast<ArrayType>(T)->element_type;
    case TK_Vector: return cast<VectorType>(T)->element_type;
    case TK_Tuple: return cast<TupleType>(T)->type_at_index(i);
    case TK_Union: return cast<UnionType>(T)->type_at_index(i);
    case TK_Function: return cast<FunctionType>(T)->type_at_index(i);
    case TK_Extern: return cast<ExternType>(T)->pointer_type;
    case TK_Image: return cast<ImageType>(T)->type;
    case TK_SampledImage: return cast<SampledImageType>(T)->type;
    default: {
        StyledString ss;
        ss.out << "storage type " << T << " has no elements" << std::endl;
        location_error(ss.str());
    } break;
    }
    return nullptr;
}

static int f_elementindex(const Type *T, Symbol name) {
    T = storage_type(T);
    switch(T->kind()) {
    case TK_Tuple: return cast<TupleType>(T)->field_index(name);
    case TK_Union: return cast<UnionType>(T)->field_index(name);
    default: {
        StyledString ss;
        ss.out << "storage type " << T << " has no named elements" << std::endl;
        location_error(ss.str());
    } break;
    }
    return -1;
}

static Symbol f_elementname(const Type *T, int index) {
    T = storage_type(T);
    switch(T->kind()) {
    case TK_Tuple: return cast<TupleType>(T)->field_name(index);
    case TK_Union: return cast<UnionType>(T)->field_name(index);
    default: {
        StyledString ss;
        ss.out << "storage type " << T << " has no named elements" << std::endl;
        location_error(ss.str());
    } break;
    }
    return SYM_Unnamed;
}

static const Type *f_pointertype(const Type *T, uint64_t flags, Symbol storage_class) {
    return Pointer(T, flags, storage_class);
}

static uint64_t f_pointer_type_flags(const Type *T) {
    verify_kind<TK_Pointer>(T);
    return cast<PointerType>(T)->flags;
}

static const Type *f_pointer_type_set_flags(const Type *T, uint64_t flags) {
    verify_kind<TK_Pointer>(T);
    auto pt = cast<PointerType>(T);
    return Pointer(pt->element_type, flags, pt->storage_class);
}

static const Symbol f_pointer_type_storage_class(const Type *T) {
    verify_kind<TK_Pointer>(T);
    return cast<PointerType>(T)->storage_class;
}

static int32_t f_extern_type_location(const Type *T) {
    T = storage_type(T);
    verify_kind<TK_Extern>(T);
    return cast<ExternType>(T)->location;
}

static int32_t f_extern_type_binding(const Type *T) {
    T = storage_type(T);
    verify_kind<TK_Extern>(T);
    return cast<ExternType>(T)->binding;
}

static const Type *f_pointer_type_set_storage_class(const Type *T, Symbol storage_class) {
    verify_kind<TK_Pointer>(T);
    auto pt = cast<PointerType>(T);
    return Pointer(pt->element_type, pt->flags, storage_class);
}

static const Type *f_pointer_type_set_element_type(const Type *T, const Type *ET) {
    verify_kind<TK_Pointer>(T);
    auto pt = cast<PointerType>(T);
    return Pointer(ET, pt->flags, pt->storage_class);
}

static const List *f_list_cons(Any at, const List *next) {
    return List::from(at, next);
}

static int32_t f_type_kind(const Type *T) {
    return T->kind();
}

static void f_type_debug_abi(const Type *T) {
    ABIClass classes[MAX_ABI_CLASSES];
    size_t sz = abi_classify(T, classes);
    StyledStream ss(std::cout);
    ss << T << " -> " << sz;
    for (size_t i = 0; i < sz; ++i) {
        ss << " " << abi_class_to_string(classes[i]);
    }
    ss << std::endl;
}

static int32_t f_bitcountof(const Type *T) {
    T = storage_type(T);
    switch(T->kind()) {
    case TK_Integer:
        return cast<IntegerType>(T)->width;
    case TK_Real:
        return cast<RealType>(T)->width;
    default: {
        StyledString ss;
        ss.out << "type " << T << " has no bitcount" << std::endl;
        location_error(ss.str());
    } break;
    }
    return 0;
}

static bool f_issigned(const Type *T) {
    T = storage_type(T);
    verify_kind<TK_Integer>(T);
    return cast<IntegerType>(T)->issigned;
}

static const Type *f_type_storage(const Type *T) {
    return storage_type(T);
}

static void f_error(const String *msg) {
    const Exception *exc = new Exception(nullptr, msg);
    error(exc);
}

static void f_anchor_error(const String *msg) {
    location_error(msg);
}

static void f_raise(Any value) {
    error(value);
}

static void f_set_anchor(const Anchor *anchor) {
    set_active_anchor(anchor);
}

static const Type *f_integer_type(int width, bool issigned) {
    return Integer(width, issigned);
}

static const Type *f_typename_type(const String *str) {
    return Typename(str);
}

static I3 f_compiler_version() {
    return {
        SCOPES_VERSION_MAJOR,
        SCOPES_VERSION_MINOR,
        SCOPES_VERSION_PATCH };
}

static const Syntax *f_syntax_new(const Anchor *anchor, Any value, bool quoted) {
    return Syntax::from(anchor, value, quoted);
}

static Parameter *f_parameter_new(const Anchor *anchor, Symbol symbol, const Type *type) {
    if (ends_with_parenthesis(symbol)) {
        return Parameter::variadic_from(anchor, symbol, type);
    } else {
        return Parameter::from(anchor, symbol, type);
    }
}

static const String *f_realpath(const String *path) {
    char buf[PATH_MAX];
    auto result = realpath(path->data, buf);
    if (!result) {
        return Symbol(SYM_Unnamed).name();
    } else {
        return String::from_cstr(result);
    }
}

static const String *f_dirname(const String *path) {
    auto pathcopy = strdup(path->data);
    auto result = String::from_cstr(dirname(pathcopy));
    free(pathcopy);
    return result;
}

static const String *f_basename(const String *path) {
    auto pathcopy = strdup(path->data);
    auto result = String::from_cstr(basename(pathcopy));
    free(pathcopy);
    return result;
}

static int f_parameter_index(const Parameter *param) {
    return param->index;
}

static Symbol f_parameter_name(const Parameter *param) {
    return param->name;
}

static const String *f_string_new(const char *ptr, size_t count) {
    return String::from(ptr, count);
}

static bool f_is_file(const String *path) {
    struct stat s;
    if( stat(path->data,&s) == 0 ) {
        if( s.st_mode & S_IFDIR ) {
        } else if ( s.st_mode & S_IFREG ) {
            return true;
        }
    }
    return false;
}

static bool f_is_directory(const String *path) {
    struct stat s;
    if( stat(path->data,&s) == 0 ) {
        if( s.st_mode & S_IFDIR ) {
            return true;
        }
    }
    return false;
}

static const Syntax *f_list_load(const String *path) {
    auto sf = SourceFile::from_file(path);
    if (!sf) {
        StyledString ss;
        ss.out << "no such file: " << path;
        location_error(ss.str());
    }
    LexerParser parser(sf);
    return parser.parse();
}

static const Syntax *f_list_parse(const String *str) {
    auto sf = SourceFile::from_string(Symbol("<string>"), str);
    assert(sf);
    LexerParser parser(sf);
    return parser.parse();
}

static Scope *f_scope_new() {
    return Scope::from();
}
static Scope *f_scope_clone(Scope *clone) {
    return Scope::from(nullptr, clone);
}
static Scope *f_scope_new_subscope(Scope *scope) {
    return Scope::from(scope);
}
static Scope *f_scope_clone_subscope(Scope *scope, Scope *clone) {
    return Scope::from(scope, clone);
}

static Scope *f_scope_parent(Scope *scope) {
    return scope->parent;
}

static Scope *f_globals() {
    return globals;
}

static void f_set_globals(Scope *s) {
    globals = s;
}

static Label *f_eval(const Syntax *expr, Scope *scope) {
    Solver solver;
    return solver.solve(typify_single(nullptr, expand_module(expr, scope), {}));
}

static void f_set_scope_symbol(Scope *scope, Symbol sym, Any value) {
    scope->bind(sym, value);
}

static void f_del_scope_symbol(Scope *scope, Symbol sym) {
    scope->del(sym);
}

static Label *f_typify(Closure *srcl, int numtypes, const Type **typeargs) {
    std::vector<const Type *> types;
    for (int i = 0; i < numtypes; ++i) {
        types.push_back(typeargs[i]);

    }
    Solver solver;
    return solver.solve(typify_single(srcl->frame, srcl->label, types));
}

static Any f_compile(Label *srcl, uint64_t flags) {
    return compile(srcl, flags);
}

static const String *f_compile_spirv(Symbol target, Label *srcl, uint64_t flags) {
    return compile_spirv(target, srcl, flags);
}

static const String *f_compile_glsl(Symbol target, Label *srcl, uint64_t flags) {
    return compile_glsl(target, srcl, flags);
}

void f_compile_object(const String *path, Scope *table, uint64_t flags) {
    compile_object(path, table, flags);
}

static const Type *f_array_type(const Type *element_type, size_t count) {
    return Array(element_type, count);
}

static const Type *f_vector_type(const Type *element_type, size_t count) {
    return Vector(element_type, count);
}

static const String *f_default_styler(Symbol style, const String *str) {
    StyledString ss;
    if (!style.is_known()) {
        location_error(String::from("illegal style"));
    }
    ss.out << Style(style.known_value()) << str->data << Style_None;
    return ss.str();
}

typedef struct { const String *_0; bool _1; } StringBoolPair;
static StringBoolPair f_prompt(const String *s, const String *pre) {
    if (pre->count) {
        linenoisePreloadBuffer(pre->data);
    }
    char *r = linenoise(s->data);
    if (!r) {
        return { Symbol(SYM_Unnamed).name(), false };
    }
    linenoiseHistoryAdd(r);
    return { String::from_cstr(r), true };
}

static const Scope *autocomplete_scope = nullptr;
static void f_set_autocomplete_scope(const Scope* scope) {
    autocomplete_scope = scope;
}
static void prompt_completion_cb(const char *buf, linenoiseCompletions *lc) {
    // Tab on an empty string gives an indentation
    if (*buf == 0) {
        linenoiseAddCompletion(lc, "    ");
        return;
    }

    const String* name = String::from_cstr(buf);
    Symbol sym(name);
    const Scope *scope = autocomplete_scope ? autocomplete_scope : globals;
    for (const auto& m : scope->find_elongations(sym))
        linenoiseAddCompletion(lc, m.name()->data);
}

static const String *f_format_message(const Anchor *anchor, const String *message) {
    StyledString ss;
    if (anchor) {
        ss.out << anchor << " ";
    }
    ss.out << message->data << std::endl;
    if (anchor) {
        anchor->stream_source_line(ss.out);
    }
    return ss.str();
}

static const String *f_symbol_to_string(Symbol sym) {
    return sym.name();
}

static void f_set_signal_abort(bool value) {
    signal_abort = value;
}

ExceptionPad *f_set_exception_pad(ExceptionPad *pad) {
    ExceptionPad *last_exc_pad = _exc_pad;
    _exc_pad = pad;
    return last_exc_pad;
}

Any f_exception_value(ExceptionPad *pad) {
    return pad->value;
}

static bool f_any_eq(Any a, Any b) {
    return a == b;
}

static const List *f_list_join(List *a, List *b) {
    return List::join(a, b);
}

typedef struct { Any _0; Any _1; } AnyAnyPair;
typedef struct { Symbol _0; Any _1; } SymbolAnyPair;
static SymbolAnyPair f_scope_next(Scope *scope, Symbol key) {
    auto &&map = *scope->map;
    Scope::Map::const_iterator it;
    if (key == SYM_Unnamed) {
        it = map.begin();
    } else {
        it = map.find(key);
        if (it != map.end()) it++;
    }
    while (it != map.end()) {
        if (is_typed(it->second.value)) {
            return { it->first, it->second.value };
        }
        it++;
    }
    return { SYM_Unnamed, none };
}

static SymbolAnyPair f_type_next(const Type *type, Symbol key) {
    auto &&map = type->get_symbols();
    Type::Map::const_iterator it;
    if (key == SYM_Unnamed) {
        it = map.begin();
    } else {
        it = map.find(key);
        if (it != map.end()) it++;
    }
    if (it != map.end()) {
        return { it->first, it->second };
    }
    return { SYM_Unnamed, none };
}

static std::unordered_map<const String *, regexp::Reprog *> pattern_cache;
static bool f_string_match(const String *pattern, const String *text) {
    auto it = pattern_cache.find(pattern);
    regexp::Reprog *m = nullptr;
    if (it == pattern_cache.end()) {
        const char *error = nullptr;
        m = regexp::regcomp(pattern->data, 0, &error);
        if (error) {
            const String *err = String::from_cstr(error);
            regexp::regfree(m);
            location_error(err);
        }
        pattern_cache.insert({ pattern, m });
    } else {
        m = it->second;
    }
    return (regexp::regexec(m, text->data, nullptr, 0) == 0);
}

static void f_load_library(const String *name) {
#ifdef SCOPES_WIN32
    // try to load library through regular interface first
    dlerror();
    void *handle = dlopen(name->data, RTLD_LAZY);
    if (!handle) {
        StyledString ss;
        ss.out << "error loading library " << name;
        char *err = dlerror();
        if (err) {
            ss.out << ": " << err;
        }
        location_error(ss.str());
    }
#endif
    if (LLVMLoadLibraryPermanently(name->data)) {
        StyledString ss;
        ss.out << "error loading library " << name;
        location_error(ss.str());
    }
}

static const String *f_type_name(const Type *T) {
    return T->name();
}

static bool f_function_type_is_variadic(const Type *T) {
    verify_kind<TK_Function>(T);
    auto ft = cast<FunctionType>(T);
    return ft->flags & FF_Variadic;
}

static void f_set_typename_super(const Type *T, const Type *ST) {
    verify_kind<TK_Typename>(T);
    verify_kind<TK_Typename>(ST);
    // if T <=: ST, the operation is illegal
    const Type *S = ST;
    while (S) {
        if (S == T) {
            StyledString ss;
            ss.out << "typename " << ST << " can not be a supertype of " << T;
            location_error(ss.str());
        }
        if (S == TYPE_Typename)
            break;
        S = superof(S);
    }
    auto tn = cast<TypenameType>(T);
    const_cast<TypenameType *>(tn)->super_type = ST;
}

static const Anchor *f_label_anchor(Label *label) {
    return label->anchor;
}

static Symbol f_label_name(Label *label) {
    return label->name;
}

static size_t f_label_parameter_count(Label *label) {
    return label->params.size();
}

static Parameter *f_label_parameter(Label *label, size_t index) {
    verify_range(index, label->params.size());
    return label->params[index];
}

static Label *f_closure_label(const Closure *closure) {
    return closure->label;
}

static Frame *f_closure_frame(const Closure *closure) {
    return closure->frame;
}

size_t f_label_countof_reachable(Label *label) {
    std::unordered_set<Label *> labels;
    label->build_reachable(labels);
    return labels.size();
}

static void f_enter_solver_cli () {
    Solver::enable_step_debugger = true;
}

static const String *f_label_docstring(Label *label) {
    if (label->docstring) {
        return label->docstring;
    } else {
        return Symbol(SYM_Unnamed).name();
    }
}

static size_t f_verify_stack () {
    size_t ssz = memory_stack_size();
    if (ssz >= SCOPES_MAX_STACK_SIZE) {
        location_error(String::from("verify-stack!: stack overflow"));
    }
    return ssz;
}

static uint64_t f_hash (uint64_t data, size_t size) {
    return CityHash64((const char *)&data, (size > 8)?8:size);
}

static uint64_t f_hash2x64(uint64_t a, uint64_t b) {
    return HashLen16(a, b);
}

static uint64_t f_hashbytes (const char *data, size_t size) {
    return CityHash64(data, size);
}

static void init_globals(int argc, char *argv[]) {

#define DEFINE_C_FUNCTION(SYMBOL, FUNC, RETTYPE, ...) \
    globals->bind(SYMBOL, \
        Any::from_pointer(Pointer(Function(RETTYPE, { __VA_ARGS__ }), \
            PTF_NonWritable, SYM_Unnamed), (void *)FUNC));
#define DEFINE_C_VARARG_FUNCTION(SYMBOL, FUNC, RETTYPE, ...) \
    globals->bind(SYMBOL, \
        Any::from_pointer(Pointer(Function(RETTYPE, { __VA_ARGS__ }, FF_Variadic), \
            PTF_NonWritable, SYM_Unnamed), (void *)FUNC));
#define DEFINE_PURE_C_FUNCTION(SYMBOL, FUNC, RETTYPE, ...) \
    globals->bind(SYMBOL, \
        Any::from_pointer(Pointer(Function(RETTYPE, { __VA_ARGS__ }, FF_Pure), \
            PTF_NonWritable, SYM_Unnamed), (void *)FUNC));

    //const Type *rawstring = Pointer(TYPE_I8);

    DEFINE_PURE_C_FUNCTION(FN_ImportC, f_import_c, TYPE_Scope, TYPE_String, TYPE_String, TYPE_List);
    DEFINE_PURE_C_FUNCTION(FN_ScopeAt, f_scope_at, Tuple({TYPE_Any,TYPE_Bool}), TYPE_Scope, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_ScopeLocalAt, f_scope_local_at, Tuple({TYPE_Any,TYPE_Bool}), TYPE_Scope, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_ScopeDocString, f_scope_docstring, TYPE_String, TYPE_Scope, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_RuntimeTypeAt, f_type_at, Tuple({TYPE_Any,TYPE_Bool}), TYPE_Type, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_SymbolNew, f_symbol_new, TYPE_Symbol, TYPE_String);
    DEFINE_PURE_C_FUNCTION(FN_Repr, f_repr, TYPE_String, TYPE_Any);
    DEFINE_PURE_C_FUNCTION(FN_AnyString, f_any_string, TYPE_String, TYPE_Any);
    DEFINE_PURE_C_FUNCTION(FN_StringJoin, f_string_join, TYPE_String, TYPE_String, TYPE_String);
    DEFINE_PURE_C_FUNCTION(FN_ElementType, f_elementtype, TYPE_Type, TYPE_Type, TYPE_I32);
    DEFINE_PURE_C_FUNCTION(FN_ElementIndex, f_elementindex, TYPE_I32, TYPE_Type, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_ElementName, f_elementname, TYPE_Symbol, TYPE_Type, TYPE_I32);
    DEFINE_PURE_C_FUNCTION(FN_SizeOf, f_sizeof, TYPE_USize, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_Alignof, f_alignof, TYPE_USize, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_PointerType, f_pointertype, TYPE_Type, TYPE_Type, TYPE_U64, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_PointerFlags, f_pointer_type_flags, TYPE_U64, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_PointerSetFlags, f_pointer_type_set_flags, TYPE_Type, TYPE_Type, TYPE_U64);
    DEFINE_PURE_C_FUNCTION(FN_PointerStorageClass, f_pointer_type_storage_class, TYPE_Symbol, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_PointerSetStorageClass, f_pointer_type_set_storage_class, TYPE_Type, TYPE_Type, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_PointerSetElementType, f_pointer_type_set_element_type, TYPE_Type, TYPE_Type, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_ExternLocation, f_extern_type_location, TYPE_I32, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_ExternBinding, f_extern_type_binding, TYPE_I32, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_ListCons, f_list_cons, TYPE_List, TYPE_Any, TYPE_List);
    DEFINE_PURE_C_FUNCTION(FN_TypeKind, f_type_kind, TYPE_I32, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_TypeDebugABI, f_type_debug_abi, TYPE_Void, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_BitCountOf, f_bitcountof, TYPE_I32, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_IsSigned, f_issigned, TYPE_Bool, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_TypeStorage, f_type_storage, TYPE_Type, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_IsOpaque, is_opaque, TYPE_Bool, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_IntegerType, f_integer_type, TYPE_Type, TYPE_I32, TYPE_Bool);
    DEFINE_PURE_C_FUNCTION(FN_CompilerVersion, f_compiler_version, Tuple({TYPE_I32, TYPE_I32, TYPE_I32}));
    DEFINE_PURE_C_FUNCTION(FN_TypeName, f_type_name, TYPE_String, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_TypenameType, f_typename_type, TYPE_Type, TYPE_String);
    DEFINE_PURE_C_FUNCTION(FN_SyntaxNew, f_syntax_new, TYPE_Syntax, TYPE_Anchor, TYPE_Any, TYPE_Bool);
    DEFINE_PURE_C_FUNCTION(FN_SyntaxWrap, wrap_syntax, TYPE_Any, TYPE_Anchor, TYPE_Any, TYPE_Bool);
    DEFINE_PURE_C_FUNCTION(FN_SyntaxStrip, strip_syntax, TYPE_Any, TYPE_Any);
    DEFINE_PURE_C_FUNCTION(FN_ParameterNew, f_parameter_new, TYPE_Parameter, TYPE_Anchor, TYPE_Symbol, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_ParameterIndex, f_parameter_index, TYPE_I32, TYPE_Parameter);
    DEFINE_PURE_C_FUNCTION(FN_ParameterName, f_parameter_name, TYPE_Symbol, TYPE_Parameter);
    DEFINE_PURE_C_FUNCTION(FN_StringNew, f_string_new, TYPE_String, NativeROPointer(TYPE_I8), TYPE_USize);
    DEFINE_PURE_C_FUNCTION(FN_DumpLabel, f_dump_label, TYPE_Void, TYPE_Label);
    DEFINE_PURE_C_FUNCTION(FN_DumpList, f_dump_list, TYPE_List, TYPE_List);
    DEFINE_PURE_C_FUNCTION(FN_DumpFrame, f_dump_frame, TYPE_Void, TYPE_Frame);
    DEFINE_PURE_C_FUNCTION(FN_Eval, f_eval, TYPE_Label, TYPE_Syntax, TYPE_Scope);
    DEFINE_PURE_C_FUNCTION(FN_Typify, f_typify, TYPE_Label, TYPE_Closure, TYPE_I32, NativeROPointer(TYPE_Type));
    DEFINE_PURE_C_FUNCTION(FN_ArrayType, f_array_type, TYPE_Type, TYPE_Type, TYPE_USize);
    DEFINE_PURE_C_FUNCTION(FN_ImageType, Image, TYPE_Type,
        TYPE_Type, TYPE_Symbol, TYPE_I32, TYPE_I32, TYPE_I32, TYPE_I32, TYPE_Symbol, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_SampledImageType, SampledImage, TYPE_Type, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_VectorType, f_vector_type, TYPE_Type, TYPE_Type, TYPE_USize);
    DEFINE_PURE_C_FUNCTION(FN_TypeCountOf, f_type_countof, TYPE_I32, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_SymbolToString, f_symbol_to_string, TYPE_String, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(Symbol("Any=="), f_any_eq, TYPE_Bool, TYPE_Any, TYPE_Any);
    DEFINE_PURE_C_FUNCTION(FN_ListJoin, f_list_join, TYPE_List, TYPE_List, TYPE_List);
    DEFINE_PURE_C_FUNCTION(FN_ScopeNext, f_scope_next, Tuple({TYPE_Symbol, TYPE_Any}), TYPE_Scope, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_TypeNext, f_type_next, Tuple({TYPE_Symbol, TYPE_Any}), TYPE_Type, TYPE_Symbol);
    DEFINE_PURE_C_FUNCTION(FN_StringMatch, f_string_match, TYPE_Bool, TYPE_String, TYPE_String);
    DEFINE_PURE_C_FUNCTION(SFXFN_SetTypenameSuper, f_set_typename_super, TYPE_Void, TYPE_Type, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_SuperOf, superof, TYPE_Type, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_FunctionTypeIsVariadic, f_function_type_is_variadic, TYPE_Bool, TYPE_Type);
    DEFINE_PURE_C_FUNCTION(FN_LabelAnchor, f_label_anchor, TYPE_Anchor, TYPE_Label);
    DEFINE_PURE_C_FUNCTION(FN_LabelParameterCount, f_label_parameter_count, TYPE_USize, TYPE_Label);
    DEFINE_PURE_C_FUNCTION(FN_LabelParameter, f_label_parameter, TYPE_Parameter, TYPE_Label, TYPE_USize);
    DEFINE_PURE_C_FUNCTION(FN_LabelName, f_label_name, TYPE_Symbol, TYPE_Label);
    DEFINE_PURE_C_FUNCTION(FN_ClosureLabel, f_closure_label, TYPE_Label, TYPE_Closure);
    DEFINE_PURE_C_FUNCTION(FN_ClosureFrame, f_closure_frame, TYPE_Frame, TYPE_Closure);
    DEFINE_PURE_C_FUNCTION(FN_LabelCountOfReachable, f_label_countof_reachable, TYPE_USize, TYPE_Label);
    DEFINE_PURE_C_FUNCTION(FN_EnterSolverCLI, f_enter_solver_cli, TYPE_Void);
    DEFINE_PURE_C_FUNCTION(FN_LabelDocString, f_label_docstring, TYPE_String, TYPE_Label);

    DEFINE_PURE_C_FUNCTION(FN_DefaultStyler, f_default_styler, TYPE_String, TYPE_Symbol, TYPE_String);

    DEFINE_C_FUNCTION(FN_Compile, f_compile, TYPE_Any, TYPE_Label, TYPE_U64);
    DEFINE_PURE_C_FUNCTION(FN_CompileSPIRV, f_compile_spirv, TYPE_String, TYPE_Symbol, TYPE_Label, TYPE_U64);
    DEFINE_PURE_C_FUNCTION(FN_CompileGLSL, f_compile_glsl, TYPE_String, TYPE_Symbol, TYPE_Label, TYPE_U64);
    DEFINE_PURE_C_FUNCTION(FN_CompileObject, f_compile_object, TYPE_Void, TYPE_String, TYPE_Scope, TYPE_U64);
    DEFINE_C_FUNCTION(FN_Prompt, f_prompt, Tuple({TYPE_String, TYPE_Bool}), TYPE_String, TYPE_String);
    DEFINE_C_FUNCTION(FN_SetAutocompleteScope, f_set_autocomplete_scope, TYPE_Void, TYPE_Scope);
    DEFINE_C_FUNCTION(FN_LoadLibrary, f_load_library, TYPE_Void, TYPE_String);

    DEFINE_C_FUNCTION(FN_IsFile, f_is_file, TYPE_Bool, TYPE_String);
    DEFINE_C_FUNCTION(FN_IsDirectory, f_is_directory, TYPE_Bool, TYPE_String);
    DEFINE_C_FUNCTION(FN_ListLoad, f_list_load, TYPE_Syntax, TYPE_String);
    DEFINE_C_FUNCTION(FN_ListParse, f_list_parse, TYPE_Syntax, TYPE_String);
    DEFINE_C_FUNCTION(FN_ScopeNew, f_scope_new, TYPE_Scope);
    DEFINE_C_FUNCTION(FN_ScopeCopy, f_scope_clone, TYPE_Scope, TYPE_Scope);
    DEFINE_C_FUNCTION(FN_ScopeNewSubscope, f_scope_new_subscope, TYPE_Scope, TYPE_Scope);
    DEFINE_C_FUNCTION(FN_ScopeCopySubscope, f_scope_clone_subscope, TYPE_Scope, TYPE_Scope, TYPE_Scope);
    DEFINE_C_FUNCTION(FN_ScopeParent, f_scope_parent, TYPE_Scope, TYPE_Scope);
    DEFINE_C_FUNCTION(KW_Globals, f_globals, TYPE_Scope);
    DEFINE_C_FUNCTION(SFXFN_SetGlobals, f_set_globals, TYPE_Void, TYPE_Scope);
    DEFINE_C_FUNCTION(SFXFN_SetScopeSymbol, f_set_scope_symbol, TYPE_Void, TYPE_Scope, TYPE_Symbol, TYPE_Any);
    DEFINE_C_FUNCTION(SFXFN_DelScopeSymbol, f_del_scope_symbol, TYPE_Void, TYPE_Scope, TYPE_Symbol);
    DEFINE_C_FUNCTION(FN_SetScopeDocString, f_scope_set_docstring, TYPE_Void, TYPE_Scope, TYPE_Symbol, TYPE_String);
    DEFINE_C_FUNCTION(FN_RealPath, f_realpath, TYPE_String, TYPE_String);
    DEFINE_C_FUNCTION(FN_DirName, f_dirname, TYPE_String, TYPE_String);
    DEFINE_C_FUNCTION(FN_BaseName, f_basename, TYPE_String, TYPE_String);
    DEFINE_C_FUNCTION(FN_FormatMessage, f_format_message, TYPE_String, TYPE_Anchor, TYPE_String);
    DEFINE_C_FUNCTION(FN_ActiveAnchor, get_active_anchor, TYPE_Anchor);
    DEFINE_C_FUNCTION(FN_Write, f_write, TYPE_Void, TYPE_String);
    DEFINE_C_FUNCTION(SFXFN_SetAnchor, f_set_anchor, TYPE_Void, TYPE_Anchor);
    DEFINE_C_FUNCTION(SFXFN_Error, f_error, TYPE_Void, TYPE_String);
    DEFINE_C_FUNCTION(SFXFN_AnchorError, f_anchor_error, TYPE_Void, TYPE_String);
    DEFINE_C_FUNCTION(SFXFN_Raise, f_raise, TYPE_Void, TYPE_Any);
    DEFINE_C_FUNCTION(SFXFN_Abort, f_abort, TYPE_Void);
    DEFINE_C_FUNCTION(FN_Exit, f_exit, TYPE_Void, TYPE_I32);
    DEFINE_C_FUNCTION(FN_CheckStack, f_verify_stack, TYPE_USize);
    DEFINE_PURE_C_FUNCTION(FN_Hash, f_hash, TYPE_U64, TYPE_U64, TYPE_USize);
    DEFINE_PURE_C_FUNCTION(FN_Hash2x64, f_hash2x64, TYPE_U64, TYPE_U64, TYPE_U64);
    DEFINE_PURE_C_FUNCTION(FN_HashBytes, f_hashbytes, TYPE_U64, NativeROPointer(TYPE_I8), TYPE_USize);

    //DEFINE_C_FUNCTION(FN_Malloc, malloc, NativePointer(TYPE_I8), TYPE_USize);

    const Type *exception_pad_type = Array(TYPE_U8, sizeof(ExceptionPad));
    const Type *p_exception_pad_type = NativePointer(exception_pad_type);

    DEFINE_C_FUNCTION(Symbol("set-exception-pad"), f_set_exception_pad,
        p_exception_pad_type, p_exception_pad_type);
    #ifdef SCOPES_WIN32
    DEFINE_C_FUNCTION(Symbol("catch-exception"), _setjmpex, TYPE_I32,
        p_exception_pad_type, NativeROPointer(TYPE_I8));
    #else
    DEFINE_C_FUNCTION(Symbol("catch-exception"), setjmp, TYPE_I32,
        p_exception_pad_type);
    #endif
    DEFINE_C_FUNCTION(Symbol("exception-value"), f_exception_value,
        TYPE_Any, p_exception_pad_type);
    DEFINE_C_FUNCTION(Symbol("set-signal-abort!"), f_set_signal_abort,
        TYPE_Void, TYPE_Bool);



#undef DEFINE_C_FUNCTION

    auto stub_file = SourceFile::from_string(Symbol("<internal>"), String::from_cstr(""));
    auto stub_anchor = Anchor::from(stub_file, 1, 1);

    {
        // launch arguments
        // this is a function returning vararg constants
        Label *fn = Label::function_from(stub_anchor, FN_Args);
        fn->body.anchor = stub_anchor;
        fn->body.enter = fn->params[0];
        globals->bind(FN_Args, fn);
        if (argv && argc) {
            auto &&args = fn->body.args;
            args.push_back(none);
            for (int i = 0; i < argc; ++i) {
                char *s = argv[i];
                if (!s)
                    break;
                args.push_back(String::from_cstr(s));
            }
        }
    }

#ifdef SCOPES_WIN32
#define SCOPES_SYM_OS "windows"
#else
#ifdef SCOPES_MACOS
#define SCOPES_SYM_OS "macos"
#else
#ifdef SCOPES_LINUX
#define SCOPES_SYM_OS "linux"
#else
#define SCOPES_SYM_OS "unknown"
#endif
#endif
#endif
    globals->bind(Symbol("operating-system"), Symbol(SCOPES_SYM_OS));
#undef SCOPES_SYM_OS

    globals->bind(Symbol("unroll-limit"), SCOPES_MAX_RECURSIONS);
    globals->bind(KW_True, true);
    globals->bind(KW_False, false);
    globals->bind(KW_ListEmpty, EOL);
    globals->bind(KW_None, none);
    globals->bind(Symbol("unnamed"), Symbol(SYM_Unnamed));
    globals->bind(SYM_CompilerDir,
        String::from(scopes_compiler_dir, strlen(scopes_compiler_dir)));
    globals->bind(SYM_CompilerPath,
        String::from(scopes_compiler_path, strlen(scopes_compiler_path)));
    globals->bind(SYM_DebugBuild, scopes_is_debug());
    globals->bind(SYM_CompilerTimestamp,
        String::from_cstr(scopes_compile_time_date()));

    for (uint64_t i = STYLE_FIRST; i <= STYLE_LAST; ++i) {
        Symbol sym = Symbol((KnownSymbol)i);
        globals->bind(sym, sym);
    }

    globals->bind(Symbol("exception-pad-type"), exception_pad_type);

#define T(TYPE, NAME) \
    globals->bind(Symbol(NAME), TYPE);
B_TYPES()
#undef T

#define T(NAME, BNAME) \
    globals->bind(Symbol(BNAME), (int32_t)NAME);
    B_TYPE_KIND()
#undef T

    globals->bind(Symbol("pointer-flag-non-readable"), (uint64_t)PTF_NonReadable);
    globals->bind(Symbol("pointer-flag-non-writable"), (uint64_t)PTF_NonWritable);

    globals->bind(Symbol(SYM_DumpDisassembly), (uint64_t)CF_DumpDisassembly);
    globals->bind(Symbol(SYM_DumpModule), (uint64_t)CF_DumpModule);
    globals->bind(Symbol(SYM_DumpFunction), (uint64_t)CF_DumpFunction);
    globals->bind(Symbol(SYM_DumpTime), (uint64_t)CF_DumpTime);
    globals->bind(Symbol(SYM_NoDebugInfo), (uint64_t)CF_NoDebugInfo);
    globals->bind(Symbol(SYM_O1), (uint64_t)CF_O1);
    globals->bind(Symbol(SYM_O2), (uint64_t)CF_O2);
    globals->bind(Symbol(SYM_O3), (uint64_t)CF_O3);

#define T(NAME) globals->bind(NAME, Builtin(NAME));
#define T0(NAME, STR) globals->bind(NAME, Builtin(NAME));
#define T1 T2
#define T2T T2
#define T2(UNAME, LNAME, PFIX, OP) \
    globals->bind(FN_ ## UNAME ## PFIX, Builtin(FN_ ## UNAME ## PFIX));
    B_GLOBALS()
#undef T
#undef T0
#undef T1
#undef T2
#undef T2T
}

//------------------------------------------------------------------------------
// SCOPES CORE
//------------------------------------------------------------------------------

/* this function looks for a header at the end of the compiler executable
   that indicates a scopes core.

   the header has the format (core-size <size>), where size is a i32 value
   holding the size of the core source file in bytes.

   the compiler uses this function to override the default scopes core 'core.sc'
   located in the compiler's directory.

   to later override the default core file and load your own, cat the new core
   file behind the executable and append the header, like this:

   $ cp scopes myscopes
   $ cat mycore.sc >> myscopes
   $ echo "(core-size " >> myscopes
   $ wc -c < mycore.sc >> myscopes
   $ echo ")" >> myscopes

   */

static Any load_custom_core(const char *executable_path) {
    // attempt to read bootstrap expression from end of binary
    auto file = SourceFile::from_file(
        Symbol(String::from_cstr(executable_path)));
    if (!file) {
        stb_fprintf(stderr, "could not open binary\n");
        return none;
    }
    auto ptr = file->strptr();
    auto size = file->size();
    auto cursor = ptr + size - 1;
    while ((*cursor == '\n')
        || (*cursor == '\r')
        || (*cursor == ' ')) {
        // skip the trailing text formatting garbage
        // that win32 echo produces
        cursor--;
        if (cursor < ptr) return none;
    }
    if (*cursor != ')') return none;
    cursor--;
    // seek backwards to find beginning of expression
    while ((cursor >= ptr) && (*cursor != '('))
        cursor--;

    LexerParser footerParser(file, cursor - ptr);
    auto expr = footerParser.parse();
    if (expr.type == TYPE_Nothing) {
        stb_fprintf(stderr, "could not parse footer expression\n");
        return none;
    }
    expr = strip_syntax(expr);
    if ((expr.type != TYPE_List) || (expr.list == EOL)) {
        stb_fprintf(stderr, "footer parser returned illegal structure\n");
        return none;
    }
    expr = ((const List *)expr)->at;
    if (expr.type != TYPE_List)  {
        stb_fprintf(stderr, "footer expression is not a symbolic list\n");
        return none;
    }
    auto symlist = expr.list;
    auto it = symlist;
    if (it == EOL) {
        stb_fprintf(stderr, "footer expression is empty\n");
        return none;
    }
    auto head = it->at;
    it = it->next;
    if (head.type != TYPE_Symbol)  {
        stb_fprintf(stderr, "footer expression does not begin with symbol\n");
        return none;
    }
    if (head != Any(Symbol("core-size")))  {
        stb_fprintf(stderr, "footer expression does not begin with 'core-size'\n");
        return none;
    }
    if (it == EOL) {
        stb_fprintf(stderr, "footer expression needs two arguments\n");
        return none;
    }
    auto arg = it->at;
    it = it->next;
    if (arg.type != TYPE_I32)  {
        stb_fprintf(stderr, "script-size argument is not of type i32\n");
        return none;
    }
    auto script_size = arg.i32;
    if (script_size <= 0) {
        stb_fprintf(stderr, "script-size must be larger than zero\n");
        return none;
    }
    LexerParser parser(file, cursor - script_size - ptr, script_size);
    return parser.parse();
}

//------------------------------------------------------------------------------
// MAIN
//------------------------------------------------------------------------------

static bool terminal_supports_ansi() {
#ifdef SCOPES_WIN32
    if (isatty(STDOUT_FILENO))
        return true;
    return getenv("TERM") != nullptr;
#else
    //return isatty(fileno(stdout));
    return isatty(STDOUT_FILENO);
#endif
}

static void setup_stdio() {
    if (terminal_supports_ansi()) {
        stream_default_style = stream_ansi_style;
        #ifdef SCOPES_WIN32
        #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
        #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
        #endif

        // turn on ANSI code processing
        auto hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        auto hStdErr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode;
        GetConsoleMode(hStdOut, &mode);
        SetConsoleMode(hStdOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        GetConsoleMode(hStdErr, &mode);
        SetConsoleMode(hStdErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        setbuf(stdout, 0);
        setbuf(stderr, 0);
        SetConsoleOutputCP(65001);
        #endif
    }
}

static void on_shutdown() {
#if SCOPES_PRINT_TIMERS
    Timer::print_timers();
    std::cerr << "largest recorded stack size: " << g_largest_stack_size << std::endl;
#endif
}

} // namespace scopes

#ifndef SCOPES_WIN32
static void crash_handler(int sig) {
  void *array[20];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 20);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}
#endif

int main(int argc, char *argv[]) {
    using namespace scopes;
    uint64_t c = 0;
    g_stack_start = (char *)&c;

    Symbol::_init_symbols();
    init_llvm();

    setup_stdio();
    scopes_argc = argc;
    scopes_argv = argv;

    scopes::global_c_namespace = dlopen(NULL, RTLD_LAZY);

    scopes_compiler_path = nullptr;
    scopes_compiler_dir = nullptr;
    scopes_clang_include_dir = nullptr;
    scopes_include_dir = nullptr;
    if (argv) {
        if (argv[0]) {
            std::string loader = GetExecutablePath(argv[0]);
            // string must be kept resident
            scopes_compiler_path = strdup(loader.c_str());
        } else {
            scopes_compiler_path = strdup("");
        }

        char *path_copy = strdup(scopes_compiler_path);
        scopes_compiler_dir = format("%s/..", dirname(path_copy))->data;
        free(path_copy);
        scopes_clang_include_dir = format("%s/lib/clang/include", scopes_compiler_dir)->data;
        scopes_include_dir = format("%s/include", scopes_compiler_dir)->data;
    }

    init_types();
    init_globals(argc, argv);

    linenoiseSetCompletionCallback(prompt_completion_cb);

    Any expr = load_custom_core(scopes_compiler_path);
    if (expr != none) {
        goto skip_regular_load;
    }

    {
        SourceFile *sf = nullptr;
#if 0
        Symbol name = format("%s/lib/scopes/%i.%i.%i/core.sc",
            scopes_compiler_dir,
            SCOPES_VERSION_MAJOR,
            SCOPES_VERSION_MINOR,
            SCOPES_VERSION_PATCH);
#else
        Symbol name = format("%s/lib/scopes/core.sc",
            scopes_compiler_dir);
#endif
        sf = SourceFile::from_file(name);
        if (!sf) {
            location_error(String::from("core missing\n"));
        }
        LexerParser parser(sf);
        expr = parser.parse();
    }

skip_regular_load:
    Label *fn = expand_module(expr, Scope::from(globals));

#if SCOPES_DEBUG_CODEGEN
    StyledStream ss(std::cout);
    std::cout << "non-normalized:" << std::endl;
    stream_label(ss, fn, StreamLabelFormat::debug_all());
    std::cout << std::endl;
#endif

    Solver solver;
    fn = solver.solve(typify_single(nullptr, fn, {}));
#if SCOPES_DEBUG_CODEGEN
    std::cout << "normalized:" << std::endl;
    stream_label(ss, fn, StreamLabelFormat::debug_all());
    std::cout << std::endl;
#endif

    typedef void (*MainFuncType)();
    MainFuncType fptr = (MainFuncType)compile(fn, 0).pointer;
    fptr();

    return 0;
}

#endif // SCOPES_CPP_IMPL
