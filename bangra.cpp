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
    BANGRA_VERSION_MINOR = 3,
    BANGRA_VERSION_PATCH = 0,
};

#ifdef BANGRA_CPP_IMPL
namespace bangra {
struct Value;
struct Environment;
} // namespace bangra
typedef bangra::Value Value;
typedef bangra::Environment Environment;
#else
typedef struct _Environment Environment;
typedef struct _Value Value;
#endif

typedef Value *ValueRef;

extern int bang_argc;
extern char **bang_argv;
extern char *bang_executable_path;

// high level
//------------------------------------------------------------------------------

int bangra_main(int argc, char ** argv);
ValueRef bangra_parse_file(const char *path);

// LLVM compatibility
//------------------------------------------------------------------------------

Environment *bangra_parent_env(Environment *env);
/*
void *bangra_import_c_module(ValueRef dest,
    const char *path, const char **args, int argcount);
void *bangra_import_c_string(ValueRef dest,
    const char *str, const char *path, const char **args, int argcount);
*/

// methods that apply to all types
//------------------------------------------------------------------------------

int bangra_get_kind(ValueRef expr);
int bangra_eq(Value *a, Value *b);

ValueRef bangra_clone(ValueRef expr);

ValueRef bangra_next(ValueRef expr);
ValueRef bangra_set_next(ValueRef lhs, ValueRef rhs);
ValueRef bangra_set_next_mutable(ValueRef lhs, ValueRef rhs);

void bangra_print_value(ValueRef expr, int depth);
ValueRef bangra_format_value(ValueRef expr, int depth);

const char *bangra_anchor_path(ValueRef expr);
int bangra_anchor_lineno(ValueRef expr);
int bangra_anchor_column(ValueRef expr);
int bangra_anchor_offset(ValueRef expr);
ValueRef bangra_set_anchor(
    ValueRef expr, const char *path, int lineno, int column, int offset);
ValueRef bangra_set_anchor_mutable(
    ValueRef expr, const char *path, int lineno, int column, int offset);

// pointer
//------------------------------------------------------------------------------

ValueRef bangra_ref(ValueRef lhs);
ValueRef bangra_at(ValueRef expr);
ValueRef bangra_set_at_mutable(ValueRef lhs, ValueRef rhs);

// string and symbol
//------------------------------------------------------------------------------

ValueRef bangra_string(const char *value, signed long long int size);
ValueRef bangra_symbol(const char *value);
const char *bangra_string_value(ValueRef expr);
signed long long int bangra_string_size(ValueRef expr);
ValueRef bangra_string_concat(ValueRef a, ValueRef b);
ValueRef bangra_string_slice(ValueRef expr, int start, int end);

// real
//------------------------------------------------------------------------------

ValueRef bangra_real(double value);
double bangra_real_value(ValueRef value);

// integer
//------------------------------------------------------------------------------

ValueRef bangra_integer(signed long long int value);
signed long long int bangra_integer_value(ValueRef value);

// exception handling
//------------------------------------------------------------------------------

void *bangra_xpcall (void *ctx,
    void *(*try_func)(void *),
    void *(*except_func)(void *, ValueRef));
void bangra_raise (ValueRef expr);

// metaprogramming
//------------------------------------------------------------------------------

typedef ValueRef (*bangra_preprocessor)(Environment *, ValueRef );

void bangra_error_message(
    Environment *env, ValueRef context, const char *format, ...);
void bangra_set_preprocessor(const char *name, bangra_preprocessor f);
bangra_preprocessor bangra_get_preprocessor(const char *name);
ValueRef bangra_unique_symbol(const char *name);

#if defined __cplusplus
}
#endif

#endif // BANGRA_CPP
#ifdef BANGRA_CPP_IMPL

/*
TODO:
    - validate getelementptr arguments where possible
    - validate that used values are in the same block
    - validate: Call parameter type does not match function signature!
    - validate: PHI node operands are not the same type as the result!
    - validate: Called function must be a pointer!
    - validate: Function return type does not match operand type of return inst!
    - validate: Function arguments must have first-class types!
        (passing function without pointer to constructor)
*/

#define BANGRA_HEADER "bangra"
//#define BANGRA_DEBUG_IL

//------------------------------------------------------------------------------
// SHARED LIBRARY IMPLEMENTATION
//------------------------------------------------------------------------------

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

std::string quoteReprString(const std::string &value) {
    return ansi(ANSI_STYLE_STRING,
        format("\"%s\"",
            quoteString(value, "\"").c_str()));
}

std::string quoteReprInteger(int64_t value) {
    return ansi(ANSI_STYLE_NUMBER, format("%" PRIi64, value));
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
// DATA MODEL
//------------------------------------------------------------------------------

enum ValueKind {
    V_None = 0,
    V_Pointer = 1,
    V_String = 2,
    V_Symbol = 3,
    V_Integer = 4,
    V_Real = 5,
};

static const char *valueKindName(int kind) {
    switch(kind) {
    case V_None: return "null";
    case V_Pointer: return "list";
    case V_String: return "string";
    case V_Symbol: return "symbol";
    case V_Integer: return "integer";
    case V_Real: return "real";
    default: return "#corrupted#";
    }
}

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

    static void printErrorV (const Anchor *anchor, const char *fmt, va_list args) {
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
        exit(1);
    }

};

struct Value;
static void printValue(ValueRef e, size_t depth=0, bool naked=true);
static std::string formatValue(ValueRef e, size_t depth=0, bool naked=true);

static ValueRef next(ValueRef expr);

struct Value {
    private: const ValueKind kind;
    public: Anchor anchor;
    // NULL = end of list
    private: ValueRef next;
protected:
    Value(ValueKind kind_, ValueRef next_ = nullptr) :
        kind(kind_),
        next(next_) {
    }

public:
    Anchor *findValidAnchor();
    ValueRef getNext() const {
        return next;
    }

    void setNext(ValueRef next) {
        this->next = next;
    }

    size_t count() {
        ValueRef self = this;
        size_t count = 0;
        while (self) {
            ++ count;
            self = bangra::next(self);
        }
        return count;
    }

    ValueKind getKind() const {
        return kind;
    }

    std::string getHeader() const;
};

static Value *clone(Value *value);

//------------------------------------------------------------------------------

static ValueRef at(ValueRef expr);
static bool isAtom(ValueRef expr);

// NULL = empty list
struct Pointer : Value {
protected:
    ValueRef _at;
public:
    Pointer(ValueRef at = NULL, ValueRef next_ = NULL) :
        Value(V_Pointer, next_),
        _at(at) {
    }

    ValueRef getAt() const {
        return _at;
    }

    void setAt(ValueRef at) {
        this->_at = at;
    }

    static bool classof(const Value *expr) {
        return expr->getKind() == V_Pointer;
    }

    static ValueKind kind() {
        return V_Pointer;
    }
};

static ValueRef at(ValueRef expr) {
    assert(expr && (expr->getKind() == V_Pointer));
    if (auto pointer = llvm::dyn_cast<Pointer>(expr)) {
        return pointer->getAt();
    }
    return NULL;
}

template<typename T>
static T *kindAt(ValueRef expr) {
    ValueRef val = at(expr);
    if (val) {
        return llvm::dyn_cast<T>(val);
    }
    return NULL;
}

static int kindOf(ValueRef expr) {
    if (expr) {
        return expr->getKind();
    }
    return V_None;
}

static ValueRef cons(ValueRef lhs, ValueRef rhs) {
    assert(lhs);
    lhs = clone(lhs);
    lhs->setNext(rhs);
    return lhs;
}

template<typename T>
static bool isKindOf(ValueRef expr) {
    return kindOf(expr) == T::kind();
}

static ValueRef next(ValueRef expr) {
    if (expr) {
        return expr->getNext();
    }
    return NULL;
}

static bool isAtom(ValueRef expr) {
    if (expr) {
        if (kindOf(expr) == V_Pointer) {
            if (at(expr))
                return false;
        }
    }
    // empty list
    return true;
}

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

//------------------------------------------------------------------------------

struct Integer : Value {
protected:
    int64_t value;
    bool is_unsigned;

public:
    Integer(int64_t number, bool is_unsigned_, ValueRef next_ = NULL) :
        Value(V_Integer, next_),
        value(number),
        is_unsigned(is_unsigned_)
        {}

    Integer(int64_t number, ValueRef next_ = NULL) :
        Value(V_Integer, next_),
        value(number),
        is_unsigned(false)
        {}

    bool isUnsigned() const {
        return is_unsigned;
    }

    int64_t getValue() const {
        return value;
    }

    static bool classof(const Value *expr) {
        auto kind = expr->getKind();
        return (kind == V_Integer);
    }

    static ValueKind kind() {
        return V_Integer;
    }

};

//------------------------------------------------------------------------------

struct Real : Value {
protected:
    double value;

public:
    Real(double number, ValueRef next_ = NULL) :
        Value(V_Real, next_),
        value(number)
        {}

    double getValue() const {
        return value;
    }

    static bool classof(const Value *expr) {
        auto kind = expr->getKind();
        return (kind == V_Real);
    }

    static ValueKind kind() {
        return V_Real;
    }
};

//------------------------------------------------------------------------------

struct String : Value {
protected:
    std::string value;

    String(ValueKind kind, const char *s, size_t len, ValueRef next_) :
        Value(kind, next_),
        value(s, len)
        {}

public:
    String(const char *s, size_t len, ValueRef next_ = NULL) :
        Value(V_String, next_),
        value(s, len)
        {}

    String(const char *s, ValueRef next_ = NULL) :
        Value(V_String, next_),
        value(s, strlen(s))
        {}

    const std::string &getValue() const {
        return value;
    }

    const char *c_str() const {
        return value.c_str();
    }

    size_t size() const {
        return value.size();
    };

    const char &operator [](size_t i) const {
        return value[i];
    }

    char &operator [](size_t i) {
        return value[i];
    }

    static bool classof(const Value *expr) {
        auto kind = expr->getKind();
        return (kind == V_String) || (kind == V_Symbol);
    }

    void unescape() {
        value.resize(inplace_unescape(&value[0]));
    }

    static ValueKind kind() {
        return V_String;
    }

};

const char *stringAt(ValueRef expr) {
    ValueRef val = at(expr);
    if (val) {
        if (auto sym = llvm::dyn_cast<String>(val)) {
            return sym->c_str();
        }
    }
    return NULL;
}

//------------------------------------------------------------------------------

struct Symbol : String {
    Symbol(const char *s, size_t len, ValueRef next_ = NULL) :
        String(V_Symbol, s, len, next_) {}

    Symbol(const char *s, ValueRef next_ = NULL) :
        String(V_Symbol, s, strlen(s), next_) {}

    static bool classof(const Value *expr) {
        return expr->getKind() == V_Symbol;
    }

    static ValueKind kind() {
        return V_Symbol;
    }
};

//------------------------------------------------------------------------------

Anchor *Value::findValidAnchor() {
    if (anchor.isValid()) return &anchor;
    if (auto pointer = llvm::dyn_cast<Pointer>(this)) {
        if (pointer->getAt()) {
            Anchor *result = pointer->getAt()->findValidAnchor();
            if (result) return result;
        }
    }
    if (getNext()) {
        Anchor *result = getNext()->findValidAnchor();
        if (result) return result;
    }
    return NULL;
}

std::string Value::getHeader() const {
    if (auto pointer = llvm::dyn_cast<Pointer>(this)) {
        if (pointer->getAt()) {
            if (auto head = llvm::dyn_cast<Symbol>(pointer->getAt())) {
                return head->getValue();
            }
        }
    }
    return "";
}

//------------------------------------------------------------------------------

static Value *clone(Value *value) {
    assert(value);
    switch(value->getKind()) {
        case V_Pointer: {
            return new Pointer(*llvm::cast<Pointer>(value));
        } break;
        case V_Integer: {
            return new Integer(*llvm::cast<Integer>(value));
        } break;
        case V_Real: {
            return new Real(*llvm::cast<Real>(value));
        } break;
        case V_String: {
            return new String(*llvm::cast<String>(value));
        } break;
        case V_Symbol: {
            return new Symbol(*llvm::cast<Symbol>(value));
        } break;
        default: {
            assert(false && "illegal value kind");
            return nullptr;
        } break;
    }
}

//------------------------------------------------------------------------------

// matches ((///...))
static bool isComment(ValueRef expr) {
    if (isAtom(expr)) return false;
    if (isKindOf<Symbol>(at(expr)) && !memcmp(stringAt(expr),"///",3)) {
        return true;
    }
    return false;
}

static ValueRef strip(ValueRef expr) {
    if (!expr) return nullptr;
    if (isComment(expr)) {
        // skip
        return strip(next(expr));
    } else if (!isAtom(expr)) {
        ValueRef atelem = at(expr);
        ValueRef nextelem = next(expr);
        ValueRef newatelem = strip(atelem);
        ValueRef newnextelem = strip(nextelem);
        if ((newatelem == atelem) && (newnextelem == nextelem))
            return expr;
        else
            return new Pointer(newatelem, newnextelem);
    } else {
        ValueRef nextelem = next(expr);
        ValueRef newnextelem = strip(nextelem);
        if (newnextelem == nextelem)
            return expr;
        else {
            expr = clone(expr);
            expr->setNext(newnextelem);
            return expr;
        }
    }
}

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
    double real;

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
        real = std::strtod(cursor, &end);
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

    ValueRef getAsString() {
        auto result = new String(string + 1, string_len - 2);
        initAnchor(result->anchor);
        result->unescape();
        return result;
    }

    ValueRef getAsSymbol() {
        auto result = new Symbol(string, string_len);
        initAnchor(result->anchor);
        result->unescape();
        return result;
    }

    ValueRef getAsInteger() {
        auto result = new Integer(integer, is_unsigned);
        initAnchor(result->anchor);
        return result;
    }

    ValueRef getAsReal() {
        auto result = new Real(real);
        initAnchor(result->anchor);
        return result;
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
        ValueRef result;
        ValueRef start;
        ValueRef lastend;
        ValueRef tail;
        Anchor anchor;

        ListBuilder(Lexer &lexer) :
            result(nullptr),
            start(nullptr),
            lastend(nullptr),
            tail(nullptr) {
            lexer.initAnchor(anchor);
        }

        void resetStart() {
            start = nullptr;
        }

        bool split() {
            // wrap newly added elements in new list
            if (!start) {
                return false;
            }
            ValueRef sublist = new Pointer(start);
            if (lastend) {
                // if a previous tail is known, reroute
                lastend->setNext(sublist);
            } else {
                // list starts with sublist
                result = sublist;
            }
            lastend = sublist;
            tail = sublist;
            start = nullptr;
            return true;
        }

        void append(ValueRef newtail) {
            if (tail) {
                tail->setNext(newtail);
            } else if (!result) {
                result = newtail;
                //result->anchor = anchor;
            }
            tail = newtail;
            if (!start)
                start = tail;
        }

        ValueRef getResult() {
            auto ptr = new Pointer(result);
            ptr->anchor = anchor;
            return ptr;
        }

    };

    ValueRef parseList(int end_token) {
        ListBuilder builder(lexer);
        lexer.readToken();
        while (true) {
            if (lexer.token == end_token) {
                break;
            } else if (lexer.token == token_escape) {
                int column = lexer.column();
                lexer.readToken();
                auto elem = parseNaked(column, 1, end_token);
                if (errors) return nullptr;
                builder.append(elem);
            } else if (lexer.token == token_eof) {
                error("missing closing bracket");
                // point to beginning of list
                error_origin = builder.anchor;
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

    ValueRef parseAny () {
        assert(lexer.token != token_eof);
        if (lexer.token == token_open) {
            return parseList(token_close);
        } else if (lexer.token == token_square_open) {
            auto list = parseList(token_square_close);
            if (errors) return nullptr;
            auto result = new Pointer(new Symbol("[", at(list)));
            result->anchor = list->anchor;
            return result;
        } else if (lexer.token == token_curly_open) {
            auto list = parseList(token_curly_close);
            if (errors) return nullptr;
            auto result = new Pointer(new Symbol("{", at(list)));
            result->anchor = list->anchor;
            return result;
        } else if ((lexer.token == token_close)
            || (lexer.token == token_square_close)
            || (lexer.token == token_curly_close)) {
            error("stray closing bracket");
        } else if (lexer.token == token_string) {
            return lexer.getAsString();
        } else if (lexer.token == token_symbol) {
            return lexer.getAsSymbol();
        } else if (lexer.token == token_integer) {
            return lexer.getAsInteger();
        } else if (lexer.token == token_real) {
            return lexer.getAsReal();
        } else {
            error("unexpected token: %c (%i)", *lexer.cursor, (int)*lexer.cursor);
        }

        return nullptr;
    }

    ValueRef parseNaked (int column = 0, int depth = 0, int end_token = token_none) {
        int lineno = lexer.lineno;

        bool escape = false;
        int subcolumn = 0;

        ListBuilder builder(lexer);

        while (lexer.token != token_eof) {
            if (lexer.token == end_token) {
                break;
            } if (lexer.token == token_escape) {
                escape = true;
                lexer.readToken();
                if (lexer.lineno <= lineno) {
                    error("escape character is not at end of line");
                    parse_origin = builder.anchor;
                    return nullptr;
                }
                lineno = lexer.lineno;
            } else if (lexer.lineno > lineno) {
                if (depth > 0) {
                    if (subcolumn == 0) {
                        subcolumn = lexer.column();
                    } else if (lexer.column() != subcolumn) {
                        error("indentation mismatch");
                        parse_origin = builder.anchor;
                        return nullptr;
                    }
                } else {
                    subcolumn = lexer.column();
                }
                if (column != subcolumn) {
                    if ((column + 4) != subcolumn) {
                        //printf("%i %i\n", column, subcolumn);
                        error("indentations must nest by 4 spaces.");
                        return nullptr;
                    }
                }

                escape = false;
                builder.resetStart();
                lineno = lexer.lineno;
                // keep adding elements while we're in the same line
                while ((lexer.token != token_eof)
                        && (lexer.token != end_token)
                        && (lexer.lineno == lineno)) {
                    auto elem = parseNaked(subcolumn, depth + 1, end_token);
                    if (errors) return nullptr;
                    builder.append(elem);
                }
            } else if (lexer.token == token_statement) {
                if (!builder.split()) {
                    error("empty expression");
                    return nullptr;
                }
                lexer.readToken();
                if (depth > 0) {
                    // if we are in the same line and there was no preceding ":",
                    // continue in parent
                    if (lexer.lineno == lineno)
                        break;
                }
            } else {
                auto elem = parseAny();
                if (errors) return nullptr;
                builder.append(elem);
                lineno = lexer.next_lineno;
                lexer.readToken();
            }

            if (depth > 0) {
                if ((!escape || (lexer.lineno > lineno))
                    && (lexer.column() <= column)) {
                    break;
                }
            }
        }

        if (!builder.result) {
            assert(depth == 0);
            return builder.getResult();
        } else if (!builder.result->getNext()) {
            return builder.result;
        } else {
            return builder.getResult();
        }
    }

    ValueRef parseMemory (
        const char *input_stream, const char *eof, const char *path, int offset = 0) {
        init();
        lexer.init(input_stream, eof, path, offset);

        lexer.readToken();

        auto result = parseNaked(lexer.column());

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
            return nullptr;
        }

        assert(result);
        return strip(result);
    }

    ValueRef parseFile (const char *path) {
        auto file = MappedFile::open(path);
        if (file) {
            return parseMemory(
                file->strptr(), file->strptr() + file->size(),
                path);
        } else {
            fprintf(stderr, "unable to open file: %s\n", path);
            return NULL;
        }
    }


};

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

static bool isNested(ValueRef e) {
    if (isAtom(e)) return false;
    e = at(e);
    while (e) {
        if (!isAtom(e)) {
            return true;
        }
        e = next(e);
    }
    return false;
}

template<typename T>
static void streamAnchor(T &stream, ValueRef e, size_t depth=0) {
    if (e) {
        Anchor *anchor = e->findValidAnchor();
        if (!anchor)
            anchor = &e->anchor;
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
static void streamValue(T &stream, ValueRef e, size_t depth=0, bool naked=true) {
    if (naked) {
        streamAnchor(stream, e, depth);
    }

	if (!e) {
        stream << "#null#";
        if (naked)
            stream << '\n';
        return;
    }

	switch(e->getKind()) {
	case V_Pointer: {
        e = at(e);
        if (!e) {
            stream << "()";
            if (naked)
                stream << '\n';
            break;
        }
        if (naked) {
            int offset = 0;
            bool single = !next(e);
        print_terse:
            streamValue(stream, e, depth, false);
            e = next(e);
            offset++;
            while (e) {
                if (isNested(e))
                    break;
                stream << ' ';
                streamValue(stream, e, depth, false);
                e = next(e);
                offset++;
            }
            stream << (single?";\n":"\n");
        //print_sparse:
            while (e) {
                if (isAtom(e) // not a list
                    && (offset >= 1) // not first element in list
                    && next(e) // not last element in list
                    && !isNested(next(e))) { // next element can be terse packed too
                    single = false;
                    streamAnchor(stream, e, depth + 1);
                    stream << "\\ ";
                    goto print_terse;
                }
                streamValue(stream, e, depth + 1);
                e = next(e);
                offset++;
            }

        } else {
            stream << '(';
            int offset = 0;
            while (e) {
                if (offset > 0)
                    stream << ' ';
                streamValue(stream, e, depth + 1, false);
                e = next(e);
                offset++;
            }
            stream << ')';
            if (naked)
                stream << '\n';
        }
    } return;
    case V_Integer: {
        const Integer *a = llvm::cast<Integer>(e);

        if (a->isUnsigned())
            stream << format("%" PRIu64, a->getValue());
        else
            stream << format("%" PRIi64, a->getValue());
        if (naked)
            stream << '\n';
    } return;
    case V_Real: {
        const Real *a = llvm::cast<Real>(e);
        stream << format("%g", a->getValue());
        if (naked)
            stream << '\n';
    } return;
	case V_Symbol:
	case V_String: {
        const String *a = llvm::cast<String>(e);
		if (a->getKind() == V_String) stream << '"';
        streamString(stream, a->getValue(), (a->getKind() == V_Symbol)?"[]{}()\"":"\"");
		if (a->getKind() == V_String) stream << '"';
        if (naked)
            stream << '\n';
    } return;
    default:
        printf("invalid kind: %i\n", e->getKind());
        assert (false); break;
	}
}

static std::string formatValue(ValueRef e, size_t depth, bool naked) {
    std::stringstream ss;
    streamValue(ss, e, depth, naked);
    return ss.str();
}

static void printValue(ValueRef e, size_t depth, bool naked) {
    streamValue(std::cout, e, depth, naked);
}

//------------------------------------------------------------------------------
// TYPE SYSTEM
//------------------------------------------------------------------------------

#define TYPE_ENUM_KINDS() \
    TYPE_KIND(Any) \
    TYPE_KIND(Void) \
    TYPE_KIND(Null) \
    TYPE_KIND(Integer) \
    TYPE_KIND(Real) \
    TYPE_KIND(Pointer) \
    TYPE_KIND(Array) \
    TYPE_KIND(Vector) \
    TYPE_KIND(Tuple) \
    TYPE_KIND(Struct) \
    TYPE_KIND(CFunction) \
    TYPE_KIND(Continuation)

enum TypeKind {
#define TYPE_KIND(NAME) T_ ## NAME,
    TYPE_ENUM_KINDS()
#undef TYPE_KIND
};

//------------------------------------------------------------------------------

struct Type;
typedef std::vector<Type *> TypeArray;
typedef std::vector<std::string> NameArray;

struct Type {
private:
    const TypeKind kind;

    static Type *newIntegerType(int _width, bool _signed);
    static Type *newRealType(int _width);
    static Type *newPointerType(Type *_element);
    static Type *newArrayType(Type *_element, unsigned _size);
    static Type *newVectorType(Type *_element, unsigned _size);
    static Type *newTupleType(TypeArray _elements);
    static Type *newCFunctionType(
        Type *_returntype, TypeArray _parameters, bool vararg);
    static Type *newContinuationType(TypeArray _parameters);

protected:
    Type(TypeKind kind_) :
        kind(kind_)
        {}

public:
    static Type *TypePointer;
    static Type *Void;
    static Type *Null;
    static Type *Bool;
    static Type *Empty;
    static Type *Any;

    static Type *Opaque;
    static Type *OpaquePointer;

    static Type *Int8;
    static Type *Int16;
    static Type *Int32;
    static Type *Int64;

    static Type *UInt8;
    static Type *UInt16;
    static Type *UInt32;
    static Type *UInt64;

    static Type *Half;
    static Type *Float;
    static Type *Double;

    static Type *Rawstring;

    TypeKind getKind() const {
        return kind;
    }

    static void initTypes();

    static std::function<Type * (int, bool)> Integer;
    static std::function<Type * (int)> Real;
    static std::function<Type * (Type *)> Pointer;
    static std::function<Type * (Type *, unsigned)> Array;
    static std::function<Type * (Type *, unsigned)> Vector;
    static std::function<Type * (TypeArray)> Tuple;
    static Type *Struct(
        const std::string &name, bool builtin = false, bool union_ = false);
    static std::function<Type * (Type *, TypeArray, bool)> CFunction;
    static std::function<Type * (TypeArray)> Continuation;

    std::string getRepr();
};

Type *Type::TypePointer;
Type *Type::Void;
Type *Type::Null;
Type *Type::Bool;
Type *Type::Int8;
Type *Type::Int16;
Type *Type::Int32;
Type *Type::Int64;
Type *Type::UInt8;
Type *Type::UInt16;
Type *Type::UInt32;
Type *Type::UInt64;
Type *Type::Half;
Type *Type::Float;
Type *Type::Double;
Type *Type::Rawstring;
Type *Type::Empty;
Type *Type::Any;
Type *Type::Opaque;
Type *Type::OpaquePointer;
std::function<Type * (int, bool)> Type::Integer = memo(Type::newIntegerType);
std::function<Type * (int)> Type::Real = memo(Type::newRealType);
std::function<Type * (Type *)> Type::Pointer = memo(Type::newPointerType);
std::function<Type * (Type *, unsigned)> Type::Array = memo(Type::newArrayType);
std::function<Type * (Type *, unsigned)> Type::Vector = memo(Type::newVectorType);
std::function<Type * (TypeArray)> Type::Tuple = memo(Type::newTupleType);
std::function<Type * (Type *, TypeArray, bool)> Type::CFunction = memo(Type::newCFunctionType);
std::function<Type * (TypeArray)> Type::Continuation = memo(Type::newContinuationType);

//------------------------------------------------------------------------------

template<class T, TypeKind KindT>
struct TypeImpl : Type {
    TypeImpl() :
        Type(KindT)
        {}

    static bool classof(const Type *node) {
        return node->getKind() == KindT;
    }
};

//------------------------------------------------------------------------------

struct VoidType : TypeImpl<VoidType, T_Void> {
    std::string getRepr() {
        return ansi(ANSI_STYLE_TYPE, "void");
    }
};

//------------------------------------------------------------------------------

struct NullType : TypeImpl<NullType, T_Null> {
    std::string getRepr() {
        return ansi(ANSI_STYLE_TYPE, "null");
    }
};

//------------------------------------------------------------------------------

struct AnyType : TypeImpl<AnyType, T_Any> {
    std::string getRepr() {
        return ansi(ANSI_STYLE_ERROR, "any");
    }
};

//------------------------------------------------------------------------------

struct IntegerType : TypeImpl<IntegerType, T_Integer> {
protected:
    int width;
    bool is_signed;

public:
    int getWidth() const { return width; }
    bool isSigned() const { return is_signed; }

    IntegerType(int _width, bool _signed) :
        width(_width),
        is_signed(_signed)
        {}

    std::string getRepr() {
        return ansi(ANSI_STYLE_TYPE, format("%sint%i", is_signed?"":"u", width));
    }

};

Type *Type::newIntegerType(int _width, bool _signed) {
    return new IntegerType(_width, _signed);
}

//------------------------------------------------------------------------------

struct RealType : TypeImpl<RealType, T_Real> {
protected:
    int width;

public:
    int getWidth() const { return width; }

    RealType(int _width) :
        width(_width)
        {}

    std::string getRepr() {
        return ansi(ANSI_STYLE_TYPE, format("real%i", width));
    }
};

Type *Type::newRealType(int _width) {
    return new RealType(_width);
}

//------------------------------------------------------------------------------

struct PointerType : TypeImpl<PointerType, T_Pointer> {
protected:
    Type *element;

public:
    PointerType(Type *_element) :
        element(_element)
        {}

    Type *getElement() {
        return element;
    }

    std::string getRepr() {
        return format("(%s %s)",
                ansi(ANSI_STYLE_KEYWORD, "pointer").c_str(),
                element->getRepr().c_str());
    }

};

Type *Type::newPointerType(Type *_element) {
    return new PointerType(_element);
}

//------------------------------------------------------------------------------

struct ArrayType : TypeImpl<ArrayType, T_Array> {
protected:
    Type *element;
    unsigned size;

public:
    Type *getElement() {
        return element;
    }

    unsigned getSize() {
        return size;
    }

    ArrayType(Type *_element, unsigned _size) :
        element(_element),
        size(_size)
        {}

    std::string getRepr() {
        return format("(%s %s %i)",
                ansi(ANSI_STYLE_KEYWORD, "array").c_str(),
                element->getRepr().c_str(),
                size);
    }

};

Type *Type::newArrayType(Type *_element, unsigned _size) {
    return new ArrayType(_element, _size);
}

//------------------------------------------------------------------------------

struct VectorType : TypeImpl<VectorType, T_Vector> {
protected:
    Type *element;
    unsigned size;

public:
    Type *getElement() {
        return element;
    }

    unsigned getSize() {
        return size;
    }

    VectorType(Type *_element, unsigned _size) :
        element(_element),
        size(_size)
        {}

    std::string getRepr() {
        return format("(%s %s %i)",
                ansi(ANSI_STYLE_KEYWORD, "vector").c_str(),
                element->getRepr().c_str(),
                size);
    }

};

Type *Type::newVectorType(Type *_element, unsigned _size) {
    return new VectorType(_element, _size);
}

//------------------------------------------------------------------------------

struct TupleType : TypeImpl<TupleType, T_Tuple> {
protected:
    TypeArray elements;

public:
    static std::string getSpecRepr(const TypeArray &elements) {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "tuple");
        for (size_t i = 0; i < elements.size(); ++i) {
            ss << " ";
            ss << elements[i]->getRepr();
        }
        ss << ")";
        return ss.str();
    }

    TupleType(const TypeArray &_elements) :
        elements(_elements)
        {}

    std::string getRepr() {
        return getSpecRepr(elements);
    }

};

Type *Type::newTupleType(TypeArray _elements) {
    return new TupleType(_elements);
}

//------------------------------------------------------------------------------

struct StructType : TypeImpl<StructType, T_Struct> {
public:
    struct Field {
    protected:
        std::string name;
        Type *type;
        Anchor anchor;

    public:
        Field() :
            type(nullptr) {}

        Field(const std::string &name_, Type *type_) :
            name(name_),
            type(type_)
            {}

        Field(const std::string &name_, Type *type_, const Anchor &anchor_) :
            name(name_),
            type(type_),
            anchor(anchor_)
            {}

        Type *getType() const {
            return type;
        }

        std::string getName() const {
            return name;
        }

        bool isUnnamed() const {
            return name.size() == 0;
        }

        std::string getRepr() const {
            return format("(%s %s)",
                    quoteReprString(name).c_str(),
                    type->getRepr().c_str());
        }
    };

protected:
    std::string name;
    std::vector<Field> fields;
    std::unordered_map<std::string, size_t> byname;
    bool builtin;
    bool isunion;
    Anchor anchor;

public:

    StructType(const std::string &name_, bool builtin_, bool union_) :
        name(name_),
        builtin(builtin_),
        isunion(union_)
        {}

    const Anchor &getAnchor() {
        return anchor;
    }

    void setAnchor(const Anchor &anchor_) {
        anchor = anchor_;
    }

    void addField(const Field &field) {
        if (!field.isUnnamed()) {
            byname[field.getName()] = fields.size();
        }
        fields.push_back(field);
    }

    void addFields(const std::vector<Field> &fields_) {
        for (auto &field : fields_) {
            addField(field);
        }
    }

    size_t getFieldIndex(const std::string &name) {
        auto it = byname.find(name);
        if (it != byname.end()) {
            return it->second;
        } else {
            return (size_t)-1;
        }
    }

    size_t getFieldCount() {
        return fields.size();
    }

    const Field &getField(size_t i) {
        assert(i < fields.size());
        return fields[i];
    }

    std::string getName() {
        return name;
    }

    bool isUnion() {
        return isunion;
    }

    bool isBuiltin() {
        return builtin;
    }

    bool isUnnamed() {
        return name.size() == 0;
    }

    std::string getNameRepr() {
        std::stringstream ss;
        if (isUnnamed()) {
            ss << this;
        } else {
            ss << quoteReprString(name);
        }
        return ss.str();
    }

    std::string getRepr() {
        if (builtin) {
            return ansi(ANSI_STYLE_TYPE, name).c_str();
        } else {
            std::stringstream ss;
            ss << "(" << ansi(ANSI_STYLE_KEYWORD,
                isUnion()?"union":"struct") << " ";
            ss << getNameRepr();
            for (size_t i = 0; i < fields.size(); ++i) {
                ss << " " << fields[i].getRepr();
            }
            ss << ")";
            return ss.str();
        }
    }

};

Type *Type::Struct(const std::string &name, bool builtin, bool union_) {
    return new StructType(name, builtin, union_);
}

//------------------------------------------------------------------------------

struct ContinuationType : TypeImpl<ContinuationType, T_Continuation> {
protected:
    TypeArray parameters;

public:
    static std::string getSpecRepr(const TypeArray &parameters) {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "fn") << " ";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i != 0)
                ss << " ";
            ss << parameters[i]->getRepr();
        }
        ss << ")";
        return ss.str();
    }

    size_t getParameterCount() const {
        return parameters.size();
    }

    Type *getParameter(int index) const {
        if (index >= (int)parameters.size())
            return NULL;
        else
            return parameters[index];
    }

    ContinuationType(const TypeArray &_parameters) :
        parameters(_parameters)
        {}

    std::string getRepr() {
        return getSpecRepr(parameters);
    }

};

Type *Type::newContinuationType(TypeArray _parameters) {
    return new ContinuationType(_parameters);
}

//------------------------------------------------------------------------------

struct CFunctionType : TypeImpl<CFunctionType, T_CFunction> {
protected:
    Type *result;
    TypeArray parameters;
    bool isvararg;

public:
    static std::string getSpecRepr(Type *result,
        const TypeArray &parameters, bool isvararg) {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "cdecl") << " ";
        ss << result->getRepr();
        ss << " (";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i != 0)
                ss << " ";
            ss << parameters[i]->getRepr();
        }
        if (isvararg) {
            if (parameters.size())
                ss << " ";
            ss << ansi(ANSI_STYLE_KEYWORD, "...");
        }
        ss << "))";
        return ss.str();
    }

    size_t getParameterCount() const {
        return parameters.size();
    }

    Type *getParameter(int index) const {
        if (index >= (int)parameters.size())
            return NULL;
        else
            return parameters[index];
    }

    Type *getResult() const {
        return result;
    }

    bool isVarArg() const { return isvararg; }

    CFunctionType(Type *result_, const TypeArray &_parameters, bool _isvararg) :
        result(result_),
        parameters(_parameters),
        isvararg(_isvararg)
        {}

    std::string getRepr() {
        return getSpecRepr(result, parameters, isvararg);
    }

};

Type *Type::newCFunctionType(Type *_returntype, TypeArray _parameters, bool _isvararg) {
    return new CFunctionType(_returntype, _parameters, _isvararg);
}

//------------------------------------------------------------------------------

std::string Type::getRepr() {
#define TYPE_KIND(NAME) \
    case T_ ## NAME: {\
        auto spec = llvm::cast<NAME ## Type>(this); \
        return spec->getRepr(); \
    } break;
    switch(kind) {
    TYPE_ENUM_KINDS()
    }
#undef TYPE_KIND
}

//------------------------------------------------------------------------------

void Type::initTypes() {
    TypePointer = Pointer(Struct("Type", true));
    //BasicBlock = Struct("BasicBlock", true);

    Empty = Tuple({});

    Any = new AnyType();
    Void = new VoidType();
    Null = new NullType();

    Opaque = Struct("opaque", true);
    OpaquePointer = Pointer(Opaque);

    Bool = Integer(1, false);

    Int8 = Integer(8, true);
    Int16 = Integer(16, true);
    Int32 = Integer(32, true);
    Int64 = Integer(64, true);

    UInt8 = Integer(8, false);
    UInt16 = Integer(16, false);
    UInt32 = Integer(32, false);
    UInt64 = Integer(64, false);

    Half = Real(16);
    Float = Real(32);
    Double = Real(64);

    Rawstring = Pointer(Int8);

}

//------------------------------------------------------------------------------
// MID-LEVEL IL
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

struct ILValue;
struct ILContinuation;
struct ILParameter;
struct ILPrimitive;
struct ILBuilder;
struct ILIntrinsic;
struct ILConstant;

//------------------------------------------------------------------------------

#define ILVALUE_ENUM_KINDS() \
    ILVALUE_KIND(Intrinsic) \
    ILVALUE_KIND_ABSTRACT(Primitive) \
        ILVALUE_KIND_ABSTRACT(Constant) \
            ILVALUE_KIND(ConstString) \
            ILVALUE_KIND(ConstInteger) \
            ILVALUE_KIND(ConstReal) \
            ILVALUE_KIND(ConstPointer) \
            ILVALUE_KIND(ConstTuple) \
            ILVALUE_KIND(ConstStruct) \
            ILVALUE_KIND(ConstClosure) \
            ILVALUE_KIND(ConstCFunction) \
            ILVALUE_KIND(ConstBuiltin) \
            ILVALUE_KIND_EOK(ConstantEnd) \
        ILVALUE_KIND(Parameter) \
        ILVALUE_KIND_EOK(PrimitiveEnd) \
    ILVALUE_KIND(Continuation)

//------------------------------------------------------------------------------

struct ILValue {
#define ILVALUE_KIND(NAME) NAME,
#define ILVALUE_KIND_ABSTRACT(NAME) NAME,
#define ILVALUE_KIND_EOK(NAME) NAME,
    enum Kind {
        ILVALUE_ENUM_KINDS()
    };
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK

public:

    const Kind kind;
    Anchor anchor;

    ILValue(Kind kind_) :
        kind(kind_)
        {}

    std::string getRepr () const;
    std::string getRefRepr () const;
    Type *inferType() const;

    Type *getType() const {
        return inferType();
    }
};

std::string getRepr (const ILValue *self);

static void ilMessage (const ILValue *value, const char *format, ...) {
    const Anchor *anchor = NULL;
    if (value) {
        std::cout << "at\n  " << value->getRepr() << "\n";
        if (value->anchor.isValid()) {
            anchor = &value->anchor;
        }
    }
    va_list args;
    va_start (args, format);
    Anchor::printMessageV(anchor, format, args);
    va_end (args);
}

static void ilError (const ILValue *value, const char *format, ...) {
    const Anchor *anchor = NULL;
    if (value) {
        std::cout << "at\n  " << value->getRepr() << "\n";
        if (value->anchor.isValid()) {
            anchor = &value->anchor;
        }
    }
    va_list args;
    va_start (args, format);
    Anchor::printErrorV(anchor, format, args);
    va_end (args);
}

//------------------------------------------------------------------------------

template<typename SelfT, ILValue::Kind KindT, typename BaseT>
struct ILValueImpl : BaseT {
    typedef ILValueImpl<SelfT, KindT, BaseT> ValueImplType;

    ILValueImpl() :
        BaseT(KindT)
    {}

    static bool classof(const ILValue *value) {
        return value->kind == KindT;
    }
};

//------------------------------------------------------------------------------

struct ILPrimitive : ILValue {
    ILPrimitive(Kind kind_) :
        ILValue(kind_)
        {}

    std::string getRepr () const {
        return getRefRepr();
    }

    static bool classof(const ILValue *value) {
        return (value->kind >= Primitive) && (value->kind < PrimitiveEnd);
    }
};

//------------------------------------------------------------------------------

struct ILConstant : ILPrimitive {
    ILConstant(Kind kind_) :
        ILPrimitive(kind_)
        {}

    static bool classof(const ILValue *value) {
        return (value->kind >= Constant) && (value->kind < ConstantEnd);
    }
};

//------------------------------------------------------------------------------

struct ILParameter :
    ILValueImpl<ILParameter, ILValue::Parameter, ILPrimitive> {
    ILContinuation *parent;
    size_t index;
    Type *parameter_type;

    ILParameter() :
        parent(nullptr),
        index(-1),
        parameter_type(nullptr) {
    }

    ILContinuation *getParent() const {
        return parent;
    }

    Type *inferType() const {
        if (parameter_type)
            return parameter_type;
        else
            return Type::Any;
    }

    std::string getRepr() const {
        return format("%s%zu %s %s",
            ansi(ANSI_STYLE_OPERATOR,"@").c_str(),
            index,
            ansi(ANSI_STYLE_OPERATOR,":").c_str(),
            getType()->getRepr().c_str());
    }

    std::string getRefRepr () const;

    static ILParameter *create(Type *type = nullptr) {
        auto value = new ILParameter();
        value->index = (size_t)-1;
        value->parameter_type = type;
        return value;
    }
};

//------------------------------------------------------------------------------

struct ILIntrinsic :
    ILValueImpl<ILIntrinsic, ILValue::Intrinsic, ILValue> {
    static ILIntrinsic *Branch;
    static ILIntrinsic *Return;

    std::string name;
    Type *intrinsic_type;

    Type *inferType() const {
        return intrinsic_type;
    }

    std::string getRepr () const {
        std::stringstream ss;
        ss << ansi(ANSI_STYLE_INSTRUCTION, name);
        ss << " " << ansi(ANSI_STYLE_OPERATOR, ":") << " ";
        ss << getType()->getRepr();
        return ss.str();
    }

    std::string getRefRepr () const {
        return ansi(ANSI_STYLE_INSTRUCTION, name);
    }

    static ILIntrinsic *create(
        const std::string &name, Type *type) {
        assert(type);
        auto value = new ILIntrinsic();
        value->name = name;
        value->intrinsic_type = type;
        return value;
    }

    static void initIntrinsics() {
        Branch = create("branch",
            Type::Continuation({ Type::Bool,
                Type::Continuation({}),
                Type::Continuation({}) }));
        Return = create("return",
            Type::Continuation({}));
    }
};

ILIntrinsic *ILIntrinsic::Branch;
ILIntrinsic *ILIntrinsic::Return;

//------------------------------------------------------------------------------

struct ILContinuation :
    ILValueImpl<ILContinuation, ILValue::Continuation, ILValue> {
private:
    static int64_t unique_id_counter;
protected:
    int64_t uid;

public:
    ILContinuation() :
        uid(unique_id_counter++) {
    }

    std::string name;
    std::vector<ILParameter *> parameters;
    std::vector<ILValue *> values;

    void clear() {
        parameters.clear();
        values.clear();
    }

    size_t getParameterCount() {
        return parameters.size();
    }

    ILParameter *getParameter(size_t i) {
        return parameters[i];
    }

    size_t getValueCount() {
        return values.size();
    }

    ILValue *getValue(size_t i) {
        return values[i];
    }

    size_t getArgumentCount() {
        return values.size() - 1;
    }

    ILValue *getArgument(size_t i) {
        return values[i + 1];
    }

    std::string getRepr () const {
        std::stringstream ss;
        ss << getRefRepr();
        ss << " " << ansi(ANSI_STYLE_OPERATOR, "(");
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i != 0) {
                ss << ansi(ANSI_STYLE_OPERATOR, ", ");
            }
            ss << parameters[i]->getRepr();
        }
        ss << ansi(ANSI_STYLE_OPERATOR, ")");
        if (values.size()) {
            for (size_t i = 0; i < values.size(); ++i) {
                ss << " ";
                ss << values[i]->getRefRepr();
            }
        } else {
            ss << ansi(ANSI_STYLE_ERROR, "<missing term>");
        }
        return ss.str();
    }

    std::string getRefRepr () const {
        return format("%s%s%" PRId64,
            ansi(ANSI_STYLE_KEYWORD, "λ").c_str(),
            name.c_str(),
            uid);
    }

    Type *inferType() const {
        std::vector<Type *> params;
        for (size_t i = 0; i < parameters.size(); ++i) {
            params.push_back(parameters[i]->getType());
        }
        return Type::Continuation(params);
    }

    ILParameter *appendParameter(ILParameter *param) {
        param->parent = this;
        param->index = parameters.size();
        parameters.push_back(param);
        return param;
    }

    static ILContinuation *create(
        size_t paramcount = 0,
        const std::string &name = "") {
        auto value = new ILContinuation();
        value->name = name;
        for (size_t i = 0; i < paramcount; ++i) {
            value->appendParameter(ILParameter::create());
        }
        return value;
    }
};

int64_t ILContinuation::unique_id_counter = 1;

std::string ILParameter::getRefRepr () const {
    auto parent = getParent();
    if (parent) {
        return format("%s%s%zu",
            parent->getRefRepr().c_str(),
            ansi(ANSI_STYLE_OPERATOR, "@").c_str(),
            index);
    } else {
        return ansi(ANSI_STYLE_ERROR, "<unbound>");
    }
}

//------------------------------------------------------------------------------

struct ILConstString :
    ILValueImpl<ILConstString, ILValue::ConstString, ILConstant> {
    std::string value;

    static ILConstString *create(const std::string &s) {
        auto result = new ILConstString();
        result->value = s;
        return result;
    }

    static ILConstString *create(String *c) {
        assert(c);
        return create(c->getValue());
    }

    Type *inferType() const {
        return Type::Array(Type::Int8, value.size() + 1);
    }

    std::string getRefRepr() const {
        return quoteReprString(value);
    }
};

//------------------------------------------------------------------------------

struct ILConstInteger :
    ILValueImpl<ILConstInteger, ILValue::ConstInteger, ILConstant> {
    int64_t value;
    Type *value_type;

    static ILConstInteger *create(int64_t value, IntegerType *cdest) {
        assert(cdest);
        auto result = new ILConstInteger();
        result->value_type = cdest;
        result->value = value;
        return result;
    }

    static ILConstInteger *create(Integer *c, IntegerType *cdest) {
        assert(c);
        return create(c->getValue(), cdest);
    }

    Type *inferType() const {
        return value_type;
    }

    std::string getRefRepr() const {
        if (value_type == Type::Bool) {
            return ansi(ANSI_STYLE_KEYWORD,
                value?"true":"false");
        } else {
            return ansi(ANSI_STYLE_NUMBER,
                format("%" PRIi64, value));
        }
    }
};

//------------------------------------------------------------------------------

struct ILConstReal :
    ILValueImpl<ILConstReal, ILValue::ConstReal, ILConstant> {
    double value;
    Type *value_type;

    static ILConstReal *create(double value, RealType *cdest) {
        assert(cdest);
        auto result = new ILConstReal();
        result->value_type = cdest;
        result->value = value;
        return result;
    }

    static ILConstReal *create(Real *c, RealType *cdest) {
        assert(c);
        return create(c->getValue(), cdest);
    }

    Type *inferType() const {
        return value_type;
    }

    std::string getRefRepr() const {
        return ansi(ANSI_STYLE_NUMBER,
            format("%f", value));
    }
};

//------------------------------------------------------------------------------

struct ILConstTuple :
    ILValueImpl<ILConstTuple, ILValue::ConstTuple, ILConstant> {
    std::vector<ILConstant *> values;

    static ILConstTuple *create(
        const std::vector<ILConstant *> &values_) {
        auto result = new ILConstTuple();
        result->values = values_;
        return result;
    }

    Type *inferType() const {
        TypeArray types;
        for (auto &value : values) {
            types.push_back(value->getType());
        }
        return Type::Tuple(types);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "tupleof");
        for (auto &value : values) {
            ss << " " << value->getRefRepr();
        }
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct ILConstStruct :
    ILValueImpl<ILConstStruct, ILValue::ConstTuple, ILConstant> {
    std::vector<ILConstant *> values;
    StructType *struct_type;

    static ILConstStruct *create(const std::vector<ILConstant *> &values,
        StructType *struct_type) {
        auto result = new ILConstStruct();
        result->values = values;
        result->struct_type = struct_type;
        assert(result->values.size() == struct_type->getFieldCount());
        return result;
    }

    void addField(ILConstant *c, const StructType::Field &field) {
        struct_type->addField(field);
        values.push_back(c);
    }

    Type *inferType() const {
        return struct_type;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << struct_type->getNameRepr();
        ss << " " << this;
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct ILConstPointer :
    ILValueImpl<ILConstPointer, ILValue::ConstPointer, ILConstant> {
    void *value;
    Type *pointer_type;

    static ILConstPointer *create(
        void *value, Type *pointer_type) {
        auto result = new ILConstPointer();
        result->value = value;
        result->pointer_type = pointer_type;
        return result;
    }

    Type *inferType() const {
        return pointer_type;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        if (pointer_type == Type::TypePointer) {
            ss << "(" << pointer_type->getRepr() << " "
                << ((Type *)value)->getRepr() << ")";
        } else {
            ss << "(" << pointer_type->getRepr() << " " << value << ")";
        }
        return ss.str();
    }
};

/*
static Type *extract_consttype(const ILValue *value) {
    if (auto resulttype = llvm::dyn_cast<ILConstTypePointer>(value.get())) {
        return resulttype->value;
    }
    return Type::Any;
}
*/

//------------------------------------------------------------------------------

typedef ILConstant *(*ILBuiltinFunction)(const std::vector<ILConstant *> &args);

struct ILConstBuiltin :
    ILValueImpl<ILConstBuiltin, ILValue::ConstBuiltin, ILConstant> {

    ILBuiltinFunction handler;

    static ILConstBuiltin *create(ILBuiltinFunction func) {
        auto result = new ILConstBuiltin();
        result->handler = func;
        return result;
    }

    Type *inferType() const {
        return Type::Any;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "builtin");
        ss << " " << handler;
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct ILConstCFunction :
    ILValueImpl<ILConstCFunction, ILValue::ConstCFunction, ILConstant> {
    std::string name;
    Type *function_type;

    static ILConstCFunction *create(
        const std::string &name,
        Type *function_type) {
        auto result = new ILConstCFunction();
        result->name = name;
        result->function_type = function_type;
        return result;
    }

    Type *inferType() const {
        return function_type;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "cfunc");
        ss << " " << name;
        ss << " " << function_type->getRepr();
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct ILFrame;

struct ILConstClosure :
    ILValueImpl<ILConstClosure, ILValue::ConstClosure, ILConstant> {
    ILContinuation *cont;
    ILFrame *frame;

    static ILConstClosure *create(
        ILContinuation *cont,
        ILFrame *frame) {
        auto result = new ILConstClosure();
        result->cont = cont;
        result->frame = frame;
        return result;
    }

    Type *inferType() const {
        return Type::Any;
    }

    std::string getRefRepr() const;
};

//------------------------------------------------------------------------------

std::string ILValue::getRepr () const {
#define ILVALUE_KIND(NAME) \
    case ILValue::NAME: { \
        auto spec = llvm::cast<IL ## NAME>(this); \
        return spec->getRepr(); \
    } break;
#define ILVALUE_KIND_ABSTRACT(NAME)
#define ILVALUE_KIND_EOK(NAME)
    switch(kind) {
        ILVALUE_ENUM_KINDS()
        default: {
            assert(false && "invalid IL value kind");
            return "?";
        } break;
    }
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK
}

std::string ILValue::getRefRepr () const {
#define ILVALUE_KIND(NAME) \
    case ILValue::NAME: { \
        auto spec = llvm::cast<IL ## NAME>(this); \
        return spec->getRefRepr(); \
    } break;
#define ILVALUE_KIND_ABSTRACT(NAME)
#define ILVALUE_KIND_EOK(NAME)
    switch(kind) {
        ILVALUE_ENUM_KINDS()
        default: {
            assert(false && "invalid IL value kind");
            return "?";
        } break;
    }
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK
}

Type *ILValue::inferType () const {
#define ILVALUE_KIND(NAME) \
    case ILValue::NAME: { \
        auto spec = llvm::cast<IL ## NAME>(this); \
        return spec->inferType(); \
    } break;
#define ILVALUE_KIND_ABSTRACT(NAME)
#define ILVALUE_KIND_EOK(NAME)
    switch(kind) {
        ILVALUE_ENUM_KINDS()
        default: {
            assert(false && "invalid IL value kind");
            return nullptr;
        } break;
    }
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

struct ILBuilder {
    ILContinuation *continuation;

    void continueAt(ILContinuation *cont) {
        this->continuation = cont;
        assert(!cont->values.size());
    }

    void insertAndAdvance(
        const std::vector<ILValue *> &values,
        ILContinuation *next) {
        assert(continuation);
        assert(!continuation->values.size());
        continuation->values = values;
        continuation = next;
    }

    void br(const std::vector<ILValue *> &arguments) {
        insertAndAdvance(arguments, nullptr);
    }

    /*
    ILParameterRef toparam(const ILValue *value) {
        if (llvm::isa<ILParameter>(value.get())) {
            return value->getSharedPtr<ILParameter>();
        } else {
            auto next = ILContinuation::create(module, 1);
            insertAndAdvance( { next, value } , next);
            return next->parameters[0];
        }
    }
    */

    ILParameter *call(std::vector<ILValue *> values) {
        auto next = ILContinuation::create(1, "cret");
        values.push_back(next);
        insertAndAdvance(values, next);
        return next->parameters[0];
    }

};

//------------------------------------------------------------------------------
// FOREIGN FUNCTION INTERFACE
//------------------------------------------------------------------------------

struct FFI {
    typedef std::pair<CFunctionType *, LLVMValueRef> TypeLLVMValuePair;

    LLVMModuleRef module;
    LLVMExecutionEngineRef engine;
    std::unordered_map<Type *, LLVMTypeRef> typecache;
    std::unordered_map<std::string, TypeLLVMValuePair > functions;

    LLVMTypeRef thunkfunctype;

    FFI() :
        engine(nullptr) {
        module = LLVMModuleCreateWithName("ffi");

        LLVMTypeRef paramtypes[] = {
            LLVMPointerType(LLVMInt8Type(), 0),
            LLVMPointerType(LLVMPointerType(LLVMInt8Type(), 0), 0)
        };
        thunkfunctype = LLVMFunctionType(
                    LLVMVoidType(), paramtypes, 2, false);
    }

    ~FFI() {
        destroyEngine();
        LLVMDisposeModule(module);
    }

    void destroyEngine() {
        if (!engine) return;
        // leak the EE for now so we can reuse the module
        //LLVMDisposeExecutionEngine(engine);
        engine = nullptr;
    }

    void verifyModule() {
        char *error = NULL;
        LLVMVerifyModule(module, LLVMAbortProcessAction, &error);
        LLVMDisposeMessage(error);
    }

    void createEngine() {
        if (engine) return;

        char *error = NULL;

        #if 0
        LLVMCreateExecutionEngineForModule(&engine,
                                        module,
                                        &error);
        #else
        LLVMMCJITCompilerOptions options;
        LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
        //options.OptLevel = 0;
        //options.EnableFastISel = true;
        //options.CodeModel = LLVMCodeModelLarge;

        LLVMCreateMCJITCompilerForModule(&engine, module,
            &options, sizeof(options), &error);
        #endif

        if (error) {
            LLVMDisposeMessage(error);
            engine = nullptr;
            return;
        }
    }

    LLVMTypeRef convertType(Type *il_type) {
        LLVMTypeRef result = typecache[il_type];
        if (!result) {
            switch(il_type->getKind()) {
                case T_Void: {
                    result = LLVMVoidType();
                } break;
                case T_Null: {
                    result = LLVMVoidType();
                } break;
                case T_Integer: {
                    auto integer = llvm::cast<IntegerType>(il_type);
                    result = LLVMIntType(integer->getWidth());
                } break;
                case T_Real: {
                    auto real = llvm::cast<RealType>(il_type);
                    switch(real->getWidth()) {
                        case 16: {
                            result = LLVMHalfType();
                        } break;
                        case 32: {
                            result = LLVMFloatType();
                        } break;
                        case 64: {
                            result = LLVMDoubleType();
                        } break;
                        default: {
                            ilError(nullptr, "illegal real type");
                        } break;
                    }
                } break;
                case T_Array: {
                    auto array = llvm::cast<ArrayType>(il_type);
                    result = LLVMArrayType(
                        convertType(
                            array->getElement()),
                        array->getSize());
                } break;
                case T_Pointer: {
                    result = LLVMPointerType(
                        convertType(
                            llvm::cast<PointerType>(il_type)->getElement()),
                        0);
                } break;
                case T_CFunction: {
                    auto ftype = llvm::cast<CFunctionType>(il_type);

                    LLVMTypeRef parameters[ftype->getParameterCount()];
                    for (size_t i = 0; i < ftype->getParameterCount(); ++i) {
                        parameters[i] = convertType(ftype->getParameter(i));
                    }
                    result = LLVMFunctionType(
                        convertType(ftype->getResult()),
                        parameters, ftype->getParameterCount(),
                        ftype->isVarArg());
                } break;
                default: {
                    ilError(nullptr, "can not translate type");
                } break;
            }
            assert(result);
            typecache[il_type] = result;
        }
        return result;
    }

    Type *varArgType(Type *type) {
        // todo: expand float to double
        return type;
    }

    struct Variant {
        union {
            int8_t int8val;
            int16_t int16val;
            int32_t int32val;
            int64_t int64val;
            float floatval;
            double doubleval;
            void *ptrval;
        };

        void set_integer(Type *il_type, int64_t value) {
            auto integer = llvm::cast<IntegerType>(il_type);
            switch(integer->getWidth()) {
                case 8: { int8val = value; } break;
                case 16: { int16val = value; } break;
                case 32: { int32val = value; } break;
                case 64: { int64val = value; } break;
                default: {
                    ilError(nullptr, "can't convert constant");
                } break;
            }
        }

        void set_real(Type *il_type, double value) {
            auto real = llvm::cast<RealType>(il_type);
            switch(real->getWidth()) {
                case 32: { floatval = value; } break;
                case 64: { doubleval = value; } break;
                default: {
                    ilError(nullptr, "can't convert constant");
                } break;
            }
        }

    };

    ILConstant *makeConstant(Type *il_type, const Variant &value) {
        switch(il_type->getKind()) {
            case T_Void: {
                return ILConstTuple::create({});
            } break;
            case T_Integer: {
                auto integer = llvm::cast<IntegerType>(il_type);
                int64_t ivalue = 0;
                switch(integer->getWidth()) {
                    case 8: { ivalue = value.int8val; } break;
                    case 16: { ivalue = value.int16val; } break;
                    case 32: { ivalue = value.int32val; } break;
                    case 64: { ivalue = value.int64val; } break;
                    default: {
                        ilError(nullptr, "cannot make integer constant");
                    } break;
                }
                return ILConstInteger::create(ivalue,
                    static_cast<IntegerType *>(il_type));
            } break;
            case T_Real: {
                auto real = llvm::cast<RealType>(il_type);
                double flvalue = 0.0;
                switch(real->getWidth()) {
                    case 32: { flvalue = value.floatval; } break;
                    case 64: { flvalue = value.doubleval; } break;
                    default: {
                        ilError(nullptr, "cannot make real constant");
                    } break;
                }
                return ILConstReal::create(flvalue,
                    static_cast<RealType *>(il_type));
            } break;
            case T_Pointer: {
                return ILConstPointer::create(value.ptrval, il_type);
            } break;
            default: {
                ilError(nullptr, "can not make constant for type");
                return nullptr;
            } break;
        }
    }

    Variant convertConstant(
        Type *il_type, const ILConstant *c) {
        Variant result;
        switch(il_type->getKind()) {
            case T_Integer: {
                switch(c->kind) {
                    case ILValue::ConstInteger: {
                        auto ci = llvm::cast<ILConstInteger>(c);
                        result.set_integer(il_type, ci->value);
                    } break;
                    case ILValue::ConstReal: {
                        auto cr = llvm::cast<ILConstReal>(c);
                        result.set_integer(il_type, (int64_t)cr->value);
                    } break;
                    default: {
                        ilError(nullptr, "can't convert %s to integer",
                            c->getType()->getRepr().c_str());
                    } break;
                }
            } break;
            case T_Real: {
                switch(c->kind) {
                    case ILValue::ConstInteger: {
                        auto ci = llvm::cast<ILConstInteger>(c);
                        result.set_real(il_type, (double)ci->value);
                    } break;
                    case ILValue::ConstReal: {
                        auto cr = llvm::cast<ILConstReal>(c);
                        result.set_real(il_type, cr->value);
                    } break;
                    default: {
                        ilError(nullptr, "can't convert %s to real",
                            c->getType()->getRepr().c_str());
                    } break;
                }
            } break;
            case T_Pointer: {
                switch(c->kind) {
                    case ILValue::ConstPointer: {
                        auto cp = llvm::cast<ILConstPointer>(c);
                        result.ptrval = cp->value;
                    } break;
                    case ILValue::ConstString: {
                        auto cs = llvm::cast<ILConstString>(c);
                        result.ptrval = (void *)cs->value.c_str();
                    } break;
                    default: {
                        ilError(nullptr, "can't convert %s to pointer",
                            c->getType()->getRepr().c_str());
                    } break;
                }
            } break;
            default: {
                ilError(nullptr, "can not convert constant");
            } break;
        }
        return result;
    }

    Type *pointerifyType(Type *il_type) {
        if (il_type->getKind() == T_Pointer)
            return il_type;
        else
            return Type::Pointer(il_type);
    }

    TypeLLVMValuePair convertFunction(
        const ILConstant *value) {
        auto ilfunc = llvm::dyn_cast<ILConstCFunction>(value);
        if (!ilfunc) {
            ilError(nullptr, "not a C function");
        }
        auto &result = functions[ilfunc->name];
        if (!result.second) {
            if (ilfunc->function_type->getKind() != T_CFunction) {
                ilError(nullptr, "not a function type");
            }
            auto ilfunctype = static_cast<CFunctionType *>(ilfunc->function_type);

            LLVMTypeRef ofunctype = convertType(ilfunctype);
            LLVMValueRef ofunc = LLVMAddFunction(
                module, ilfunc->name.c_str(), ofunctype);

            LLVMValueRef thunkfunc = LLVMAddFunction(
                module, ("bangra_" + ilfunc->name + "_thunk").c_str(),
                thunkfunctype);

            LLVMBuilderRef builder = LLVMCreateBuilder();
            LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlock(thunkfunc, ""));

            size_t paramcount = ilfunctype->getParameterCount();
            LLVMValueRef args[paramcount];
            LLVMValueRef argparam = LLVMGetParam(thunkfunc, 1);
            for (size_t i = 0; i < paramcount; ++i) {
                Type *paramtype = ilfunctype->getParameter(i);

                LLVMValueRef indices[] = {
                    LLVMConstInt(LLVMInt32Type(), i, false) };

                LLVMValueRef arg = LLVMBuildGEP(
                    builder, argparam, indices, 1, "");

                arg = LLVMBuildBitCast(builder, arg,
                    convertType(Type::Pointer(Type::Pointer(paramtype))), "");

                arg = LLVMBuildLoad(builder, arg, "");

                args[i] = LLVMBuildLoad(builder, arg, "");
            }
            LLVMValueRef cr = LLVMBuildCall(builder, ofunc, args, paramcount, "");
            if (ilfunctype->getResult() != Type::Void) {
                Type *rettype = ilfunctype->getResult();
                LLVMValueRef retparam = LLVMGetParam(thunkfunc, 0);

                retparam = LLVMBuildBitCast(builder, retparam,
                      convertType(Type::Pointer(rettype)), "");

                LLVMBuildStore(builder, cr, retparam);
            }

            LLVMBuildRetVoid(builder);
            LLVMDisposeBuilder(builder);

            //LLVMDumpModule(module);
            //verifyModule();

            result.first = ilfunctype;
            result.second = thunkfunc;

            destroyEngine();
        }
        return result;
    }

    ILConstant *runFunction(
        const ILConstant *func, const std::vector<ILConstant *> &args) {

        auto F = convertFunction(func);
        auto ilfunctype = F.first;

        bool isvararg = ilfunctype->isVarArg();
        unsigned paramcount = ilfunctype->getParameterCount();

        if (args.size() < paramcount) {
            ilError(nullptr, "not enough arguments");
        } else if (!isvararg && (args.size() > paramcount)) {
            ilError(nullptr, "too many arguments");
        }

        Variant variants[paramcount];
        void *ptrs[paramcount];
        for (size_t i = 0; i < paramcount; ++i) {
            variants[i] = convertConstant(
                ilfunctype->getParameter(i), args[i]);
            ptrs[i] = &variants[i];
        }

        verifyModule();
        createEngine();
        typedef void (*FuncType)(void *, void **);

        FuncType f = (FuncType)LLVMGetPointerToGlobal(engine, F.second);

        Variant result;
        f(&result, ptrs);

        return makeConstant(ilfunctype->getResult(), result);
    }

};

static FFI *ffi;

//------------------------------------------------------------------------------
// INTERPRETER
//------------------------------------------------------------------------------

typedef std::unordered_map<ILContinuation *, std::vector<ILConstant *> >
    Cont2ValuesMap;

struct ILFrame {
    size_t idx;
    ILFrame *parent;
    Cont2ValuesMap map;

    std::string getRefRepr() {
        std::stringstream ss;
        ss << "#" << idx << ":" << this;
        return ss.str();
    }

    std::string getRepr() {
        std::stringstream ss;
        ss << "#" << idx << ":" << this << ":\n";
        bool any_entries = false;
        for (auto &entry : map) {
            any_entries = true;
            ss << "  " << entry.first->getRefRepr();
            auto &value = entry.second;
            for (size_t i = 0; i < value.size(); ++i) {
                ss << " " << value[i]->getRefRepr();
            }
            ss << "\n";
        }
        return ss.str();
    }

    static ILFrame *create() {
        // create closure
        ILFrame *newframe = new ILFrame();
        newframe->parent = nullptr;
        newframe->idx = 0;
        return newframe;
    }

    static ILFrame *create(ILFrame *frame) {
        // create closure
        ILFrame *newframe = new ILFrame();
        newframe->parent = frame;
        newframe->idx = frame->idx + 1;
        return newframe;
    }
};

std::string ILConstClosure::getRefRepr() const {
    std::stringstream ss;
    ss << "(" << ansi(ANSI_STYLE_KEYWORD, "closure");
    ss << " " << cont->getRefRepr();
    ss << " " << frame->getRefRepr();
    ss << ")";
    return ss.str();
}

typedef std::vector<ILValue *> ILValueArray;

ILConstant *evaluate(ILFrame *frame, ILValue *value) {
    if (auto c = llvm::dyn_cast<ILConstant>(value)) {
        return c;
    }
    switch(value->kind) {
        case ILValue::Parameter: {
            auto param = llvm::cast<ILParameter>(value);
            ILFrame *ptr = frame;
            while (ptr) {
                auto cont = param->parent;
                assert(cont && "parameter has no parent");
                if (ptr->map.count(cont)) {
                    auto &values = ptr->map[cont];
                    assert(param->index < values.size());
                    return values[param->index];
                }
                ptr = ptr->parent;
            }
            assert(false && "parameter not bound in any frame");
            return nullptr;
        } break;
        case ILValue::Continuation: {
            // create closure
            return ILConstClosure::create(
                llvm::cast<ILContinuation>(value),
                frame);
        } break;
        default:
            ilError(value, "value can not be evaluated");
            break;
    }
    return nullptr;
}

void evaluate_values(
    const ILValueArray &arguments,
    ILFrame *frame,
    std::vector<ILConstant *> &values) {
    size_t argcount = arguments.size();
    for (size_t i = 1; i < argcount; ++i) {
        auto value = evaluate(frame, arguments[i]);
        assert(value);
        values.push_back(value);
    }
}

void map_continuation_arguments(
    const ILValueArray &arguments,
    ILContinuation *nextcont,
    ILFrame *frame,
    ILFrame *nextframe) {
    std::vector<ILConstant *> values;
    evaluate_values(arguments, frame, values);
    size_t argcount = arguments.size() - 1;
    size_t paramcount = nextcont->getParameterCount();
    if (argcount != paramcount) {
        ilError(nullptr, "argument count mismatch (%zu != %zu)",
            argcount, paramcount);
    }
    nextframe->map[nextcont] = values;
}

void map_closure(ILConstClosure *closure,
    ILValueArray &arguments,
    ILFrame *&frame) {
    map_continuation_arguments(
        arguments, closure->cont, frame, closure->frame);
    frame = closure->frame;
    arguments = closure->cont->values;
}

static bool extract_bool(const ILConstant *value);
void execute(ILContinuation *entry) {
    assert(entry->getParameterCount() == 0);

    //ILContinuation *cont = entry;
    std::vector<ILValue *> arguments = entry->values;
    ILFrame *frame = ILFrame::create();
    frame->idx = 0;

    while (true) {
#ifdef BANGRA_DEBUG_IL
        std::cout << frame->getRepr() << "\n";
        //std::cout << cont->getRepr() << "\n";
        fflush(stdout);
#endif
        assert(arguments.size() >= 1);
        ILValue *callee = arguments[0];
        if (callee->kind == ILValue::Parameter) {
            callee = evaluate(frame, callee);
        }
        switch(callee->kind) {
            case ILValue::ConstClosure: {
                map_closure(
                    llvm::cast<ILConstClosure>(callee), arguments, frame);
            } break;
            case ILValue::Continuation: {
                auto nextcont = llvm::cast<ILContinuation>(callee);
                ILFrame *nextframe = ILFrame::create(frame);
                map_continuation_arguments(arguments, nextcont, frame, nextframe);
                arguments = nextcont->values;
                frame = nextframe;
            } break;
            case ILValue::Intrinsic: {
                if (callee == ILIntrinsic::Return) {
                    return;
                } else if (callee == ILIntrinsic::Branch) {
                    auto condarg = arguments[1];
                    bool condvalue = extract_bool(
                        evaluate(frame, condarg));
                    auto thenarg = arguments[2];
                    auto elsearg = arguments[3];
                    assert(thenarg->kind == ILValue::Continuation);
                    assert(elsearg->kind == ILValue::Continuation);
                    if (condvalue) {
                        arguments = llvm::cast<ILContinuation>(thenarg)->values;
                    } else {
                        arguments = llvm::cast<ILContinuation>(elsearg)->values;
                    }
                } else {
                    ilError(callee, "unhandled intrinsic");
                }
            } break;
            case ILValue::ConstBuiltin: {
                auto cb = llvm::cast<ILConstBuiltin>(callee);
                std::vector<ILConstant *> values;
                evaluate_values(arguments, frame, values);
                assert(values.size() >= 1);
                ILConstant *closure = values.back();
                values.pop_back();
                ILConstant *result = cb->handler(values);
                // generate fitting resume
                arguments.clear();
                arguments.push_back(closure);
                arguments.push_back(result);
            } break;
            case ILValue::ConstCFunction: {
                auto cb = llvm::cast<ILConstCFunction>(callee);
                std::vector<ILConstant *> values;
                evaluate_values(arguments, frame, values);
                assert(values.size() >= 1);
                ILConstant *closure = values.back();
                values.pop_back();
                ILConstant *result = ffi->runFunction(cb, values);
                // generate fitting resume
                arguments.clear();
                arguments.push_back(closure);
                arguments.push_back(result);
            } break;
            default: {
                ilError(callee, "can not apply expression");
            } break;
        }
    }

}

//------------------------------------------------------------------------------
// CLANG SERVICES
//------------------------------------------------------------------------------

class CVisitor : public clang::RecursiveASTVisitor<CVisitor> {
public:
    ILConstStruct *dest;
    clang::ASTContext *Context;
    std::unordered_map<clang::RecordDecl *, bool> record_defined;
    std::unordered_map<clang::EnumDecl *, bool> enum_defined;
    std::unordered_map<const char *, char *> path_cache;
    std::unordered_map<std::string, Type *> named_structs;
    std::unordered_map<std::string, ILConstStruct *> named_enums;
    std::unordered_map<std::string, Type *> typedefs;

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

    void SetContext(clang::ASTContext * ctx, ILConstStruct *dest_) {
        Context = ctx;
        dest = dest_;
    }

    void GetFields(StructType *struct_type, clang::RecordDecl * rd) {
        auto &rl = Context->getASTRecordLayout(rd);

        bool opaque = false;
        for(clang::RecordDecl::field_iterator it = rd->field_begin(), end = rd->field_end(); it != end; ++it) {
            clang::DeclarationName declname = it->getDeclName();

            unsigned idx = it->getFieldIndex();

            auto offset = rl.getFieldOffset(idx);
            //unsigned width = it->getBitWidthValue(*Context);

            if(it->isBitField() || (!it->isAnonymousStructOrUnion() && !declname)) {
                opaque = true;
                break;
            }
            clang::QualType FT = it->getType();
            Type *fieldtype = TranslateType(FT);
            if(!fieldtype) {
                opaque = true;
                break;
            }
            // todo: work offset into structure
            struct_type->addField(
                StructType::Field(
                    it->isAnonymousStructOrUnion()?"":
                                        declname.getAsString(),
                    fieldtype,
                    anchorFromLocation(it->getSourceRange().getBegin())));
        }
    }

    Type *TranslateRecord(clang::RecordDecl *rd) {
        if (!rd->isStruct() && !rd->isUnion()) return NULL;

        std::string name = rd->getName();

        StructType *struct_type = nullptr;
        if (name.size() && named_structs.count(name)) {
            struct_type = llvm::cast<StructType>(named_structs[name]);
        } else {
            struct_type =
                llvm::cast<StructType>(
                    Type::Struct(name, false, rd->isUnion()));
            if (!struct_type->isUnnamed()) {
                named_structs[name] = struct_type;
            }
        }

        Anchor anchor = anchorFromLocation(rd->getSourceRange().getBegin());
        struct_type->setAnchor(anchor);

        clang::RecordDecl * defn = rd->getDefinition();
        if (defn && !record_defined[rd]) {
            GetFields(struct_type, defn);

            auto &rl = Context->getASTRecordLayout(rd);
            auto align = rl.getAlignment();
            auto size = rl.getSize();

            // todo: make sure these fit
            // align.getQuantity()
            // size.getQuantity()

            record_defined[rd] = true;
        }

        return struct_type;
    }

    ILConstStruct *TranslateEnum(clang::EnumDecl *ed) {
        std::string name = ed->getName();

        ILConstStruct *enum_type = nullptr;
        if (name.size() && named_enums.count(name)) {
            enum_type = named_enums[name];
        } else {
            auto struct_type = llvm::cast<StructType>(
                    Type::Struct(name, false, false));
            enum_type = ILConstStruct::create({}, struct_type);
            if (!struct_type->isUnnamed()) {
                named_enums[name] = enum_type;
            }
        }

        llvm::cast<StructType>(enum_type->getType())->setAnchor(
            anchorFromLocation(ed->getIntegerTypeRange().getBegin()));

        clang::EnumDecl * defn = ed->getDefinition();
        if (defn && !enum_defined[ed]) {
            IntegerType *type = llvm::cast<IntegerType>(
                TranslateType(ed->getIntegerType()));

            for (auto it : ed->enumerators()) {
                Anchor anchor = anchorFromLocation(it->getSourceRange().getBegin());
                auto &val = it->getInitVal();

                enum_type->addField(
                    ILConstInteger::create(val.getExtValue(), type),
                    StructType::Field(it->getName().data(), type, anchor));
            }

            enum_defined[ed] = true;
        }

        return enum_type;
    }

    Type *TranslateType(clang::QualType T) {
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
            const EnumType *ET = dyn_cast<EnumType>(Ty);
            EnumDecl * ed = ET->getDecl();
            // todo: use actual enum type
            TranslateEnum(ed);
            return Type::Int32;
        } break;
        case clang::Type::Builtin:
            switch (cast<BuiltinType>(Ty)->getKind()) {
            case clang::BuiltinType::Void: {
                return Type::Void;
            } break;
            case clang::BuiltinType::Bool: {
                return Type::Bool;
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
                return Type::Integer(sz, !Ty->isUnsignedIntegerType());
            } break;
            case clang::BuiltinType::Half: {
                return Type::Half;
            } break;
            case clang::BuiltinType::Float: {
                return Type::Float;
            } break;
            case clang::BuiltinType::Double: {
                return Type::Double;
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
            Type *pointee = TranslateType(ETy);
            if (pointee != NULL) {
                return Type::Pointer(pointee);
            }
        } break;
        case clang::Type::VariableArray:
        case clang::Type::IncompleteArray:
            break;
        case clang::Type::ConstantArray: {
            const ConstantArrayType *ATy = cast<ConstantArrayType>(Ty);
            Type *at = TranslateType(ATy->getElementType());
            if(at) {
                int sz = ATy->getSize().getZExtValue();
                Type::Array(at, sz);
            }
        } break;
        case clang::Type::ExtVector:
        case clang::Type::Vector: {
                const clang::VectorType *VT = cast<clang::VectorType>(T);
                Type *at = TranslateType(VT->getElementType());
                if(at) {
                    int n = VT->getNumElements();
                    return Type::Vector(at, n);
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

    Type *TranslateFuncType(const clang::FunctionType * f) {

        bool valid = true; // decisions about whether this function can be exported or not are delayed until we have seen all the potential problems
        clang::QualType RT = f->getReturnType();

        Type *returntype = TranslateType(RT);

        if (!returntype)
            valid = false;

        bool vararg = false;

        const clang::FunctionProtoType * proto = f->getAs<clang::FunctionProtoType>();
        //proto is null if the function was declared without an argument list (e.g. void foo() and not void foo(void))
        //we don't support old-style C parameter lists, we just treat them as empty
        std::vector<Type *> argtypes;
        if(proto) {
            vararg = proto->isVariadic();
            for(size_t i = 0; i < proto->getNumParams(); i++) {
                clang::QualType PT = proto->getParamType(i);
                Type *paramtype = TranslateType(PT);
                if(!paramtype) {
                    valid = false; //keep going with attempting to parse type to make sure we see all the reasons why we cannot support this function
                } else if(valid) {
                    argtypes.push_back(paramtype);
                }
            }
        }

        if(valid) {
            return Type::CFunction(returntype, argtypes, vararg);
        }

        return NULL;
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
            Anchor anchor = anchorFromLocation(vd->getSourceRange().getBegin());

            Type *type = TranslateType(vd->getType());
            if (!type) return true;

            // TODO: global external
            // vd->getName().data(), type
        }

        return true;

    }

    bool TraverseTypedefDecl(clang::TypedefDecl *td) {

        Anchor anchor = anchorFromLocation(td->getSourceRange().getBegin());

        Type *type = TranslateType(td->getUnderlyingType());
        if (!type) return true;

        typedefs[td->getName().data()] = type;

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

        Type *functype = TranslateFuncType(fntyp);
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

        // TODO: global external
        // FuncName.c_str(), functype

        return true;
    }
};

class CodeGenProxy : public clang::ASTConsumer {
public:
    ILConstStruct *dest;
    CVisitor visitor;

    CodeGenProxy(ILConstStruct *dest_) : dest(dest_) {}
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
    ILConstStruct *dest;

    BangEmitLLVMOnlyAction(ILConstStruct *dest_) :
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

static LLVMModuleRef importCModule (ILConstStruct **table,
    const char *path, const char **args, int argcount,
    const char *buffer = NULL) {
    using namespace clang;

    std::vector<const char *> aargs;
    aargs.push_back("clang");
    aargs.push_back(path);
    for (int i = 0; i < argcount; ++i) {
        aargs.push_back(args[i]);
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

    *table = ILConstStruct::create({},
        llvm::cast<StructType>(Type::Struct("", false)));

    // Create and execute the frontend to generate an LLVM bitcode module.
    std::unique_ptr<CodeGenAction> Act(new BangEmitLLVMOnlyAction(*table));
    if (compiler.ExecuteAction(*Act)) {
        M = (LLVMModuleRef)Act->takeModule().release();
        assert(M);
        return M;
    }

    return NULL;
}

//------------------------------------------------------------------------------
// BUILTINS
//------------------------------------------------------------------------------

static bool builtin_checkparams (const std::vector<ILConstant *> &args,
    int mincount, int maxcount) {
    if ((mincount <= 0) && (maxcount == -1))
        return true;

    int argcount = (int)args.size();

    if ((maxcount >= 0) && (argcount > maxcount)) {
        ilError(nullptr,
            "excess argument. At most %i arguments expected", maxcount);
        return false;
    }
    if ((mincount >= 0) && (argcount < mincount)) {
        ilError(nullptr, "at least %i arguments expected", mincount);
        return false;
    }
    return true;
}

static bool extract_bool(const ILConstant *value) {
    if (auto resulttype = llvm::dyn_cast<ILConstInteger>(value)) {
        if (resulttype->getType() == Type::Bool) {
            return (bool)resulttype->value;
        }
    }
    ilError(value, "boolean expected");
    return false;
}

static std::string extract_string(const ILConstant *value) {
    auto resulttype = llvm::dyn_cast<ILConstString>(value);
    if (!resulttype) {
        ilError(value, "string constant expected");
    }
    return resulttype->value;
}

static Type *extract_type(const ILConstant *value) {
    auto resulttype = llvm::dyn_cast<ILConstPointer>(value);
    if (!resulttype) {
        ilError(value, "pointer constant expected");
    }
    if (resulttype->getType() != Type::TypePointer) {
        ilError(value, "pointer type constant expected");
    }
    return (Type *)resulttype->value;
}

static const std::vector<ILConstant *> &extract_tuple(
    const ILConstant *value) {
    auto resulttype = llvm::dyn_cast<ILConstTuple>(value);
    if (!resulttype) {
        ilError(value, "tuple expected");
    }
    return resulttype->values;
}

static ILConstPointer *wrap(Type *type) {
    return ILConstPointer::create(type, Type::TypePointer);
}

static ILConstInteger *wrap(bool value) {
    return ILConstInteger::create((int64_t)value,
        static_cast<IntegerType *>(Type::Bool));
}

static ILConstInteger *wrap(int64_t value) {
    return ILConstInteger::create(value,
        static_cast<IntegerType *>(Type::Int64));
}

static ILConstTuple *wrap(
    const std::vector<ILConstant *> &args) {
    return ILConstTuple::create(args);
}

static ILConstReal *wrap(double value) {
    return ILConstReal::create(value,
        static_cast<RealType *>(Type::Double));
}

static ILConstBuiltin *wrap(ILBuiltinFunction func) {
    return ILConstBuiltin::create(func);
}

static ILConstString *wrap(const std::string &s) {
    return ILConstString::create(s);
}

static ILConstant *builtin_print(const std::vector<ILConstant *> &args) {
    builtin_checkparams(args, 0, -1);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0)
            std::cout << " ";
        auto &arg = args[i];
        switch(arg->kind) {
            case ILValue::ConstString: {
                auto cs = llvm::cast<ILConstString>(arg);
                std::cout << cs->value;
            } break;
            default: {
                std::cout << args[i]->getRefRepr();
            } break;
        }
    }
    std::cout << "\n";
    return ILConstTuple::create({});
}

static ILConstant *builtin_repr(const std::vector<ILConstant *> &args) {
    builtin_checkparams(args, 1, 1);
    return wrap(args[0]->getRepr());
}

static ILConstant *builtin_tupleof(const std::vector<ILConstant *> &args) {
    builtin_checkparams(args, 0, -1);
    return wrap(args);
}

static ILConstant *builtin_cdecl(const std::vector<ILConstant *> &args) {
    builtin_checkparams(args, 3, 3);
    Type *rettype = extract_type(args[0]);
    const std::vector<ILConstant *> &params = extract_tuple(args[1]);
    bool vararg = extract_bool(args[2]);

    std::vector<Type *> paramtypes;
    size_t paramcount = params.size();
    for (size_t i = 0; i < paramcount; ++i) {
        paramtypes.push_back(extract_type(params[i]));
    }
    return wrap(Type::CFunction(rettype, paramtypes, vararg));
}

static ILConstant *builtin_external(const std::vector<ILConstant *> &args) {
    builtin_checkparams(args, 2, 2);
    std::string name = extract_string(args[0]);
    Type *rettype = extract_type(args[1]);
    return ILConstCFunction::create(name, rettype);
}

template<typename A, typename B>
class cast_join_type {};
template<typename T> class cast_join_type<T, T> {
    public: typedef T return_type; };
template<> class cast_join_type<int64_t, double> {
    public: typedef double return_type; };
template<> class cast_join_type<double, int64_t> {
    public: typedef double return_type; };

class builtin_add_op { public:
    template<typename Ta, typename Tb>
    static typename cast_join_type<Ta,Tb>::return_type
        operate(const Ta &a, const Tb &b) { return a + b; }
};
class builtin_sub_op { public:
    template<typename Ta, typename Tb>
    static typename cast_join_type<Ta,Tb>::return_type
        operate(const Ta &a, const Tb &b) { return a - b; }
};
class builtin_mul_op { public:
    template<typename Ta, typename Tb>
    static typename cast_join_type<Ta,Tb>::return_type
        operate(const Ta &a, const Tb &b) { return a * b; }
};
class builtin_div_op { public:
    template<typename Ta, typename Tb>
    static typename cast_join_type<Ta,Tb>::return_type
        operate(const Ta &a, const Tb &b) { return a / b; }
    static double operate(const int64_t &a, const int64_t &b) {
            return (double)a / (double)b; }
};
class builtin_mod_op { public:
    template<typename Ta, typename Tb>
    static typename cast_join_type<Ta,Tb>::return_type
        operate(const Ta &a, const Tb &b) { return fmod(a,b); }
};

class builtin_bitand_op { public:
    template<typename T>
    static T operate(const T &a, const T &b) { return a & b; }
};
class builtin_bitor_op { public:
    template<typename T>
    static T operate(const T &a, const T &b) { return a | b; }
};
class builtin_bitxor_op { public:
    template<typename T>
    static T operate(const T &a, const T &b) { return a ^ b; }
};
class builtin_bitnot_op { public:
    template<typename T>
    static T operate(const T &x) { return ~x; }
};
class builtin_not_op { public:
    template<typename T>
    static T operate(const T &x) { return !x; }
};

class builtin_eq_op { public:
    template<typename Ta, typename Tb>
    static bool operate(const Ta &a, const Tb &b) { return a == b; }
};
class builtin_ne_op { public:
    template<typename Ta, typename Tb>
    static bool operate(const Ta &a, const Tb &b) { return a != b; }
};
class builtin_gt_op { public:
    template<typename Ta, typename Tb>
    static bool operate(const Ta &a, const Tb &b) { return a > b; }
};
class builtin_ge_op { public:
    template<typename Ta, typename Tb>
    static bool operate(const Ta &a, const Tb &b) { return a >= b; }
};
class builtin_lt_op { public:
    template<typename Ta, typename Tb>
    static bool operate(const Ta &a, const Tb &b) { return a < b; }
};
class builtin_le_op { public:
    template<typename Ta, typename Tb>
    static bool operate(const Ta &a, const Tb &b) { return a <= b; }
};

template <class NextT>
class builtin_filter_op { public:
    static ILConstant *operate(const double &a, const int64_t &b) {
        return wrap(NextT::operate(a, b));
    }
    static ILConstant *operate(const int64_t &a, const double &b) {
        return wrap(NextT::operate(a, b));
    }
    template<typename T>
    static ILConstant *operate(const T &a, const T &b) {
        return wrap(NextT::operate(a, b));
    }
    template<typename Ta, typename Tb>
    static ILConstant *operate(const Ta &a, const Tb &b) {
        ilError(nullptr, "illegal operands");
        return nullptr;
    }
};


class dispatch_types_failed {
public:
    template<typename F>
    static ILConstant *dispatch(const ILConstant *v, const F &next) {
        ilError(v, "illegal operand");
        return nullptr;
    }
};

template<typename NextT>
class dispatch_string_type {
public:
    template<typename F>
    static ILConstant *dispatch(const ILConstant *v, const F &next) {
        if (v->kind == ILValue::ConstString) {
            auto ca = llvm::cast<ILConstString>(v);
            return next(ca->value);
        } else {
            return NextT::template dispatch<F>(v, next);
        }
    }
};

template<typename NextT>
class dispatch_integer_type {
public:
    template<typename F>
    static ILConstant *dispatch(const ILConstant *v, const F &next) {
        if (v->kind == ILValue::ConstInteger) {
            auto ca = llvm::cast<ILConstInteger>(v);
            if (ca->value_type != Type::Bool) {
                return next(ca->value);
            }
        }
        return NextT::template dispatch<F>(v, next);
    }
};

template<typename NextT>
class dispatch_bool_type {
public:
    template<typename F>
    static ILConstant *dispatch(const ILConstant *v, const F &next) {
        if (v->kind == ILValue::ConstInteger) {
            auto ca = llvm::cast<ILConstInteger>(v);
            if (ca->value_type == Type::Bool) {
                return next((bool)ca->value);
            }
        }
        return NextT::template dispatch<F>(v, next);
    }
};

template<typename NextT>
class dispatch_real_type {
public:
    template<typename F>
    static ILConstant *dispatch(const ILConstant *v, const F &next) {
        if (v->kind == ILValue::ConstReal) {
            auto ca = llvm::cast<ILConstReal>(v);
            return next(ca->value);
        } else {
            return NextT::template dispatch<F>(v, next);
        }
    }
};

typedef dispatch_integer_type<
    dispatch_real_type <
        dispatch_types_failed> >
    dispatch_arithmetic_types;

typedef dispatch_integer_type<
            dispatch_real_type<
                dispatch_types_failed> >
    dispatch_arith_types;
typedef dispatch_integer_type<
            dispatch_real_type<
                dispatch_string_type<
                    dispatch_types_failed> > >
    dispatch_arith_string_types;
typedef dispatch_arithmetic_types dispatch_arith_cmp_types;
typedef dispatch_arith_string_types dispatch_cmp_types;

typedef dispatch_integer_type<dispatch_types_failed>
    dispatch_bit_types;
typedef dispatch_bool_type<dispatch_types_failed>
    dispatch_boolean_types;

template<class D, class F>
class builtin_binary_op1 {
public:
    template<typename Q>
    ILConstant *operator ()(const Q &ca_value) const {
        return wrap(F::operate(ca_value));
    }
};

template<class D, class F, typename T>
class builtin_binary_op3 {
public:
    const T &ca_value;
    builtin_binary_op3(const T &ca_value_) : ca_value(ca_value_) {}

    template<typename Q>
    ILConstant *operator ()(const Q &cb_value) const {
        return builtin_filter_op<F>::operate(ca_value, cb_value);
    }
};

template<class D, class F>
class builtin_binary_op2 {
public:
    const ILConstant *b;
    builtin_binary_op2(const ILConstant *b_) : b(b_) {}

    template<typename T>
    ILConstant *operator ()(const T &ca_value) const {
        return D::dispatch(b, builtin_binary_op3<D, F, T>(ca_value));
    }
};

template<class D, class F>
static ILConstant *builtin_binary_op(
    const std::vector<ILConstant *> &args) {
    if (args.size() != 2) {
        ilError(nullptr, "invalid number of arguments");
    }
    return D::dispatch(args[0], builtin_binary_op2<D, F>(args[1]));
}


template<class D, class F>
static ILConstant *builtin_unary_op(
    const std::vector<ILConstant *> &args) {
    if (args.size() != 1) {
        ilError(nullptr, "invalid number of arguments");
    }
    return D::dispatch(args[0], builtin_binary_op1<D, F>());
}

//------------------------------------------------------------------------------
// TRANSLATION
//------------------------------------------------------------------------------

typedef std::map<std::string, bangra_preprocessor> NameMacroMap;
typedef std::unordered_map<std::string, Type *> NameTypeMap;
typedef std::unordered_map<std::string, ILValue *> NameValueMap;

//------------------------------------------------------------------------------

struct GlobalEnvironment {
    ILBuilder builder;
};

struct Environment {
protected:
    NameValueMap values;
public:
    GlobalEnvironment &global;
    Environment *parent;

    Environment(GlobalEnvironment &global_) :
        global(global_),
        parent(nullptr)
    {
        assert(!values.size());
    }

    Environment(Environment &parent_) :
        global(parent_.global),
        parent(&parent_)
    {
        assert(!values.size());
    }

    const NameValueMap &getLocals() {
        return values;
    }

    void dump() {
        printf("%i values\n",
            (int)values.size());
        for (auto entry : values) {
            ilMessage(entry.second, "%p.%s\n",
                (void *)this, entry.first.c_str());
            assert(entry.second);
        }
    }

    bool isLocal(const std::string &name) {
        return values.count(name);
    }

    ILValue *getLocal(const std::string &name) {
        if (values.count(name)) {
            return values.at(name);
        }
        return nullptr;
    }

    /*
    bool set(const std::string &name, ILValue *value) {
        if (isLocal(name)) {
            replaceLocal(name, value);
        } else {
            if (resolve(name)) {
                replaceScoped(name, value);
            } else {
                return false;
            }
        }
        return true;
    }
    */

    void setLocal(const std::string &name, ILValue *value) {
        assert(!isLocal(name));
        assert(value);
        values[name] = value;
    }

    void replaceLocal(const std::string &name, ILValue *value) {
        assert(isLocal(name));
        assert(value);
        values[name] = value;
    }

    ILValue *resolve(const std::string &name) {
        Environment *penv = this;
        while (penv) {
            ILValue *result = (*penv).getLocal(name);
            if (result) {
                return result;
            }
            penv = penv->parent;
        }
        return nullptr;
    }

};

static std::unordered_map<std::string, bangra_preprocessor> preprocessors;

//------------------------------------------------------------------------------

static bool isSymbol (const Value *expr, const char *sym) {
    if (expr) {
        if (auto symexpr = llvm::dyn_cast<Symbol>(expr))
            return (symexpr->getValue() == sym);
    }
    return false;
}

//------------------------------------------------------------------------------

#define UNPACK_ARG(expr, name) \
    expr = next(expr); ValueRef name = expr

//------------------------------------------------------------------------------

ILValue *translate(Environment &env, ValueRef expr);

void valueError (ValueRef expr, const char *format, ...) {
    Anchor *anchor = NULL;
    if (expr)
        anchor = expr->findValidAnchor();
    if (!anchor) {
        if (expr)
            printValue(expr);
    }
    va_list args;
    va_start (args, format);
    Anchor::printErrorV(anchor, format, args);
    va_end (args);
}

void valueErrorV (ValueRef expr, const char *fmt, va_list args) {
    Anchor *anchor = NULL;
    if (expr)
        anchor = expr->findValidAnchor();
    if (!anchor) {
        if (expr)
            printValue(expr);
    }
    Anchor::printErrorV(anchor, fmt, args);
}

template <typename T>
static T *astVerifyKind(ValueRef expr) {
    T *obj = expr?llvm::dyn_cast<T>(expr):NULL;
    if (obj) {
        return obj;
    } else {
        valueError(expr, "%s expected, not %s",
            valueKindName(T::kind()),
            valueKindName(kindOf(expr)));
    }
    return nullptr;
}

static ILValue *parse_do (Environment &env, ValueRef expr) {
    expr = next(expr);

    Environment subenv(env);

    ILValue *value;
    while (expr) {
        value = translate(subenv, expr);
        expr = next(expr);
    }

    if (value)
        return value;
    else
        return ILConstTuple::create({});

}

static ILValue *parse_function (Environment &env, ValueRef expr) {
    UNPACK_ARG(expr, expr_parameters);

    auto currentblock = env.global.builder.continuation;

    auto function = ILContinuation::create(0, "func");

    env.global.builder.continueAt(function);
    Environment subenv(env);

    subenv.setLocal("this-function", function);

    Pointer *params = astVerifyKind<Pointer>(expr_parameters);
    ValueRef param = at(params);
    while (param) {
        Symbol *symname = astVerifyKind<Symbol>(param);
        auto bp = ILParameter::create();
        function->appendParameter(bp);
        subenv.setLocal(symname->getValue(), bp);
        param = next(param);
    }
    auto ret = function->appendParameter(ILParameter::create());

    auto result = parse_do(subenv, expr_parameters);

    env.global.builder.br({ret, result});

    env.global.builder.continueAt(currentblock);

    return function;
}

static ILValue *parse_implicit_apply (Environment &env, ValueRef expr) {
    ValueRef expr_callable = expr;
    UNPACK_ARG(expr, arg);

    ILValue *callable = translate(env, expr_callable);

    std::vector<ILValue *> args;
    args.push_back(callable);

    while (arg) {
        args.push_back(translate(env, arg));

        arg = next(arg);
    }

    return env.global.builder.call(args);
}

static ILValue *parse_apply (Environment &env, ValueRef expr) {
    expr = next(expr);
    return parse_implicit_apply(env, expr);
}

bool hasTypeValue(Type *type) {
    assert(type);
    if ((type == Type::Void) || (type == Type::Empty))
        return false;
    return true;
}

static ILValue *parse_select (Environment &env, ValueRef expr) {
    UNPACK_ARG(expr, expr_condition);
    UNPACK_ARG(expr, expr_true);
    UNPACK_ARG(expr, expr_false);

    ILValue *condition = translate(env, expr_condition);
    auto bbstart = env.global.builder.continuation;

    auto bbtrue = ILContinuation::create(0, "then");
    env.global.builder.continueAt(bbtrue);

    Environment subenv_true(env);
    ILValue *trueexpr = translate(subenv_true, expr_true);
    auto bbtrue_end = env.global.builder.continuation;

    bool returnValue = hasTypeValue(trueexpr->getType());

    ILContinuation *bbfalse = nullptr;
    ILContinuation *bbfalse_end = nullptr;
    ILValue *falseexpr = nullptr;
    if (expr_false) {
        Environment subenv_false(env);
        bbfalse = ILContinuation::create(0, "else");
        env.global.builder.continueAt(bbfalse);
        falseexpr = translate(subenv_false, expr_false);
        bbfalse_end = env.global.builder.continuation;
        returnValue = returnValue && hasTypeValue(falseexpr->getType());
    } else {
        returnValue = false;
    }

    auto bbdone = ILContinuation::create(0, "endif");
    ILValue *result;
    std::vector<ILValue *> trueexprs;
    trueexprs.push_back(bbdone);
    if (returnValue) {
        auto result_param = ILParameter::create();
        bbdone->appendParameter(result_param);
        trueexprs.push_back(trueexpr);
        result = result_param;
    } else {
        result = ILConstTuple::create({});
    }

    env.global.builder.continueAt(bbtrue_end);
    env.global.builder.br(trueexprs);

    if (bbfalse) {
        env.global.builder.continueAt(bbfalse_end);
        std::vector<ILValue *> falsexprs;
        falsexprs.push_back(bbdone);
        if (returnValue)
            falsexprs.push_back(falseexpr);
        env.global.builder.br(falsexprs);
    } else {
        bbfalse = bbdone;
    }

    env.global.builder.continueAt(bbstart);
    env.global.builder.br(
        { ILIntrinsic::Branch, condition, bbtrue, bbfalse });

    env.global.builder.continueAt(bbdone);

    return result;
}

static ILValue *parse_let(Environment &env, ValueRef expr) {
    UNPACK_ARG(expr, expr_sym);
    UNPACK_ARG(expr, expr_value);

    Symbol *symname = astVerifyKind<Symbol>(expr_sym);
    ILValue *value;

    if (expr_value)
        value = translate(env, expr_value);
    else
        value = ILConstTuple::create({});

    if (env.isLocal(symname->getValue())) {
        valueError(symname, "already defined");
    }
    env.setLocal(symname->getValue(), value);

    return value;
}

struct TranslateTable {
    typedef ILValue *(*TranslatorFunc)(Environment &env, ValueRef expr);

    struct Translator {
        int mincount;
        int maxcount;
        unsigned flags;

        TranslatorFunc translate;

        Translator() :
            mincount(-1),
            maxcount(-1),
            flags(0),
            translate(NULL)
            {}

    };

    std::unordered_map<std::string, Translator> translators;

    void set(TranslatorFunc translate, const std::string &name,
        int mincount, int maxcount, unsigned flags=0) {
        Translator translator;
        translator.mincount = mincount;
        translator.maxcount = maxcount;
        translator.translate = translate;
        translator.flags = flags;
        translators[name] = translator;
    }

    static bool verifyParameterCount (ValueRef expr, int mincount, int maxcount) {
        if ((mincount <= 0) && (maxcount == -1))
            return true;

        ValueRef head = expr;
        expr = next(expr);

        int argcount = 0;
        while (expr) {
            ++ argcount;
            if (maxcount >= 0) {
                if (argcount > maxcount) {
                    valueError(expr, "excess argument. At most %i arguments expected", maxcount);
                    return false;
                }
            } else if (mincount >= 0) {
                if (argcount >= mincount)
                    break;
            }
            expr = next(expr);
        }
        if ((mincount >= 0) && (argcount < mincount)) {
            valueError(head, "at least %i arguments expected", mincount);
            return false;
        }
        return true;
    }

    TranslatorFunc match(ValueRef expr) {
        Symbol *head = astVerifyKind<Symbol>(expr);
        auto &t = translators[head->getValue()];
        if (!t.translate) return nullptr;
        verifyParameterCount(expr, t.mincount, t.maxcount);
        return t.translate;
    }

};

static TranslateTable translators;

//------------------------------------------------------------------------------

static void registerTranslators() {
    auto &t = translators;
    t.set(parse_let, "let", 1, 2);
    t.set(parse_apply, "apply", 1, -1);
    t.set(parse_do, "do", 0, -1);
    t.set(parse_select, "select", 2, 3);
    t.set(parse_function, "function", 1, -1);
}

static ILValue *translateFromList (Environment &env, ValueRef expr) {
    assert(expr);
    astVerifyKind<Symbol>(expr);
    auto func = translators.match(expr);
    if (func) {
        return func(env, expr);
    } else {
        return parse_implicit_apply(env, expr);
    }
}

ILValue *translate (Environment &env, ValueRef expr) {
    assert(expr);
    ILValue *result = nullptr;
    if (!isAtom(expr)) {
        result = translateFromList(env, at(expr));
    } else if (auto sym = llvm::dyn_cast<Symbol>(expr)) {
        std::string value = sym->getValue();
        result = env.resolve(value);
        if (!result) {
            valueError(expr, "unknown symbol '%s'",
                value.c_str());
        }
    } else if (auto str = llvm::dyn_cast<String>(expr)) {
        result = ILConstString::create(str);
    } else if (auto integer = llvm::dyn_cast<Integer>(expr)) {
        result = ILConstInteger::create(integer,
            llvm::cast<IntegerType>(Type::Int32));
    } else if (auto real = llvm::dyn_cast<Real>(expr)) {
        result = ILConstReal::create(real,
            llvm::cast<RealType>(Type::Double));
    } else {
        valueError(expr, "expected expression, not %s",
            valueKindName(kindOf(expr)));
    }
    if (result && !result->anchor.isValid()) {
        Anchor *anchor = expr->findValidAnchor();
        if (anchor) {
            result->anchor = *anchor;
        }
    }
    assert(result);
    return result;
}

//------------------------------------------------------------------------------
// INITIALIZATION
//------------------------------------------------------------------------------

static void init() {
    bangra::support_ansi = isatty(fileno(stdout));

    Type::initTypes();
    ILIntrinsic::initIntrinsics();
    registerTranslators();

    LLVMEnablePrettyStackTrace();
    LLVMLinkInMCJIT();
    //LLVMLinkInInterpreter();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmParser();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeDisassembler();

    ffi = new FFI();

}

//------------------------------------------------------------------------------

static void setupRootEnvironment (Environment &env) {
    env.setLocal("void", wrap(Type::Void));
    env.setLocal("null", wrap(Type::Null));
    env.setLocal("half", wrap(Type::Half));
    env.setLocal("float", wrap(Type::Float));
    env.setLocal("double", wrap(Type::Double));
    env.setLocal("bool", wrap(Type::Bool));

    env.setLocal("int8", wrap(Type::Int8));
    env.setLocal("int16", wrap(Type::Int16));
    env.setLocal("int32", wrap(Type::Int32));
    env.setLocal("int64", wrap(Type::Int64));

    env.setLocal("uint8", wrap(Type::UInt8));
    env.setLocal("uint16", wrap(Type::UInt16));
    env.setLocal("uint32", wrap(Type::UInt32));
    env.setLocal("uint64", wrap(Type::UInt64));

    env.setLocal("usize_t",
        wrap(Type::Integer(sizeof(size_t)*8,false)));

    env.setLocal("rawstring", wrap(Type::Rawstring));

    env.setLocal("int", env.resolve("int32"));

    auto booltype = llvm::cast<IntegerType>(Type::Bool);
    env.setLocal("true", ILConstInteger::create(new Integer(1), booltype));
    env.setLocal("false", ILConstInteger::create(new Integer(0), booltype));

    env.setLocal("print", wrap(builtin_print));
    env.setLocal("repr", wrap(builtin_repr));
    env.setLocal("cdecl", wrap(builtin_cdecl));
    env.setLocal("tupleof", wrap(builtin_tupleof));
    env.setLocal("external", wrap(builtin_external));

    env.setLocal("+",
        wrap(builtin_binary_op<dispatch_arith_string_types, builtin_add_op>));
    env.setLocal("-",
        wrap(builtin_binary_op<dispatch_arith_types, builtin_sub_op>));
    env.setLocal("*",
        wrap(builtin_binary_op<dispatch_arith_types, builtin_mul_op>));
    env.setLocal("/",
        wrap(builtin_binary_op<dispatch_arith_types, builtin_div_op>));
    env.setLocal("%",
        wrap(builtin_binary_op<dispatch_arith_types, builtin_mod_op>));

    env.setLocal("&",
        wrap(builtin_binary_op<dispatch_bit_types, builtin_bitand_op>));
    env.setLocal("|",
        wrap(builtin_binary_op<dispatch_bit_types, builtin_bitor_op>));
    env.setLocal("^",
        wrap(builtin_binary_op<dispatch_bit_types, builtin_bitxor_op>));
    env.setLocal("~",
        wrap(builtin_unary_op<dispatch_bit_types, builtin_bitnot_op>));

    env.setLocal("not",
        wrap(builtin_unary_op<dispatch_boolean_types, builtin_not_op>));

    env.setLocal("==",
        wrap(builtin_binary_op<dispatch_cmp_types, builtin_eq_op>));
    env.setLocal("!=",
        wrap(builtin_binary_op<dispatch_cmp_types, builtin_ne_op>));
    env.setLocal(">",
        wrap(builtin_binary_op<dispatch_cmp_types, builtin_gt_op>));
    env.setLocal(">=",
        wrap(builtin_binary_op<dispatch_cmp_types, builtin_ge_op>));
    env.setLocal("<",
        wrap(builtin_binary_op<dispatch_cmp_types, builtin_lt_op>));
    env.setLocal("<=",
        wrap(builtin_binary_op<dispatch_cmp_types, builtin_le_op>));

}

static void handleException(Environment &env, ValueRef expr) {
    ValueRef tb = expr->getNext();
    if (tb && tb->getKind() == V_String) {
        std::cerr << llvm::cast<String>(tb)->getValue();
    }
    streamValue(std::cerr, expr, 0, true);
    valueError(expr, "an exception was raised");
}

static bool translateRootValueList (Environment &env, ValueRef expr) {

    auto mainfunc = ILContinuation::create();
    env.global.builder.continueAt(mainfunc);

    parse_do(env, expr);
    env.global.builder.br({ ILIntrinsic::Return });

/*
#ifdef BANGRA_DEBUG_IL
    std::cout << env.global.module->getRepr();
    fflush(stdout);
#endif
*/

    execute(mainfunc);

    return true;
}

template <typename T>
static T *translateKind(Environment &env, ValueRef expr) {
    T *obj = expr?llvm::dyn_cast<T>(expr):NULL;
    if (obj) {
        return obj;
    } else {
        valueError(expr, "%s expected, not %s",
            valueKindName(T::kind()),
            valueKindName(kindOf(expr)));
    }
    return nullptr;
}

static bool compileMain (ValueRef expr) {
    assert(expr);
    expr = at(expr);
    assert(expr);

    GlobalEnvironment global;
    Environment env(global);
    setupRootEnvironment(env);

    std::string lastlang = "";
    while (true) {
        Symbol *head = translateKind<Symbol>(env, expr);
        if (!head) return false;
        if (head->getValue() == BANGRA_HEADER)
            break;
        auto preprocessor = preprocessors[head->getValue()];
        if (!preprocessor) {
            valueError(expr, "unrecognized header: '%s'; try '%s' instead.",
                head->getValue().c_str(),
                BANGRA_HEADER);
            return false;
        }
        if (lastlang == head->getValue()) {
            valueError(expr,
                "header has not changed after preprocessing; is still '%s'.",
                head->getValue().c_str());
        }
        lastlang = head->getValue();
        ValueRef orig_expr = expr;
        try {
            expr = preprocessor(&env, new Pointer(expr));
        } catch (ValueRef expr) {
            handleException(env, expr);
            return false;
        }
        if (!expr) {
            valueError(orig_expr,
                "preprocessor returned null.",
                head->getValue().c_str());
            return false;
        }
        expr = at(expr);
    }

    return translateRootValueList (env, expr);
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

static ValueRef parseLoader(const char *executable_path) {
    // attempt to read bootstrap expression from end of binary
    auto file = MappedFile::open(executable_path);
    if (!file) {
        fprintf(stderr, "could not open binary\n");
        return NULL;
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
        if (cursor < ptr) return NULL;
    }
    if (*cursor != ')') return NULL;
    cursor--;
    // seek backwards to find beginning of expression
    while ((cursor >= ptr) && (*cursor != '('))
        cursor--;

    bangra::Parser footerParser;
    ValueRef expr = footerParser.parseMemory(
        cursor, ptr + size, executable_path, cursor - ptr);
    if (!expr) {
        fprintf(stderr, "could not parse footer expression\n");
        return NULL;
    }
    if (expr->getKind() != V_Pointer)  {
        fprintf(stderr, "footer expression is not a list\n");
        return NULL;
    }
    expr = at(expr);
    if (expr->getKind() != V_Symbol)  {
        fprintf(stderr, "footer expression does not begin with symbol\n");
        return NULL;
    }
    if (!isSymbol(expr, "script-size"))  {
        fprintf(stderr, "footer expression does not begin with 'script-size'\n");
        return NULL;
    }
    expr = next(expr);
    if (expr->getKind() != V_Integer)  {
        fprintf(stderr, "script-size argument is not integer\n");
        return NULL;
    }
    auto offset = llvm::cast<Integer>(expr)->getValue();
    if (offset <= 0) {
        fprintf(stderr, "script-size must be larger than zero\n");
        return NULL;
    }
    bangra::Parser parser;
    auto script_start = cursor - offset;
    return parser.parseMemory(script_start, cursor, executable_path, script_start - ptr);
}

static bool compileStartupScript() {
    char *base = strdup(bang_executable_path);
    char *ext = extension(base);
    if (ext) {
        *ext = 0;
    }
    std::string path = format("%s.b", base);
    free(base);

    ValueRef expr = NULL;
    {
        auto file = MappedFile::open(path.c_str());
        if (file) {
            // keep a resident copy
            char *pathcpy = strdup(path.c_str());
            bangra::Parser parser;
            expr = parser.parseMemory(
                file->strptr(), file->strptr() + file->size(),
                pathcpy);
        }
    }

    if (expr && bangra::isKindOf<bangra::Pointer>(expr)) {
        return bangra::compileMain(expr);
    }

    return true;
}

} // namespace bangra

// C API
//------------------------------------------------------------------------------

char *bang_executable_path = NULL;
int bang_argc = 0;
char **bang_argv = NULL;

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
    , bang_executable_path
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
    "   --                          terminate option list.\n"
    , exename
    );
    exit(0);
}

int bangra_main(int argc, char ** argv) {
    bang_argc = argc;
    bang_argv = argv;

    bangra::init();

    ValueRef expr = NULL;

    if (argv) {
        if (argv[0]) {
            std::string loader = bangra::GetExecutablePath(argv[0]);
            // string must be kept resident
            bang_executable_path = strdup(loader.c_str());

            expr = bangra::parseLoader(bang_executable_path);
        }

        if (!expr) {
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

            if (!skip_startup && bang_executable_path) {
                if (!bangra::compileStartupScript()) {
                    return 1;
                }
            }

            if (sourcepath) {
                bangra::Parser parser;
                expr = parser.parseFile(sourcepath);
            }
        }
    }

    if (expr && bangra::isKindOf<bangra::Pointer>(expr)) {
        bangra::compileMain(expr);
    } else {
        return 1;
    }

    return 0;
}

ValueRef bangra_parse_file(const char *path) {
    bangra::Parser parser;
    return parser.parseFile(path);
}

void bangra_print_value(ValueRef expr, int depth) {
    if (depth < 0) {
        bangra::printValue(expr, 0, false);
    } else {
        bangra::printValue(expr, (size_t)depth, true);
    }
}

ValueRef bangra_format_value(ValueRef expr, int depth) {
    std::string str;
    if (depth < 0) {
        str = bangra::formatValue(expr, 0, false);
    } else {
        str = bangra::formatValue(expr, (size_t)depth, true);
    }
    return new bangra::String(str.c_str(), str.size());

}

void bangra_set_preprocessor(const char *name, bangra_preprocessor f) {
    bangra::preprocessors[name] = f;
}

bangra_preprocessor bangra_get_preprocessor(const char *name) {
    return bangra::preprocessors[name];
}

int bangra_get_kind(ValueRef expr) {
    return kindOf(expr);
}

ValueRef bangra_at(ValueRef expr) {
    if (expr) {
        if (bangra::isKindOf<bangra::Pointer>(expr)) {
            return at(expr);
        }
    }
    return NULL;
}

ValueRef bangra_clone(ValueRef expr) {
    if (expr) {
        return clone(expr);
    }
    return NULL;
}

ValueRef bangra_next(ValueRef expr) {
    return next(expr);
}

ValueRef bangra_set_next(ValueRef lhs, ValueRef rhs) {
    if (lhs) {
        if (lhs->getNext() != rhs) {
            return cons(lhs, rhs);
        } else {
            return lhs;
        }
    }
    return NULL;
}

ValueRef bangra_set_at_mutable(ValueRef lhs, ValueRef rhs) {
    if (lhs) {
        if (auto ptr = llvm::dyn_cast<bangra::Pointer>(lhs)) {
            ptr->setAt(rhs);
            return lhs;
        }
    }
    return NULL;
}

ValueRef bangra_set_next_mutable(ValueRef lhs, ValueRef rhs) {
    if (lhs) {
        lhs->setNext(rhs);
        return lhs;
    }
    return NULL;
}

const char *bangra_string_value(ValueRef expr) {
    if (expr) {
        if (auto str = llvm::dyn_cast<bangra::String>(expr)) {
            return str->getValue().c_str();
        }
    }
    return NULL;
}

signed long long int bangra_string_size(ValueRef expr) {
    if (expr) {
        if (auto str = llvm::dyn_cast<bangra::String>(expr)) {
            return (signed long long int)str->getValue().size() + 1;
        }
    }
    return 0;
}

ValueRef bangra_string_concat(ValueRef a, ValueRef b) {
    if (a && b) {
        auto str_a = llvm::dyn_cast<bangra::String>(a);
        auto str_b = llvm::dyn_cast<bangra::String>(b);
        if (str_a && str_b) {
            auto str_result = str_a->getValue() + str_b->getValue();
            if (str_a->getKind() == bangra::V_String) {
                return new bangra::String(str_result.c_str(), str_result.size());
            } else {
                return new bangra::Symbol(str_result.c_str(), str_result.size());
            }
        }
    }
    return NULL;
}

ValueRef bangra_string_slice(ValueRef expr, int start, int end) {
    if (expr) {
        if (auto str = llvm::dyn_cast<bangra::String>(expr)) {
            auto value = str->getValue();
            int size = (int)value.size();
            if (start < 0)
                start = size + start;
            if (start < 0)
                start = 0;
            else if (start > size)
                start = size;
            if (end < 0)
                end = size + end;
            if (end < start)
                end = start;
            else if (end > size)
                end = size;
            int len = end - start;
            value = value.substr((size_t)start, (size_t)len);
            if (str->getKind() == bangra::V_String) {
                return new bangra::String(value.c_str(), value.size());
            } else {
                return new bangra::Symbol(value.c_str(), value.size());
            }
        }
    }
    return NULL;
}

void bangra_error_message(ValueRef context, const char *format, ...) {
    va_list args;
    va_start (args, format);
    bangra::valueErrorV(context, format, args);
    va_end (args);
}

int bangra_eq(Value *a, Value *b) {
    if (a == b) return true;
    if (a && b) {
        auto kind = a->getKind();
        if (kind != b->getKind())
            return false;
        switch (kind) {
            case bangra::V_String:
            case bangra::V_Symbol: {
                bangra::String *sa = llvm::cast<bangra::String>(a);
                bangra::String *sb = llvm::cast<bangra::String>(b);
                return sa->getValue() == sb->getValue();
            } break;
            case bangra::V_Real: {
                bangra::Real *sa = llvm::cast<bangra::Real>(a);
                bangra::Real *sb = llvm::cast<bangra::Real>(b);
                return sa->getValue() == sb->getValue();
            } break;
            case bangra::V_Integer: {
                bangra::Integer *sa = llvm::cast<bangra::Integer>(a);
                bangra::Integer *sb = llvm::cast<bangra::Integer>(b);
                return sa->getValue() == sb->getValue();
            } break;
            case bangra::V_Pointer: {
                bangra::Pointer *sa = llvm::cast<bangra::Pointer>(a);
                bangra::Pointer *sb = llvm::cast<bangra::Pointer>(b);
                return sa->getAt() == sb->getAt();
            } break;
            default: break;
        };
    }
    return false;
}

ValueRef bangra_set_anchor(
    ValueRef expr, const char *path, int lineno, int column, int offset) {
    if (expr) {
        ValueRef newexpr = clone(expr);
        newexpr->anchor.path = path;
        newexpr->anchor.lineno = lineno;
        newexpr->anchor.column = column;
        newexpr->anchor.offset = offset;
        return newexpr;
    }
    return NULL;
}

ValueRef bangra_set_anchor_mutable(
    ValueRef expr, const char *path, int lineno, int column, int offset) {
    if (expr) {
        expr->anchor.path = path;
        expr->anchor.lineno = lineno;
        expr->anchor.column = column;
        expr->anchor.offset = offset;
        return expr;
    }
    return NULL;
}

ValueRef bangra_ref(ValueRef lhs) {
    return new bangra::Pointer(lhs);
}

ValueRef bangra_string(const char *value, signed long long int size) {
    if (size < 0)
        size = strlen(value);
    return new bangra::String(value, (size_t)size);
}
ValueRef bangra_symbol(const char *value) {
    return new bangra::Symbol(value);
}

ValueRef bangra_real(double value) {
    return new bangra::Real(value);
}
double bangra_real_value(ValueRef value) {
    if (value) {
        if (auto real = llvm::dyn_cast<bangra::Real>(value)) {
            return real->getValue();
        }
    }
    return 0.0;
}

ValueRef bangra_integer(signed long long int value) {
    return new bangra::Integer(value);
}
signed long long int bangra_integer_value(ValueRef value) {
    if (value) {
        if (auto integer = llvm::dyn_cast<bangra::Integer>(value)) {
            return integer->getValue();
        }
    }
    return 0;
}

const char *bangra_anchor_path(ValueRef expr) {
    if (expr) { return expr->anchor.path; }
    return NULL;
}

int bangra_anchor_lineno(ValueRef expr) {
    if (expr) { return expr->anchor.lineno; }
    return 0;
}

int bangra_anchor_column(ValueRef expr) {
    if (expr) { return expr->anchor.column; }
    return 0;
}

int bangra_anchor_offset(ValueRef expr) {
    if (expr) { return expr->anchor.offset; }
    return 0;
}

Environment *bangra_parent_env(Environment *env) {
    return env->parent;
}

static int unique_symbol_counter = 0;
ValueRef bangra_unique_symbol(const char *name) {
    if (!name)
        name = "";
    auto symname = bangra::format("#%s%i", name, unique_symbol_counter++);
    return new bangra::Symbol(symname.c_str());
}

/*
void *bangra_import_c_module(ValueRef dest,
    const char *path, const char **args, int argcount) {
    return bangra::importCModule(dest, path, args, argcount);
}

void *bangra_import_c_string(ValueRef dest,
    const char *str, const char *path, const char **args, int argcount) {
    return bangra::importCModule(dest, path, args, argcount, str);
}
*/

void *bangra_xpcall (void *ctx,
    void *(*try_func)(void *),
    void *(*except_func)(void *, ValueRef)) {
    try {
        return try_func(ctx);
    } catch (ValueRef expr) {
        return except_func(ctx, expr);
    }
}

void bangra_raise (ValueRef expr) {
    /*
    std::string tb = bangra::formatTraceback();
    ValueRef annot_expr =
        bangra::cons(expr,
            new bangra::String(tb.c_str(), tb.size()));
    throw annot_expr;
    */
    throw expr;
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

#endif // BANGRA_MAIN_CPP_IMPL
