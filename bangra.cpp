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

extern int bang_argc;
extern char **bang_argv;
extern char *bang_executable_path;

int bangra_main(int argc, char ** argv);

#if defined __cplusplus
}
#endif

#endif // BANGRA_CPP
#ifdef BANGRA_CPP_IMPL

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

std::string quoteReprSymbol(const std::string &value) {
    return quoteString(value, "[]{}()\"\'");
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
// ANCHORS
//------------------------------------------------------------------------------

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
    TYPE_KIND(Enum) \
    TYPE_KIND(CFunction) \
    TYPE_KIND(Flow)

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
    static Type *newFlowType(TypeArray _parameters);

protected:
    Type(TypeKind kind_) :
        kind(kind_)
        {}

public:
    static Type *TypePointer;
    static Type *ValuePointer;
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
    static Type *Enum(const std::string &name);
    static std::function<Type * (Type *, TypeArray, bool)> CFunction;
    static std::function<Type * (TypeArray)> Flow;
};

static std::string getRepr(Type *type);

Type *Type::TypePointer;
Type *Type::ValuePointer;
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
std::function<Type * (TypeArray)> Type::Flow = memo(Type::newFlowType);

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
                ansi(ANSI_STYLE_KEYWORD, "&").c_str(),
                bangra::getRepr(element).c_str());
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

    unsigned getCount() {
        return size;
    }

    ArrayType(Type *_element, unsigned _size) :
        element(_element),
        size(_size)
        {}

    std::string getRepr() {
        return format("(%s %s %i)",
                ansi(ANSI_STYLE_KEYWORD, "array").c_str(),
                bangra::getRepr(element).c_str(),
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

    unsigned getCount() {
        return size;
    }

    VectorType(Type *_element, unsigned _size) :
        element(_element),
        size(_size)
        {}

    std::string getRepr() {
        return format("(%s %s %i)",
                ansi(ANSI_STYLE_KEYWORD, "vector").c_str(),
                bangra::getRepr(element).c_str(),
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
    Type *getElement(size_t i) {
        assert(i < elements.size());
        return elements[i];
    }

    size_t getCount() {
        return elements.size();
    }

    static std::string getSpecRepr(const TypeArray &elements) {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "tuple");
        for (size_t i = 0; i < elements.size(); ++i) {
            ss << " ";
            ss << bangra::getRepr(elements[i]);
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
                    bangra::getRepr(type).c_str());
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

    std::string getFullRepr() {
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

    std::string getRepr() {
        if (builtin) {
            return ansi(ANSI_STYLE_TYPE, name).c_str();
        } else {
            std::stringstream ss;
            ss << "(" << ansi(ANSI_STYLE_KEYWORD,
                isUnion()?"union":"struct") << " ";
            ss << getNameRepr();
            ss << ")";
            return ss.str();
        }
    }

};

Type *Type::Struct(const std::string &name, bool builtin, bool union_) {
    return new StructType(name, builtin, union_);
}

//------------------------------------------------------------------------------

struct EnumType : TypeImpl<EnumType, T_Enum> {
public:
    struct Field {
    protected:
        std::string name;
        int64_t value;
        Anchor anchor;

    public:
        Field() {}

        Field(const std::string &name_, int64_t value_) :
            name(name_),
            value(value_)
            { assert(name.size() != 0); }

        Field(const std::string &name_, int64_t value_, const Anchor &anchor_) :
            name(name_),
            value(value_),
            anchor(anchor_)
            {}

        std::string getName() const {
            return name;
        }

        std::string getRepr() const {
            return format("(%s %s)",
                    quoteReprString(name).c_str(),
                    quoteReprInteger(value).c_str());
        }
    };

protected:
    std::string name;
    std::vector<Field> fields;
    std::unordered_map<std::string, size_t> byname;
    IntegerType *enum_type;
    Anchor anchor;

public:

    EnumType(const std::string &name_) :
        name(name_),
        enum_type(nullptr)
        {}

    void setType(Type *type) {
        enum_type = llvm::cast<IntegerType>(type);
    }

    Type *getType() {
        return enum_type;
    }

    const Anchor &getAnchor() {
        return anchor;
    }

    void setAnchor(const Anchor &anchor_) {
        anchor = anchor_;
    }

    void addField(const Field &field) {
        byname[field.getName()] = fields.size();
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
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "enum") << " ";
        ss << getNameRepr();
        ss << " " << ansi(ANSI_STYLE_OPERATOR, ":") << " ";
        if (enum_type) {
            ss << enum_type->getRepr();
        } else {
            ss << bangra::getRepr(Type::Any);
        }
        for (size_t i = 0; i < fields.size(); ++i) {
            ss << " " << fields[i].getRepr();
        }
        ss << ")";
        return ss.str();
    }

};

Type *Type::Enum(const std::string &name) {
    return new EnumType(name);
}

//------------------------------------------------------------------------------

struct FlowType : TypeImpl<FlowType, T_Flow> {
protected:
    TypeArray parameters;

public:
    static std::string getSpecRepr(const TypeArray &parameters) {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "fn") << " ";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i != 0)
                ss << " ";
            ss << bangra::getRepr(parameters[i]);
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

    FlowType(const TypeArray &_parameters) :
        parameters(_parameters)
        {}

    std::string getRepr() {
        return getSpecRepr(parameters);
    }

};

Type *Type::newFlowType(TypeArray _parameters) {
    return new FlowType(_parameters);
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
        ss << bangra::getRepr(result);
        ss << " (";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i != 0)
                ss << " ";
            ss << bangra::getRepr(parameters[i]);
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

std::string getRepr(Type *type) {
#define TYPE_KIND(NAME) \
    case T_ ## NAME: {\
        auto spec = llvm::cast<NAME ## Type>(type); \
        return spec->getRepr(); \
    } break;
    switch(type->getKind()) {
    TYPE_ENUM_KINDS()
    }
#undef TYPE_KIND
    return "?";
}

//------------------------------------------------------------------------------

void Type::initTypes() {
    TypePointer = Pointer(Struct("Type", true));
    ValuePointer = Pointer(Struct("SymEx", true));
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

struct Value;
struct FlowValue;
struct ParameterValue;
struct ILBuilder;

//------------------------------------------------------------------------------

#define ILVALUE_ENUM_KINDS() \
    ILVALUE_KIND(String) \
    ILVALUE_KIND(Symbol) \
    ILVALUE_KIND(Integer) \
    ILVALUE_KIND(Real) \
    ILVALUE_KIND(Pointer) \
    ILVALUE_KIND(Unit) \
    ILVALUE_KIND(Tuple) \
    ILVALUE_KIND(Struct) \
    ILVALUE_KIND(Closure) \
    ILVALUE_KIND(External) \
    ILVALUE_KIND(Builtin) \
    ILVALUE_KIND(BuiltinFlow) \
    ILVALUE_KIND(Parameter) \
    ILVALUE_KIND(Frame) \
    ILVALUE_KIND(TypeRef) \
    ILVALUE_KIND(Flow)

//------------------------------------------------------------------------------

struct Value {
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

    Value(Kind kind_) :
        kind(kind_)
        {}
};

static std::string getRepr (const Value *value);
static std::string getRefRepr (const Value *value);
static Type *getType(const Value *value);
static const Anchor *find_valid_anchor(const Value *expr);

/*
static void ilMessage (const Value *value, const char *format, ...) {
    const Anchor *anchor = NULL;
    if (value) {
        std::cout << "at\n  " << getRepr(value) << "\n";
        anchor = find_valid_anchor(value);
    }
    va_list args;
    va_start (args, format);
    Anchor::printMessageV(anchor, format, args);
    va_end (args);
}
*/

static void ilError (const Value *value, const char *format, ...) {
    const Anchor *anchor = NULL;
    if (value) {
        std::cout << "at\n  " << getRepr(value) << "\n";
        anchor = find_valid_anchor(value);
    }
    va_list args;
    va_start (args, format);
    Anchor::printErrorV(anchor, format, args);
    va_end (args);
}

//------------------------------------------------------------------------------

template<typename SelfT, Value::Kind KindT, typename BaseT>
struct ValueImpl : BaseT {
    typedef ValueImpl<SelfT, KindT, BaseT> ValueImplType;

    ValueImpl() :
        BaseT(KindT)
    {}

    static Value::Kind classkind() {
        return KindT;
    }

    static bool classof(const Value *value) {
        return value->kind == KindT;
    }
};

//------------------------------------------------------------------------------

struct ParameterValue :
    ValueImpl<ParameterValue, Value::Parameter, Value> {
    FlowValue *parent;
    size_t index;
    Type *parameter_type;
    std::string name;

    ParameterValue() :
        parent(nullptr),
        index(-1),
        parameter_type(nullptr) {
    }

    FlowValue *getParent() const {
        return parent;
    }

    Type *inferType() const {
        if (parameter_type)
            return parameter_type;
        else
            return Type::Any;
    }

    std::string getReprName() const {
        if (name.empty()) {
            return format("%s%zu",
                ansi(ANSI_STYLE_OPERATOR,"@").c_str(),
                index);
        } else {
            return name;
        }
    }

    std::string getRepr() const {
        return format("%s %s %s",
            getReprName().c_str(),
            ansi(ANSI_STYLE_OPERATOR,":").c_str(),
            bangra::getRepr(getType(this)).c_str());
    }

    std::string getRefRepr () const;

    static ParameterValue *create(const std::string &name = "") {
        auto value = new ParameterValue();
        value->index = (size_t)-1;
        value->name = name;
        value->parameter_type = nullptr;
        return value;
    }
};

//------------------------------------------------------------------------------

struct StringValue :
    ValueImpl<StringValue, Value::String, Value> {
    std::string value;

    static StringValue *create(const std::string &s) {
        auto result = new StringValue();
        result->value = s;
        return result;
    }

    static StringValue *create(const char *s, size_t len) {
        return create(std::string(s, len));
    }

    Type *inferType() const {
        return Type::Array(Type::Int8, value.size() + 1);
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        return quoteReprString(value);
    }
};

//------------------------------------------------------------------------------

struct SymbolValue :
    ValueImpl<SymbolValue, Value::Symbol, Value> {
    std::string value;

    static SymbolValue *create(const std::string &s) {
        auto result = new SymbolValue();
        result->value = s;
        return result;
    }

    static SymbolValue *create(const char *s, size_t len) {
        return create(std::string(s, len));
    }

    Type *inferType() const {
        return Type::Array(Type::Int8, value.size() + 1);
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        return quoteReprSymbol(value);
    }
};

//------------------------------------------------------------------------------

struct IntegerValue :
    ValueImpl<IntegerValue, Value::Integer, Value> {
    int64_t value;
    IntegerType *value_type;

    static IntegerValue *create(int64_t value, Type *cdest) {
        assert(cdest);
        auto result = new IntegerValue();
        result->value_type = llvm::cast<IntegerType>(cdest);
        result->value = value;
        return result;
    }

    Type *inferType() const {
        return value_type;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
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

struct RealValue :
    ValueImpl<RealValue, Value::Real, Value> {
    double value;
    RealType *value_type;

    static RealValue *create(double value, Type *cdest) {
        assert(cdest);
        auto result = new RealValue();
        result->value_type = llvm::cast<RealType>(cdest);
        result->value = value;
        return result;
    }

    Type *inferType() const {
        return value_type;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        return ansi(ANSI_STYLE_NUMBER,
            format("%f", value));
    }
};

//------------------------------------------------------------------------------

struct UnitValue :
    ValueImpl<UnitValue, Value::Unit, Value> {
    Type *unit_type;

    static UnitValue *create_null() {
        auto result = new UnitValue();
        result->unit_type = Type::Null;
        return result;
    }

    Type *inferType() const {
        return unit_type;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        assert (unit_type == Type::Null);
        return ansi(ANSI_STYLE_KEYWORD, "null");
    }
};

//------------------------------------------------------------------------------

struct TupleValue :
    ValueImpl<TupleValue, Value::Tuple, Value> {
    std::vector<Value *> values;

    static TupleValue *create(
        const std::vector<Value *> &values_) {
        auto result = new TupleValue();
        result->values = values_;
        return result;
    }


    Type *inferType() const {
        TypeArray types;
        for (auto &value : values) {
            types.push_back(getType(value));
        }
        return Type::Tuple(types);
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "tupleof");
        for (auto &value : values) {
            ss << " " << bangra::getRefRepr(value);
        }
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct StructValue :
    ValueImpl<StructValue, Value::Struct, Value> {
    std::vector<Value *> values;
    StructType *struct_type;

    static StructValue *create(const std::vector<Value *> &values,
        Type *struct_type) {
        auto result = new StructValue();
        result->values = values;
        result->struct_type = llvm::cast<StructType>(struct_type);
        assert(result->values.size() == result->struct_type->getFieldCount());
        return result;
    }

    void addField(Value *c, const StructType::Field &field) {
        struct_type->addField(field);
        values.push_back(c);
    }

    Type *inferType() const {
        return struct_type;
    }

    std::string getRepr() const {
        std::stringstream ss;
        ss << "(" << struct_type->getRepr();
        ss << " " << this;
        for (size_t i = 0; i < struct_type->getFieldCount(); ++i) {
            auto &field = struct_type->getField(i);
            ss << "\n  " << quoteReprString(field.getName())
                << " " << ansi(ANSI_STYLE_OPERATOR, "=") << " "
                << bangra::getRefRepr(values[i]);
        }
        ss << ")";
        return ss.str();
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << struct_type->getRepr();
        ss << " " << this << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct TypeRefValue :
    ValueImpl<TypeRefValue, Value::TypeRef, Value> {
    Type *value;

    static TypeRefValue *create(Type *value) {
        auto result = new TypeRefValue();
        result->value = value;
        return result;
    }

    Type *inferType() const {
        return Type::TypePointer;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << bangra::getRepr((Type *)value);
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct PointerValue :
    ValueImpl<PointerValue, Value::Pointer, Value> {
    void *value;
    Type *pointer_type;

    static PointerValue *create(
        void *value, Type *pointer_type) {
        auto result = new PointerValue();
        result->value = value;
        result->pointer_type = pointer_type;
        return result;
    }

    Type *inferType() const {
        return pointer_type;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << bangra::getRepr(pointer_type) << " " << value << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct FlowValue :
    ValueImpl<FlowValue, Value::Flow, Value> {
private:
    static int64_t unique_id_counter;
protected:
    int64_t uid;

public:
    FlowValue() :
        uid(unique_id_counter++),
        arguments(nullptr) {
    }

    std::string name;
    std::vector<ParameterValue *> parameters;

    // default path
    TupleValue *arguments;

    size_t getParameterCount() {
        return parameters.size();
    }

    ParameterValue *getParameter(size_t i) {
        return parameters[i];
    }

    bool hasArguments() const {
        return arguments && arguments->values.size();
    }

    std::string getRepr () const {
        std::stringstream ss;
        ss << getRefRepr();
        ss << " " << ansi(ANSI_STYLE_OPERATOR, "(");
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i != 0) {
                ss << ansi(ANSI_STYLE_OPERATOR, ", ");
            }
            ss << bangra::getRepr(parameters[i]);
        }
        ss << ansi(ANSI_STYLE_OPERATOR, ")");
        if (hasArguments()) {
            for (size_t i = 0; i < arguments->values.size(); ++i) {
                ss << " ";
                ss << bangra::getRefRepr(arguments->values[i]);
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
            params.push_back(getType(parameters[i]));
        }
        return Type::Flow(params);
    }

    ParameterValue *appendParameter(ParameterValue *param) {
        param->parent = this;
        param->index = parameters.size();
        parameters.push_back(param);
        return param;
    }

    static FlowValue *create(
        size_t paramcount = 0,
        const std::string &name = "") {
        auto value = new FlowValue();
        value->name = name;
        for (size_t i = 0; i < paramcount; ++i) {
            value->appendParameter(ParameterValue::create());
        }
        return value;
    }
};

int64_t FlowValue::unique_id_counter = 1;

std::string ParameterValue::getRefRepr () const {
    auto parent = getParent();
    if (parent) {
        return format("%s%s%s",
            bangra::getRefRepr(parent).c_str(),
            ansi(ANSI_STYLE_OPERATOR,".").c_str(),
            getReprName().c_str());
    } else {
        return ansi(ANSI_STYLE_ERROR, "<unbound>");
    }
}

//------------------------------------------------------------------------------

typedef Value *(*ILBuiltinFunction)(const std::vector<Value *> &args);

struct BuiltinValue :
    ValueImpl<BuiltinValue, Value::Builtin, Value> {

    ILBuiltinFunction handler;
    std::string name;

    static BuiltinValue *create(ILBuiltinFunction func,
        const std::string &name) {
        auto result = new BuiltinValue();
        result->handler = func;
        result->name = name;
        return result;
    }

    Type *inferType() const {
        return Type::Any;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "builtin");
        ss << " " << name;
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

typedef std::vector<Value *> (*ILBuiltinFlowFunction)(
    const std::vector<Value *> &args);

struct BuiltinFlowValue :
    ValueImpl<BuiltinFlowValue, Value::BuiltinFlow, Value> {

    ILBuiltinFlowFunction handler;
    std::string name;

    static BuiltinFlowValue *create(ILBuiltinFlowFunction func,
        const std::string &name) {
        auto result = new BuiltinFlowValue();
        result->handler = func;
        result->name = name;
        return result;
    }

    Type *inferType() const {
        return Type::Any;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "builtin-cc");
        ss << " " << name;
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

struct ExternalValue :
    ValueImpl<ExternalValue, Value::External, Value> {
    std::string name;
    Type *external_type;

    static ExternalValue *create(
        const std::string &name,
        Type *external_type) {
        auto result = new ExternalValue();
        result->name = name;
        result->external_type = external_type;
        return result;
    }

    Type *inferType() const {
        return external_type;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "external");
        ss << " " << name;
        ss << " " << bangra::getRepr(external_type);
        ss << ")";
        return ss.str();
    }
};

//------------------------------------------------------------------------------

typedef std::unordered_map<FlowValue *, std::vector<Value *> >
    FlowValuesMap;

struct FrameValue :
    ValueImpl<FrameValue, Value::Frame, Value> {
    size_t idx;
    FrameValue *parent;
    FlowValuesMap map;

    Type *inferType() const {
        return Type::Any;
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "#" << idx << ":" << this;
        return ss.str();
    }

    std::string getRepr() const {
        std::stringstream ss;
        ss << "#" << idx << ":" << this << ":\n";
        for (auto &entry : map) {
            ss << "  " << entry.first->getRefRepr();
            auto &value = entry.second;
            for (size_t i = 0; i < value.size(); ++i) {
                ss << " " << bangra::getRefRepr(value[i]);
            }
            ss << "\n";
        }
        return ss.str();
    }

    static FrameValue *create() {
        // create closure
        FrameValue *newframe = new FrameValue();
        newframe->parent = nullptr;
        newframe->idx = 0;
        return newframe;
    }

    static FrameValue *create(FrameValue *frame) {
        // create closure
        FrameValue *newframe = new FrameValue();
        newframe->parent = frame;
        newframe->idx = frame->idx + 1;
        return newframe;
    }
};

//------------------------------------------------------------------------------

struct ClosureValue :
    ValueImpl<ClosureValue, Value::Closure, Value> {
    FlowValue *cont;
    FrameValue *frame;

    static ClosureValue *create(
        FlowValue *cont,
        FrameValue *frame) {
        auto result = new ClosureValue();
        result->cont = cont;
        result->frame = frame;
        return result;
    }

    Type *inferType() const {
        return Type::Any;
    }

    std::string getRepr () const {
        return bangra::getRefRepr(this);
    }

    std::string getRefRepr() const {
        std::stringstream ss;
        ss << "(" << ansi(ANSI_STYLE_KEYWORD, "closure");
        ss << " " << cont->getRefRepr();
        ss << " " << frame->getRefRepr();
        ss << ")";
        return ss.str();
    }

};

//------------------------------------------------------------------------------

std::string getRepr (const Value *value) {
    assert(value);
#define ILVALUE_KIND(NAME) \
    case Value::NAME: { \
        auto spec = llvm::cast<NAME ## Value>(value); \
        return spec->getRepr(); \
    } break;
#define ILVALUE_KIND_ABSTRACT(NAME)
#define ILVALUE_KIND_EOK(NAME)
    switch(value->kind) {
        ILVALUE_ENUM_KINDS()
    }
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK
    assert(false && "invalid IL value kind");
    return "?";
}

std::string getRefRepr (const Value *value) {
    assert(value);
#define ILVALUE_KIND(NAME) \
    case Value::NAME: { \
        auto spec = llvm::cast<NAME ## Value>(value); \
        return spec->getRefRepr(); \
    } break;
#define ILVALUE_KIND_ABSTRACT(NAME)
#define ILVALUE_KIND_EOK(NAME)
    switch(value->kind) {
        ILVALUE_ENUM_KINDS()
    }
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK
    assert(false && "invalid IL value kind");
    return "?";
}

Type *getType(const Value *value) {
    assert(value);
#define ILVALUE_KIND(NAME) \
    case Value::NAME: { \
        auto spec = llvm::cast<NAME ## Value>(value); \
        return spec->inferType(); \
    } break;
#define ILVALUE_KIND_ABSTRACT(NAME)
#define ILVALUE_KIND_EOK(NAME)
    switch(value->kind) {
        ILVALUE_ENUM_KINDS()
    }
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK
    assert(false && "invalid IL value kind");
    return nullptr;
}

const char *getClassName(Value::Kind kind) {
#define ILVALUE_KIND(NAME) \
    case Value::NAME: { \
        return #NAME; \
    } break;
#define ILVALUE_KIND_ABSTRACT(NAME)
#define ILVALUE_KIND_EOK(NAME)
    switch(kind) {
        ILVALUE_ENUM_KINDS()
    }
#undef ILVALUE_KIND
#undef ILVALUE_KIND_ABSTRACT
#undef ILVALUE_KIND_EOK
    return "?";
}

//------------------------------------------------------------------------------
// IL MODEL UTILITY FUNCTIONS
//------------------------------------------------------------------------------

static void unescape(StringValue *s) {
    s->value.resize(inplace_unescape(&s->value[0]));
}

static void unescape(SymbolValue *s) {
    s->value.resize(inplace_unescape(&s->value[0]));
}

// matches ((///...))
static bool is_comment(Value *expr) {
    if (auto tuple = llvm::dyn_cast<TupleValue>(expr)) {
        if (tuple->values.size() > 0) {
            if (auto sym = llvm::dyn_cast<SymbolValue>(tuple->values.front())) {
                if (!memcmp(sym->value.c_str(),"///",3))
                    return true;
            }
        }
    }
    return false;
}

static Value *strip(Value *expr) {
    if (!expr) return nullptr;
    if (auto tuple = llvm::dyn_cast<TupleValue>(expr)) {
        auto copy = TupleValue::create({});
        auto &values = tuple->values;
        for (size_t i = 0; i < values.size(); ++i) {
            auto value = strip(values[i]);
            if (!is_comment(value)) {
                copy->values.push_back(value);
            }
        }
        return copy;
    }
    return expr;
}

static const Anchor *find_valid_anchor(const Value *expr) {
    if (!expr) return nullptr;
    if (expr->anchor.isValid()) return &expr->anchor;
    if (auto tuple = llvm::dyn_cast<TupleValue>(expr)) {
        auto &values = tuple->values;
        for (size_t i = 0; i < values.size(); ++i) {
            const Anchor *result = find_valid_anchor(values[i]);
            if (result) return result;
        }
    }
    return nullptr;
}

struct TupleIter {
protected:
    TupleValue *tuple;
    size_t index;
public:
    size_t get_index() const {
        return index;
    }

    TupleIter(TupleValue *value, size_t i) :
        tuple(value),
        index(i)
    {}

    TupleIter(Value *value, size_t i=0) :
        tuple(llvm::cast<TupleValue>(value)),
        index(i) {
    }

    bool operator ==(const TupleIter &other) const {
        return (tuple == other.tuple) && (index == other.index);
    }
    bool operator !=(const TupleIter &other) const {
        return (tuple != other.tuple) || (index != other.index);
    }

    TupleIter operator ++(int) {
        auto oldself = *this;
        ++index;
        return oldself;
    }

    operator bool() const {
        return index < tuple->values.size();
    }

    Value *operator *() const {
        assert(index < tuple->values.size());
        return tuple->values[index];
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

static bool isNested(Value *e) {
    if (auto tuple = llvm::dyn_cast<TupleValue>(e)) {
        auto &values = tuple->values;
        for (size_t i = 0; i < values.size(); ++i) {
            if (llvm::isa<TupleValue>(values[i]))
                return true;
        }
    }
    return false;
}

template<typename T>
static void streamAnchor(T &stream, Value *e, size_t depth=0) {
    if (e) {
        const Anchor *anchor = find_valid_anchor(e);
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
static void streamValue(T &stream, Value *e, size_t depth=0, bool naked=true) {
    if (naked) {
        streamAnchor(stream, e, depth);
    }

	if (!e) {
        stream << "#null#";
        if (naked)
            stream << '\n';
        return;
    }

	switch(e->kind) {
	case Value::Tuple: {
        auto tuple = llvm::cast<TupleValue>(e);
        auto &values = tuple->values;
        if (values.empty()) {
            stream << "()";
            if (naked)
                stream << '\n';
            break;
        }
        if (naked) {
            size_t offset = 0;
            bool single = ((offset + 1) == values.size());
        print_terse:
            streamValue(stream, values[offset], depth, false);
            offset++;
            while (offset != values.size()) {
                if (isNested(values[offset]))
                    break;
                stream << ' ';
                streamValue(stream, values[offset], depth, false);
                offset++;
            }
            stream << (single?";\n":"\n");
        //print_sparse:
            while (offset != values.size()) {
                auto value = values[offset];
                if (!llvm::isa<TupleValue>(value) // not a list
                    && (offset >= 1) // not first element in list
                    && ((offset + 1) != values.size()) // not last element in list
                    && !isNested(values[offset + 1])) { // next element can be terse packed too
                    single = false;
                    streamAnchor(stream, values[offset], depth + 1);
                    stream << "\\ ";
                    goto print_terse;
                }
                streamValue(stream, value, depth + 1);
                offset++;
            }

        } else {
            stream << '(';
            size_t offset = 0;
            while (offset != values.size()) {
                if (offset > 0)
                    stream << ' ';
                streamValue(stream, values[offset], depth + 1, false);
                offset++;
            }
            stream << ')';
            if (naked)
                stream << '\n';
        }
    } return;
    case Value::Integer: {
        auto a = llvm::cast<IntegerValue>(e);

        if (a->value_type->isSigned())
            stream << format("%" PRIi64, a->value);
        else
            stream << format("%" PRIu64, a->value);
        if (naked)
            stream << '\n';
    } return;
    case Value::Real: {
        auto a = llvm::cast<RealValue>(e);
        stream << format("%g", a->value);
        if (naked)
            stream << '\n';
    } return;
	case Value::Symbol: {
        auto a = llvm::cast<SymbolValue>(e);
        streamString(stream, a->value, "[]{}()\"");
        if (naked)
            stream << '\n';
    } return;
	case Value::String: {
        auto a = llvm::cast<StringValue>(e);
		stream << '"';
        streamString(stream, a->value, "\"");
		stream << '"';
        if (naked)
            stream << '\n';
    } return;
    default:
        printf("invalid kind: %i\n", e->kind);
        assert (false); break;
	}
}

/*
static std::string formatValue(Value *e, size_t depth=0, bool naked=false) {
    std::stringstream ss;
    streamValue(ss, e, depth, naked);
    return ss.str();
}
*/

static void printValue(Value *e, size_t depth=0, bool naked=false) {
    streamValue(std::cout, e, depth, naked);
}

void valueError (Value *expr, const char *format, ...) {
    const Anchor *anchor = find_valid_anchor(expr);
    if (!anchor) {
        if (expr)
            printValue(expr);
    }
    va_list args;
    va_start (args, format);
    Anchor::printErrorV(anchor, format, args);
    va_end (args);
}

void valueErrorV (Value *expr, const char *fmt, va_list args) {
    const Anchor *anchor = find_valid_anchor(expr);
    if (!anchor) {
        if (expr)
            printValue(expr);
    }
    Anchor::printErrorV(anchor, fmt, args);
}

template <typename T>
static T *verifyValueKind(Value *expr) {
    T *obj = expr?llvm::dyn_cast<T>(expr):NULL;
    if (obj) {
        return obj;
    } else {
        valueError(expr, "%s expected, not %s",
            getClassName(T::classkind()),
            getClassName(expr->kind));
    }
    return nullptr;
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

    StringValue *getAsString() {
        auto result = StringValue::create(string + 1, string_len - 2);
        initAnchor(result->anchor);
        unescape(result);
        return result;
    }

    SymbolValue *getAsSymbol() {
        auto result = SymbolValue::create(string, string_len);
        initAnchor(result->anchor);
        unescape(result);
        return result;
    }

    IntegerValue *getAsInteger() {
        auto result = IntegerValue::create(integer,
            is_unsigned?Type::UInt64:Type::Int64);
        initAnchor(result->anchor);
        return result;
    }

    RealValue *getAsReal() {
        auto result = RealValue::create(real, Type::Double);
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
    protected:
        std::vector<Value *> values;
        size_t start;
        Anchor anchor;
    public:

        ListBuilder(Lexer &lexer) :
            start(0) {
            lexer.initAnchor(anchor);
        }

        const Anchor &getAnchor() const {
            return anchor;
        }

        void resetStart() {
            start = getResultSize();
        }

        bool split() {
            // if we haven't appended anything, that's an error
            if (start == getResultSize()) {
                return false;
            }
            // move tail to new list
            std::vector<Value *> newvalues(
                values.begin() + start, values.end());
            // remove tail
            values.erase(
                values.begin() + start,
                values.end());
            // append new list
            append(TupleValue::create(newvalues));
            resetStart();
            return true;
        }

        void append(Value *item) {
            values.push_back(item);
        }

        size_t getResultSize() {
            return values.size();
        }

        Value *getSingleResult() {
            return values.front();
        }

        TupleValue *getResult() {
            auto result = TupleValue::create(values);
            result->anchor = anchor;
            return result;
        }

    };

    TupleValue *parseList(int end_token) {
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

    Value *parseAny () {
        assert(lexer.token != token_eof);
        if (lexer.token == token_open) {
            return parseList(token_close);
        } else if (lexer.token == token_square_open) {
            auto list = parseList(token_square_close);
            if (errors) return nullptr;
            auto sym = SymbolValue::create("[");
            sym->anchor = list->anchor;
            list->values.insert(list->values.begin(), sym);
            return list;
        } else if (lexer.token == token_curly_open) {
            auto list = parseList(token_curly_close);
            if (errors) return nullptr;
            auto sym = SymbolValue::create("{");
            sym->anchor = list->anchor;
            list->values.insert(list->values.begin(), sym);
            return list;
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

    Value *parseNaked (int column = 0, int depth = 0, int end_token = token_none) {
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
                    parse_origin = builder.getAnchor();
                    return nullptr;
                }
                lineno = lexer.lineno;
            } else if (lexer.lineno > lineno) {
                if (depth > 0) {
                    if (subcolumn == 0) {
                        subcolumn = lexer.column();
                    } else if (lexer.column() != subcolumn) {
                        error("indentation mismatch");
                        parse_origin = builder.getAnchor();
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

        if (builder.getResultSize() == 1) {
            return builder.getSingleResult();
        } else {
            return builder.getResult();
        }
    }

    Value *parseMemory (
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

    Value *parseFile (const char *path) {
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
//------------------------------------------------------------------------------

struct ILBuilder {
    FlowValue *flow;

    void continueAt(FlowValue *cont) {
        this->flow = cont;
        assert(!cont->hasArguments());
    }

    void insertAndAdvance(
        const std::vector<Value *> &values,
        FlowValue *next) {
        assert(flow);
        assert(!flow->hasArguments());
        flow->arguments = TupleValue::create(values);
        flow = next;
    }

    void br(const std::vector<Value *> &arguments) {
        insertAndAdvance(arguments, nullptr);
    }

    ParameterValue *call(std::vector<Value *> values) {
        auto next = FlowValue::create(1, "cret");
        values.push_back(next);
        insertAndAdvance(values, next);
        return next->parameters[0];
    }

};

static ILBuilder *builder;

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
                case T_Struct: {
                    auto st = llvm::cast<StructType>(il_type);
                    size_t fieldcount = st->getFieldCount();
                    LLVMTypeRef types[fieldcount];
                    for (size_t i = 0; i < fieldcount; ++i) {
                        types[i] = convertType(st->getField(i).getType());
                    }
                    result = LLVMStructType(types, fieldcount, false);
                } break;
                case T_Array: {
                    auto array = llvm::cast<ArrayType>(il_type);
                    result = LLVMArrayType(
                        convertType(
                            array->getElement()),
                        array->getCount());
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
                    ilError(nullptr, "can not translate type %s",
                        bangra::getRepr(il_type).c_str());
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

    Value *makeConstant(Type *il_type, const Variant &value) {
        switch(il_type->getKind()) {
            case T_Void: {
                return TupleValue::create({});
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
                return IntegerValue::create(ivalue,
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
                return RealValue::create(flvalue,
                    static_cast<RealType *>(il_type));
            } break;
            case T_Pointer: {
                return PointerValue::create(value.ptrval, il_type);
            } break;
            default: {
                ilError(nullptr, "can not make constant for type");
                return nullptr;
            } break;
        }
    }

    Variant convertConstant(
        Type *il_type, const Value *c) {
        Variant result;
        switch(il_type->getKind()) {
            case T_Integer: {
                switch(c->kind) {
                    case Value::Integer: {
                        auto ci = llvm::cast<IntegerValue>(c);
                        result.set_integer(il_type, ci->value);
                    } break;
                    case Value::Real: {
                        auto cr = llvm::cast<RealValue>(c);
                        result.set_integer(il_type, (int64_t)cr->value);
                    } break;
                    default: {
                        ilError(nullptr, "can't convert %s to integer",
                            bangra::getRepr(getType(c)).c_str());
                    } break;
                }
            } break;
            case T_Real: {
                switch(c->kind) {
                    case Value::Integer: {
                        auto ci = llvm::cast<IntegerValue>(c);
                        result.set_real(il_type, (double)ci->value);
                    } break;
                    case Value::Real: {
                        auto cr = llvm::cast<RealValue>(c);
                        result.set_real(il_type, cr->value);
                    } break;
                    default: {
                        ilError(nullptr, "can't convert %s to real",
                            bangra::getRepr(getType(c)).c_str());
                    } break;
                }
            } break;
            case T_Pointer: {
                switch(c->kind) {
                    case Value::Pointer: {
                        auto cp = llvm::cast<PointerValue>(c);
                        result.ptrval = cp->value;
                    } break;
                    case Value::String: {
                        auto cs = llvm::cast<StringValue>(c);
                        result.ptrval = const_cast<char *>(cs->value.c_str());
                    } break;
                    default: {
                        ilError(nullptr, "can't convert %s to pointer",
                            bangra::getRepr(getType(c)).c_str());
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
        const Value *value) {
        auto ilfunc = llvm::dyn_cast<ExternalValue>(value);
        if (!ilfunc) {
            ilError(nullptr, "not an external");
        }
        auto &result = functions[ilfunc->name];
        if (!result.second) {
            if (ilfunc->external_type->getKind() != T_CFunction) {
                ilError(nullptr, "not a C function type");
            }
            auto ilfunctype = static_cast<CFunctionType *>(ilfunc->external_type);

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

    Value *runFunction(
        const Value *func, const std::vector<Value *> &args) {

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

typedef std::vector<Value *> ILValueArray;

Value *evaluate(size_t argindex, FrameValue *frame, Value *value) {
    switch(value->kind) {
        case Value::Parameter: {
            auto param = llvm::cast<ParameterValue>(value);
            FrameValue *ptr = frame;
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
            // return unbound value
            return value;
        } break;
        case Value::Flow: {
            if (argindex == 0)
                // no closure creation required
                return value;
            else
                // create closure
                return ClosureValue::create(
                    llvm::cast<FlowValue>(value),
                    frame);
        } break;
        default:
            break;
    }
    return value;
}

Value *execute(std::vector<Value *> arguments) {

    FrameValue *frame = FrameValue::create();
    frame->idx = 0;

    auto retcont = FlowValue::create(1);
    // add special flow as return function
    arguments.push_back(retcont);

    while (true) {
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
        Value *callee = arguments[0];
        if (callee == retcont) {
            if (arguments.size() >= 2) {
                return arguments[1];
            } else {
                return TupleValue::create({});
            }
        }

        if (callee->kind == Value::Closure) {
            auto closure = llvm::cast<ClosureValue>(callee);

            frame = closure->frame;
            callee = closure->cont;
        }

        switch(callee->kind) {
            case Value::Flow: {
                auto flow = llvm::cast<FlowValue>(callee);

                arguments.erase(arguments.begin());
                if (arguments.size() > 0) {
                    if (frame->map.count(flow)) {
                        frame = FrameValue::create(frame);
                    }
                    frame->map[flow] = arguments;
                }

                assert(flow->arguments);
                size_t argcount = flow->arguments->values.size();
                arguments.resize(argcount);
                for (size_t i = 0; i < argcount; ++i) {
                    arguments[i] = evaluate(i, frame,
                        flow->arguments->values[i]);
                }
            } break;
            case Value::Builtin: {
                auto cb = llvm::cast<BuiltinValue>(callee);
                Value *closure = arguments.back();
                arguments.pop_back();
                arguments.erase(arguments.begin());
                Value *result = cb->handler(arguments);
                // generate fitting resume
                arguments.resize(2);
                arguments[0] = closure;
                arguments[1] = result;
            } break;
            case Value::BuiltinFlow: {
                auto cb = llvm::cast<BuiltinFlowValue>(callee);
                arguments = cb->handler(arguments);
            } break;
            case Value::External: {
                auto cb = llvm::cast<ExternalValue>(callee);
                Value *closure = arguments.back();
                arguments.pop_back();
                arguments.erase(arguments.begin());
                Value *result = ffi->runFunction(cb, arguments);
                // generate fitting resume
                arguments.resize(2);
                arguments[0] = closure;
                arguments[1] = result;
            } break;
            default: {
                ilError(callee, "can not apply %s",
                    getClassName(callee->kind));
            } break;
        }
    }

    return nullptr;
}

//------------------------------------------------------------------------------
// C BRIDGE (CLANG)
//------------------------------------------------------------------------------

class CVisitor : public clang::RecursiveASTVisitor<CVisitor> {
public:
    StructValue *dest;
    clang::ASTContext *Context;
    std::unordered_map<clang::RecordDecl *, bool> record_defined;
    std::unordered_map<clang::EnumDecl *, bool> enum_defined;
    std::unordered_map<const char *, char *> path_cache;
    std::unordered_map<std::string, Type *> named_structs;
    std::unordered_map<std::string, Type *> named_enums;
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

    void SetContext(clang::ASTContext * ctx, StructValue *dest_) {
        Context = ctx;
        dest = dest_;
    }

    void GetFields(StructType *struct_type, clang::RecordDecl * rd) {
        //auto &rl = Context->getASTRecordLayout(rd);

        for(clang::RecordDecl::field_iterator it = rd->field_begin(), end = rd->field_end(); it != end; ++it) {
            clang::DeclarationName declname = it->getDeclName();

            //unsigned idx = it->getFieldIndex();

            //auto offset = rl.getFieldOffset(idx);
            //unsigned width = it->getBitWidthValue(*Context);

            if(it->isBitField() || (!it->isAnonymousStructOrUnion() && !declname)) {
                break;
            }
            clang::QualType FT = it->getType();
            Type *fieldtype = TranslateType(FT);
            if(!fieldtype) {
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

        clang::RecordDecl * defn = rd->getDefinition();
        if (defn && !record_defined[rd]) {
            Anchor anchor = anchorFromLocation(rd->getSourceRange().getBegin());
            struct_type->setAnchor(anchor);

            GetFields(struct_type, defn);

            //auto &rl = Context->getASTRecordLayout(rd);
            //auto align = rl.getAlignment();
            //auto size = rl.getSize();

            // todo: make sure these fit
            // align.getQuantity()
            // size.getQuantity()

            record_defined[rd] = true;
        }

        return struct_type;
    }

    Type *TranslateEnum(clang::EnumDecl *ed) {
        std::string name = ed->getName();

        EnumType *enum_type = nullptr;
        if (name.size() && named_enums.count(name)) {
            enum_type = llvm::cast<EnumType>(named_enums[name]);
        } else {
            enum_type = llvm::cast<EnumType>(Type::Enum(name));
            if (!enum_type->isUnnamed()) {
                named_enums[name] = enum_type;
            }
        }

        clang::EnumDecl * defn = ed->getDefinition();
        if (defn && !enum_defined[ed]) {
            enum_type->setAnchor(
                anchorFromLocation(ed->getIntegerTypeRange().getBegin()));

            enum_type->setType(TranslateType(ed->getIntegerType()));

            for (auto it : ed->enumerators()) {
                Anchor anchor = anchorFromLocation(it->getSourceRange().getBegin());
                auto &val = it->getInitVal();

                enum_type->addField(
                    EnumType::Field(it->getName().data(),
                        val.getExtValue(), anchor));
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
            const clang::EnumType *ET = dyn_cast<clang::EnumType>(Ty);
            EnumDecl * ed = ET->getDecl();
            return TranslateEnum(ed);
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

    void exportType(const std::string &name, Type *type, const Anchor &anchor) {
        dest->addField(
            TypeRefValue::create(type),
            StructType::Field(
                name,
                Type::TypePointer,
                anchor));
    }

    void exportExternal(const std::string &name, Type *type, const Anchor &anchor) {
        dest->addField(
            ExternalValue::create(name, type),
            StructType::Field(
                name,
                type,
                anchor));
    }

    bool TraverseRecordDecl(clang::RecordDecl *rd) {
        if (rd->isFreeStanding()) {
            auto type = llvm::cast<StructType>(TranslateRecord(rd));
            if (!type->isUnnamed()) {
                exportType(type->getName(), type, type->getAnchor());
            }
        }
        return true;
    }

    bool TraverseEnumDecl(clang::EnumDecl *ed) {
        if (ed->isFreeStanding()) {
            auto type = llvm::cast<EnumType>(TranslateEnum(ed));
            if (!type->isUnnamed()) {
                exportType(type->getName(), type, type->getAnchor());
            }
        }
        return true;
    }

    bool TraverseVarDecl(clang::VarDecl *vd) {
        if (vd->isExternC()) {
            Anchor anchor = anchorFromLocation(vd->getSourceRange().getBegin());

            Type *type = TranslateType(vd->getType());
            if (!type) return true;

            exportExternal(vd->getName().data(), type, anchor);
        }

        return true;

    }

    bool TraverseTypedefDecl(clang::TypedefDecl *td) {

        Anchor anchor = anchorFromLocation(td->getSourceRange().getBegin());

        Type *type = TranslateType(td->getUnderlyingType());
        if (!type) return true;

        typedefs[td->getName().data()] = type;
        exportType(td->getName().data(), type, anchor);

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

        exportExternal(FuncName.c_str(), functype, anchor);

        return true;
    }
};

class CodeGenProxy : public clang::ASTConsumer {
public:
    StructValue *dest;
    CVisitor visitor;

    CodeGenProxy(StructValue *dest_) : dest(dest_) {}
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
    StructValue *dest;

    BangEmitLLVMOnlyAction(StructValue *dest_) :
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

static StructValue *importCModule (
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


    auto result = StructValue::create({},
        llvm::cast<StructType>(Type::Struct("", false)));

    // Create and execute the frontend to generate an LLVM bitcode module.
    std::unique_ptr<CodeGenAction> Act(new BangEmitLLVMOnlyAction(result));
    if (compiler.ExecuteAction(*Act)) {
        M = (LLVMModuleRef)Act->takeModule().release();
        assert(M);
        return result;
    } else {
        ilError(nullptr, "compilation failed");
    }

    return nullptr;
}

//------------------------------------------------------------------------------
// BUILTINS
//------------------------------------------------------------------------------

static bool builtin_checkparams (const std::vector<Value *> &args,
    int mincount, int maxcount, int skip = 0) {
    if ((mincount <= 0) && (maxcount == -1))
        return true;

    int argcount = (int)args.size() - skip;

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

static bool extract_bool(const Value *value) {
    if (auto resulttype = llvm::dyn_cast<IntegerValue>(value)) {
        if (getType(resulttype) == Type::Bool) {
            return (bool)resulttype->value;
        }
    }
    ilError(value, "boolean expected");
    return false;
}

static std::string extract_string(const Value *value) {
    auto resulttype = llvm::dyn_cast<StringValue>(value);
    if (!resulttype) {
        ilError(value, "string constant expected");
    }
    return resulttype->value;
}

static Type *extract_type(const Value *value) {
    auto resulttype = llvm::dyn_cast<TypeRefValue>(value);
    if (!resulttype) {
        ilError(value, "type constant expected");
    }
    return resulttype->value;
}

static const std::vector<Value *> &extract_tuple(
    const Value *value) {
    auto resulttype = llvm::dyn_cast<TupleValue>(value);
    if (!resulttype) {
        ilError(value, "tuple expected");
    }
    return resulttype->values;
}

static TypeRefValue *wrap(Type *type) {
    return TypeRefValue::create(type);
}

static IntegerValue *wrap(bool value) {
    return IntegerValue::create((int64_t)value,
        static_cast<IntegerType *>(Type::Bool));
}

static IntegerValue *wrap(int64_t value) {
    return IntegerValue::create(value,
        static_cast<IntegerType *>(Type::Int64));
}

static TupleValue *wrap(
    const std::vector<Value *> &args) {
    return TupleValue::create(args);
}

static RealValue *wrap(double value) {
    return RealValue::create(value,
        static_cast<RealType *>(Type::Double));
}

static StringValue *wrap(const std::string &s) {
    return StringValue::create(s);
}

static Value *builtin_print(const std::vector<Value *> &args) {
    builtin_checkparams(args, 0, -1);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0)
            std::cout << " ";
        auto &arg = args[i];
        switch(arg->kind) {
            case Value::String: {
                auto cs = llvm::cast<StringValue>(arg);
                std::cout << cs->value;
            } break;
            default: {
                std::cout << getRefRepr(args[i]);
            } break;
        }
    }
    std::cout << "\n";
    return TupleValue::create({});
}

static std::vector<Value *> builtin_branch(const std::vector<Value *> &args) {
    builtin_checkparams(args, 4, 4, 1);
    auto cond = extract_bool(args[1]);
    if (cond) {
        return { args[2], args[4] };
    } else {
        return { args[3], args[4] };
    }
}

static std::vector<Value *> builtin_call_cc(const std::vector<Value *> &args) {
    builtin_checkparams(args, 2, 2, 1);
    return { args[1], args[2], args[2] };
}

static Value *builtin_repr(const std::vector<Value *> &args) {
    builtin_checkparams(args, 1, 1);
    return wrap(getRepr(args[0]));
}

static Value *builtin_tupleof(const std::vector<Value *> &args) {
    builtin_checkparams(args, 0, -1);
    return wrap(args);
}

static Value *builtin_structof(const std::vector<Value *> &args) {
    builtin_checkparams(args, 0, -1);
    auto result = StructValue::create({}, Type::Struct(""));

    for (size_t i = 0; i < args.size(); ++i) {
        auto &pair = extract_tuple(args[i]);
        if (pair.size() != 2)
            ilError(args[i], "tuple must have exactly two elements");
        auto name = extract_string(pair[0]);
        auto value = pair[1];
        result->addField(value,
            StructType::Field(name, getType(value)));
    }

    return result;
}

static Value *builtin_typeof(const std::vector<Value *> &args) {
    builtin_checkparams(args, 1, 1);
    return wrap(getType(args[0]));
}

static Value *builtin_dump(const std::vector<Value *> &args) {
    builtin_checkparams(args, 1, 1);
    auto start_value = args[0];
    std::list<Value *> todo;
    std::unordered_set<Value *> visited;
    todo.push_back(start_value);
    while (!todo.empty()) {
        auto value = todo.back();
        todo.pop_back();
        if (!visited.count(value)) {
            visited.insert(value);
            std::cout << getRepr(value) << "\n";
            switch (value->kind) {
                case Value::Closure: {
                    auto closure = llvm::cast<ClosureValue>(value);
                    todo.push_front(closure->frame);
                    todo.push_front(closure->cont);
                } break;
                case Value::Flow: {
                    auto flow = llvm::cast<FlowValue>(value);
                    if (flow->hasArguments()) {
                        for (size_t i = 0;
                            i < flow->arguments->values.size(); ++i) {
                            auto dest = flow->arguments->values[i];
                            if (llvm::isa<FlowValue>(dest)) {
                                todo.push_front(dest);
                            }
                        }
                    }
                } break;
                default: break;
            }
        }
    }
    return start_value;
}


static Value *builtin_cdecl(const std::vector<Value *> &args) {
    builtin_checkparams(args, 3, 3);
    Type *rettype = extract_type(args[0]);
    const std::vector<Value *> &params = extract_tuple(args[1]);
    bool vararg = extract_bool(args[2]);

    std::vector<Type *> paramtypes;
    size_t paramcount = params.size();
    for (size_t i = 0; i < paramcount; ++i) {
        paramtypes.push_back(extract_type(params[i]));
    }
    return wrap(Type::CFunction(rettype, paramtypes, vararg));
}

static Value *builtin_external(const std::vector<Value *> &args) {
    builtin_checkparams(args, 2, 2);
    std::string name = extract_string(args[0]);
    Type *type = extract_type(args[1]);
    return ExternalValue::create(name, type);
}

// (import-c const-path (tupleof const-string ...))
static Value *builtin_import_c(const std::vector<Value *> &args) {
    builtin_checkparams(args, 2, 2);
    std::string path = extract_string(args[0]);
    auto compile_args = extract_tuple(args[1]);
    std::vector<std::string> cargs;
    for (size_t i = 0; i < compile_args.size(); ++i) {
        cargs.push_back(extract_string(compile_args[i]));
    }
    return bangra::importCModule(path, cargs);
}

static Value *builtin_at_op(const std::vector<Value *> &args) {
    builtin_checkparams(args, 2, 2);
    Value *obj = args[0];
    Value *key = args[1];
    switch(obj->kind) {
        case Value::Tuple: {
            auto cs = llvm::cast<TupleValue>(obj);
            auto t = llvm::cast<TupleType>(getType(cs));
            switch(key->kind) {
                case Value::Integer: {
                    auto ci = llvm::cast<IntegerValue>(key);
                    if ((size_t)ci->value < t->getCount()) {
                        return cs->values[ci->value];
                    } else {
                        ilError(key, "index out of bounds");
                        return nullptr;
                    }
                } break;
                default: {
                    ilError(key, "illegal index type");
                    return nullptr;
                } break;
            }
        } break;
        case Value::Struct: {
            auto cs = llvm::cast<StructValue>(obj);
            auto t = llvm::cast<StructType>(getType(cs));
            switch(key->kind) {
                case Value::Integer: {
                    auto ci = llvm::cast<IntegerValue>(key);
                    if ((size_t)ci->value < t->getFieldCount()) {
                        return cs->values[ci->value];
                    } else {
                        ilError(key, "index out of bounds");
                        return nullptr;
                    }
                } break;
                case Value::String: {
                    auto cstr = llvm::cast<StringValue>(key);
                    size_t idx = t->getFieldIndex(cstr->value);
                    if (idx != (size_t)-1) {
                        return cs->values[idx];
                    } else {
                        ilError(key, "no such member");
                        return nullptr;
                    }
                } break;
                default: {
                    ilError(key, "illegal key type");
                    return nullptr;
                } break;
            }
        } break;
        default: {
            ilError(obj, "unsubscriptable type");
            return nullptr;
        } break;
    }

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
    static Value *operate(const double &a, const int64_t &b) {
        return wrap(NextT::operate(a, b));
    }
    static Value *operate(const int64_t &a, const double &b) {
        return wrap(NextT::operate(a, b));
    }
    template<typename T>
    static Value *operate(const T &a, const T &b) {
        return wrap(NextT::operate(a, b));
    }
    template<typename Ta, typename Tb>
    static Value *operate(const Ta &a, const Tb &b) {
        ilError(nullptr, "illegal operands");
        return nullptr;
    }
};


class dispatch_types_failed {
public:
    template<typename F>
    static Value *dispatch(const Value *v, const F &next) {
        ilError(v, "illegal operand");
        return nullptr;
    }
};

template<typename NextT>
class dispatch_string_type {
public:
    template<typename F>
    static Value *dispatch(const Value *v, const F &next) {
        if (v->kind == Value::String) {
            auto ca = llvm::cast<StringValue>(v);
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
    static Value *dispatch(const Value *v, const F &next) {
        if (v->kind == Value::Integer) {
            auto ca = llvm::cast<IntegerValue>(v);
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
    static Value *dispatch(const Value *v, const F &next) {
        if (v->kind == Value::Integer) {
            auto ca = llvm::cast<IntegerValue>(v);
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
    static Value *dispatch(const Value *v, const F &next) {
        if (v->kind == Value::Real) {
            auto ca = llvm::cast<RealValue>(v);
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
    Value *operator ()(const Q &ca_value) const {
        return wrap(F::operate(ca_value));
    }
};

template<class D, class F, typename T>
class builtin_binary_op3 {
public:
    const T &ca_value;
    builtin_binary_op3(const T &ca_value_) : ca_value(ca_value_) {}

    template<typename Q>
    Value *operator ()(const Q &cb_value) const {
        return builtin_filter_op<F>::operate(ca_value, cb_value);
    }
};

template<class D, class F>
class builtin_binary_op2 {
public:
    const Value *b;
    builtin_binary_op2(const Value *b_) : b(b_) {}

    template<typename T>
    Value *operator ()(const T &ca_value) const {
        return D::dispatch(b, builtin_binary_op3<D, F, T>(ca_value));
    }
};

template<class D, class F>
static Value *builtin_binary_op(
    const std::vector<Value *> &args) {
    if (args.size() != 2) {
        ilError(nullptr, "invalid number of arguments");
    }
    return D::dispatch(args[0], builtin_binary_op2<D, F>(args[1]));
}


template<class D, class F>
static Value *builtin_unary_op(
    const std::vector<Value *> &args) {
    if (args.size() != 1) {
        ilError(nullptr, "invalid number of arguments");
    }
    return D::dispatch(args[0], builtin_binary_op1<D, F>());
}

//------------------------------------------------------------------------------
// TRANSLATION
//------------------------------------------------------------------------------

typedef Value *(*bangra_preprocessor)(StructValue *, Value *);

typedef std::map<std::string, bangra_preprocessor> NameMacroMap;
typedef std::unordered_map<std::string, Type *> NameTypeMap;
typedef std::unordered_map<std::string, Value *> NameValueMap;

//------------------------------------------------------------------------------

static StructValue *new_scope() {
    auto scope = StructValue::create({}, Type::Struct("scope"));
    scope->addField(UnitValue::create_null(),
        StructType::Field("#parent", Type::Null));
    return scope;
}

static StructValue *new_scope(StructValue *scope) {
    assert(scope);
    auto subscope = StructValue::create({}, Type::Struct("scope"));
    subscope->addField(scope,
        StructType::Field("#parent", getType(scope)));
    return subscope;
}

static void setLocal(StructValue *scope, const std::string &name, Value *value) {
    assert(scope);
    scope->addField(value,
        StructType::Field(name, getType(value)));
}

static void setBuiltin(
    StructValue *scope, const std::string &name, ILBuiltinFunction func) {
    assert(scope);
    setLocal(scope, name, BuiltinValue::create(func, name));
}

static void setBuiltin(
    StructValue *scope, const std::string &name, ILBuiltinFlowFunction func) {
    assert(scope);
    setLocal(scope, name, BuiltinFlowValue::create(func, name));
}

static bool isLocal(StructValue *scope, const std::string &name) {
    assert(scope);
    size_t idx = scope->struct_type->getFieldIndex(name);
    if (idx == (size_t)-1) return false;
    return true;
}

static StructValue *getParent(StructValue *scope) {
    size_t idx = scope->struct_type->getFieldIndex("#parent");
    if (idx != (size_t)-1) {
        return llvm::dyn_cast<StructValue>(scope->values[idx]);
    }
    return nullptr;
}

static Value *getLocal(StructValue *scope, const std::string &name) {
    assert(scope);
    while (scope) {
        size_t idx = scope->struct_type->getFieldIndex(name);
        if (idx != (size_t)-1) {
            return scope->values[idx];
        }
        scope = getParent(scope);
    }
    return nullptr;
}

static std::unordered_map<std::string, bangra_preprocessor> preprocessors;

//------------------------------------------------------------------------------

static bool isSymbol (const Value *expr, const char *sym) {
    if (expr) {
        if (auto symexpr = llvm::dyn_cast<SymbolValue>(expr))
            return (symexpr->value == sym);
    }
    return false;
}

//------------------------------------------------------------------------------

static StructValue *globals = nullptr;

struct Cursor {
    Value *value;
    TupleIter next;
};

Cursor translate(StructValue *env, TupleIter it);

static Cursor parse_expr_list (StructValue *env, TupleIter it) {
    Value *value = nullptr;
    while (it) {
        auto cur = translate(env, it);
        value = cur.value;
        it = cur.next;
    }

    if (!value)
        value = TupleValue::create({});

    return { value, it };
}

static Cursor parse_do (StructValue *env, TupleIter topit) {
    auto cur = parse_expr_list(env, TupleIter(*topit++, 1));
    return { cur.value, topit };
}

static Cursor parse_function (StructValue *env, TupleIter topit) {
    TupleIter it(*topit++, 1);
    auto expr_parameters = *it++;

    auto currentblock = builder->flow;

    auto function = FlowValue::create(0, "func");

    builder->continueAt(function);
    auto subenv = new_scope(env);

    auto params = verifyValueKind<TupleValue>(expr_parameters);
    TupleIter param(params);
    while (param) {
        auto symname = verifyValueKind<SymbolValue>(*param);
        auto bp = ParameterValue::create(symname->value);
        function->appendParameter(bp);
        setLocal(subenv, symname->value, bp);
        param++;
    }
    auto ret = function->appendParameter(ParameterValue::create());

    auto result = parse_expr_list(subenv, it);

    builder->br({ret, result.value});

    builder->continueAt(currentblock);

    return { function, topit };
}

static Cursor parse_letrec (StructValue *env, TupleIter topit) {
    TupleIter it(*topit++, 1);

    auto currentblock = builder->flow;

    auto function = FlowValue::create(0, "let");

    builder->continueAt(function);
    auto subenv = new_scope(env);

    bool multi = true;
    if ((*it)->kind == Value::Symbol) {
        multi = false;
        auto symname = verifyValueKind<SymbolValue>(*it++);
        auto bp = ParameterValue::create(symname->value);
        function->appendParameter(bp);
        setLocal(subenv, symname->value, bp);
    } else {
        // declare parameters
        TupleIter param(it);
        while (param) {
            auto pair = verifyValueKind<TupleValue>(*param);
            if (pair->values.size() < 2) {
                ilError(pair, "at least two arguments expected");
            }
            auto symname = verifyValueKind<SymbolValue>(pair->values[0]);
            auto bp = ParameterValue::create(symname->value);
            function->appendParameter(bp);
            setLocal(subenv, symname->value, bp);
            param++;
        }
    }

    auto ret = function->appendParameter(ParameterValue::create());

    auto result = parse_expr_list(subenv, topit);
    topit = result.next;

    builder->br({ret, result.value});

    builder->continueAt(currentblock);

    std::vector<Value *> args;
    args.push_back(function);

    if (!multi) {
        auto cur = parse_expr_list(subenv, it);
        args.push_back(cur.value);
    } else {
        // initialize parameters
        TupleIter param(it);
        while (param) {
            auto pair = verifyValueKind<TupleValue>(*param);
            auto cur = parse_expr_list(subenv, TupleIter(pair, 1));
            args.push_back(cur.value);
            param++;
        }
    }

    return { builder->call(args), topit };
}

static Cursor parse_implicit_apply (StructValue *env, TupleIter it) {
    auto ccur = translate(env, it);
    Value *callable = ccur.value;
    it = ccur.next;

    std::vector<Value *> args;
    args.push_back(callable);

    while (it) {
        auto cur = translate(env, it);
        args.push_back(cur.value);
        it = cur.next;
    }

    return { builder->call(args), it };
}

static Cursor parse_apply (StructValue *env, TupleIter topit) {
    auto cur = parse_implicit_apply(env, TupleIter(*topit++, 1));
    return { cur.value, topit };
}

bool hasTypeValue(Type *type) {
    assert(type);
    if ((type == Type::Void) || (type == Type::Empty))
        return false;
    return true;
}

static Cursor parse_quote (StructValue *env, TupleIter topit) {
    TupleIter it(*topit++, 1);
    auto expr_value = *it++;
    if (it) {
        // multiple values, wrap in list
        std::vector<Value *> lines = { expr_value };
        while (it) {
            lines.push_back(*it);
            it++;
        }
        return { TupleValue::create(lines), topit };
    } else {
        // single value
        return { expr_value, topit };
    }
}

static Cursor parse_decorate1 (StructValue *env, TupleIter topit) {
    auto startit = topit;
    TupleIter it(*topit++, 1);

    std::vector<Value *> values;
    while (it) {
        values.push_back(*it);
        it++;
    }

    if (topit) {
        auto cur = translate(env, topit);
        values.push_back(cur.value);
        topit = cur.next;
    } else {
        valueError(*startit, "missing next line");
    }

    std::vector<Value *> topvalues = { TupleValue::create(values) };
    while (topit) {
        topvalues.push_back(*topit++);
    }

    return translate(env, TupleIter(TupleValue::create(topvalues), 0));
}

static Cursor parse_decorate_all (StructValue *env, TupleIter topit) {
    TupleIter it(*topit++, 1);

    std::vector<Value *> values;
    while (it) {
        values.push_back(*it);
        it++;
    }

    while (topit) {
        values.push_back(*topit++);
    }

    return translate(env, TupleIter(
        TupleValue::create({ TupleValue::create(values) }), 0));
}

static Cursor parse_locals (StructValue *env, TupleIter topit) {
    std::vector<Value *> args;
    args.push_back(getLocal(globals, "structof"));

    auto tupleof = getLocal(globals, "tupleof");

    std::unordered_set<std::string> visited;
    while (env) {
        auto struct_type = env->struct_type;
        size_t fieldcount = struct_type->getFieldCount();
        for (size_t i = 0; i < fieldcount; ++i) {
            auto &field = struct_type->getField(i);
            auto name = field.getName();
            if (!visited.count(name)) {
                visited.insert(name);
                args.push_back(
                    builder->call(
                        {tupleof, wrap(name), env->values[i]}));
            }
        }
        env = getParent(env);
    }

    topit++;
    return { builder->call(args), topit };
}

static Cursor parse_syntax_run (StructValue *env, TupleIter topit) {
    TupleIter it(*topit++, 1);

    auto currentblock = builder->flow;

    auto mainfunc = FlowValue::create();
    auto ret = mainfunc->appendParameter(ParameterValue::create());

    auto subenv = new_scope(env);

    builder->continueAt(mainfunc);

    auto retval = parse_expr_list(subenv, it);
    builder->br({ ret, retval.value });

    auto result = execute({mainfunc});

    builder->continueAt(currentblock);

    std::vector<Value *> values = { result };
    while (topit) {
        values.push_back(*topit++);
    }
    return translate(env, TupleIter(TupleValue::create(values), 0));
}

static bool verifyParameterCount (TupleValue *expr,
    int mincount, int maxcount) {
    if ((mincount <= 0) && (maxcount == -1))
        return true;
    int argcount = (int)expr->values.size() - 1;

    if ((maxcount >= 0) && (argcount > maxcount)) {
        valueError(expr->values[maxcount + 1],
            "excess argument. At most %i arguments expected", maxcount);
        return false;
    }
    if ((mincount >= 0) && (argcount < mincount)) {
        valueError(expr, "at least %i arguments expected", mincount);
        return false;
    }
    return true;
}

struct TranslateTable {
    typedef Cursor (*TranslatorFunc)(StructValue *env, TupleIter topit);

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

    TranslatorFunc match(TupleValue *expr) {
        auto head = verifyValueKind<SymbolValue>(expr->values.front());
        auto &t = translators[head->value];
        if (!t.translate) return nullptr;
        verifyParameterCount(expr, t.mincount, t.maxcount);
        return t.translate;
    }

};

static TranslateTable translators;

//------------------------------------------------------------------------------

static void registerTranslators() {
    auto &t = translators;
    t.set(parse_apply, "apply", 1, -1);
    t.set(parse_letrec, "let", 1, -1);
    t.set(parse_do, "do", 0, -1);
    t.set(parse_function, "function", 1, -1);
    t.set(parse_quote, "quote", 1, -1);
    t.set(parse_decorate1, "::", 1, -1);
    t.set(parse_decorate_all, "::*", 1, -1);
    t.set(parse_locals, "locals", 0, 0);
    t.set(parse_syntax_run, "syntax-run", 1, -1);
}

static Cursor translateFromList (StructValue *env, TupleIter topit) {
    auto expr = llvm::cast<TupleValue>(*topit);
    assert(expr);
    if (expr->values.size() < 1) {
        valueError(expr, "symbol expected");
    }
    auto func = translators.match(expr);
    if (func) {
        return func(env, topit);
    } else {
        auto cur = parse_implicit_apply(env, TupleIter(*topit++, 0));
        return { cur.value, topit };
    }
}

Cursor translate (StructValue *env, TupleIter topit) {
    auto expr = *topit;
    assert(expr);
    Value *result = nullptr;
    switch(expr->kind) {
        case Value::Tuple: {
            auto cur = translateFromList(env, topit);
            result = cur.value;
            assert(cur.next != topit);
            topit = cur.next;
        } break;
        case Value::Symbol: {
            auto sym = llvm::cast<SymbolValue>(expr);
            std::string value = sym->value;
            result = getLocal(env, value);
            if (!result) {
                valueError(expr,
                    "unknown symbol '%s'", value.c_str());
            }
            topit++;
        } break;
        default: {
            result = expr;
            topit++;
        } break;
    }
    if (result && !result->anchor.isValid()) {
        const Anchor *anchor = find_valid_anchor(expr);
        if (anchor) {
            result->anchor = *anchor;
        }
    }
    assert(result);
    return { result, topit };
}

static Value *builtin_eval(const std::vector<Value *> &args) {
    builtin_checkparams(args, 2, 2);
    auto scope = verifyValueKind<StructValue>(args[0]);
    auto expr_eval = args[1];

    auto mainfunc = FlowValue::create();
    auto ret = mainfunc->appendParameter(ParameterValue::create());

    auto subenv = new_scope(scope);

    builder->continueAt(mainfunc);

    auto arg = TupleValue::create({expr_eval});
    auto retval = translate(subenv, TupleIter(arg, 0));
    builder->br({ ret, retval.value });

    return mainfunc;
}

//------------------------------------------------------------------------------
// FILE SYSTEM
//------------------------------------------------------------------------------

struct ModuleLoader {
    std::unordered_map<std::string, StructValue *> modules;

    ModuleLoader()
    {}

    StructValue *import_module(const std::string &name) {
        auto it = modules.find(name);
        if (it != modules.end()) {
            return it->second;
        }
        return nullptr;
    }
};

ModuleLoader loader;


//------------------------------------------------------------------------------
// INITIALIZATION
//------------------------------------------------------------------------------

static void initGlobals () {
    globals = new_scope();
    auto env = globals;

    setLocal(env, "globals", env);

    setLocal(env, "void", wrap(Type::Void));
    setLocal(env, "null", wrap(Type::Null));
    setLocal(env, "half", wrap(Type::Half));
    setLocal(env, "float", wrap(Type::Float));
    setLocal(env, "double", wrap(Type::Double));
    setLocal(env, "bool", wrap(Type::Bool));

    setLocal(env, "int8", wrap(Type::Int8));
    setLocal(env, "int16", wrap(Type::Int16));
    setLocal(env, "int32", wrap(Type::Int32));
    setLocal(env, "int64", wrap(Type::Int64));

    setLocal(env, "uint8", wrap(Type::UInt8));
    setLocal(env, "uint16", wrap(Type::UInt16));
    setLocal(env, "uint32", wrap(Type::UInt32));
    setLocal(env, "uint64", wrap(Type::UInt64));

    setLocal(env, "usize_t",
        wrap(Type::Integer(sizeof(size_t)*8,false)));

    setLocal(env, "rawstring", wrap(Type::Rawstring));

    setLocal(env, "int", getLocal(env, "int32"));

    auto booltype = llvm::cast<IntegerType>(Type::Bool);
    setLocal(env, "true", IntegerValue::create(1, booltype));
    setLocal(env, "false", IntegerValue::create(0, booltype));

    setLocal(env, "null", UnitValue::create_null());

    setBuiltin(env, "print", builtin_print);
    setBuiltin(env, "repr", builtin_repr);
    setBuiltin(env, "cdecl", builtin_cdecl);
    setBuiltin(env, "tupleof", builtin_tupleof);
    setBuiltin(env, "structof", builtin_structof);
    setBuiltin(env, "typeof", builtin_typeof);
    setBuiltin(env, "external", builtin_external);
    setBuiltin(env, "import-c", builtin_import_c);
    setBuiltin(env, "eval", builtin_eval);
    setBuiltin(env, "branch", builtin_branch);
    setBuiltin(env, "call/cc", builtin_call_cc);
    setBuiltin(env, "dump", builtin_dump);

    setBuiltin(env, "@", builtin_at_op);

    setBuiltin(env, "+",
        builtin_binary_op<dispatch_arith_string_types, builtin_add_op>);
    setBuiltin(env, "-",
        builtin_binary_op<dispatch_arith_types, builtin_sub_op>);
    setBuiltin(env, "*",
        builtin_binary_op<dispatch_arith_types, builtin_mul_op>);
    setBuiltin(env, "/",
        builtin_binary_op<dispatch_arith_types, builtin_div_op>);
    setBuiltin(env, "%",
        builtin_binary_op<dispatch_arith_types, builtin_mod_op>);

    setBuiltin(env, "&",
        builtin_binary_op<dispatch_bit_types, builtin_bitand_op>);
    setBuiltin(env, "|",
        builtin_binary_op<dispatch_bit_types, builtin_bitor_op>);
    setBuiltin(env, "^",
        builtin_binary_op<dispatch_bit_types, builtin_bitxor_op>);
    setBuiltin(env, "~",
        builtin_unary_op<dispatch_bit_types, builtin_bitnot_op>);

    setBuiltin(env, "not",
        builtin_unary_op<dispatch_boolean_types, builtin_not_op>);

    setBuiltin(env, "==",
        builtin_binary_op<dispatch_cmp_types, builtin_eq_op>);
    setBuiltin(env, "!=",
        builtin_binary_op<dispatch_cmp_types, builtin_ne_op>);
    setBuiltin(env, ">",
        builtin_binary_op<dispatch_cmp_types, builtin_gt_op>);
    setBuiltin(env, ">=",
        builtin_binary_op<dispatch_cmp_types, builtin_ge_op>);
    setBuiltin(env, "<",
        builtin_binary_op<dispatch_cmp_types, builtin_lt_op>);
    setBuiltin(env, "<=",
        builtin_binary_op<dispatch_cmp_types, builtin_le_op>);

}

static void init() {
    bangra::support_ansi = isatty(fileno(stdout));

    Type::initTypes();
    registerTranslators();

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

static void handleException(StructValue *env, Value *expr) {
    streamValue(std::cerr, expr, 0, true);
    valueError(expr, "an exception was raised");
}

static bool translateRootValueList (StructValue *env, Value *expr) {

    auto mainfunc = FlowValue::create();
    auto ret = mainfunc->appendParameter(ParameterValue::create());
    builder->continueAt(mainfunc);

    parse_expr_list(env, TupleIter(expr, 1));
    builder->br({ ret });

/*
#ifdef BANGRA_DEBUG_IL
    std::cout << env.global.module->getRepr();
    fflush(stdout);
#endif
*/

    execute({mainfunc});

    return true;
}

static bool compileMain (Value *expr) {
    assert(expr);
    auto tuple = verifyValueKind<TupleValue>(expr);

    auto env = globals;

    std::string lastlang = "";
    while (true) {
        auto head = verifyValueKind<SymbolValue>(tuple->values.front());
        if (!head) return false;
        if (head->value == BANGRA_HEADER)
            break;
        auto preprocessor = preprocessors[head->value];
        if (!preprocessor) {
            valueError(expr, "unrecognized header: '%s'; try '%s' instead.",
                head->value.c_str(),
                BANGRA_HEADER);
            return false;
        }
        if (lastlang == head->value) {
            valueError(expr,
                "header has not changed after preprocessing; is still '%s'.",
                head->value.c_str());
        }
        lastlang = head->value;
        auto orig_expr = expr;
        try {
            expr = preprocessor(env, expr);
        } catch (Value *expr) {
            handleException(env, expr);
            return false;
        }
        if (!expr) {
            valueError(orig_expr,
                "preprocessor returned null.");
            return false;
        }
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

static Value *parseLoader(const char *executable_path) {
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
    auto expr = footerParser.parseMemory(
        cursor, ptr + size, executable_path, cursor - ptr);
    if (!expr) {
        fprintf(stderr, "could not parse footer expression\n");
        return NULL;
    }
    if (expr->kind != Value::Tuple)  {
        fprintf(stderr, "footer expression is not a list\n");
        return NULL;
    }
    auto tuple = llvm::cast<TupleValue>(expr);
    if (tuple->values.size() < 2) {
        fprintf(stderr, "footer needs at least two arguments\n");
        return NULL;
    }
    auto head = tuple->values[0];
    if (head->kind != Value::Symbol)  {
        fprintf(stderr, "footer expression does not begin with symbol\n");
        return NULL;
    }
    if (!isSymbol(head, "script-size"))  {
        fprintf(stderr, "footer expression does not begin with 'script-size'\n");
        return NULL;
    }
    auto arg = tuple->values[1];
    if (arg->kind != Value::Integer)  {
        fprintf(stderr, "script-size argument is not integer\n");
        return NULL;
    }
    auto offset = llvm::cast<IntegerValue>(expr)->value;
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

    Value *expr = NULL;
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

    if (expr) {
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

    bangra::Value *expr = NULL;

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

    if (expr) {
        bangra::compileMain(expr);
    } else {
        return 1;
    }

    return 0;
}

bangra::Value *bangra_parse_file(const char *path) {
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

#endif // BANGRA_MAIN_CPP_IMPL
