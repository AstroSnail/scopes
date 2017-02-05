#ifndef BANGRA_CPP
#define BANGRA_CPP

//------------------------------------------------------------------------------
// C HEADER
//------------------------------------------------------------------------------

#if defined __cplusplus
extern "C" {
#endif

enum {
    // semver style versioning
    BANGRA_VERSION_MAJOR = 0,
    BANGRA_VERSION_MINOR = 5,
    BANGRA_VERSION_PATCH = 0,
};

extern int bangra_argc;
extern char **bangra_argv;
extern char *bangra_interpreter_path;
extern char *bangra_interpreter_dir;

int bangra_main(int argc, char ** argv);
int print_number(int value);

#if defined __cplusplus
}
#endif

#endif // BANGRA_CPP
#ifdef BANGRA_CPP_IMPL

//#define BANGRA_DEBUG_IL

//------------------------------------------------------------------------------
// SHARED LIBRARY IMPLEMENTATION
//------------------------------------------------------------------------------

// CFF form implemented after
// Leissa et al., Graph-Based Higher-Order Intermediate Representation
// http://compilers.cs.uni-saarland.de/papers/lkh15_cgo.pdf

// some parts of the paper use hindley-milner notation
// https://en.wikipedia.org/wiki/Hindley%E2%80%93Milner_type_system

// more reading material:
// Simple and Effective Type Check Removal through Lazy Basic Block Versioning
// https://arxiv.org/pdf/1411.0352v2.pdf
// Julia: A Fast Dynamic Language for Technical Computing
// http://arxiv.org/pdf/1209.5145v1.pdf


#undef NDEBUG
#include <sys/types.h>
#ifdef _WIN32
#include "mman.h"
#include "stdlib_ex.h"
#else
// for backtrace
#include <execinfo.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <libgen.h>

#include <map>
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>

#include <ffi.h>

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/ErrorHandling.h>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
//#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Support/Casting.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/RecordLayout.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/MultiplexConsumer.h"

namespace bangra {

//------------------------------------------------------------------------------
// UTILITIES
//------------------------------------------------------------------------------

template<typename ... Args>
std::string format( const std::string& format, Args ... args ) {
    size_t size = snprintf( nullptr, 0, format.c_str(), args ... );
    std::string str;
    str.resize(size);
    snprintf( &str[0], size + 1, format.c_str(), args ... );
    return str;
}

template <typename R, typename... Args>
std::function<R (Args...)> memo(R (*fn)(Args...)) {
    std::map<std::tuple<Args...>, R> list;
    return [fn, list](Args... args) mutable -> R {
        auto argt = std::make_tuple(args...);
        auto memoized = list.find(argt);
        if(memoized == list.end()) {
            auto result = fn(args...);
            list[argt] = result;
            return result;
        } else {
            return memoized->second;
        }
    };
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

static char *extension(char *path) {
    char *dot = path + strlen(path);
    while (dot > path) {
        switch(*dot) {
            case '.': return dot; break;
            case '/': case '\\': {
                return NULL;
            } break;
            default: break;
        }
        dot--;
    }
    return NULL;
}

static size_t inplace_unescape(char *buf) {
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

template<typename T>
static void streamString(T &stream, const std::string &a, const char *quote_chars = nullptr) {
    for (size_t i = 0; i < a.size(); i++) {
        char c = a[i];
        switch(c) {
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if ((c < 32) || (c >= 127)) {
                unsigned char uc = c;
                stream << format("\\x%02x", uc);
            } else {
                if ((c == '\\') || (quote_chars && strchr(quote_chars, c)))
                    stream << '\\';
                stream << c;
            }
            break;
        }
    }
}

static std::string quoteString(const std::string &a, const char *quote_chars = nullptr) {
    std::stringstream ss;
    streamString(ss, a, quote_chars);
    return ss.str();
}

#define ANSI_RESET "\033[0m"
#define ANSI_COLOR_BLACK "\033[30m"
#define ANSI_COLOR_RED "\033[31m"
#define ANSI_COLOR_GREEN "\033[32m"
#define ANSI_COLOR_YELLOW "\033[33m"
#define ANSI_COLOR_BLUE "\033[34m"
#define ANSI_COLOR_MAGENTA "\033[35m"
#define ANSI_COLOR_CYAN "\033[36m"
#define ANSI_COLOR_GRAY60 "\033[37m"

#define ANSI_COLOR_GRAY30 "\033[30;1m"
#define ANSI_COLOR_XRED "\033[31;1m"
#define ANSI_COLOR_XGREEN "\033[32;1m"
#define ANSI_COLOR_XYELLOW "\033[33;1m"
#define ANSI_COLOR_XBLUE "\033[34;1m"
#define ANSI_COLOR_XMAGENTA "\033[35;1m"
#define ANSI_COLOR_XCYAN "\033[36;1m"
#define ANSI_COLOR_WHITE "\033[37;1m"

#define ANSI_STYLE_STRING ANSI_COLOR_XMAGENTA
#define ANSI_STYLE_NUMBER ANSI_COLOR_XGREEN
#define ANSI_STYLE_KEYWORD ANSI_COLOR_XBLUE
#define ANSI_STYLE_OPERATOR ANSI_COLOR_XCYAN
#define ANSI_STYLE_INSTRUCTION ANSI_COLOR_YELLOW
#define ANSI_STYLE_TYPE ANSI_COLOR_XYELLOW
#define ANSI_STYLE_COMMENT ANSI_COLOR_GRAY30
#define ANSI_STYLE_ERROR ANSI_COLOR_XRED
#define ANSI_STYLE_LOCATION ANSI_COLOR_XCYAN

static bool support_ansi = false;
static std::string ansi(const std::string &code, const std::string &content) {
    if (support_ansi) {
        return code + content + ANSI_RESET;
    } else {
        return content;
    }
}

static std::string quoteReprString(const std::string &value) {
    return ansi(ANSI_STYLE_STRING,
        format("\"%s\"",
            quoteString(value, "\"").c_str()));
}

/*
static std::string quoteRepr
bol(const std::string &value) {
    return quoteString(value, "[]{}()\"\'");
}

static std::string quoteReprInteger(int64_t value) {
    return ansi(ANSI_STYLE_NUMBER, format("%" PRIi64, value));
}

static size_t padding(size_t offset, size_t align) {
    return (-offset) & (align - 1);
}
*/

static size_t align(size_t offset, size_t align) {
    return (offset + align - 1) & ~(align - 1);
}



//------------------------------------------------------------------------------
// FILE I/O
//------------------------------------------------------------------------------

struct MappedFile {
protected:
    int fd;
    off_t length;
    void *ptr;

    MappedFile() :
        fd(-1),
        length(0),
        ptr(NULL)
        {}

public:
    ~MappedFile() {
        if (ptr)
            munmap(ptr, length);
        if (fd >= 0)
            close(fd);
    }

    const char *strptr() const {
        return (const char *)ptr;
    }

    size_t size() const {
        return length;
    }

    static std::unique_ptr<MappedFile> open(const char *path) {
        std::unique_ptr<MappedFile> file(new MappedFile());
        file->fd = ::open(path, O_RDONLY);
        if (file->fd < 0)
            return nullptr;
        file->length = lseek(file->fd, 0, SEEK_END);
        file->ptr = mmap(NULL, file->length, PROT_READ, MAP_PRIVATE, file->fd, 0);
        if (file->ptr == MAP_FAILED) {
            return nullptr;
        }
        return file;
    }

    void dumpLine(size_t offset) {
        if (offset >= (size_t)length) {
            return;
        }
        const char *str = strptr();
        size_t start = offset;
        size_t end = offset;
        while (start > 0) {
            if (str[start-1] == '\n')
                break;
            start--;
        }
        while (end < (size_t)length) {
            if (str[end] == '\n')
                break;
            end++;
        }
        printf("%.*s\n", (int)(end - start), str + start);
        size_t column = offset - start;
        for (size_t i = 0; i < column; ++i) {
            putchar(' ');
        }
        puts(ansi(ANSI_STYLE_OPERATOR, "^").c_str());
    }
};

static void dumpFileLine(const char *path, int offset) {
    auto file = MappedFile::open(path);
    if (file) {
        file->dumpLine((size_t)offset);
    }
}

//------------------------------------------------------------------------------
// ANCHORS
//------------------------------------------------------------------------------

struct RuntimeException {};

struct Anchor {
    const char *path;
    int lineno;
    int column;
    int offset;

    Anchor() {
        path = NULL;
        lineno = 0;
        column = 0;
        offset = 0;
    }

    bool isValid() const {
        return (path != NULL) && (lineno != 0) && (column != 0);
    }

    bool operator ==(const Anchor &other) const {
        return
            path == other.path
                && lineno == other.lineno
                && column == other.column
                && offset == other.offset;
    }

    void printMessage (const std::string &msg) const {
        printf("%s:%i:%i: %s\n", path, lineno, column, msg.c_str());
        dumpFileLine(path, offset);
    }

    static void printMessageV (const Anchor *anchor, const char *fmt, va_list args) {
        if (anchor) {
            std::cout
                << ansi(ANSI_STYLE_LOCATION, anchor->path)
                << ansi(ANSI_STYLE_OPERATOR, ":")
                << ansi(ANSI_STYLE_NUMBER, format("%i", anchor->lineno))
                << ansi(ANSI_STYLE_OPERATOR, ":")
                << ansi(ANSI_STYLE_NUMBER, format("%i", anchor->column))
                << " ";
        }
        vprintf (fmt, args);
        putchar('\n');
        if (anchor) {
            dumpFileLine(anchor->path, anchor->offset);
        }
    }

    static void printMessage (const Anchor *anchor, const char *fmt, ...) {
        va_list args;
        va_start (args, fmt);
        Anchor::printMessageV(anchor, fmt, args);
        va_end (args);
    }

    static void printErrorV (const Anchor *anchor, const char *fmt, va_list args) {
        if (anchor && !anchor->isValid())
            anchor = nullptr;
        if (anchor) {
            std::cout
                << ansi(ANSI_STYLE_LOCATION, anchor->path)
                << ansi(ANSI_STYLE_OPERATOR, ":")
                << ansi(ANSI_STYLE_NUMBER, format("%i", anchor->lineno))
                << ansi(ANSI_STYLE_OPERATOR, ":")
                << ansi(ANSI_STYLE_NUMBER, format("%i", anchor->column))
                << " "
                << ansi(ANSI_STYLE_ERROR, "error:")
                << " ";
        } else {
            std::cout << ansi(ANSI_STYLE_ERROR, "error:") << " ";
        }
        vprintf (fmt, args);
        putchar('\n');
        if (anchor) {
            dumpFileLine(anchor->path, anchor->offset);
        }
    }

    static void printError (const Anchor *anchor, const char *fmt, ...) {
        va_list args;
        va_start (args, fmt);
        Anchor::printErrorV(anchor, fmt, args);
        va_end (args);
    }

};

//------------------------------------------------------------------------------
// MID-LEVEL IL
//------------------------------------------------------------------------------

struct Parameter;
struct Flow;
struct Any;
struct Hash;
struct List;
struct Table;
struct Closure;
struct Builtin;
struct BuiltinFlow;
struct BuiltinMacro;
struct Type;
struct Macro;
struct Frame;
struct SpecialForm;

template<typename T>
struct Slice {
    const T *ptr;
    size_t count;
};

typedef Slice<char> String;

typedef Any (*BuiltinFunction)(const std::vector<Any> &args);
typedef std::vector<Any> (*BuiltinFlowFunction)(const std::vector<Any> &args);

typedef uint64_t Symbol;

struct Any {
    union {
        const bool *p_i1;

        const int8_t *p_i8;
        const int16_t *p_i16;
        const int32_t *p_i32;
        const int64_t *p_i64;

        const uint8_t *p_u8;
        const uint16_t *p_u16;
        const uint32_t *p_u32;
        const uint64_t *p_u64;

        const float *p_r32;
        const double *p_r64;

        const void *ptr;
        const BuiltinFunction func;
        const String *str;
        const Symbol *symbol;

        const char **pc_str;
        const void **pptr;
        const List **plist;
        const Type **ptype;
        const Parameter **pparameter;
        const Flow **pflow;
        const Closure **pclosure;
        const Frame **pframe;
        const Builtin **pbuiltin;
        const BuiltinFlow **pbuiltin_flow;
        const BuiltinMacro **pbuiltin_macro;
        const SpecialForm **pspecial_form;
        const Macro **pmacro;
        const Table **ptable;
        const Anchor *anchorref;
    };
    const Type *type;

    Any() {}
    Any &operator =(const Any &other) {
        ptr = other.ptr;
        type = other.type;
        return *this;
    }
};

static Any make_any(const Type *type) {
    Any any;
    any.type = type;
    return any;
}

//------------------------------------------------------------------------------

typedef std::unordered_map<const void *, const Anchor *> PtrAnchorMap;

static PtrAnchorMap anchor_map;

static const Anchor *get_anchor(const void *ptr) {
    auto it = anchor_map.find(ptr);
    if (it != anchor_map.end()) {
        return it->second;
    }
    return nullptr;
}

static const Anchor *get_anchor(const Any &value) {
    return get_anchor(value.ptr);
}

static void set_anchor(const void *ptr, const Anchor *anchor) {
    if (anchor) {
        anchor_map[ptr] = anchor;
    } else {
        auto it = anchor_map.find(ptr);
        if (it != anchor_map.end()) {
            anchor_map.erase(it);
        }
    }
}

static void set_anchor(const Any &value, const Anchor *anchor) {
    set_anchor(value.ptr, anchor);
}

//------------------------------------------------------------------------------

enum {
    SYM_Unnamed = 0,

    SYM_Parent,

    SYM_FunctionForm,
    SYM_QuoteForm,

    // wildcards
    SYM_ListWC,
    SYM_SymbolWC,

    // auto-generated flow names
    SYM_FunctionFlow,
    SYM_ReturnFlow,

    SYM_ScriptSize,
    SYM_Count,
};

static Symbol next_symbol_id = SYM_Count;
static std::map<Symbol, std::string> map_symbol_name;
static std::map<std::string, Symbol> map_name_symbol;

static void map_symbol(Symbol id, const std::string &name) {
    map_name_symbol[name] = id;
    map_symbol_name[id] = name;
}

static Symbol get_symbol(const std::string &name) {
    auto it = map_name_symbol.find(name);
    if (it != map_name_symbol.end()) {
        return it->second;
    } else {
        Symbol id = ++next_symbol_id;
        map_symbol(id, name);
        return id;
    }
}

static std::string get_symbol_name(Symbol id) {
    return map_symbol_name[id];
}

static void initSymbols() {
    map_symbol(SYM_Unnamed, "");
    map_symbol(SYM_Parent, "#parent");
    map_symbol(SYM_FunctionForm, "form:function");
    map_symbol(SYM_QuoteForm, "form:quote");
    map_symbol(SYM_ListWC, "#list");
    map_symbol(SYM_SymbolWC, "#symbol");
    map_symbol(SYM_ScriptSize, "script-size");
    map_symbol(SYM_FunctionFlow, "function");
    map_symbol(SYM_ReturnFlow, "return");
}

//------------------------------------------------------------------------------

namespace Types {
    static const Type *TType;
    static const Type *TArray;
    static const Type *TVector;
    static const Type *TTuple;
    static const Type *TPointer;
    static const Type *TCFunction;
    static const Type *TInteger;
    static const Type *TReal;
    static const Type *TStruct;
    static const Type *TEnum;

    static const Type *Bool;
    static const Type *I8;
    static const Type *I16;
    static const Type *I32;
    static const Type *I64;
    static const Type *U8;
    static const Type *U16;
    static const Type *U32;
    static const Type *U64;

    static const Type *R16;
    static const Type *R32;
    static const Type *R64;

    static const Type *SizeT;

    static const Type *Rawstring;
    static const Type *None;
    static const Type *String; // Slice<char>
    static const Type *Symbol; // Symbol token (uint64_t)

    static const Type *Any;
    static const Type *AnchorRef;

    // opaque internal pointers
    static const Type *_Table;
    static const Type *_List;

    static const Type *PList;
    static const Type *PType;
    static const Type *PTable;
    static const Type *PParameter;
    static const Type *PBuiltin;
    static const Type *PBuiltinFlow;
    static const Type *PFlow;
    static const Type *PSpecialForm;
    static const Type *PBuiltinMacro;
    static const Type *PFrame;
    static const Type *PClosure;
    static const Type *PMacro;

    static const Type *_new_array_type(const Type *element, size_t size);
    static const Type *_new_vector_type(const Type *element, size_t size);
    static const Type *_new_tuple_type(std::vector<const Type *> types);
    static const Type *_new_cfunction_type(
        const Type *result, const Type *parameters, bool vararg);
    static const Type *_new_pointer_type(const Type *element);
    static const Type *_new_integer_type(size_t width, bool signed_);
    static const Type *_new_real_type(size_t width);

    static auto Array = memo(_new_array_type);
    static auto Vector = memo(_new_vector_type);
    static auto Tuple = memo(_new_tuple_type);
    static auto CFunction = memo(_new_cfunction_type);
    static auto Pointer = memo(_new_pointer_type);
    static auto Integer = memo(_new_integer_type);
    static auto Real = memo(_new_real_type);

} // namespace Types

//------------------------------------------------------------------------------

static Any const_none;

//------------------------------------------------------------------------------

static const Anchor *find_valid_anchor(const Any &expr);

static void throw_any(const Any &any) {
    throw any;
}

static void error (const char *format, ...);

//------------------------------------------------------------------------------

struct Table {
    typedef std::unordered_map<Symbol, Any> map_type;

    map_type _;
    Table *meta;
};

//------------------------------------------------------------------------------

enum Ordering {
    Less = -1,
    Equal = 0,
    Greater = 1,
    Unordered = 0x7f,
};

typedef
    Any (*UnaryOpFunction)(const Type *self, const Any &value);
typedef
    Any (*BinaryOpFunction)(const Type *self, const Any &a, const Any &b);
typedef
    bool (*BoolBinaryOpFunction)(const Type *self, const Any &a, const Any &b);
typedef
    Ordering (*CompareFunction)(const Type *self, const Any &a, const Any &b);

enum {
    OP1_Neg = 0,
    OP1_Not, // bit not (~)
    OP1_BoolNot,
    OP1_Rcp,

    OP1_Count,
};

enum {
    OP2_Add = 0,
    OP2_Sub,
    OP2_Mul,
    OP2_Div,
    OP2_Mod,
    OP2_And,
    OP2_Or,
    OP2_Xor,
    OP2_Concat,
    OP2_At,
    OP2_LShift,
    OP2_RShift,

    OP2_Count,
};

struct Type {
    // dynamic attributes
    Table table;

    // bootstrapped attributes

    std::string name;

    // implementation of unary operators
    UnaryOpFunction op1[OP1_Count];
    // implementation of binary operators
    BinaryOpFunction op2[OP2_Count];
    // implementation of reverse binary operators
    BinaryOpFunction rop2[OP2_Count];
    // implementation of comparison
    CompareFunction cmp;
    // return true if self supports the interface of other
    bool (*eq_type)(const Type *self, const Type *other);
    // short string representation
    std::string (*tostring)(const Type *self, const Any &value);
    // return range
    Any (*slice)(const Type *self, const Any &value, size_t i0, size_t i1);
    // apply the type
    Any (*apply_type)(const Type *self, const std::vector<Any> &args);
    // length of value, if any
    size_t (*length)(const Type *self, const Any &value);

    size_t size;
    size_t alignment;

    union {
        // integer: is type signed?
        bool is_signed;
        // function: is vararg?
        bool is_vararg;
    };
    union {
        // integer, real: width in bits
        size_t width;
        // array, vector: number of elements
        size_t count;
    };
    union {
        // array, vector, pointer: type of element
        const Type *element_type;
        // enum: type of tag
        const Type *tag_type;
        // function: type of result
        const Type *result_type;
    };
    // tuple, struct, union: field types
    // function: parameter types
    // enum: payload types
    std::vector<const Type *> types;
    // tuple, struct: field offsets
    std::vector<size_t> offsets;
    // enum: tag values
    std::vector<int64_t> tags;
    // struct, union: name to types index lookup table
    // enum: name to types & tag index lookup table
    std::unordered_map<Symbol, size_t> name_index_map;
};

static std::string get_name(const Type *self) {
    assert(self);
    return self->name;
}

static size_t get_size(const Type *self) {
    return self->size;
}

static size_t get_width(const Type *self) {
    return self->width;
}

static bool is_signed(const Type *self) {
    return self->is_signed;
}

static const Type *get_type(const Type *self, size_t i) {
    return self->types[i];
}

static size_t get_field_count(const Type *self) {
    return self->types.size();
}

static bool is_vararg(const Type *self) {
    return self->is_vararg;
}

static size_t get_parameter_count(const Type *self) {
    return get_field_count(self->types[0]);
}

static const Type *get_parameter_type(const Type *self, size_t i) {
    return get_type(self->types[0], i);
}

static size_t get_offset(const Type *self, size_t i) {
    return self->offsets[i];
}

static size_t get_alignment(const Type *self) {
    return self->alignment;
}

static const Type *get_element_type(const Type *self) {
    return self->element_type;
}
/*
static Type *get_tag_type(Type *self) {
    return self->tag_type;
}
*/
static const Type *get_result_type(const Type *self) {
    return self->result_type;
}

static bool type_eq_type_default(const Type *self, const Type *other) {
    assert(self && other);
    return false;
}

static Any op1_default(const Type *self, const Any &value) {
    error("unary operator not applicable to type %s",
        get_name(self).c_str());
    return const_none;
}

static Any op2_default(const Type *self, const Any &a, const Any &b) {
    error("binary operator not applicable to type %s",
        get_name(self).c_str());
    return const_none;
}

static Ordering cmp_default(const Type *self, const Any &a, const Any &b) {
    error("comparison not applicable to type %s",
        get_name(self).c_str());
    return Unordered;
}

static Any type_apply_default(const Type *self, const std::vector<Any> &args) {
    error("type %s has no constructor",
        get_name(self).c_str());
    return const_none;
}

static std::string type_tostring_default(const Type *self, const Any &value) {
    assert(self);
    return format("<value of %s>", get_name(self).c_str());
}

static size_t type_length_default(const Type *self, const Any &value) {
    return (size_t)-1;
}

static Any type_slice_default(
    const Type *self, const Any &value, size_t i0, size_t i1) {
    error("type %s not sliceable", get_name(self).c_str());
    return const_none;
}


static Type *new_type(const std::string &name) {
    //assert(!name.empty());
    auto result = new Type();
    result->size = 0;
    result->alignment = 1;
    result->name = name;
    result->eq_type = type_eq_type_default;
    result->apply_type = type_apply_default;
    result->tostring = type_tostring_default;
    result->length = type_length_default;
    result->slice = type_slice_default;
    for (size_t i = 0; i < OP1_Count; ++i) {
        result->op1[i] = op1_default;
    }
    for (size_t i = 0; i < OP2_Count; ++i) {
        result->op2[i] = op2_default;
        result->rop2[i] = op2_default;
    }
    result->cmp = cmp_default;
    result->is_signed = false;
    result->is_vararg = false;
    result->width = 0;
    result->count = 0;
    //result->ctype = nullptr;
    return result;
}

static bool eq(const Type *self, const Type *other) {
    if (self == other) return true;
    assert(self && other);
    return self->eq_type(self, other) || other->eq_type(other, self);
}

template<int OP>
static Any op2(const Any &a, const Any &b) {
    try {
        return a.type->op2[OP](a.type, a, b);
    } catch (const Any &any) {
        return b.type->rop2[OP](b.type, b, a);
    }
}

static Ordering compare(const Any &a, const Any &b) {
    try {
        return a.type->cmp(a.type, a, b);
    } catch (const Any &any) {
        // reverse lesser/greater
        auto result = b.type->cmp(b.type, b, a);
        switch(result) {
            case Less: return Greater;
            case Greater: return Less;
            default: return result;
        }
    }
}

static bool eq(const Any &a, const Any &b) {
    //try {
        return compare(a, b) == Equal;
    //} catch (const Any &any) {
    //    return false;
    //}
}

static bool ne(const Any &a, const Any &b) {
    //try {
        return compare(a, b) != Equal;
    //} catch (const Any &any) {
    //    return true;
    //}
}

static bool gt(const Any &a, const Any &b) {
    //try {
        return compare(a, b) == Greater;
    //} catch (const Any &any) {
    //    return false;
    //}
}

static bool ge(const Any &a, const Any &b) {
    //try {
        auto result = compare(a, b);
        return (result == Equal) || (result == Greater);
    //} catch (const Any &any) {
    //    return false;
    //}
}

static bool lt(const Any &a, const Any &b) {
    //try {
        return compare(a, b) == Less;
    //} catch (const Any &any) {
    //    return false;
    //}
}

static bool le(const Any &a, const Any &b) {
    //try {
        auto result = compare(a, b);
        return (result == Equal) || (result == Less);
    //} catch (const Any &any) {
    //    return false;
    //}
}

static Any add(const Any &a, const Any &b) {
    return op2<OP2_Add>(a, b);
}

static Any sub(const Any &a, const Any &b) {
    return op2<OP2_Sub>(a, b);
}

static Any mul(const Any &a, const Any &b) {
    return op2<OP2_Mul>(a, b);
}

static Any div(const Any &a, const Any &b) {
    return op2<OP2_Div>(a, b);
}

static Any mod(const Any &a, const Any &b) {
    return op2<OP2_Mod>(a, b);
}

static Any bit_and(const Any &a, const Any &b) {
    return op2<OP2_And>(a, b);
}

static Any bit_or(const Any &a, const Any &b) {
    return op2<OP2_Or>(a, b);
}

static Any bit_xor(const Any &a, const Any &b) {
    return op2<OP2_Xor>(a, b);
}

static Any concat(const Any &a, const Any &b) {
    return op2<OP2_Concat>(a, b);
}

static Any at(const Any &value, const Any &index) {
    return op2<OP2_At>(value, index);
}

static Any lshift(const Any &a, const Any &b) {
    return op2<OP2_LShift>(a, b);
}

static Any rshift(const Any &a, const Any &b) {
    return op2<OP2_RShift>(a, b);
}

static Any neg(const Any &value) {
    return value.type->op1[OP1_Neg](value.type, value);
}

static Any bit_not(const Any &value) {
    return value.type->op1[OP1_Not](value.type, value);
}

static Any bool_not(const Any &value) {
    return value.type->op1[OP1_BoolNot](value.type, value);
}

static Any rcp(const Any &value) {
    return value.type->op1[OP1_Rcp](value.type, value);
}

static std::string get_string(const Any &value) {
    return value.type->tostring(value.type, value);
}

static Any slice(const Any &value, size_t i0, size_t i1) {
    return value.type->slice(value.type, value, i0, i1);
}

static size_t length(const Any &value) {
    auto s = value.type->length(value.type, value);
    if (s == (size_t)-1) {
        error("length operator not applicable");
    }
    return s;
}

//------------------------------------------------------------------------------

static Table *new_table() {
    auto result = new Table();
    result->meta = nullptr;
    return result;
}

/*
static void set_meta(Table &table, Table *meta) {
    table.meta = meta;
}

static Table *get_meta(Table &table) {
    return table.meta;
}
*/

static void set_key(Table &table, const Symbol &key, const Any &value) {
    table._[key] = value;
}

static bool has_key(const Table &table, const Symbol &key) {
    return table._.find(key) != table._.end();
}

static const Any &get_key(const Table &table,
    const Symbol &key, const Any &defvalue) {
    auto it = table._.find(key);
    if (it == table._.end())
        return defvalue;
    else
        return it->second;
}

static bool isnone(const Any &value) {
    return (value.type == Types::None);
}

/*
static Any call(const Any &what, const std::vector<Any> &args) {
    return const_none;
}
*/

static bool is_typeref_type(const Type *type) {
    return eq(type, Types::PType);
}

static bool is_none_type(const Type *type) {
    return eq(type, Types::None);
}

static bool is_bool_type(const Type *type) {
    return eq(type, Types::Bool);
}

static bool is_integer_type(const Type *type) {
    return eq(type, Types::TInteger);
}

static bool is_real_type(const Type *type) {
    return eq(type, Types::TReal);
}

/*
static bool is_struct_type(Type *type) {
    return eq(type, Types::TStruct);
}
*/

static bool is_tuple_type(const Type *type) {
    return eq(type, Types::TTuple);
}

/*
static bool is_array_type(Type *type) {
    return eq(type, Types::TArray);
}
*/

static bool is_cfunction_type(const Type *type) {
    return eq(type, Types::TCFunction);
}

static bool is_pointer_type(const Type *type) {
    return eq(type, Types::TPointer);
}

static bool is_table_type(const Type *type) {
    return eq(type, Types::PTable);
}

static std::string extract_string(const Any &value) {
    if (eq(value.type, Types::Rawstring)) {
        return *value.pc_str;
    } else if (eq(value.type, Types::String)) {
        return std::string(value.str->ptr, value.str->count);
    } else {
        error("string expected");
        return "";
    }
}

static const char *extract_cstr(const Any &value) {
    if (eq(value.type, Types::Rawstring)) {
        return *value.pc_str;
    } else if (eq(value.type, Types::String)) {
        return value.str->ptr;
    } else {
        error("string expected");
        return "";
    }
}

static Symbol extract_symbol(const Any &value) {
    if (eq(value.type, Types::Symbol)) {
        return *value.symbol;
    } else {
        error("symbol expected");
        return 0;
    }
}

static std::string extract_symbol_string(const Any &value) {
    return get_symbol_name(extract_symbol(value));
}

static bool extract_bool(const Any &value) {
    if (eq(value.type, Types::Bool)) {
        return *value.p_i1;
    }
    error("boolean expected");
    return false;
}

static const Type *extract_type(const Any &value) {
    if (is_typeref_type(value.type)) {
        return *value.ptype;
    }
    error("type constant expected");
    return nullptr;
}

template<typename T>
static Slice<T> *alloc_slice(const T *ptr, size_t count) {
    Slice<T> *s = (Slice<T> *)malloc(sizeof(Slice<T>));
    s->ptr = ptr;
    s->count = count;
    return s;
}

static String *alloc_string(const char *ptr, size_t count) {
    char *ptrcpy = (char *)malloc(sizeof(char) * count + 1);
    memcpy(ptrcpy, ptr, sizeof(char) * count);
    ptrcpy[count] = 0;
    return alloc_slice(ptrcpy, count);
}

static Any wrap(const Type *type, const void *ptr) {
    Any any = make_any(type);
    any.ptr = const_cast<void *>(ptr);
    return any;
}

static Any pstring(const std::string &s) {
    return wrap(Types::String, alloc_string(s.c_str(), s.size()));
}

template <typename T>
static T **alloc_ptr(T *value) {
    T **ptr = (T **)malloc(sizeof(T *));
    *ptr = value;
    return ptr;
}

template <typename T>
static T *alloc_int(int64_t value) {
    T *ptr = (T *)malloc(sizeof(T));
    *ptr = (T)value;
    return ptr;
}

template <typename T>
static T *alloc_uint(uint64_t value) {
    T *ptr = (T *)malloc(sizeof(T));
    *ptr = (T)value;
    return ptr;
}

static Any wrap_symbol(const Symbol &s) {
    return wrap(Types::Symbol, alloc_uint<Symbol>(s));
}

static Any symbol(const std::string &s) {
    return wrap_symbol(get_symbol(s));
}

static Any wrap_ptr(const Type *type, const void *ptr) {
    Any any = make_any(type);
    any.ptr = alloc_ptr(ptr);
    return any;
}

static Any integer(const Type *type, int64_t value) {
    Any any = make_any(type);
    if (type == Types::Bool) {
        any.p_i1 = alloc_int<bool>(value);
    } else if (type == Types::I8) {
        any.p_i8 = alloc_int<int8_t>(value);
    } else if (type == Types::I16) {
        any.p_i16 = alloc_int<int16_t>(value);
    } else if (type == Types::I32) {
        any.p_i32 = alloc_int<int32_t>(value);
    } else if (type == Types::I64) {
        any.p_i64 = alloc_int<int64_t>(value);
    } else if (type == Types::U8) {
        any.p_u8 = alloc_uint<uint8_t>((uint64_t)value);
    } else if (type == Types::U16) {
        any.p_u16 = alloc_uint<uint16_t>((uint64_t)value);
    } else if (type == Types::U32) {
        any.p_u32 = alloc_uint<uint32_t>((uint64_t)value);
    } else if (type == Types::U64) {
        any.p_u64 = alloc_uint<uint64_t>((uint64_t)value);
    } else {
        assert(false && "not an integer type");
    }
    return any;
}

static int64_t extract_integer(const Any &value) {
    auto type = value.type;
    if (type == Types::Bool) {
        return (int64_t)*value.p_i1;
    } else if (type == Types::I8) {
        return (int64_t)*value.p_i8;
    } else if (type == Types::I16) {
        return (int64_t)*value.p_i16;
    } else if (type == Types::I32) {
        return (int64_t)*value.p_i32;
    } else if (type == Types::I64) {
        return *value.p_i64;
    } else if (type == Types::U8) {
        return (int64_t)*value.p_u8;
    } else if (type == Types::U16) {
        return (int64_t)*value.p_u16;
    } else if (type == Types::U32) {
        return (int64_t)*value.p_u32;
    } else if (type == Types::U64) {
        return (int64_t)*value.p_u64;
    } else {
        error("integer expected, not %s", get_name(type).c_str());
        return 0;
    }
}

template <typename T>
static T *alloc_real(double value) {
    T *ptr = (T *)malloc(sizeof(T));
    *ptr = (T)value;
    return ptr;
}

static Any real(const Type *type, double value) {
    Any any = make_any(type);
    if (type == Types::R32) {
        any.p_r32 = alloc_real<float>(value);
    } else if (type == Types::R64) {
        any.p_r64 = alloc_real<double>(value);
    } else {
        assert(false && "not a real type");
    }
    return any;
}

static double extract_real(const Any &value) {
    auto type = value.type;
    if (type == Types::R32) {
        return (double)*value.p_r32;
    } else if (type == Types::R64) {
        return (double)*value.p_r64;
    } else {
        error("real expected, not %s", get_name(type).c_str());
        return 0;
    }
}

template<typename T>
static T *extract_ptr(const bangra::Any &value) {
    return *(T **)value.ptr;
}

static Any wrap(bool value) {
    return integer(Types::Bool, value);
}

static Any wrap(int32_t value) {
    return integer(Types::I32, value);
}

static Any wrap(int64_t value) {
    return integer(Types::I64, value);
}

static Any wrap(size_t value) {
    return integer(Types::SizeT, value);
}

// undefined catch-all that produces diagnostic messages when it is selected,
// which should never happen.
template<typename T>
static Any wrap(const T &arg);

static std::vector<Any> extract_tuple(const Any &value) {
    if (is_tuple_type(value.type)) {
        std::vector<Any> result;
        size_t count = get_field_count(value.type);
        for (size_t i = 0; i < count; ++i) {
            result.push_back(at(value, wrap(i)));
        }
        return result;
    }
    error("can not extract tuple");
    return {};
}

static bangra::Any pointer_element(
    const Type *self, const bangra::Any &value) {
    return wrap(get_element_type(self), *(char **)value.ptr);
}

static const Table *extract_table(const Any &value) {
    if (is_table_type(value.type)) {
        return *value.ptable;
    } else if (eq(value.type, Types::_Table)) {
        return (Table *)value.ptr;
    }
    error("table expected, not %s", get_name(value.type).c_str());
    return nullptr;
}

//------------------------------------------------------------------------------

struct Parameter {
    // changes need to be mirrored to Types::PParameter

    Flow *parent;
    size_t index;
    const Type *parameter_type;
    Symbol name;

    Parameter() :
        parent(nullptr),
        index(-1),
        parameter_type(nullptr),
        name(SYM_Unnamed) {
    }

    Flow *getParent() const {
        return parent;
    }

    std::string getReprName() const {
        if (name == SYM_Unnamed) {
            return format("%s%zu",
                ansi(ANSI_STYLE_OPERATOR,"@").c_str(),
                index);
        } else {
            return get_symbol_name(name);
        }
    }

    /*
    std::string getRepr() const {
        return format("%s %s %s",
            getReprName().c_str(),
            ansi(ANSI_STYLE_OPERATOR,":").c_str(),
            bangra::getRepr(getType(this)).c_str());
    }
    */

    std::string getRefRepr () const;

    static Parameter *create(const Symbol &name = SYM_Unnamed) {
        auto value = new Parameter();
        value->index = (size_t)-1;
        value->name = name;
        value->parameter_type = Types::Any;
        return value;
    }
};

//------------------------------------------------------------------------------

static Any wrap(const List *slist) {
    return wrap_ptr(Types::PList, slist);
}
static Any wrap(List *slist) {
    return wrap_ptr(Types::PList, slist);
}

struct List {
    Any at;
    const List *next;

    static List *create(const Any &at, const List *next) {
        auto result = new List();
        result->at = at;
        result->next = next;
        //set_anchor(result, get_anchor(at));
        return result;
    }

    static List *create(const Any &at, const List *next, const Anchor *anchor) {
        auto result = create(at, next);
        set_anchor(result, anchor);
        return result;
    }

    static const List *create_from_c_array(Any *values, size_t count) {
        List *result = nullptr;
        while (count) {
            --count;
            result = create(values[count], result);
        }
        return result;
    }

    static const List *create_from_array(const std::vector<Any> &values) {
        return create_from_c_array(const_cast<Any *>(&values[0]), values.size());
    }

    /*
    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }
    */

    /*
    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "list");
        auto expr = this;
        get_eox();
        while (expr != EOX) {
            ss << " " << bangra::getRefRepr(expr->at);
            expr = expr->next;
        }
        ss << ")";
        return ss.str();
    }
    */
};

static const List *extract_list(const Any &value) {
    if (eq(value.type, Types::PList)) {
        return *value.plist;
    } else if (eq(value.type, Types::_List)) {
        return (const List *)value.ptr;
    } else {
        error("list expected, not %s", get_name(value.type).c_str());
    }
    return nullptr;
}

#if 0
// (a . (b . (c . (d . NIL)))) -> (d . (c . (b . (a . NIL))))
// this is the version for immutables; input lists are not modified
static const List *reverse_list(const List *l, const List *eol = nullptr) {
    const List *next = nullptr;
    while (l != eol) {
        next = List::create(l->at, next, get_anchor(l));
        l = l->next;
    }
    return next;
}
#endif

// (a . (b . (c . (d . NIL)))) -> (d . (c . (b . (a . NIL))))
// this is the mutating version; input lists are modified, direction is inverted
static const List *reverse_list_inplace(
    const List *l, const List *eol = nullptr) {
    const List *next = nullptr;
    while (l != eol) {
        const List *iternext = l->next;
        const_cast<List *>(l)->next = next;
        next = l;
        l = iternext;
    }
    return next;
}

//------------------------------------------------------------------------------

struct ListIter {
protected:
    const List *expr;
public:
    const List *getList() const {
        return expr;
    }

    const Anchor *getAnchor() const {
        return get_anchor(expr);
    }

    ListIter(const List *value, size_t c=0) :
        expr(value) {
        for (size_t i = 0; i < c; ++i) {
            assert(expr);
            expr = expr->next;
        }
    }

    ListIter(const Any &value, size_t i=0) :
        ListIter(extract_list(value), i)
    {}

    bool operator ==(const ListIter &other) const {
        return (expr == other.expr);
    }
    bool operator !=(const ListIter &other) const {
        return (expr != other.expr);
    }

    ListIter operator +(int offset) const {
        return ListIter(expr, (size_t)offset);
    }

    ListIter operator ++(int) {
        auto oldself = *this;
        assert (expr);
        expr = expr->next;
        return oldself;
    }

    bool isValid() const {
        return expr != nullptr;
    }

    operator bool() const {
        return isValid();
    }

    const Any &operator *() const {
        assert(expr);
        return expr->at;
    }

};

//------------------------------------------------------------------------------

struct Flow {
private:
    static int64_t unique_id_counter;
protected:
    int64_t uid;

public:
    Flow() :
        uid(unique_id_counter++) {
    }

    Symbol name;
    std::vector<Parameter *> parameters;

    // default path
    std::vector<Any> arguments;

    size_t getParameterCount() {
        return parameters.size();
    }

    Parameter *getParameter(size_t i) {
        return parameters[i];
    }

    bool hasArguments() const {
        return !arguments.empty();
    }

    int64_t getUID() const {
        return uid;
    }

    std::string getRefRepr () const {
        return format("%s%s%" PRId64,
            ansi(ANSI_STYLE_KEYWORD, "λ").c_str(),
            get_symbol_name(name).c_str(),
            uid);
    }

    Parameter *appendParameter(Parameter *param) {
        param->parent = this;
        param->index = parameters.size();
        parameters.push_back(param);
        return param;
    }

    static Flow *create(
        size_t paramcount = 0,
        const Symbol &name = SYM_Unnamed) {
        auto value = new Flow();
        value->name = name;
        for (size_t i = 0; i < paramcount; ++i) {
            value->appendParameter(Parameter::create());
        }
        return value;
    }
};

int64_t Flow::unique_id_counter = 1;

static const Flow *extract_flow(const Any &value) {
    if (eq(value.type, Types::PFlow)) {
        return *value.pflow;
    }
    error("flow expected");
    return nullptr;
}

//------------------------------------------------------------------------------

struct Builtin {

    BuiltinFunction handler;
    Symbol name;

    static Builtin *create(BuiltinFunction func,
        const Symbol &name) {
        auto result = new Builtin();
        result->handler = func;
        result->name = name;
        return result;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "builtin");
        ss << " " << get_symbol_name(name);
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct BuiltinFlow {

    BuiltinFlowFunction handler;
    Symbol name;

    static BuiltinFlow *create(BuiltinFlowFunction func,
        const Symbol &name) {
        auto result = new BuiltinFlow();
        result->handler = func;
        result->name = name;
        return result;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "builtin-cc");
        ss << " " << get_symbol_name(name);
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

typedef Any (*SpecialFormFunction)(ListIter);

struct SpecialForm {

    SpecialFormFunction handler;
    Symbol name;

    static SpecialForm *create(SpecialFormFunction func,
        const Symbol &name) {
        auto result = new SpecialForm();
        result->handler = func;
        result->name = name;
        return result;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "form");
        ss << " " << get_symbol_name(name);
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct Cursor {
    Any value;
    ListIter next;
};

typedef Cursor (*MacroBuiltinFunction)(const Table *, ListIter);

struct BuiltinMacro {

    MacroBuiltinFunction handler;
    Symbol name;

    static BuiltinMacro *create(MacroBuiltinFunction func,
        const Symbol &name) {
        auto result = new BuiltinMacro();
        result->handler = func;
        result->name = name;
        return result;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "builtin-macro");
        ss << " " << get_symbol_name(name);
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

typedef std::unordered_map<const Flow *, std::vector<Any> >
    FlowValuesMap;

struct Frame {
    size_t idx;
    Frame *parent;
    FlowValuesMap map;

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "#" << idx << ":" << this;
        return ss.str();
    }

    std::string getRepr() const {
        std::stringstream ss;
        ss << "#" << idx << ":" << this << ":\n";
        for (auto &entry : map) {
            ss << "  " << get_string(wrap_ptr(Types::PFlow, entry.first));
            auto &value = entry.second;
            for (size_t i = 0; i < value.size(); ++i) {
                ss << " " << get_string(value[i]);
            }
            ss << "\n";
        }
        return ss.str();
    }

    static Frame *create() {
        // create closure
        Frame *newframe = new Frame();
        newframe->parent = nullptr;
        newframe->idx = 0;
        return newframe;
    }

    static Frame *create(Frame *frame) {
        // create closure
        Frame *newframe = new Frame();
        newframe->parent = frame;
        newframe->idx = frame->idx + 1;
        return newframe;
    }
};

static const Frame *extract_frame(const Any &value) {
    if (eq(value.type, Types::PFrame)) {
        return *value.pframe;
    }
    error("frame expected");
    return nullptr;
}

//------------------------------------------------------------------------------

struct Closure {
    // changes here must be mirrored to Types::PClosure
    const Flow *entry;
    Frame *frame;
};

static const Closure *create_closure(
    const Flow *entry,
    Frame *frame) {
    auto result = new Closure();
    result->entry = entry;
    result->frame = frame;
    return result;
}

static const Closure *extract_closure(const Any &value) {
    if (eq(value.type, Types::PClosure)) {
        return *value.pclosure;
    }
    error("closure expected");
    return nullptr;
}

//------------------------------------------------------------------------------

struct Macro {

    Any value;

    static Macro *create(const Any &value) {
        auto result = new Macro();
        result->value = value;
        return result;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "macro");
        ss << " " << get_string(value);
        ss << ")";
        return ss.str();
    }
};

/*
static Any tuple(const std::vector<Any> &values) {
    std::vector<Type *> types;
    size_t size;
    for (auto &v : values) {
        types.push_back(v.type);
    }
    Type *tuple_type = Types::Tuple(types);
}
*/

static Any wrap(const Flow *flow) {
    return wrap_ptr(Types::PFlow, flow);
}
static Any wrap(Flow *flow) {
    return wrap_ptr(Types::PFlow, flow);
}

static Any wrap(const Parameter *param) {
    return wrap_ptr(Types::PParameter, param);
}
static Any wrap(Parameter *param) {
    return wrap_ptr(Types::PParameter, param);
}

static Any wrap(const Type *type) {
    return wrap_ptr(Types::PType, type);
}

static Any wrap(const Table *table) {
    return wrap_ptr(Types::PTable, table);
}
static Any wrap(Table *table) {
    return wrap_ptr(Types::PTable, table);
}

static Any wrap(
    const std::vector<Any> &args) {

    std::vector<const Type *> types;
    for (auto &arg : args) {
        types.push_back(arg.type);
    }
    const Type *type = Types::Tuple(types);
    char *data = (char *)malloc(get_size(type));
    for (size_t i = 0; i < args.size(); ++i) {
        auto elemtype = get_type(type, i);
        auto offset = get_offset(type, i);
        auto srcptr = args[i].ptr;
        auto size = get_size(elemtype);
        if (size) {
            memcpy(data + offset, srcptr, size);
        } else if (!isnone(args[i])) {
            error("attempting to pack opaque type %s in tuple",
                get_name(args[i].type).c_str());
        }
    }

    return wrap(type, data);
}

static Any wrap(double value) {
    return real(Types::R64, value);
}

static Any wrap(const std::string &s) {
    return pstring(s);
}

static bool builtin_checkparams (const std::vector<Any> &args,
    int mincount, int maxcount, int skip = 0) {
    if ((mincount <= 0) && (maxcount == -1))
        return true;

    int argcount = (int)args.size() - skip;

    if ((maxcount >= 0) && (argcount > maxcount)) {
        error("excess argument. At most %i arguments expected", maxcount);
        return false;
    }
    if ((mincount >= 0) && (argcount < mincount)) {
        error("at least %i arguments expected", mincount);
        return false;
    }
    return true;
}

static void error (const char *format, ...) {
    //const Anchor *anchor = nullptr;
    // TODO: find valid anchor
    //std::cout << "at\n  " << getRepr(value) << "\n";
    //anchor = find_valid_anchor(value);
    va_list args;

    va_start (args, format);
    size_t size = vsnprintf(nullptr, 0, format, args);
    va_end (args);

    std::string str;
    str.resize(size);

    va_start (args, format);
    vsnprintf(&str[0], size + 1, format, args);
    va_end (args);

    throw_any(wrap(str));
}


//------------------------------------------------------------------------------
// IL MODEL UTILITY FUNCTIONS
//------------------------------------------------------------------------------

static void unescape(String &s) {
    s.count = inplace_unescape(const_cast<char *>(s.ptr));
}

// matches (///...)
static bool is_comment(const Any &expr) {
    if (eq(expr.type, Types::PList)) {
        if (*expr.plist) {
            const Any &sym = (*expr.plist)->at;
            if (eq(sym.type, Types::Symbol)) {
                auto s = extract_symbol_string(sym);
                if (!memcmp(s.c_str(),"///",3))
                    return true;
            }
        }
    }
    return false;
}

static const Anchor *find_valid_anchor(const List *l) {
    const Anchor *a = nullptr;
    while (l) {
        a = get_anchor(l);
        if (!a) {
            a = find_valid_anchor(l->at);
        }
        if (!a) {
            l = l->next;
        } else {
            break;
        }
    }
    return a;
}

static const Anchor *find_valid_anchor(const Any &expr) {
    const Anchor *a = get_anchor(expr);
    if (!a) {
        if (eq(expr.type, Types::PList)) {
            a = find_valid_anchor(*expr.plist);
        }
    }
    return a;
}

static Any strip(const Any &expr) {
    if (eq(expr.type, Types::PList)) {
        const List *l = nullptr;
        auto it = ListIter(expr);
        while (it) {
            auto value = strip(*it);
            if (!is_comment(value)) {
                l = List::create(value, l, it.getAnchor());
            }
            it++;
        }
        return wrap(reverse_list_inplace(l));
    }
    return expr;
}

static size_t getSize(const List *expr) {
    ListIter it(expr, 0);
    size_t c = 0;
    while (it) {
        c++;
        it++;
    }
    return c;
}

//------------------------------------------------------------------------------
// PRINTING
//------------------------------------------------------------------------------

#if 0
template<typename T>
static void streamTraceback(T &stream) {
    void *array[10];
    size_t size;
    char **strings;
    size_t i;

    size = backtrace (array, 10);
    strings = backtrace_symbols (array, size);

    for (i = 0; i < size; i++) {
        stream << format("%s\n", strings[i]);
    }

    free (strings);
}

static std::string formatTraceback() {
    std::stringstream ss;
    streamTraceback(ss);
    return ss.str();
}
#endif

static bool isNested(const Any &e) {
    if (e.type == Types::PList) {
        auto it = ListIter(e);
        while (it) {
            if ((*it).type == Types::PList)
                return true;
            it++;
        }
    }
    return false;
}

template<typename T>
static void streamAnchor(T &stream, const Any &e, size_t depth=0) {
    const Anchor *anchor = find_valid_anchor(e);
    if (anchor) {
        stream <<
            format("%s:%i:%i: ",
                anchor->path,
                anchor->lineno,
                anchor->column);
    }
    for(size_t i = 0; i < depth; i++)
        stream << "    ";
}

template<typename T>
static void streamValue(T &stream, const Any &e, size_t depth=0, bool naked=true) {
    if (naked) {
        streamAnchor(stream, e, depth);
    }

    if (e.type == Types::PList) {
        //auto slist = llvm::cast<ListValue>(e);
        auto it = ListIter(e);
        if (!it) {
            stream << "()";
            if (naked)
                stream << '\n';
            return;
        }
        size_t offset = 0;
        if (naked) {
            bool single = !(it + 1);
        print_terse:
            streamValue(stream, *it++, depth, false);
            offset++;
            while (it) {
                if (isNested(*it))
                    break;
                stream << ' ';
                streamValue(stream, *it, depth, false);
                offset++;
                it++;
            }
            stream << (single?";\n":"\n");
        //print_sparse:
            while (it) {
                auto value = *it;
                if ((value.type != Types::PList) // not a list
                    && (offset >= 1) // not first element in list
                    && (it + 1) // not last element in list
                    && !isNested(*(it + 1))) { // next element can be terse packed too
                    single = false;
                    streamAnchor(stream, *it, depth + 1);
                    stream << "\\ ";
                    goto print_terse;
                }
                streamValue(stream, value, depth + 1);
                offset++;
                it++;
            }

        } else {
            stream << '(';
            while (it) {
                if (offset > 0)
                    stream << ' ';
                streamValue(stream, *it, depth + 1, false);
                offset++;
                it++;
            }
            stream << ')';
            if (naked)
                stream << '\n';
        }
    } else {
        if (e.type == Types::Symbol) {
            stream << ANSI_STYLE_KEYWORD;
            streamString(stream, extract_symbol_string(e), "[]{}()\"");
            stream << ANSI_RESET;
        } else if (e.type == Types::String) {
            stream << ANSI_STYLE_STRING;
            stream << '"';
            streamString(stream, extract_string(e), "\"");
            stream << '"';
            stream << ANSI_RESET;
        } else {
            auto s = get_string(e);
            if (e.type == Types::Bool) {
                stream << ANSI_STYLE_KEYWORD << s << ANSI_RESET;
            } else if (is_integer_type(e.type)) {
                stream << ANSI_STYLE_NUMBER << s << ANSI_RESET;
            } else {
                stream << s;
            }
        }
        if (naked)
            stream << '\n';
    }
}

static std::string formatValue(const Any &e, size_t depth=0, bool naked=false) {
    std::stringstream ss;
    streamValue(ss, e, depth, naked);
    return ss.str();
}

static void printValue(const Any &e, size_t depth=0, bool naked=false) {
    streamValue(std::cout, e, depth, naked);
}

void valueError (const Any &expr, const char *format, ...) {
    const Anchor *anchor = find_valid_anchor(expr);
    if (!anchor) {
        printValue(expr);
        std::cout << "\n";
    }
    va_list args;
    va_start (args, format);
    Anchor::printErrorV(anchor, format, args);
    va_end (args);
    throw_any(const_none);
}

void valueErrorV (const Any &expr, const char *fmt, va_list args) {
    const Anchor *anchor = find_valid_anchor(expr);
    if (!anchor) {
        printValue(expr);
        std::cout << "\n";
    }
    Anchor::printErrorV(anchor, fmt, args);
    throw_any(const_none);
}

static void verifyValueKind(const Type *type, const Any &expr) {
    if (!eq(expr.type, type)) {
        valueError(expr, "%s expected, not %s",
            get_name(type).c_str(),
            get_name(expr.type).c_str());
    }
}

//------------------------------------------------------------------------------
// TYPE SETUP
//------------------------------------------------------------------------------

// set by execute()
static Frame *handler_frame = nullptr;

namespace Types {

    static std::string _string_tostring(const Type *self, const bangra::Any &value) {
        return extract_string(value);
    }

    static std::string _symbol_tostring(const Type *self, const bangra::Any &value) {
        return extract_symbol_string(value);
    }

    template<typename T>
    static std::string _named_object_tostring(const Type *self, const bangra::Any &value) {
        return get_name(self) + ":" + get_symbol_name(((T *)value.ptr)->name);
    }

    static std::string _integer_tostring(const Type *self, const bangra::Any &value) {
        auto ivalue = extract_integer(value);
        if (self->width == 1) {
            return ivalue?"true":"false";
        }
        if (self->is_signed)
            return format("%" PRId64, ivalue);
        else
            return format("%" PRIu64, ivalue);
    }

    static std::string _real_tostring(const Type *self, const bangra::Any &value) {
        return format("%g", extract_real(value));
    }

    static std::string _list_tostring(const Type *self, const bangra::Any &value) {
        std::stringstream ss;
        bangra::streamValue(ss, value, 0, false);
        return ss.str();
    }

    static Ordering _list_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        auto x = extract_list(a);
        auto y = extract_list(b);
        while (true) {
            if (x == y) return Equal;
            else if (!x) return Less;
            else if (!y) return Greater;
            auto result = compare(x->at, y->at);
            if (result != Equal) return result;
            x = x->next;
            y = y->next;
        }
    }

    static bangra::Any type_pointer_at(const Type *self,
        const bangra::Any &value, const bangra::Any &index) {
        return at(pointer_element(self, value), index);
    }

    /*
    static bangra::Any _pointer_apply_type(const Type *self,
        const std::vector<bangra::Any> &args) {
    }
    */

    static Ordering value_pointer_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        auto y = is_pointer_type(b.type)?pointer_element(b.type, b):b;
        return compare(pointer_element(self, a), y);
    }

    static Ordering _struct_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        if (!eq(a.type, b.type)) return Unordered;
        return (a.ptr == b.ptr)?Equal:Unordered;
    }

    static Ordering value_none_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return (self == b.type)?Equal:Unordered;
    }

    static bangra::Any _integer_add(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return wrap(extract_integer(a) + extract_integer(b));
    }

    static bangra::Any _integer_sub(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return wrap(extract_integer(a) - extract_integer(b));
    }

    static bangra::Any _integer_mul(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return wrap(extract_integer(a) * extract_integer(b));
    }

    static bangra::Any _integer_div(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return wrap((double)extract_integer(a) / (double)extract_integer(b));
    }

    static bangra::Any _integer_neg(const Type *self, const bangra::Any &x) {
        return wrap(-extract_integer(x));
    }

    static bangra::Any _integer_rcp(const Type *self, const bangra::Any &x) {
        return wrap(1.0 / (double)extract_integer(x));
    }

    static Ordering _integer_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        auto x = extract_integer(a);
        auto y = extract_integer(b);
        return (Ordering)((y < x) - (x < y));
    }

    static bangra::Any _bool_not(const Type *self, const bangra::Any &x) {
        return wrap(!extract_bool(x));
    }

    static Ordering value_symbol_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        if (self != b.type) error("can not compare to type");
        auto x = *a.symbol;
        auto y = *b.symbol;
        return (x == y)?Equal:Unordered;
    }

    static bangra::Any _real_add(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return real(self, extract_real(a) + extract_real(b));
    }

    static bangra::Any _real_sub(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return real(self, extract_real(a) - extract_real(b));
    }

    static bangra::Any _real_mul(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return real(self, extract_real(a) * extract_real(b));
    }

    static bangra::Any _real_div(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return real(self, extract_real(a) / extract_real(b));
    }

    static bangra::Any _real_neg(const Type *self, const bangra::Any &x) {
        return real(self, -extract_real(x));
    }

    static bangra::Any _real_rcp(const Type *self, const bangra::Any &x) {
        return real(self, 1.0 / extract_real(x));
    }

    static Ordering _real_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        auto x = extract_real(a);
        auto y = extract_real(b);
        if (std::isnan(x + y)) return Unordered;
        return (Ordering)((y < x) - (x < y));
    }

    static std::string _none_tostring(const Type *self,
        const bangra::Any &value) {
        return "none";
    }

    static std::string _pointer_tostring(const Type *self,
        const bangra::Any &value) {
        return format("%s%s %s%s",
            ansi(ANSI_STYLE_COMMENT, "<").c_str(),
            ansi(ANSI_STYLE_OPERATOR, "&").c_str(),
            get_string(pointer_element(self, value)).c_str(),
            ansi(ANSI_STYLE_COMMENT, ">").c_str());
    }

    static std::string _type_tostring(
        const Type *self, const bangra::Any &value) {
        return "type:" + get_name((const Type *)value.ptr);
    }

    static bool type_array_eq(const Type *self, const Type *other) {
        return (other == TArray);
    }

    static bool type_vector_eq(const Type *self, const Type *other) {
        return (other == TVector);
    }

    static bool type_pointer_eq(const Type *self, const Type *other) {
        return (other == TPointer);
    }

    static bool type_tuple_eq(const Type *self, const Type *other) {
        return (other == TTuple);
    }

    static bool type_cfunction_eq(const Type *self, const Type *other) {
        return (other == TCFunction);
    }

    static bool type_integer_eq(const Type *self, const Type *other) {
        return (other == TInteger);
    }

    static bool type_real_eq(const Type *self, const Type *other) {
        return (other == TReal);
    }

    static bool type_struct_eq(const Type *self, const Type *other) {
        return (other == TStruct);
    }

    static bool type_enum_eq(const Type *self, const Type *other) {
        return (other == TEnum);
    }

    static bangra::Any _symbol_apply_type(const Type *self,
        const std::vector<bangra::Any> &args) {
        builtin_checkparams(args, 1, 1);
        return symbol(get_string(args[0]));
    }

    static bangra::Any _list_apply_type(const Type *self,
        const std::vector<bangra::Any> &args) {
        builtin_checkparams(args, 0, -1);
        auto result = List::create_from_array(args);
        set_anchor(result, get_anchor(handler_frame));
        return wrap(result);
    }

    static bangra::Any _parameter_apply_type(const Type *self,
        const std::vector<bangra::Any> &args) {
        builtin_checkparams(args, 1, 1);
        auto name = extract_symbol(args[0]);
        //return wrap_ptr(Types::PParameter, Parameter::create(name));
        return wrap(Parameter::create(name));
    }

    static bangra::Any type_array_at(const Type *self,
        const bangra::Any &value, const bangra::Any &vindex) {
        auto index = (size_t)extract_integer(vindex);
        auto padded_size = align(
            get_size(self->element_type), get_alignment(self->element_type));
        auto offset = padded_size * index;
        if (offset >= self->size) {
            error("index %zu out of array bounds (%zu)",
                index, self->size / padded_size);
            return const_none;
        }
        return wrap(self->element_type, (char *)value.ptr + offset);
    }

    static Type *__new_array_type(const Type *element, size_t size) {
        assert(element);
        Type *type = new_type("");
        type->count = size;
        type->op2[OP2_At] = type_array_at;
        type->element_type = element;
        auto padded_size = align(get_size(element), get_alignment(element));
        type->size = padded_size * type->count;
        type->alignment = element->alignment;
        return type;
    }

    static const Type *_new_array_type(const Type *element, size_t size) {
        assert(element);
        Type *type = __new_array_type(element, size);
        type->name = format("(%s %s %s)",
            ansi(ANSI_STYLE_KEYWORD, "@").c_str(),
            get_name(element).c_str(),
            ansi(ANSI_STYLE_NUMBER,
                format("%zu", size)).c_str());
        type->eq_type = type_array_eq;
        return type;
    }

    static const Type *_new_vector_type(const Type *element, size_t size) {
        assert(element);
        Type *type = __new_array_type(element, size);
        type->name = format("(%s %s %s)",
            ansi(ANSI_STYLE_KEYWORD, "vector").c_str(),
            get_name(element).c_str(),
            ansi(ANSI_STYLE_NUMBER,
                format("%zu", size)).c_str());
        type->eq_type = type_vector_eq;
        return type;
    }

    static const Type *_new_pointer_type(const Type *element) {
        assert(element);
        Type *type = new_type(
            format("(%s %s)",
                ansi(ANSI_STYLE_KEYWORD, "&").c_str(),
                get_name(element).c_str()));
        type->eq_type = type_pointer_eq;
        type->op2[OP2_At] = type_pointer_at;
        type->cmp = value_pointer_cmp;
        type->tostring = _pointer_tostring;
        type->element_type = element;
        type->size = ffi_type_pointer.size;
        type->alignment = ffi_type_pointer.alignment;
        return type;
    }

    static bangra::Any type_tuple_at(const Type *self,
        const bangra::Any &value, const bangra::Any &vindex) {
        auto index = (size_t)extract_integer(vindex);
        if (index >= self->types.size()) {
            error("index %zu out of tuple bounds (%zu)",
                index, self->types.size());
            return const_none;
        }
        auto offset = self->offsets[index];
        return wrap(self->types[index], (char *)value.ptr + offset);
    }

    static std::string _tuple_tostring(const Type *self, const bangra::Any &value) {
        std::stringstream ss;
        ss << ansi(ANSI_STYLE_COMMENT, "<");
        ss << ansi(ANSI_STYLE_KEYWORD, "tupleof");
        for (size_t i = 0; i < self->types.size(); i++) {
            auto offset = self->offsets[i];
            ss << " " << get_string(
                wrap(self->types[i], (char *)value.ptr + offset));
        }
        ss << ansi(ANSI_STYLE_COMMENT, ">");
        return ss.str();
    }

    static bangra::Any type_struct_at(const Type *self,
        const bangra::Any &value, const bangra::Any &vindex) {
        if (eq(vindex.type, Types::Symbol)) {
            auto key = extract_symbol(vindex);
            auto it = self->name_index_map.find(key);
            if (it == self->name_index_map.end()) {
                error("no such field in struct: %s",
                    get_symbol_name(key).c_str());
            }
            return type_tuple_at(self, value, wrap(it->second));
        } else {
            return type_tuple_at(self, value, vindex);
        }
    }

    static bangra::Any type_table_at(const Type *self,
        const bangra::Any &value, const bangra::Any &vindex) {
        auto table = extract_table(value);
        auto key = extract_symbol(vindex);
        return get_key(*table, key, const_none);
    }

    static bangra::Any type_list_at(const Type *self,
        const bangra::Any &value, const bangra::Any &vindex) {
        auto slist = extract_list(value);
        if (!slist) {
            //error("can not index into empty list");
            return wrap((bangra::List*)nullptr);
        }
        auto index = (size_t)extract_integer(vindex);
        if (index == 0) {
            return slist->at;
        } else {
            const bangra::List *result = slist;
            while ((index != 0) && result) {
                --index;
                result = result->next;
            }
            /*
            if (index != 0) {
                error("index %zu out of list bounds", index);
            }
            */
            return wrap(result);
        }
    }

    static size_t _string_length(const Type *self, const bangra::Any &value) {
        return value.str->count;
    }

    static bangra::Any _string_slice(
        const Type *self, const bangra::Any &value, size_t i0, size_t i1) {
        return wrap(Types::String, alloc_string(
            value.str->ptr + i0, i1 - i0));
    }

    static bangra::Any _string_at(const Type *self,
        const bangra::Any &value, const bangra::Any &vindex) {
        auto index = (size_t)extract_integer(vindex);
        if (index >= value.str->count) {
            error("index %zu out of string bounds (%zu)",
                index, value.str->count);
        }
        return _string_slice(self, value, index, index + 1);
    }

    static int zucmp(size_t a, size_t b) {
        return (b < a) - (a < b);
    }

    static Ordering value_string_cmp(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        if (b.type != self)
            error("incomparable values");
        int c = zucmp(a.str->count, b.str->count);
        if (!c) {
            c = memcmp(a.str->ptr, b.str->ptr, a.str->count);
        }
        return (Ordering)c;
    }

    /*
    static bangra::Any _symbol_slice(
        const Type *self, const bangra::Any &value, size_t i0, size_t i1) {
        return wrap(Types::Symbol, alloc_string(
            value.str->ptr + i0, i1 - i0));
    }
    */

    static size_t _tuple_length(const Type *self, const bangra::Any &value) {
        return self->types.size();
    }

    static bangra::Any _string_concat(const Type *self,
        const bangra::Any &a, const bangra::Any &b) {
        return wrap(extract_string(a) + extract_string(b));
    }

    static void _set_field_names(Type *type, const std::vector<std::string> &names) {
        for (size_t i = 0; i < names.size(); ++i) {
            auto name = get_symbol(names[i]);
            if (name != SYM_Unnamed) {
                type->name_index_map[name] = i;
            }
        }
    }

    static void _set_field_names(Type *type, const std::vector<bangra::Symbol> &names) {
        for (size_t i = 0; i < names.size(); ++i) {
            auto name = names[i];
            if (name != SYM_Unnamed) {
                type->name_index_map[name] = i;
            }
        }
    }

    static void _set_struct_field_types(Type *type, const std::vector<const Type *> &types) {
        type->types = types;
        size_t offset = 0;
        size_t max_alignment = 1;
        for (auto &element : types) {
            size_t size = get_size(element);
            size_t alignment = get_alignment(element);
            max_alignment = std::max(max_alignment, alignment);
            offset = align(offset, alignment);
            type->offsets.push_back(offset);
            offset += size;
        }
        type->size = align(offset, max_alignment);
        type->alignment = max_alignment;
    }

    static void _set_union_field_types(Type *type, const std::vector<const Type *> &types) {
        type->types = types;
        size_t max_size = 0;
        size_t max_alignment = 1;
        for (auto &element : types) {
            size_t size = get_size(element);
            size_t alignment = get_alignment(element);
            max_size = std::max(max_size, size);
            max_alignment = std::max(max_alignment, alignment);
        }
        type->size = align(max_size, max_alignment);
        type->alignment = max_alignment;
    }

    static const Type *_new_tuple_type(std::vector<const Type *> types) {
        std::stringstream ss;
        ss << "(";
        ss << ansi(ANSI_STYLE_KEYWORD, "tuple");
        for (auto &element : types) {
            ss << " " << get_name(element);
        }
        ss << ")";
        Type *type = new_type(ss.str());
        type->eq_type = type_tuple_eq;
        type->op2[OP2_At] = type_tuple_at;
        type->length = _tuple_length;
        type->tostring = _tuple_tostring;
        _set_struct_field_types(type, types);
        return type;
    }

    static const Type *_new_cfunction_type(
        const Type *result, const Type *parameters, bool vararg) {
        assert(eq(parameters, TTuple));
        std::stringstream ss;
        ss << "(";
        ss << ansi(ANSI_STYLE_KEYWORD, "cfunction");
        ss << " " << get_name(result);
        ss << " " << get_name(parameters);
        ss << " " << ansi(ANSI_STYLE_KEYWORD, vararg?"true":"false") << ")";
        Type *type = new_type(ss.str());
        type->eq_type = type_cfunction_eq;
        type->is_vararg = vararg;
        type->result_type = result;
        type->types = { parameters };
        type->size = ffi_type_pointer.size;
        type->alignment = ffi_type_pointer.alignment;
        return type;
    }

    static const Type *_new_integer_type(size_t width, bool signed_) {
        ffi_type *itype = nullptr;
        if (signed_) {
            switch (width) {
                case 1: itype = &ffi_type_uint8; break;
                case 8: itype = &ffi_type_sint8; break;
                case 16: itype = &ffi_type_sint16; break;
                case 32: itype = &ffi_type_sint32; break;
                case 64: itype = &ffi_type_sint64; break;
                default: assert(false && "invalid width"); break;
            }
        } else {
            switch (width) {
                case 1: itype = &ffi_type_uint8; break;
                case 8: itype = &ffi_type_uint8; break;
                case 16: itype = &ffi_type_uint16; break;
                case 32: itype = &ffi_type_uint32; break;
                case 64: itype = &ffi_type_uint64; break;
                default: assert(false && "invalid width"); break;
            }
        }

        Type *type = new_type(
            (width == 1)?"bool":
                format("%sint%zu", signed_?"":"u", width));
        type->eq_type = type_integer_eq;

        type->op2[OP2_Add] = type->rop2[OP2_Add] = _integer_add;
        type->op2[OP2_Sub] = _integer_sub;
        type->op2[OP2_Mul] = type->rop2[OP2_Mul] = _integer_mul;
        type->op2[OP2_Div] = _integer_div;
        type->op1[OP1_Neg] = _integer_neg;
        type->op1[OP1_Rcp] = _integer_rcp;

        if (width == 1) {
            type->op1[OP1_BoolNot] = _bool_not;
        }

        type->cmp = _integer_cmp;

        type->width = width;
        type->tostring = _integer_tostring;
        type->is_signed = signed_;
        type->size = itype->size;
        type->alignment = itype->alignment;
        return type;
    }

    static const Type *_new_real_type(size_t width) {
        ffi_type *itype = nullptr;
        switch (width) {
            case 16: itype = &ffi_type_uint16; break;
            case 32: itype = &ffi_type_float; break;
            case 64: itype = &ffi_type_double; break;
            default: assert(false && "invalid width"); break;
        }

        Type *type = new_type(format("real%zu", width));
        type->tostring = _real_tostring;
        type->eq_type = type_real_eq;

        type->op2[OP2_Add] = type->rop2[OP2_Add] = _real_add;
        type->op2[OP2_Sub] = _real_sub;
        type->op2[OP2_Mul] = type->rop2[OP2_Mul] = _real_mul;
        type->op2[OP2_Div] = _real_div;
        type->op1[OP1_Neg] = _real_neg;
        type->op1[OP1_Rcp] = _real_rcp;

        type->cmp = _real_cmp;

        type->width = width;
        type->size = itype->size;
        type->alignment = itype->alignment;
        return type;
    }

    static Type *Struct(const std::string &name, bool builtin = false) {
        Type *type = new_type("");
        if (builtin) {
            type->name = name;
        } else {
            std::stringstream ss;
            ss << "(";
            ss << ansi(ANSI_STYLE_KEYWORD, "struct");
            ss << " " << quoteReprString(name);
            ss << " " << type;
            ss << ")";
            type->name = ss.str();
        }
        type->eq_type = type_struct_eq;
        type->op2[OP2_At] = type_struct_at;
        type->cmp = _struct_cmp;
        type->length = _tuple_length;
        return type;
    }

    static Type *Enum(const std::string &name) {
        std::stringstream ss;
        ss << "(";
        ss << ansi(ANSI_STYLE_KEYWORD, "enum");
        ss << " " << quoteReprString(name);
        ss << ")";
        Type *type = new_type(ss.str());
        type->eq_type = type_enum_eq;
        return type;
    }

    static void initTypes() {
        Type *tmp = Struct("type", true);
        tmp->tostring = _type_tostring;
        TType = tmp;
        PType = Pointer(TType);

        TArray = Struct("array", true);
        TVector = Struct("vector", true);
        TTuple = Struct("tuple", true);
        TPointer = Struct("pointer", true);
        TCFunction = Struct("cfunction", true);
        TInteger = Struct("integer", true);
        TReal = Struct("real", true);
        TStruct = Struct("struct", true);
        TEnum = Struct("enum", true);

        Any = Struct("Any", true);
        AnchorRef = Struct("Anchor", true);

        tmp = Struct("None", true);
        tmp->cmp = value_none_cmp;
        tmp->tostring = _none_tostring;
        None = tmp;
        const_none = make_any(Types::None);
        const_none.ptr = nullptr;

        Bool = Integer(1, true);

        I8 = Integer(8, true);
        I16 = Integer(16, true);
        I32 = Integer(32, true);
        I64 = Integer(64, true);

        U8 = Integer(8, false);
        U16 = Integer(16, false);
        U32 = Integer(32, false);
        U64 = Integer(64, false);

        SizeT = (sizeof(size_t) == 8)?U64:U32;

        R16 = Real(16);
        R32 = Real(32);
        R64 = Real(64);

        struct _string_alignment { char c; bangra::String s; };
        struct _symbol_alignment { char c; bangra::Symbol s; };

        tmp = Struct("String", true);
        tmp->tostring = _string_tostring;
        tmp->length = _string_length;
        tmp->slice = _string_slice;
        tmp->op2[OP2_At] = _string_at;
        tmp->op2[OP2_Concat] = _string_concat;
        tmp->cmp = value_string_cmp;
        tmp->size = sizeof(bangra::String);
        tmp->alignment = offsetof(_string_alignment, s);
        String = tmp;

        tmp = Struct("Symbol", true);
        tmp->tostring = _symbol_tostring;
        tmp->cmp = value_symbol_cmp;
        tmp->apply_type = _symbol_apply_type;
        tmp->size = sizeof(bangra::Symbol);
        tmp->alignment = offsetof(_symbol_alignment, s);
        Symbol = tmp;

        tmp = Struct("List", true);
        tmp->op2[OP2_At] = type_list_at;
        tmp->cmp = _list_cmp;
        //tmp->apply_type = _list_apply_type;
        //tmp->size = sizeof(bangra::List);
        //tmp->alignment = offsetof(_list_alignment, s);
        _List = tmp;
        tmp = const_cast<Type *>(Pointer(_List));
        tmp->apply_type = _list_apply_type;
        tmp->tostring = _list_tostring;
        PList = tmp;

        tmp = Struct("Table", true);
        tmp->op2[OP2_At] = type_table_at;
        _Table = tmp;
        PTable = Pointer(_Table);

        tmp = Struct("Builtin", true);
        tmp->tostring = _named_object_tostring<Builtin>;
        PBuiltin = Pointer(tmp);

        tmp = Struct("BuiltinFlow", true);
        tmp->tostring = _named_object_tostring<BuiltinMacro>;
        PBuiltinFlow = Pointer(tmp);

        PFlow = Pointer(Struct("Flow", true));

        tmp = Struct("SpecialForm", true);
        tmp->tostring = _named_object_tostring<SpecialForm>;
        PSpecialForm = Pointer(tmp);

        tmp = Struct("BuiltinMacro", true);
        tmp->tostring = _named_object_tostring<BuiltinMacro>;
        PBuiltinMacro = Pointer(tmp);

        PFrame = Pointer(Struct("Frame", true));

        {
            std::vector<const Type *> types = { PFlow, SizeT, PType, Symbol };
            std::vector<std::string> names = { "flow", "index", "type", "name" };
            tmp = Struct("Parameter", true);
            tmp->tostring = _named_object_tostring<Parameter>;
            _set_struct_field_types(tmp, types);
            _set_field_names(tmp, names);

            PParameter = Pointer(tmp);
            tmp = const_cast<Type *>(PParameter);
            tmp->apply_type = _parameter_apply_type;
        }

        {
            std::vector<const Type *> types = { PFlow, PFrame };
            std::vector<std::string> names = { "entry", "frame" };
            tmp = Struct("Closure", true);
            _set_struct_field_types(tmp, types);
            _set_field_names(tmp, names);
            PClosure = Pointer(tmp);
        }

        PMacro = Pointer(Struct("Macro", true));

        Rawstring = Pointer(I8);
    }

} // namespace Types

static void initConstants() {}

//------------------------------------------------------------------------------
// S-EXPR LEXER / TOKENIZER
//------------------------------------------------------------------------------

typedef enum {
    token_none = -1,
    token_eof = 0,
    token_open = '(',
    token_close = ')',
    token_square_open = '[',
    token_square_close = ']',
    token_curly_open = '{',
    token_curly_close = '}',
    token_string = '"',
    token_symbol = 'S',
    token_escape = '\\',
    token_statement = ';',
    token_integer = 'I',
    token_real = 'R',
} Token;

const char symbol_terminators[]  = "()[]{}\"';#";
const char integer_terminators[] = "()[]{}\"';#";
const char real_terminators[]    = "()[]{}\"';#";

struct Lexer {
    const char *path;
    const char *input_stream;
    const char *eof;
    const char *cursor;
    const char *next_cursor;
    // beginning of line
    const char *line;
    // next beginning of line
    const char *next_line;

    int lineno;
    int next_lineno;

    int base_offset;

    int token;
    const char *string;
    int string_len;
    int64_t integer;
    bool is_unsigned;
    float real;

    std::string error_string;

    Lexer() {}

    void init (const char *input_stream, const char *eof, const char *path, int offset = 0) {
        if (eof == NULL) {
            eof = input_stream + strlen(input_stream);
        }

        this->base_offset = offset;
        this->path = path;
        this->input_stream = input_stream;
        this->eof = eof;
        this->next_cursor = input_stream;
        this->next_lineno = 1;
        this->next_line = input_stream;
        this->error_string.clear();
    }

    void dumpLine() {
        dumpFileLine(path, offset());
    }

    int offset () {
        return base_offset + (cursor - input_stream);
    }

    int column () {
        return cursor - line + 1;
    }

    void initAnchor(Anchor &anchor) {
        anchor.path = path;
        anchor.lineno = lineno;
        anchor.column = column();
        anchor.offset = offset();
    }

    Anchor getAnchor() {
        Anchor anchor;
        initAnchor(anchor);
        return anchor;
    }

    const Anchor *newAnchor() {
        Anchor *anchor = new Anchor();
        initAnchor(*anchor);
        return anchor;
    }

    void error( const char *format, ... ) {
        va_list args;
        va_start (args, format);
        size_t size = vsnprintf(nullptr, 0, format, args);
        va_end (args);
        error_string.resize(size);
        va_start (args, format);
        vsnprintf( &error_string[0], size + 1, format, args );
        va_end (args);
        token = token_eof;
    }

    char next() {
        return *next_cursor++;
    }

    bool verifyGoodTaste(char c) {
        if (c == '\t') {
            error("please use spaces instead of tabs.");
            return false;
        }
        return true;
    }

    void readSymbol () {
        bool escape = false;
        while (true) {
            if (next_cursor == eof) {
                break;
            }
            char c = next();
            if (escape) {
                if (c == '\n') {
                    ++next_lineno;
                    next_line = next_cursor;
                }
                // ignore character
                escape = false;
            } else if (c == '\\') {
                // escape
                escape = true;
            } else if (isspace(c)
                || strchr(symbol_terminators, c)) {
                -- next_cursor;
                break;
            }
        }
        string = cursor;
        string_len = next_cursor - cursor;
    }

    void readSingleSymbol () {
        string = cursor;
        string_len = next_cursor - cursor;
    }

    void readString (char terminator) {
        bool escape = false;
        while (true) {
            if (next_cursor == eof) {
                error("unterminated sequence");
                break;
            }
            char c = next();
            if (c == '\n') {
                ++next_lineno;
                next_line = next_cursor;
            }
            if (escape) {
                // ignore character
                escape = false;
            } else if (c == '\\') {
                // escape
                escape = true;
            } else if (c == terminator) {
                break;
            }
        }
        string = cursor;
        string_len = next_cursor - cursor;
    }

    bool readInteger() {
        char *end;
        errno = 0;
        integer = std::strtoll(cursor, &end, 0);
        if ((end == cursor)
            || (errno == ERANGE)
            || (end >= eof)
            || (!isspace(*end) && !strchr(integer_terminators, *end)))
            return false;
        is_unsigned = false;
        next_cursor = end;
        return true;
    }

    bool readUInteger() {
        char *end;
        errno = 0;
        integer = std::strtoull(cursor, &end, 0);
        if ((end == cursor)
            || (errno == ERANGE)
            || (end >= eof)
            || (!isspace(*end) && !strchr(integer_terminators, *end)))
            return false;
        is_unsigned = true;
        next_cursor = end;
        return true;
    }

    bool readReal() {
        char *end;
        errno = 0;
        real = std::strtof(cursor, &end);
        if ((end == cursor)
            || (errno == ERANGE)
            || (end >= eof)
            || (!isspace(*end) && !strchr(real_terminators, *end)))
            return false;
        next_cursor = end;
        return true;
    }

    int readToken () {
        lineno = next_lineno;
        line = next_line;
        cursor = next_cursor;
        while (true) {
            if (next_cursor == eof) {
                token = token_eof;
                break;
            }
            char c = next();
            if (!verifyGoodTaste(c)) break;
            if (c == '\n') {
                ++next_lineno;
                next_line = next_cursor;
            }
            if (isspace(c)) {
                lineno = next_lineno;
                line = next_line;
                cursor = next_cursor;
            } else if (c == '#') {
                readString('\n');
                // and continue
                lineno = next_lineno;
                line = next_line;
                cursor = next_cursor;
            } else if (c == '(') {
                token = token_open;
                break;
            } else if (c == ')') {
                token = token_close;
                break;
            } else if (c == '[') {
                token = token_square_open;
                break;
            } else if (c == ']') {
                token = token_square_close;
                break;
            } else if (c == '{') {
                token = token_curly_open;
                break;
            } else if (c == '}') {
                token = token_curly_close;
                break;
            } else if (c == '\\') {
                token = token_escape;
                break;
            } else if (c == '"') {
                token = token_string;
                readString(c);
                break;
            } else if (c == '\'') {
                token = token_string;
                readString(c);
                break;
            } else if (c == ';') {
                token = token_statement;
                break;
            } else if (readInteger() || readUInteger()) {
                token = token_integer;
                break;
            } else if (readReal()) {
                token = token_real;
                break;
            } else {
                token = token_symbol;
                readSymbol();
                break;
            }
        }
        return token;
    }

    Any getAsString() {
        // TODO: anchor
        auto result = make_any(Types::String);
        auto s = alloc_string(string + 1, string_len - 2);
        unescape(*s);
        result.str = s;
        return result;
    }

    Any getAsSymbol() {
        // TODO: anchor
        std::string s(string, string_len);
        inplace_unescape(const_cast<char *>(s.c_str()));
        return bangra::symbol(s);
    }

    Any getAsInteger() {
        // TODO: anchor
        size_t width;
        if (is_unsigned) {
            width = ((uint64_t)integer > (uint64_t)INT_MAX)?64:32;
        } else {
            width =
                ((integer < (int64_t)INT_MIN) || (integer > (int64_t)INT_MAX))?64:32;
        }
        auto type = Types::Integer(width, !is_unsigned);
        return bangra::integer(type, this->integer);
    }

    Any getAsReal() {
        return bangra::real(Types::R32, this->real);
    }

};

//------------------------------------------------------------------------------
// S-EXPR PARSER
//------------------------------------------------------------------------------

struct Parser {
    Lexer lexer;

    Anchor error_origin;
    Anchor parse_origin;
    std::string error_string;
    int errors;

    Parser() :
        errors(0)
        {}

    void init() {
        error_string.clear();
    }

    void error( const char *format, ... ) {
        ++errors;
        lexer.initAnchor(error_origin);
        parse_origin = error_origin;
        va_list args;
        va_start (args, format);
        size_t size = vsnprintf(nullptr, 0, format, args);
        va_end (args);
        error_string.resize(size);
        va_start (args, format);
        vsnprintf( &error_string[0], size + 1, format, args );
        va_end (args);
    }

    struct ListBuilder {
    protected:
        Lexer &lexer;
        const List *prev;
        const List *eol;
        Anchor anchor;
    public:
        ListBuilder(Lexer &lexer_) :
            lexer(lexer_),
            prev(nullptr),
            eol(nullptr) {
            anchor = lexer.getAnchor();
        }

        const Anchor &getAnchor() {
            return anchor;
        }

        /*
        const List *getPrev() {
            return prev;
        }

        void setPrev(const List *prev) {
            this->prev = prev;
        }
        */

        void append(const Any &value) {
            this->prev = List::create(value, this->prev, get_anchor(value));
        }

        void resetStart() {
            eol = prev;
        }

        bool split() {
            // if we haven't appended anything, that's an error
            if (!prev) {
                return false;
            }
            // reverse what we have, up to last split point and wrap result
            // in cell
            prev = List::create(
                wrap(reverse_list_inplace(prev, eol)), eol, lexer.newAnchor());
            resetStart();
            return true;
        }

        bool isSingleResult() {
            return prev && !prev->next;
        }

        Any getSingleResult() {
            return this->prev?this->prev->at:const_none;
        }

        const List *getResult() {
            return reverse_list_inplace(this->prev);
        }
    };

    // parses a list to its terminator and returns a handle to the first cell
    const List *parseList(int end_token) {
        ListBuilder builder(lexer);
        lexer.readToken();
        while (true) {
            if (lexer.token == end_token) {
                break;
            } else if (lexer.token == token_escape) {
                int column = lexer.column();
                lexer.readToken();
                auto elem = parseNaked(column, end_token);
                if (errors) return nullptr;
                builder.append(elem);
            } else if (lexer.token == token_eof) {
                error("missing closing bracket");
                // point to beginning of list
                error_origin = builder.getAnchor();
                return nullptr;
            } else if (lexer.token == token_statement) {
                if (!builder.split()) {
                    error("empty expression");
                    return nullptr;
                }
                lexer.readToken();
            } else {
                auto elem = parseAny();
                if (errors) return nullptr;
                builder.append(elem);
                lexer.readToken();
            }
        }
        return builder.getResult();
    }

    // parses the next sequence and returns it wrapped in a cell that points
    // to prev
    Any parseAny () {
        assert(lexer.token != token_eof);
        auto anchor = lexer.newAnchor();
        Any result = const_none;
        if (lexer.token == token_open) {
            result = wrap(parseList(token_close));
        } else if (lexer.token == token_square_open) {
            const List *list = parseList(token_square_close);
            if (errors) return const_none;
            Any sym = symbol("[");
            result = wrap(List::create(sym, list, anchor));
        } else if (lexer.token == token_curly_open) {
            const List *list = parseList(token_curly_close);
            if (errors) return const_none;
            Any sym = symbol("{");
            result = wrap(List::create(sym, list, anchor));
        } else if ((lexer.token == token_close)
            || (lexer.token == token_square_close)
            || (lexer.token == token_curly_close)) {
            error("stray closing bracket");
        } else if (lexer.token == token_string) {
            result = lexer.getAsString();
        } else if (lexer.token == token_symbol) {
            result = lexer.getAsSymbol();
        } else if (lexer.token == token_integer) {
            result = lexer.getAsInteger();
        } else if (lexer.token == token_real) {
            result = lexer.getAsReal();
        } else {
            error("unexpected token: %c (%i)", *lexer.cursor, (int)*lexer.cursor);
        }
        if (errors) return const_none;
        set_anchor(result, anchor);
        return result;
    }

    Any parseRoot () {
        ListBuilder builder(lexer);

        while (lexer.token != token_eof) {
            if (lexer.token == token_none)
                break;
            auto elem = parseNaked(1, token_none);
            if (errors) return const_none;
            builder.append(elem);
        }

        return wrap(builder.getResult());
    }

    Any parseNaked (int column = 0, int end_token = token_none) {
        int lineno = lexer.lineno;

        bool escape = false;
        int subcolumn = 0;

        ListBuilder builder(lexer);

        while (lexer.token != token_eof) {
            if (lexer.token == end_token) {
                break;
            } else if (lexer.token == token_escape) {
                escape = true;
                lexer.readToken();
                if (lexer.lineno <= lineno) {
                    error("escape character is not at end of line");
                    parse_origin = builder.getAnchor();
                    return const_none;
                }
                lineno = lexer.lineno;
            } else if (lexer.lineno > lineno) {
                if (subcolumn == 0) {
                    subcolumn = lexer.column();
                } else if (lexer.column() != subcolumn) {
                    error("indentation mismatch");
                    parse_origin = builder.getAnchor();
                    return const_none;
                }
                if (column != subcolumn) {
                    if ((column + 4) != subcolumn) {
                        //printf("%i %i\n", column, subcolumn);
                        error("indentations must nest by 4 spaces.");
                        return const_none;
                    }
                }

                escape = false;
                builder.resetStart();
                lineno = lexer.lineno;
                // keep adding elements while we're in the same line
                while ((lexer.token != token_eof)
                        && (lexer.token != end_token)
                        && (lexer.lineno == lineno)) {
                    auto elem = parseNaked(
                        subcolumn, end_token);
                    if (errors) return const_none;
                    builder.append(elem);
                }
            } else if (lexer.token == token_statement) {
                if (!builder.split()) {
                    error("empty expression");
                    return const_none;
                }
                lexer.readToken();
                // if we are in the same line and there was no preceding ":",
                // continue in parent
                if (lexer.lineno == lineno)
                    break;
            } else {
                auto elem = parseAny();
                if (errors) return const_none;
                builder.append(elem);
                lineno = lexer.next_lineno;
                lexer.readToken();
            }

            if ((!escape || (lexer.lineno > lineno))
                && (lexer.column() <= column)) {
                break;
            }
        }

        if (builder.isSingleResult()) {
            return builder.getSingleResult();
        } else {
            return wrap(builder.getResult());
        }
    }

    Any parseMemory (
        const char *input_stream, const char *eof, const char *path, int offset = 0) {
        init();
        lexer.init(input_stream, eof, path, offset);

        lexer.readToken();

        auto result = parseRoot();

        if (error_string.empty() && !lexer.error_string.empty()) {
            error_string = lexer.error_string;
            lexer.initAnchor(error_origin);
            parse_origin = error_origin;
        }

        if (!error_string.empty()) {
            printf("%s:%i:%i: error: %s\n",
                error_origin.path,
                error_origin.lineno,
                error_origin.column,
                error_string.c_str());
            dumpFileLine(path, error_origin.offset);
            if (!(parse_origin == error_origin)) {
                printf("%s:%i:%i: while parsing expression\n",
                    parse_origin.path,
                    parse_origin.lineno,
                    parse_origin.column);
                dumpFileLine(path, parse_origin.offset);
            }
            return const_none;
        }

        return strip(result);
    }

    Any parseFile (const char *path) {
        auto file = MappedFile::open(path);
        if (file) {
            return parseMemory(
                file->strptr(), file->strptr() + file->size(),
                path);
        } else {
            fprintf(stderr, "unable to open file: %s\n", path);
            return const_none;
        }
    }


};

//------------------------------------------------------------------------------
// FOREIGN FUNCTION INTERFACE
//------------------------------------------------------------------------------

// TODO: libffi based calls

struct FFI {
    std::unordered_map<const Type *, ffi_type *> ffi_types;

    FFI() {}

    ~FFI() {}

    ffi_type *new_type() {
        ffi_type *result = (ffi_type *)malloc(sizeof(ffi_type));
        memset(result, 0, sizeof(ffi_type));
        return result;
    }

    ffi_type *create_ffi_type(const Type *type) {
        if (is_none_type(type)) {
            return &ffi_type_void;
        } else if (is_integer_type(type)) {
            auto width = get_width(type);
            if (is_signed(type)) {
                switch(width) {
                    case 8: return &ffi_type_sint8;
                    case 16: return &ffi_type_sint16;
                    case 32: return &ffi_type_sint32;
                    case 64: return &ffi_type_sint64;
                    default: break;
                }
            } else {
                switch(width) {
                    case 8: return &ffi_type_uint8;
                    case 16: return &ffi_type_uint16;
                    case 32: return &ffi_type_uint32;
                    case 64: return &ffi_type_uint64;
                    default: break;
                }
            }
        }
        error("can not translate %s to FFI type",
            get_name(type).c_str());
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

    Any runFunction(
        const Any &func, const std::vector<Any> &args) {
        assert(is_pointer_type(func.type));
        auto ftype = get_element_type(func.type);
        assert(is_cfunction_type(ftype));
        auto count = get_parameter_count(ftype);
        if (count != args.size()) {
            error("parameter count mismatch");
        }
        if (is_vararg(ftype)) {
            error("vararg functions not supported yet");
        }

        auto rtype = get_result_type(ftype);

        ffi_cif cif;
        ffi_type *argtypes[count];
        for (size_t i = 0; i < count; ++i) {
            auto ptype = get_parameter_type(ftype, i);
            if (!eq(ptype, args[i].type)) {
                error("%s type expected, %s provided",
                    get_name(ptype).c_str(),
                    get_name(args[i].type).c_str());
            }
            argtypes[i] = get_ffi_type(ptype);
        }
        auto prep_result = ffi_prep_cif(
            &cif, FFI_DEFAULT_ABI, count, get_ffi_type(rtype), argtypes);
        assert(prep_result == FFI_OK);

        Any result = make_any(rtype);
        // TODO: align properly
        void *rvalue = malloc(get_size(rtype));
        result.ptr = rvalue;

        void *avalues[count];
        for (size_t i = 0; i < count; ++i) {
            avalues[i] = const_cast<void *>(args[i].ptr);
        }

        ffi_call(&cif, FFI_FN(extract_ptr<void>(func)), rvalue, avalues);

        return result;
    }

};

static FFI *ffi;

//------------------------------------------------------------------------------
// INTERPRETER
//------------------------------------------------------------------------------

typedef std::vector<Any> ILValueArray;

Any evaluate(size_t argindex, const Frame *frame, const Any &value) {
    if (eq(value.type, Types::PParameter)) {
        auto param = *value.pparameter;
        auto cont = param->parent;
        if (cont) {
            // parameter is bound - attempt resolve
            const Frame *ptr = frame;
            while (ptr) {
                auto it = ptr->map.find(cont);
                if (it != ptr->map.end()) {
                    auto &values = it->second;
                    assert(param->index < values.size());
                    return values[param->index];
                }
                ptr = ptr->parent;
            }
        }
        // return unbound value
        return value;
    } else if (eq(value.type, Types::PFlow)) {
        if (argindex == 0)
            // no closure creation required
            return value;
        else
            // create closure
            return wrap_ptr(Types::PClosure,
                create_closure(*value.pflow, const_cast<Frame *>(frame)));
    }
    return value;
}

void print_stack_frames(Frame *frame) {
    if (!frame) return;
    print_stack_frames(frame->parent);
    auto anchor = get_anchor(frame);
    if (anchor) {
        Anchor::printMessage(anchor, "while evaluating");
    }
}

static bool trace_arguments = false;
Any execute(std::vector<Any> arguments) {

    Frame *frame = Frame::create();
    frame->idx = 0;

    auto retcont = Flow::create(1);
    // add special flow as return function
    arguments.push_back(wrap_ptr(Types::PFlow, retcont));

    std::vector<Any> active_arguments;

continue_execution:
    try {
        while (true) {
            active_arguments = arguments;

            assert(arguments.size() >= 1);
#ifdef BANGRA_DEBUG_IL
            std::cout << frame->getRefRepr();
            for (size_t i = 0; i < arguments.size(); ++i) {
                std::cout << " ";
                std::cout << getRefRepr(arguments[i]);
            }
            std::cout << "\n";
            fflush(stdout);
#endif
            Any callee = arguments[0];
            if (eq(callee.type, Types::PFlow)
                && (*callee.pflow == retcont)) {
                if (arguments.size() >= 2) {
                    return arguments[1];
                } else {
                    return const_none;
                }
            }

            if (eq(callee.type, Types::PClosure)) {
                auto closure = *callee.pclosure;

                frame = closure->frame;
                callee = wrap_ptr(Types::PFlow, closure->entry);
            }

            if (eq(callee.type, Types::PFlow)) {
                auto flow = *callee.pflow;
                assert(get_anchor(flow));

                arguments.erase(arguments.begin());
                if (arguments.size() > 0) {
                    if (frame->map.count(flow)) {
                        frame = Frame::create(frame);
                    }
                    frame->map[flow] = arguments;
                }
                set_anchor(frame, get_anchor(flow));
                if (trace_arguments) {
                    auto anchor = get_anchor(flow);
                    Anchor::printMessage(anchor, "trace");
                }

                assert(!flow->arguments.empty());
                size_t argcount = flow->arguments.size();
                arguments.resize(argcount);
                for (size_t i = 0; i < argcount; ++i) {
                    arguments[i] = evaluate(i, frame,
                        flow->arguments[i]);
                }
            } else if (eq(callee.type, Types::PBuiltin)) {
                auto cb = *callee.pbuiltin;
                Any closure = arguments.back();
                arguments.pop_back();
                arguments.erase(arguments.begin());
                auto _oldframe = handler_frame;
                handler_frame = frame;
                Any result = cb->handler(arguments);
                handler_frame = _oldframe;
                // generate fitting resume
                arguments.resize(2);
                arguments[0] = closure;
                arguments[1] = result;
            } else if (eq(callee.type, Types::PBuiltinFlow)) {
                auto cb = *callee.pbuiltin_flow;
                auto _oldframe = handler_frame;
                handler_frame = frame;
                arguments = cb->handler(arguments);
                handler_frame = _oldframe;
            } else if (eq(callee.type, Types::PType)) {
                auto cb = *callee.ptype;
                Any closure = arguments.back();
                arguments.pop_back();
                arguments.erase(arguments.begin());
                auto _oldframe = handler_frame;
                handler_frame = frame;
                Any result = cb->apply_type(cb, arguments);
                handler_frame = _oldframe;
                // generate fitting resume
                arguments.resize(2);
                arguments[0] = closure;
                arguments[1] = result;
            } else if (
                is_pointer_type(callee.type)
                    && is_cfunction_type(get_element_type(callee.type))) {
                Any closure = arguments.back();
                arguments.pop_back();
                arguments.erase(arguments.begin());
                Any result = ffi->runFunction(callee, arguments);
                // generate fitting resume
                arguments.resize(2);
                arguments[0] = closure;
                arguments[1] = result;
            } else {
                error("can not apply %s",
                    get_name(callee.type).c_str());
            }
        }
    } catch (const Any &any) {
        printf("while evaluating arguments:\n");
        for (size_t i = 0; i < active_arguments.size(); ++i) {
            printf("  #%zu: ", i);
            printValue(active_arguments[i], 0, true);
        }
        printf("\n");

        print_stack_frames(frame);

        std::cout << ansi(ANSI_STYLE_ERROR, "error:") << " " << get_string(any) << "\n";

        //goto continue_execution;
        fflush(stdout);

        throw_any(any);
    }

    return const_none;
}

//------------------------------------------------------------------------------
// C module utility functions
//------------------------------------------------------------------------------

static void *open_c_module(const char *name) {
    // todo: windows
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

static void *get_c_symbol(void *handle, const char *name) {
    // todo: windows
    return dlsym(handle, name);
}

//------------------------------------------------------------------------------
// C BRIDGE (CLANG)
//------------------------------------------------------------------------------

class CVisitor : public clang::RecursiveASTVisitor<CVisitor> {
public:
    Table *dest;
    clang::ASTContext *Context;
    std::unordered_map<clang::RecordDecl *, bool> record_defined;
    std::unordered_map<clang::EnumDecl *, bool> enum_defined;
    std::unordered_map<const char *, char *> path_cache;
    std::unordered_map<std::string, Type *> named_structs;
    std::unordered_map<std::string, Type *> named_enums;
    std::unordered_map<std::string, const Type *> typedefs;

    CVisitor() : Context(NULL) {
    }

    Anchor anchorFromLocation(clang::SourceLocation loc) {
        Anchor anchor;
        auto &SM = Context->getSourceManager();

        auto PLoc = SM.getPresumedLoc(loc);

        if (PLoc.isValid()) {
            auto fname = PLoc.getFilename();
            // get resident path by pointer
            char *rpath = path_cache[fname];
            if (!rpath) {
                rpath = strdup(fname);
                path_cache[fname] = rpath;
            }

            anchor.path = rpath;
            anchor.lineno = PLoc.getLine();
            anchor.column = PLoc.getColumn();
        }

        return anchor;
    }

    /*
    static ValueRef fixAnchor(Anchor &anchor, ValueRef value) {
        if (value && !value->anchor.isValid() && anchor.isValid()) {
            value->anchor = anchor;
            if (auto ptr = llvm::dyn_cast<Pointer>(value)) {
                auto elem = ptr->getAt();
                while (elem) {
                    fixAnchor(anchor, elem);
                    elem = elem->getNext();
                }
            }
            fixAnchor(anchor, value->getNext());
        }
        return value;
    }
    */

    void SetContext(clang::ASTContext * ctx, Table *dest_) {
        Context = ctx;
        dest = dest_;
    }

    void GetFields(Type *struct_type, clang::RecordDecl * rd) {
        //auto &rl = Context->getASTRecordLayout(rd);

        std::vector<std::string> names;
        std::vector<const Type *> types;
        //auto anchors = new std::vector<Anchor>();

        for(clang::RecordDecl::field_iterator it = rd->field_begin(), end = rd->field_end(); it != end; ++it) {
            clang::DeclarationName declname = it->getDeclName();

            //unsigned idx = it->getFieldIndex();

            //auto offset = rl.getFieldOffset(idx);
            //unsigned width = it->getBitWidthValue(*Context);

            if(it->isBitField() || (!it->isAnonymousStructOrUnion() && !declname)) {
                break;
            }
            clang::QualType FT = it->getType();
            const Type *fieldtype = TranslateType(FT);
            if(!fieldtype) {
                break;
            }
            // todo: work offset into structure
            names.push_back(
                it->isAnonymousStructOrUnion()?"":
                                    declname.getAsString());
            types.push_back(fieldtype);
            //anchors->push_back(anchorFromLocation(it->getSourceRange().getBegin()));
        }

        Types::_set_field_names(struct_type, names);
        if (rd->isUnion()) {
            Types::_set_union_field_types(struct_type, types);
        } else {
            Types::_set_struct_field_types(struct_type, types);
        }
    }

    const Type *TranslateRecord(clang::RecordDecl *rd) {
        if (!rd->isStruct() && !rd->isUnion()) return NULL;

        std::string name = rd->getName();

        Type *struct_type = nullptr;
        if (name.size() && named_structs.count(name)) {
            struct_type = named_structs[name];
        } else {
            struct_type = Types::Struct(name, false);
            if (name.size()) {
                named_structs[name] = struct_type;
            }
        }

        clang::RecordDecl * defn = rd->getDefinition();
        if (defn && !record_defined[rd]) {
            /*
            Anchor anchor = anchorFromLocation(rd->getSourceRange().getBegin());
            set_key(*struct_type, "anchor",
                pointer(Types::AnchorRef, new Anchor(anchor)));*/

            GetFields(struct_type, defn);

            auto &rl = Context->getASTRecordLayout(rd);
            auto align = rl.getAlignment();
            auto size = rl.getSize();
            // should match
            assert ((size_t)align.getQuantity() == struct_type->alignment);
            assert ((size_t)size.getQuantity() == struct_type->size);

            // todo: make sure these fit
            // align.getQuantity()
            // size.getQuantity()

            record_defined[rd] = true;
        }

        return struct_type;
    }

    const Type *TranslateEnum(clang::EnumDecl *ed) {
        std::string name = ed->getName();

        Type *enum_type = nullptr;
        if (name.size() && named_enums.count(name)) {
            enum_type = named_enums[name];
        } else {
            enum_type = Types::Enum(name);
            if (name.size()) {
                named_enums[name] = enum_type;
            }
        }

        clang::EnumDecl * defn = ed->getDefinition();
        if (defn && !enum_defined[ed]) {
            /*
            set_key(*enum_type, "anchor",
                pointer(Types::AnchorRef,
                    new Anchor(
                        anchorFromLocation(
                            ed->getIntegerTypeRange().getBegin()))));
            */

            enum_type->tag_type = TranslateType(ed->getIntegerType());

            std::vector<std::string> names;
            std::vector<int64_t> tags;
            std::vector<const Type *> types;
            //auto anchors = new std::vector<Anchor>();

            for (auto it : ed->enumerators()) {
                //Anchor anchor = anchorFromLocation(it->getSourceRange().getBegin());
                auto &val = it->getInitVal();

                names.push_back(it->getName().data());
                tags.push_back(val.getExtValue());
                types.push_back(Types::None);
                //anchors->push_back(anchor);
            }

            enum_type->tags = tags;
            Types::_set_union_field_types(enum_type, types);
            Types::_set_field_names(enum_type, names);

            enum_defined[ed] = true;
        }

        return enum_type;
    }

    const Type *TranslateType(clang::QualType T) {
        using namespace clang;

        const clang::Type *Ty = T.getTypePtr();

        switch (Ty->getTypeClass()) {
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
            auto it = typedefs.find(td->getName().data());
            assert (it != typedefs.end());
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
        case clang::Type::Builtin:
            switch (cast<BuiltinType>(Ty)->getKind()) {
            case clang::BuiltinType::Void: {
                return Types::None;
            } break;
            case clang::BuiltinType::Bool: {
                return Types::Bool;
            } break;
            case clang::BuiltinType::Char_S:
            case clang::BuiltinType::Char_U:
            case clang::BuiltinType::SChar:
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
                return Types::Integer(sz, !Ty->isUnsignedIntegerType());
            } break;
            case clang::BuiltinType::Half: {
                return Types::R16;
            } break;
            case clang::BuiltinType::Float: {
                return Types::R32;
            } break;
            case clang::BuiltinType::Double: {
                return Types::R64;
            } break;
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
        case clang::Type::Pointer: {
            const clang::PointerType *PTy = cast<clang::PointerType>(Ty);
            QualType ETy = PTy->getPointeeType();
            const Type *pointee = TranslateType(ETy);
            if (pointee != NULL) {
                return Types::Pointer(pointee);
            }
        } break;
        case clang::Type::VariableArray:
        case clang::Type::IncompleteArray:
            break;
        case clang::Type::ConstantArray: {
            const ConstantArrayType *ATy = cast<ConstantArrayType>(Ty);
            const Type *at = TranslateType(ATy->getElementType());
            if(at) {
                int sz = ATy->getSize().getZExtValue();
                Types::Array(at, sz);
            }
        } break;
        case clang::Type::ExtVector:
        case clang::Type::Vector: {
                const clang::VectorType *VT = cast<clang::VectorType>(T);
                const Type *at = TranslateType(VT->getElementType());
                if(at) {
                    int n = VT->getNumElements();
                    return Types::Vector(at, n);
                }
        } break;
        case clang::Type::FunctionNoProto:
        case clang::Type::FunctionProto: {
            const clang::FunctionType *FT = cast<clang::FunctionType>(Ty);
            if (FT) {
                return TranslateFuncType(FT);
            }
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
        fprintf(stderr, "type not understood: %s (%i)\n",
            T.getAsString().c_str(),
            Ty->getTypeClass());

        return NULL;
    }

    const Type *TranslateFuncType(const clang::FunctionType * f) {

        bool valid = true;
        clang::QualType RT = f->getReturnType();

        const Type *returntype = TranslateType(RT);

        if (!returntype)
            valid = false;

        bool vararg = false;

        const clang::FunctionProtoType * proto = f->getAs<clang::FunctionProtoType>();
        std::vector<const Type *> argtypes;
        if(proto) {
            vararg = proto->isVariadic();
            for(size_t i = 0; i < proto->getNumParams(); i++) {
                clang::QualType PT = proto->getParamType(i);
                const Type *paramtype = TranslateType(PT);
                if(!paramtype) {
                    valid = false;
                } else if(valid) {
                    argtypes.push_back(paramtype);
                }
            }
        }

        if(valid) {
            return Types::CFunction(returntype,
                Types::Tuple(argtypes), vararg);
        }

        return NULL;
    }

    void exportType(const std::string &name, const Type *type) {
        set_key(*dest, get_symbol(name),
            wrap(type));
    }

    void exportExternal(const std::string &name, const Type *type,
        const Anchor &anchor) {
        set_key(*dest, get_symbol(name),
            wrap(type, get_c_symbol(NULL, name.c_str())));
    }

    void exportExternalPtr(const std::string &name, const Type *type,
        const Anchor &anchor) {
        set_key(*dest, get_symbol(name),
            wrap_ptr(type, get_c_symbol(NULL, name.c_str())));
    }

    bool TraverseRecordDecl(clang::RecordDecl *rd) {
        if (rd->isFreeStanding()) {
            auto type = TranslateRecord(rd);
            auto name = get_name(type);
            if (name.size()) {
                exportType(name, type);
            }
        }
        return true;
    }

    bool TraverseEnumDecl(clang::EnumDecl *ed) {
        if (ed->isFreeStanding()) {
            auto type = TranslateEnum(ed);
            auto name = get_name(type);
            if (name.size()) {
                exportType(name, type);
            }
        }
        return true;
    }

    bool TraverseVarDecl(clang::VarDecl *vd) {
        if (vd->isExternC()) {
            Anchor anchor = anchorFromLocation(vd->getSourceRange().getBegin());

            const Type *type = TranslateType(vd->getType());
            if (!type) return true;

            exportExternal(vd->getName().data(), type, anchor);
        }

        return true;

    }

    bool TraverseTypedefDecl(clang::TypedefDecl *td) {

        //Anchor anchor = anchorFromLocation(td->getSourceRange().getBegin());

        const Type *type = TranslateType(td->getUnderlyingType());
        if (!type) return true;

        typedefs[td->getName().data()] = type;
        exportType(td->getName().data(), type);

        return true;
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
        if (!functype)
            return true;

        std::string InternalName = FuncName;
        clang::AsmLabelAttr * asmlabel = f->getAttr<clang::AsmLabelAttr>();
        if(asmlabel) {
            InternalName = asmlabel->getLabel();
            #ifndef __linux__
                //In OSX and Windows LLVM mangles assembler labels by adding a '\01' prefix
                InternalName.insert(InternalName.begin(), '\01');
            #endif
        }

        Anchor anchor = anchorFromLocation(f->getSourceRange().getBegin());

        exportExternalPtr(FuncName.c_str(), Types::Pointer(functype), anchor);

        return true;
    }
};

class CodeGenProxy : public clang::ASTConsumer {
public:
    Table *dest;
    CVisitor visitor;

    CodeGenProxy(Table *dest_) : dest(dest_) {}
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
    Table *dest;

    BangEmitLLVMOnlyAction(Table *dest_) :
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

static Table *importCModule (
    const std::string &path, const std::vector<std::string> &args,
    const char *buffer = nullptr) {
    using namespace clang;

    std::vector<const char *> aargs;
    aargs.push_back("clang");
    aargs.push_back(path.c_str());
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
            //~ CompilerInvocation::GetResourcesPath(bangra_argv[0], MainAddr);

    LLVMModuleRef M = NULL;


    auto result = new_table();

    // Create and execute the frontend to generate an LLVM bitcode module.
    std::unique_ptr<CodeGenAction> Act(new BangEmitLLVMOnlyAction(result));
    if (compiler.ExecuteAction(*Act)) {
        M = (LLVMModuleRef)Act->takeModule().release();
        assert(M);
        return result;
    } else {
        error("compilation failed");
    }

    return nullptr;
}

//------------------------------------------------------------------------------
// BUILTINS
//------------------------------------------------------------------------------

static const Table *globals = nullptr;

static Any builtin_string(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    return pstring(get_string(args[0]));
}

static Any builtin_cstr(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    if (!eq(args[0].type, Types::Rawstring))
        error("rawstring expected");
    const char *str = *args[0].pc_str;
    return wrap(Types::String, alloc_slice<char>(str, strlen(str)));
}

static Any builtin_print(const std::vector<Any> &args) {
    builtin_checkparams(args, 0, -1);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0)
            std::cout << " ";
        auto &arg = args[i];
        std::cout << get_string(arg);
    }
    std::cout << "\n";
    return const_none;
}

static Any builtin_globals(const std::vector<Any> &args) {
    builtin_checkparams(args, 0, 0);
    return wrap(globals);
}

static Any builtin_at(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    return at(args[0], args[1]);
}

/*
static Any builtin_eq(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    return wrap(eq(args[0], args[1]));
}

static Any builtin_ne(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    return wrap(ne(args[0], args[1]));
}
*/

static Any builtin_length(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    return wrap(length(args[0]));
}

static Any builtin_slice(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 3);
    int64_t i0;
    int64_t i1;
    int64_t l = (int64_t)length(args[0]);
    if (!is_integer_type(args[1].type)) {
        error("integer expected");
    }
    i0 = extract_integer(args[1]);
    if (i0 < 0)
        i0 += l;
    i0 = std::min(std::max(i0, (int64_t)0), l);
    if (args.size() > 2) {
        if (!is_integer_type(args[2].type)) {
            error("integer expected");
        }
        i1 = extract_integer(args[2]);
        if (i1 < 0)
            i1 += l;
        i1 = std::min(std::max(i1, i0), l);
    } else {
        i1 = l;
    }

    return slice(args[0], (size_t)i0, (size_t)i1);
}

static std::vector<Any> builtin_branch(const std::vector<Any> &args) {
    builtin_checkparams(args, 4, 4, 1);
    auto cond = extract_bool(args[1]);
    if (cond) {
        return { args[2], args[4] };
    } else {
        return { args[3], args[4] };
    }
}

static std::vector<Any> builtin_call_cc(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2, 1);
    return { args[1], args[2], args[2] };
}

static Any builtin_repr(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    return wrap(formatValue(args[0], 0, false));
}

static Any builtin_dump(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    printValue(args[0], 0, true);
    return args[0];
}

static Any builtin_tupleof(const std::vector<Any> &args) {
    builtin_checkparams(args, 0, -1);
    return wrap(args);
}

static Any builtin_cons(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    auto &at = args[0];
    auto next = args[1];
    verifyValueKind(Types::PList, next);
    return wrap(List::create(at, *next.plist,
        get_anchor(handler_frame)));
}

static Any builtin_structof(const std::vector<Any> &args) {
    builtin_checkparams(args, 0, -1);
    std::vector<Symbol> names;
    std::vector<Any> values;
    for (size_t i = 0; i < args.size(); ++i) {
        auto pair = extract_tuple(args[i]);
        if (pair.size() != 2)
            error("tuple must have exactly two elements");
        auto name = extract_symbol(pair[0]);
        names.push_back(name);
        auto value = pair[1];
        values.push_back(value);
    }

    Any t = wrap(values);

    auto struct_type = Types::Struct("");
    Types::_set_struct_field_types(struct_type, t.type->types);
    Types::_set_field_names(struct_type, names);

    return wrap(struct_type, t.ptr);
}

static Any builtin_table(const std::vector<Any> &args) {
    builtin_checkparams(args, 0, -1);

    auto t = new_table();

    for (size_t i = 0; i < args.size(); ++i) {
        auto pair = extract_tuple(args[i]);
        if (pair.size() != 2)
            error("tuple must have exactly two elements");
        auto name = extract_symbol(pair[0]);
        auto value = pair[1];
        set_key(*t, name, value);
    }

    return wrap(t);
}

static Any builtin_table_join(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);

    auto a = extract_table(args[0]);
    auto b = extract_table(args[1]);
    auto t = new_table();
    for (auto it = a->_.begin(); it != a->_.end(); ++it) {
        set_key(*t, it->first, it->second);
    }
    for (auto it = b->_.begin(); it != b->_.end(); ++it) {
        set_key(*t, it->first, it->second);
    }
    return wrap(t);
}

static Any builtin_set_key(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);

    auto t = const_cast<Table *>(extract_table(args[0]));
    auto pair = extract_tuple(args[1]);
    if (pair.size() != 2)
        error("tuple must have exactly two elements");
    auto name = extract_symbol(pair[0]);
    auto value = pair[1];
    set_key(*t, name, value);
    return const_none;
}

static Any builtin_next_key(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);

    auto t = extract_table(args[0]);
    Table::map_type::const_iterator it;
    if (is_none_type(args[1].type)) {
        it = t->_.begin();
    } else {
        it = t->_.find(extract_symbol(args[1]));
        ++it;
    }
    if (it == t->_.end())
        return const_none;
    std::vector<Any> result = { wrap_symbol(it->first), it->second };
    return wrap(result);
}

static Any builtin_syntax_macro(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    verifyValueKind(Types::PClosure, args[0]);
    return wrap_ptr(
        Types::PMacro,
        Macro::create(args[0]));
}

static Any builtin_typeof(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    return wrap(args[0].type);
}

static Any builtin_cfunction(const std::vector<Any> &args) {
    builtin_checkparams(args, 3, 3);
    const Type *rettype = extract_type(args[0]);
    auto params = extract_tuple(args[1]);
    bool vararg = extract_bool(args[2]);

    std::vector<const Type *> paramtypes;
    size_t paramcount = params.size();
    for (size_t i = 0; i < paramcount; ++i) {
        paramtypes.push_back(extract_type(params[i]));
    }
    return wrap(Types::CFunction(rettype, Types::Tuple(paramtypes), vararg));
}

static Any builtin_pointer(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    const Type *type = extract_type(args[0]);
    return wrap(Types::Pointer(type));
}

static Any builtin_error(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    std::string msg = extract_string(args[0]);
    error("%s", msg.c_str());
    return const_none;
}

// (import-c const-path (tupleof const-string ...))
static Any builtin_import_c(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    std::string path = extract_string(args[0]);
    auto compile_args = extract_tuple(args[1]);
    std::vector<std::string> cargs;
    for (size_t i = 0; i < compile_args.size(); ++i) {
        cargs.push_back(extract_string(compile_args[i]));
    }
    return wrap_ptr(Types::PTable, bangra::importCModule(path, cargs));
}

static Any builtin_external(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    auto sym = extract_symbol(args[0]);
    auto type = extract_type(args[1]);
    void *ptr = get_c_symbol(NULL, get_symbol_name(sym).c_str());
    if (!ptr) return const_none;
    if (is_cfunction_type(type)) {
        return wrap_ptr(Types::Pointer(type), ptr);
    } else {
        return wrap(type, ptr);
    }
}

template<BuiltinFunction F>
static Any builtin_variadic_ltr(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, -1);
    size_t k = args.size();
    Any result = F({args[0], args[1]});
    for (size_t i = 2; i < k; ++i) {
        result = F({result, args[i]});
    }
    return result;
}

template<BuiltinFunction F>
static Any builtin_variadic_rtl(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, -1);
    size_t k = args.size();
    Any result = args[k - 1];
    for (size_t i = 2; i <= k; ++i) {
        result = F({args[k - i], result});
    }
    return result;
}

template<Any (*F)(const Any &)>
static Any builtin_unary_op(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    return F(args[0]);
}

template<Any (*F1)(const Any &), Any (*F2)(const Any &, const Any &)>
static Any builtin_unary_binary_op(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 2);
    if (args.size() == 1) {
        return F1(args[0]);
    } else {
        return F2(args[0], args[1]);
    }
}

template<Any (*F)(const Any &, const Any &)>
static Any builtin_binary_op(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    return F(args[0], args[1]);
}

template<bool (*F)(const Any &, const Any &)>
static Any builtin_bool_binary_op(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    return wrap(F(args[0], args[1]));
}

static Any builtin_flow_parameter_count(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    auto flow = extract_flow(args[0]);
    return wrap(flow->parameters.size());
}

static Any builtin_flow_parameter(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    auto flow = extract_flow(args[0]);
    auto index = extract_integer(args[1]);
    return wrap(flow->parameters[index]);
}

static Any builtin_flow_argument_count(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    auto flow = extract_flow(args[0]);
    return wrap(flow->arguments.size());
}

static Any builtin_flow_argument(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    auto flow = extract_flow(args[0]);
    auto index = extract_integer(args[1]);
    return flow->arguments[index];
}

static Any builtin_flow_name(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    auto flow = extract_flow(args[0]);
    return wrap_symbol(flow->name);
}

static Any builtin_flow_id(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    auto flow = extract_flow(args[0]);
    return wrap(flow->getUID());
}

static Any builtin_frame_eval(const std::vector<Any> &args) {
    builtin_checkparams(args, 3, 3);
    auto frame = extract_frame(args[0]);
    auto argindex = extract_integer(args[1]);
    return evaluate(argindex, frame, args[2]);
}

//------------------------------------------------------------------------------
// TRANSLATION
//------------------------------------------------------------------------------

static Table *new_scope() {
    return new_table();
}

static Table *new_scope(const Table *scope) {
    assert(scope);
    auto subscope = new_table();
    set_key(*subscope, SYM_Parent, wrap_ptr(Types::PTable, scope));
    return subscope;
}

static void setLocal(Table *scope, const Symbol &name, const Any &value) {
    assert(scope);
    set_key(*scope, name, value);
}

static void setLocalString(Table *scope, const std::string &name, const Any &value) {
    setLocal(scope, get_symbol(name), value);
}

static void setBuiltin(
    Table *scope, const std::string &name, MacroBuiltinFunction func) {
    assert(scope);
    auto sym = get_symbol(name);
    setLocal(scope, sym,
        wrap_ptr(Types::PBuiltinMacro, BuiltinMacro::create(func, sym)));
}

static void setBuiltin(
    Table *scope, const std::string &name, SpecialFormFunction func) {
    assert(scope);
    auto sym = get_symbol(name);
    setLocal(scope, sym,
        wrap_ptr(Types::PSpecialForm, SpecialForm::create(func, sym)));
}

static void setBuiltin(
    Table *scope, const std::string &name, BuiltinFunction func) {
    assert(scope);
    auto sym = get_symbol(name);
    setLocal(scope, sym,
        wrap_ptr(Types::PBuiltin, Builtin::create(func, sym)));
}

static void setBuiltin(
    Table *scope, const std::string &name, BuiltinFlowFunction func) {
    assert(scope);
    auto sym = get_symbol(name);
    setLocal(scope, sym,
        wrap_ptr(Types::PBuiltinFlow, BuiltinFlow::create(func, sym)));
}

/*
static bool isLocal(StructValue *scope, const std::string &name) {
    assert(scope);
    size_t idx = scope->struct_type->getFieldIndex(name);
    if (idx == (size_t)-1) return false;
    return true;
}
*/

static const Table *getParent(const Table *scope) {
    auto parent = get_key(*scope, SYM_Parent, const_none);
    if (parent.type == Types::PTable)
        return *parent.ptable;
    return nullptr;
}

static bool hasLocal(const Table *scope, const Symbol &name) {
    assert(scope);
    while (scope) {
        if (has_key(*scope, name)) return true;
        scope = getParent(scope);
    }
    return false;
}

static Any getLocal(const Table *scope, const Symbol &name) {
    assert(scope);
    while (scope) {
        auto result = get_key(*scope, name, const_none);
        if (result.type != Types::None)
            return result;
        scope = getParent(scope);
    }
    return const_none;
}

static Any getLocalString(const Table *scope, const std::string &name) {
    return getLocal(scope, get_symbol(name));
}

//------------------------------------------------------------------------------

static bool isSymbol (const Any &expr, const Symbol &sym) {
    if (expr.type == Types::Symbol) {
        return extract_symbol(expr) == sym;
    }
    return false;
}

//------------------------------------------------------------------------------
// MACRO EXPANDER
//------------------------------------------------------------------------------

static bool verifyParameterCount (const List *expr,
    int mincount, int maxcount) {
    if ((mincount <= 0) && (maxcount == -1))
        return true;
    int argcount = (int)getSize(expr) - 1;

    if ((maxcount >= 0) && (argcount > maxcount)) {
        error("excess argument. At most %i arguments expected", maxcount);
        return false;
    }
    if ((mincount >= 0) && (argcount < mincount)) {
        error("at least %i arguments expected", mincount);
        return false;
    }
    return true;
}

static bool verifyParameterCount (ListIter topit,
    int mincount, int maxcount) {
    auto val = *topit;
    verifyValueKind(Types::PList, val);
    return verifyParameterCount(*val.plist, mincount, maxcount);
}

//------------------------------------------------------------------------------

static Cursor expand (const Table *env, ListIter topit);
static Any compile(const Any &expr);

static Any toparameter (Table *env, const Any &value) {
    if (eq(value.type, Types::PParameter))
        return value;
    verifyValueKind(Types::Symbol, value);
    auto key = extract_symbol(value);
    auto bp = wrap(Parameter::create(key));
    setLocal(env, key, bp);
    return bp;
}

static const List *expand_expr_list (const Table *env, ListIter it) {
    const List *l = nullptr;
    while (it) {
        auto cur = expand(env, it);
        l = List::create(cur.value, l, it.getAnchor());
        it = cur.next;
    }
    return reverse_list_inplace(l);
}

static Cursor expand_function (const Table *env, ListIter topit) {
    verifyParameterCount(topit, 1, -1);

    ListIter it(*topit++);
    auto topanchor = it.getAnchor();
    it++;
    auto expr_parameters = *it++;

    auto subenv = new_scope(env);

    const List *outargs = nullptr;
    verifyValueKind(Types::PList, expr_parameters);
    auto params = *expr_parameters.plist;
    ListIter param(params);
    while (param) {
        outargs = List::create(toparameter(subenv, *param),
            outargs, param.getAnchor());
        param++;
    }

    return {
        wrap(
            List::create(
                getLocal(globals, SYM_FunctionForm),
                List::create(
                    wrap(reverse_list_inplace(outargs)),
                    expand_expr_list(subenv, it),
                    get_anchor(params)),
                topanchor)),
        topit };
}

static Cursor expand_quote (const Table *env, ListIter topit) {
    verifyParameterCount(topit, 1, -1);

    ListIter it(*topit++);
    auto anchor = it.getAnchor();
    it++;
    auto result = List::create(
        getLocal(globals, SYM_QuoteForm),
        it.getList(),
        anchor);
    return { wrap(result), topit };
}

static Cursor expand_escape (const Table *env, ListIter topit) {
    ListIter it(*topit++, 1);
    return { *it, topit };
}

static Cursor expand_let_syntax (const Table *env, ListIter topit) {
    auto cur = expand_function (env, topit++);
    //printValue(cur.value, 0, true);

    auto fun = compile(cur.value);

    auto expr_env = execute({fun, wrap_ptr(
        Types::PTable, env)});
    verifyValueKind(Types::PTable, expr_env);

    auto rest = expand_expr_list(*expr_env.ptable, topit);
    if (!rest) {
        error("let-syntax: missing subsequent expression");
    }

    return { rest->at, rest->next };
}

static const List *expand_macro(
    const Table *env, const Any &handler, ListIter topit) {
    auto result = execute({handler,
        wrap(env),
        wrap(topit.getList())});
    if (isnone(result))
        return nullptr;
    verifyValueKind(Types::PList, result);
    if (!get_anchor(result)) {
        auto head = *topit;
        const Anchor *anchor = get_anchor(head);
        if (!anchor) {
            anchor = topit.getAnchor();
        }
        set_anchor(result, anchor);
    }
    return *result.plist;
}

static Cursor expand (const Table *env, ListIter topit) {
    Any result = const_none;
process:
    Any expr = *topit;
    if (eq(expr.type, Types::PList)) {
        if (!(*expr.plist)) {
            error("expression is empty");
        }

        auto head = (*expr.plist)->at;
        if (eq(head.type, Types::Symbol)) {
            head = getLocal(env, extract_symbol(head));
        }
        if (head.type != Types::None) {
            if (eq(head.type, Types::PBuiltinMacro)) {
                return (*head.pbuiltin_macro)->handler(env, topit);
            } else if (eq(head.type, Types::PMacro)) {
                auto result = expand_macro(env,
                    (*head.pmacro)->value, topit);
                if (result) {
                    topit = ListIter(result);
                    goto process;
                }
            }
        }

        auto default_handler = getLocal(env, SYM_ListWC);
        if (default_handler.type != Types::None) {
            auto result = expand_macro(env, default_handler, topit);
            if (result) {
                topit = ListIter(result);
                goto process;
            }
        }

        ListIter it(*topit);
        result = wrap(expand_expr_list(env, it));
        topit++;
    } else if (eq(expr.type, Types::Symbol)) {
        auto value = extract_symbol(expr);
        if (!hasLocal(env, value)) {
            auto default_handler = getLocal(env, SYM_SymbolWC);
            if (default_handler.type != Types::None) {
                auto result = expand_macro(env, default_handler, topit);
                if (result) {
                    topit = ListIter(result);
                    goto process;
                }
            }
            error("no such symbol in scope: '%s'",
                get_symbol_name(value).c_str());
        }
        result = getLocal(env, value);
        topit++;
    } else {
        result = expr;
        topit++;
    }
    return { result, topit };
}

static Any builtin_expand(const std::vector<Any> &args) {
    builtin_checkparams(args, 2, 2);
    auto scope = args[0];
    verifyValueKind(Types::PTable, scope);
    auto expr_eval = args[1];

    auto retval = expand(*scope.ptable, expr_eval);

    auto topexpr = const_cast<List*>(retval.next.getList());
    return wrap(List::create(retval.value, topexpr));
}

static Any builtin_set_globals(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    auto scope = args[0];
    verifyValueKind(Types::PTable, scope);

    globals = *scope.ptable;
    return const_none;
}

//------------------------------------------------------------------------------
// COMPILER
//------------------------------------------------------------------------------

struct ILBuilder {
    struct State {
        Flow *flow;
        Flow *prevflow;
    };

    State state;

    State save() {
        return state;
    }

    void restore(const State &state) {
        this->state = state;
        if (state.flow) {
            assert(!state.flow->hasArguments());
        }
    }

    void continueAt(Flow *flow) {
        restore({flow,nullptr});
    }

    void insertAndAdvance(
        const std::vector<Any> &values,
        Flow *next,
        const Anchor *anchor) {
        assert(state.flow);
        assert(!state.flow->hasArguments());
        state.flow->arguments = values;
        set_anchor(state.flow, anchor);
        state.prevflow = state.flow;
        state.flow = next;
    }

    void br(const std::vector<Any> &arguments, const Anchor *anchor) {
        // patch previous flow destination to jump right to
        // continuation when possible
        assert(state.flow);
        if (state.prevflow
            && (arguments.size() == 2)
            && (eq(Types::PParameter, arguments[1].type))
            && (*arguments[1].pparameter
                == state.flow->parameters[0])
            && (eq(Types::PFlow, state.prevflow->arguments.back().type))
            && (*(state.prevflow->arguments.back()).pflow
                == state.flow)) {
            state.prevflow->arguments.back() = arguments[0];
        } else {
            insertAndAdvance(arguments, nullptr, anchor);
        }
    }

    Parameter *call(std::vector<Any> values, const Anchor *anchor) {
        assert(anchor);
        auto next = Flow::create(1, SYM_ReturnFlow);
        values.push_back(wrap(next));
        insertAndAdvance(values, next, anchor);
        return next->parameters[0];
    }

};

static ILBuilder *builder;

//------------------------------------------------------------------------------

static Any compile_expr_list (ListIter it) {
    Any value = const_none;
    while (it) {
        value = compile(*it);
        it++;
    }
    return value;
}

static Any compile_do (ListIter it) {
    it++;
    return compile_expr_list(it);
}

static Any compile_function (ListIter it) {
    auto anchor = find_valid_anchor(it.getList());
    if (!anchor || !anchor->isValid()) {
        printValue(wrap(it.getList()), 0, true);
        error("function expression not anchored");
    }

    it++;

    auto expr_parameters = *it++;

    auto currentblock = builder->save();

    auto function = Flow::create(0, SYM_FunctionFlow);
    set_anchor(function, anchor);

    builder->continueAt(function);

    verifyValueKind(Types::PList, expr_parameters);
    ListIter param(*expr_parameters.plist);
    while (param) {
        auto pparam = *param;
        verifyValueKind(Types::PParameter, pparam);
        function->appendParameter(const_cast<Parameter*>(
            *pparam.pparameter));
        param++;
    }
    auto ret = function->appendParameter(Parameter::create());

    auto result = compile_expr_list(it);

    builder->br({wrap(ret), result}, anchor);

    builder->restore(currentblock);

    return wrap(function);
}

static Any compile_implicit_call (ListIter it,
    const Anchor *anchor = nullptr) {
    if (!anchor) {
        anchor = find_valid_anchor(it.getList());
        if (!anchor || !anchor->isValid()) {
            printValue(wrap(it.getList()), 0, true);
            error("call expression not anchored");
        }
    }

    Any callable = compile(*it++);

    std::vector<Any> args;
    args.push_back(callable);

    while (it) {
        args.push_back(compile(*it));
        it++;
    }

    return wrap(builder->call(args, anchor));
}

static Any compile_call (ListIter it) {
    auto anchor = find_valid_anchor(it.getList());
    if (!anchor || !anchor->isValid()) {
        printValue(wrap(it.getList()), 0, true);
        error("call expression not anchored");
    }
    it++;
    return compile_implicit_call(it, anchor);
}

static Any compile_quote (ListIter it) {
    it++;
    List *rest = const_cast<List*>(it.getList());
    if (!rest->next)
        return rest->at;
    else
        return wrap(rest);
}

//------------------------------------------------------------------------------

static Any compile (const Any &expr) {
    Any result = const_none;
    if (eq(expr.type, Types::PList)) {
        if (!(*expr.plist)) {
            error("empty expression");
        }
        auto slist = *expr.plist;
        auto head = slist->at;
        if (eq(head.type, Types::PSpecialForm)) {
            result = (*head.pspecial_form)->handler(
                ListIter(slist));
        } else {
            result = compile_implicit_call(ListIter(slist));
        }
        assert(result.type != Types::None);
    } else {
        result = expr;
    }
    return result;
}

//------------------------------------------------------------------------------
// INITIALIZATION
//------------------------------------------------------------------------------

static Any builtin_loadlist(const std::vector<Any> &args);

static Any builtin_eval(const std::vector<Any> &args);

static void initGlobals () {
    auto env = new_scope();
    globals = env;

    //setLocalString(env, "globals", wrap(env));

    setBuiltin(env, "form:call", compile_call);
    setBuiltin(env, "form:function", compile_function);
    setBuiltin(env, "form:quote", compile_quote);
    setBuiltin(env, "do", compile_do);

    setBuiltin(env, "function", expand_function);
    setBuiltin(env, "quote", expand_quote);
    setBuiltin(env, "let-syntax", expand_let_syntax);
    setBuiltin(env, "escape", expand_escape);

    setLocalString(env, "void", wrap(Types::None));
    setLocalString(env, "None", wrap(Types::None));

    setLocalString(env, "bool", wrap(Types::Bool));

    setLocalString(env, "int8", wrap(Types::I8));
    setLocalString(env, "int16", wrap(Types::I16));
    setLocalString(env, "int32", wrap(Types::I32));
    setLocalString(env, "int64", wrap(Types::I64));

    setLocalString(env, "uint8", wrap(Types::U8));
    setLocalString(env, "uint16", wrap(Types::U16));
    setLocalString(env, "uint32", wrap(Types::U32));
    setLocalString(env, "uint64", wrap(Types::U64));

    setLocalString(env, "int", wrap(Types::I32));
    setLocalString(env, "uint", wrap(Types::U32));

    setLocalString(env, "real16", wrap(Types::R16));
    setLocalString(env, "real32", wrap(Types::R32));
    setLocalString(env, "real64", wrap(Types::R64));

    setLocalString(env, "half", wrap(Types::R16));
    setLocalString(env, "float", wrap(Types::R32));
    setLocalString(env, "double", wrap(Types::R64));

    setLocalString(env, "symbol", wrap(Types::Symbol));
    setLocalString(env, "list", wrap(Types::PList));

    setLocalString(env, "closure", wrap(Types::PClosure));
    setLocalString(env, "parameter", wrap(Types::PParameter));
    setLocalString(env, "frame", wrap(Types::PFrame));
    setLocalString(env, "flow", wrap(Types::PFlow));

    setLocalString(env, "scope-parent-symbol", wrap_symbol(SYM_Parent));
    setLocalString(env, "scope-list-wildcard-symbol", wrap_symbol(SYM_ListWC));
    setLocalString(env, "scope-symbol-wildcard-symbol", wrap_symbol(SYM_SymbolWC));

    setLocalString(env, "usize_t",
        wrap(Types::Integer(sizeof(size_t)*8,false)));

    setLocalString(env, "rawstring", wrap(Types::Rawstring));

    setLocalString(env, "int", getLocalString(env, "int32"));

    setLocalString(env, "true", wrap(true));
    setLocalString(env, "false", wrap(false));

    setLocalString(env, "none", const_none);

    setLocalString(env, "support-ANSI?", wrap(support_ansi));

    setBuiltin(env, "globals", builtin_globals);
    setBuiltin(env, "print", builtin_print);
    setBuiltin(env, "repr", builtin_repr);
    setBuiltin(env, "cfunction", builtin_cfunction);
    setBuiltin(env, "pointer", builtin_pointer);
    setBuiltin(env, "tupleof", builtin_tupleof);
    setBuiltin(env, "cons", builtin_cons);
    setBuiltin(env, "structof", builtin_structof);
    setBuiltin(env, "table", builtin_table);
    setBuiltin(env, "table-join", builtin_table_join);
    setBuiltin(env, "set-key!", builtin_set_key);
    setBuiltin(env, "next-key", builtin_next_key);
    setBuiltin(env, "typeof", builtin_typeof);
    setBuiltin(env, "external", builtin_external);
    setBuiltin(env, "import-c", builtin_import_c);
    setBuiltin(env, "branch", builtin_branch);
    setBuiltin(env, "call/cc", builtin_call_cc);
    setBuiltin(env, "dump", builtin_dump);
    setBuiltin(env, "syntax-macro", builtin_syntax_macro);
    setBuiltin(env, "string", builtin_string);

    setBuiltin(env, "expand", builtin_expand);
    setBuiltin(env, "set-globals!", builtin_set_globals);
    //setBuiltin(env, "integer?", builtin_is_integer);
    //setBuiltin(env, "null?", builtin_is_null);
    //setBuiltin(env, "key?", builtin_is_key);
    setBuiltin(env, "error", builtin_error);
    setBuiltin(env, "length", builtin_length);
    setBuiltin(env, "list-load", builtin_loadlist);
    setBuiltin(env, "eval", builtin_eval);
    setBuiltin(env, "cstr", builtin_cstr);

    setBuiltin(env, "flow-parameter-count", builtin_flow_parameter_count);
    setBuiltin(env, "flow-parameter", builtin_flow_parameter);
    setBuiltin(env, "flow-argument-count", builtin_flow_argument_count);
    setBuiltin(env, "flow-argument", builtin_flow_argument);
    setBuiltin(env, "flow-name", builtin_flow_name);
    setBuiltin(env, "flow-id", builtin_flow_id);

    setBuiltin(env, "frame-eval", builtin_frame_eval);

    setBuiltin(env, "@", builtin_variadic_ltr<builtin_at>);

    setBuiltin(env, "slice", builtin_slice);

    setBuiltin(env, "+",
        builtin_variadic_ltr< builtin_binary_op<add> >);
    setBuiltin(env, "-",
        builtin_unary_binary_op<neg, sub>);
    setBuiltin(env, "*",
        builtin_variadic_ltr< builtin_binary_op<mul> >);
    setBuiltin(env, "/",
        builtin_unary_binary_op<rcp, div>);
    setBuiltin(env, "%",
        builtin_binary_op<mod>);

    setBuiltin(env, "..",
        builtin_variadic_rtl<builtin_binary_op<concat> >);

    setBuiltin(env, "&",
        builtin_binary_op<bit_and>);
    setBuiltin(env, "|",
        builtin_variadic_ltr< builtin_binary_op<bit_or> >);
    setBuiltin(env, "^",
        builtin_binary_op<bit_xor>);
    setBuiltin(env, "~",
        builtin_unary_op<bit_not>);

    setBuiltin(env, "<<",
        builtin_binary_op<lshift>);
    setBuiltin(env, ">>",
        builtin_binary_op<rshift>);

    setBuiltin(env, "not",
        builtin_unary_op<bool_not>);

    setBuiltin(env, "==", builtin_bool_binary_op<eq>);
    setBuiltin(env, "!=", builtin_bool_binary_op<ne>);
    setBuiltin(env, ">", builtin_bool_binary_op<gt>);
    setBuiltin(env, ">=", builtin_bool_binary_op<ge>);
    setBuiltin(env, "<", builtin_bool_binary_op<lt>);
    setBuiltin(env, "<=", builtin_bool_binary_op<le>);

}

static void init() {
    bangra::support_ansi = isatty(fileno(stdout));

    initSymbols();

    Types::initTypes();
    initConstants();

    LLVMEnablePrettyStackTrace();
    LLVMLinkInMCJIT();
    //LLVMLinkInInterpreter();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmParser();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeDisassembler();

    ffi = new FFI();
    builder = new ILBuilder();

    initGlobals();
}

//------------------------------------------------------------------------------

static void handleException(const Table *env, const Any &expr) {
    streamValue(std::cerr, expr, 0, true);
    error("an exception was raised");
}

// path must be resident
static Any compileFlow (const Table *env, const Any &expr,
    const char *path) {
    verifyValueKind(Types::PList, expr);

    Anchor *anchor = new Anchor();
    anchor->path = path;
    anchor->lineno = 1;
    anchor->column = 1;
    anchor->offset = 0;

    auto rootit = ListIter(expr);
    auto expexpr = expand_expr_list(env, rootit);

    auto mainfunc = Flow::create();
    auto ret = mainfunc->appendParameter(Parameter::create());
    builder->continueAt(mainfunc);

    auto result = compile_expr_list(ListIter(expexpr));
    builder->br({ wrap(ret), result }, anchor);

    return wrap(mainfunc);
}

// return a resident copy
static const char *resident_c_str(const std::string &path) {
    return strdup(path.c_str());
}

// path must be resident
static Any loadFile(const char *path) {
    auto file = MappedFile::open(path);
    if (file) {
        bangra::Parser parser;
        return parser.parseMemory(
            file->strptr(), file->strptr() + file->size(),
            path);
    }
    return const_none;
}

static Any builtin_loadlist(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 1);
    auto path = extract_string(args[0]);
    return loadFile(resident_c_str(path));
}

static Any builtin_eval(const std::vector<Any> &args) {
    builtin_checkparams(args, 1, 3);
    auto expr = args[0];
    auto scope = (args.size() > 1)?extract_table(args[1]):globals;
    const char *path = (args.size() > 2)?extract_cstr(args[2]):"<eval>";

    return compileFlow(scope, expr, path);
}

static bool compileMain (Any expr, const char *path) {
    execute({compileFlow(globals, expr, path)});
    return true;
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

static Any parseLoader(const char *executable_path) {
    // attempt to read bootstrap expression from end of binary
    auto file = MappedFile::open(executable_path);
    if (!file) {
        fprintf(stderr, "could not open binary\n");
        return const_none;
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
        if (cursor < ptr) return const_none;
    }
    if (*cursor != ')') return const_none;
    cursor--;
    // seek backwards to find beginning of expression
    while ((cursor >= ptr) && (*cursor != '('))
        cursor--;

    bangra::Parser footerParser;
    auto expr = footerParser.parseMemory(
        cursor, ptr + size, executable_path, cursor - ptr);
    if (isnone(expr)) {
        fprintf(stderr, "could not parse footer expression\n");
        return const_none;
    }
    if (!eq(expr.type, Types::PList))  {
        fprintf(stderr, "footer expression is not a symbolic list\n");
        return const_none;
    }
    auto symlist = *expr.plist;
    ListIter it(symlist);
    if (!it) {
        fprintf(stderr, "footer expression is empty\n");
        return const_none;
    }
    auto head = *it++;
    if (!eq(head.type, Types::Symbol))  {
        fprintf(stderr, "footer expression does not begin with symbol\n");
        return const_none;
    }
    if (!isSymbol(head, SYM_ScriptSize))  {
        fprintf(stderr, "footer expression does not begin with '%s'\n",
            get_symbol_name(SYM_ScriptSize).c_str());
        return const_none;
    }
    if (!it) {
        fprintf(stderr, "footer expression needs two arguments\n");
        return const_none;
    }
    auto arg = *it++;
    if (!is_integer_type(arg.type))  {
        fprintf(stderr, "script-size argument is not integer\n");
        return const_none;
    }
    auto offset = extract_integer(arg);
    if (offset <= 0) {
        fprintf(stderr, "script-size must be larger than zero\n");
        return const_none;
    }
    bangra::Parser parser;
    auto script_start = cursor - offset;
    return parser.parseMemory(script_start, cursor, executable_path, script_start - ptr);
}

static bool compileStartupScript() {
    char *base = strdup(bangra_interpreter_path);
    char *ext = extension(base);
    if (ext) {
        *ext = 0;
    }
    std::string path = format("%s.b", base);
    free(base);

    const char *cpath = resident_c_str(path);
    Any expr = loadFile(cpath);

    if (!isnone(expr)) {
        return bangra::compileMain(expr, cpath);
    }

    return true;
}

} // namespace bangra

// C API
//------------------------------------------------------------------------------

char *bangra_interpreter_path = NULL;
char *bangra_interpreter_dir = NULL;
int bangra_argc = 0;
char **bangra_argv = NULL;

void print_version() {
    std::string versionstr = bangra::format("%i.%i",
        BANGRA_VERSION_MAJOR, BANGRA_VERSION_MINOR);
    if (BANGRA_VERSION_PATCH) {
        versionstr += bangra::format(".%i", BANGRA_VERSION_PATCH);
    }
    printf(
    "Bangra version %s\n"
    "Executable path: %s\n"
    , versionstr.c_str()
    , bangra_interpreter_path
    );
    exit(0);
}

void print_help(const char *exename) {
    printf(
    "usage: %s [option [...]] [filename]\n"
    "Options:\n"
    "   -h, --help                  print this text and exit.\n"
    "   -v, --version               print program version and exit.\n"
    "   --skip-startup              skip startup script.\n"
    "   -a, --enable-ansi           enable ANSI output.\n"
    "   -t, --trace                 trace interpreter commands.\n"
    "   --                          terminate option list.\n"
    , exename
    );
    exit(0);
}

int bangra_main(int argc, char ** argv) {
    bangra_argc = argc;
    bangra_argv = argv;

    bangra::init();

    bangra::Any expr = bangra::const_none;
    const char *cpath = "<?>";

    try {

        if (argv) {
            if (argv[0]) {
                std::string loader = bangra::GetExecutablePath(argv[0]);
                // string must be kept resident
                bangra_interpreter_path = strdup(loader.c_str());

                expr = bangra::parseLoader(bangra_interpreter_path);
                cpath = "<loader>";
            } else {
                bangra_interpreter_path = strdup("");
            }

            bangra_interpreter_dir = dirname(strdup(bangra_interpreter_path));

            bangra::setLocalString(const_cast<bangra::Table *>(bangra::globals),
                "interpreter-path",
                bangra::wrap(std::string(bangra_interpreter_path)));
            bangra::setLocalString(const_cast<bangra::Table *>(bangra::globals),
                "interpreter-dir",
                bangra::wrap(std::string(bangra_interpreter_dir)));

            if (isnone(expr)) {
                // running in interpreter mode
                char *sourcepath = NULL;
                // skip startup script
                bool skip_startup = false;

                if (argv[1]) {
                    bool parse_options = true;

                    char ** arg = argv;
                    while (*(++arg)) {
                        if (parse_options && (**arg == '-')) {
                            if (!strcmp(*arg, "--help") || !strcmp(*arg, "-h")) {
                                print_help(argv[0]);
                            } else if (!strcmp(*arg, "--version") || !strcmp(*arg, "-v")) {
                                print_version();
                            } else if (!strcmp(*arg, "--skip-startup")) {
                                skip_startup = true;
                            } else if (!strcmp(*arg, "--enable-ansi") || !strcmp(*arg, "-a")) {
                                bangra::support_ansi = true;
                            } else if (!strcmp(*arg, "--trace") || !strcmp(*arg, "-t")) {
                                bangra::trace_arguments = true;
                            } else if (!strcmp(*arg, "--")) {
                                parse_options = false;
                            } else {
                                printf("unrecognized option: %s. Try --help for help.\n", *arg);
                                exit(1);
                            }
                        } else if (!sourcepath) {
                            sourcepath = *arg;
                        } else {
                            printf("unrecognized argument: %s. Try --help for help.\n", *arg);
                            exit(1);
                        }
                    }
                }

                if (!skip_startup && bangra_interpreter_path) {
                    if (!bangra::compileStartupScript()) {
                        return 1;
                    }
                }

                if (sourcepath) {
                    bangra::Parser parser;
                    expr = parser.parseFile(sourcepath);
                    cpath = sourcepath;
                }
            }
        }

        if (isnone(expr)) {
            return 1;
        } else {
            bangra::compileMain(expr, cpath);
        }

    } catch (const bangra::Any &any) {
        std::cout << bangra::ansi(ANSI_STYLE_ERROR, "error:")
            << " " << bangra::get_string(any) << "\n";
        fflush(stdout);
        return 1;
    }

    return 0;
}

bangra::Any bangra_parse_file(const char *path) {
    bangra::Parser parser;
    return parser.parseFile(path);
}

//------------------------------------------------------------------------------

#endif // BANGRA_CPP_IMPL
#ifdef BANGRA_MAIN_CPP_IMPL

//------------------------------------------------------------------------------
// MAIN EXECUTABLE IMPLEMENTATION
//------------------------------------------------------------------------------

int main(int argc, char ** argv) {
    return bangra_main(argc, argv);
}

int print_number(int value) {
    printf("NUMBER: %i\n", value);
    return value + 1;
}

#endif // BANGRA_MAIN_CPP_IMPL
