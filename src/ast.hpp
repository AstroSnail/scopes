/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#ifndef SCOPES_AST_HPP
#define SCOPES_AST_HPP

#include "symbol.hpp"
#include "any.hpp"
#include "result.hpp"

#include <vector>
#include <unordered_map>

namespace scopes {

struct Anchor;
struct List;
struct Scope;

#define SCOPES_AST_KIND() \
    T(ASTK_Function, "ast-kind-function", ASTFunction) \
    T(ASTK_Block, "ast-kind-block", Block) \
    T(ASTK_If, "ast-kind-if", If) \
    T(ASTK_Symbol, "ast-kind-symbol", ASTSymbol) \
    T(ASTK_ArgumentList, "ast-kind-argumentlist", ASTArgumentList) \
    T(ASTK_Call, "ast-kind-call", Call) \
    T(ASTK_Let, "ast-kind-let", Let) \
    T(ASTK_Loop, "ast-kind-loop", Loop) \
    T(ASTK_Const, "ast-kind-const", Const) \
    T(ASTK_Break, "ast-kind-break", Break) \
    T(ASTK_Repeat, "ast-kind-repeat", Repeat) \
    T(ASTK_Return, "ast-kind-break", Return) \
    T(ASTK_SyntaxExtend, "ast-kind-syntax-extend", SyntaxExtend)

enum ASTKind {
#define T(NAME, BNAME, CLASS) \
    NAME,
    SCOPES_AST_KIND()
#undef T
};

// forward declarations
#define T(NAME, BNAME, CLASS) struct CLASS;
    SCOPES_AST_KIND()
#undef T

struct ASTNode;
struct ASTValue;

typedef std::vector<ASTSymbol *> ASTSymbols;
typedef std::vector<ASTNode *> ASTNodes;
typedef std::vector<ASTValue *> ASTValues;
typedef std::vector<Block *> Blocks;

//------------------------------------------------------------------------------
/*
    each ast node can have several ways to continue flow, which are not
    mutually exclusive, and each flow method can have its own type:

    1. continue: the continue type is forwarded to whatever
        node needs to process it; if there's no continue type, the expression
        always returns

        for loops, the break statement defines the continue type.

    2. return: the return type leaves the function and types the function's
        continue type; if there's no return type, the expression always
        continues


*/

//------------------------------------------------------------------------------

struct ASTNode {
    ASTKind kind() const;

    ASTNode(ASTKind _kind, const Anchor *_anchor);
    ASTNode(const ASTNode &other) = delete;

    const Anchor *anchor() const;
    bool is_empty() const;

    const Type *get_type() const;
    const Type *get_value_type() const;
private:
    const ASTKind _kind;

protected:
    const Anchor *_anchor;
};

//------------------------------------------------------------------------------

struct ASTValue : ASTNode {
    static bool classof(const ASTNode *T);

    ASTValue(ASTKind _kind, const Anchor *anchor);
};

//------------------------------------------------------------------------------

struct ASTArgumentList : ASTValue {
    static bool classof(const ASTNode *T);

    ASTArgumentList(const Anchor *anchor, const ASTNodes &values);
    const Type *get_value_type() const;

    void append(Symbol key, ASTNode *node);
    void append(ASTNode *node);
    void flatten();

    static ASTArgumentList *from(const Anchor *anchor, const ASTNodes &values = {});

    ASTNodes values;
};

//------------------------------------------------------------------------------

struct ASTFunction : ASTValue {
    static bool classof(const ASTNode *T);

    ASTFunction(const Anchor *anchor, Symbol name, const ASTSymbols &params, Block *block);

    bool is_forward_decl() const;
    void set_inline();
    bool is_inline() const;
    void append_param(ASTSymbol *sym);
    Block *ensure_body();
    SCOPES_RESULT(ASTNode *) resolve_symbol(ASTSymbol *sym) const;
    const Type *get_value_type() const;

    static ASTFunction *from(
        const Anchor *anchor, Symbol name,
        const ASTSymbols &params = {}, Block *block = nullptr);

    Symbol name;
    ASTSymbols params;
    Block *body;
    bool _inline;
    const String *docstring;
    const Type *return_type;
    ASTFunction *scope;

    std::unordered_map<const List *, ASTFunction *> instances;
    std::unordered_map<ASTSymbol *, ASTNode *> map;
};

//------------------------------------------------------------------------------

struct Block : ASTNode {
    static bool classof(const ASTNode *T);

    Block(const Anchor *anchor, const ASTNodes &nodes);
    void append(ASTNode *node);
    const Type *get_value_type() const;

    static Block *from(const Anchor *anchor, const ASTNodes &nodes = {});

    void strip_constants();
    void flatten();

    ASTNodes body;
};

//------------------------------------------------------------------------------

struct Clause {
    ASTNode *cond;
    Block *body;
};

typedef std::vector<Clause> Clauses;

struct If : ASTNode {
    static bool classof(const ASTNode *T);

    If(const Anchor *anchor, const Clauses &clauses);
    const Type *get_value_type() const;

    static If *from(const Anchor *anchor, const Clauses &clauses = {});

    ASTNode *get_else_clause() const;
    void append(ASTNode *cond, Block *expr);
    void append(Block *expr);

    Clauses clauses;
};

//------------------------------------------------------------------------------

struct ASTSymbol : ASTValue {
    static bool classof(const ASTNode *T);

    ASTSymbol(const Anchor *anchor, Symbol name, const Type *type, bool variadic);
    static ASTSymbol *from(const Anchor *anchor, Symbol name = SYM_Unnamed, const Type *type = nullptr);
    static ASTSymbol *variadic_from(const Anchor *anchor, Symbol name = SYM_Unnamed, const Type *type = nullptr);
    const Type *get_value_type() const;

    bool is_variadic() const;

    Symbol name;
    const Type *type;
    bool variadic;
};

//------------------------------------------------------------------------------

enum CallFlags {
    CF_RawCall = (1 << 0),
    CF_TryCall = (1 << 1),
};

struct Call : ASTNode {
    static bool classof(const ASTNode *T);

    Call(const Anchor *anchor, ASTNode *callee, ASTArgumentList *args);
    static Call *from(const Anchor *anchor, ASTNode *callee, ASTArgumentList *args = nullptr);
    const Type *get_value_type() const;

    ASTArgumentList *ensure_args();

    ASTNode *callee;
    ASTArgumentList *args;
    uint32_t flags;
};

//------------------------------------------------------------------------------

struct ASTBinding {
    ASTSymbol *sym;
    ASTNode *expr;
};

struct ASTVariadicBinding {
    ASTSymbols syms;
    ASTNode *expr;

    ASTVariadicBinding() : expr(nullptr) {}
};

typedef std::vector<ASTBinding> ASTBindings;

struct LetLike : ASTNode {
    LetLike(ASTKind _kind, const Anchor *anchor, const ASTBindings &bindings, Block *body);

    void append(const ASTBinding &bind);
    void append(ASTSymbol *sym, ASTNode *expr);
    SCOPES_RESULT(void) map(const ASTSymbols &syms, const ASTNodes &nodes);
    bool has_variadic_section() const;
    bool has_assignments() const;

    Block *ensure_body();

    ASTBindings bindings;
    ASTVariadicBinding variadic;
    Block *body;
};

//------------------------------------------------------------------------------

struct Let : LetLike {
    static bool classof(const ASTNode *T);

    Let(const Anchor *anchor, const ASTBindings &bindings, Block *body);
    const Type *get_value_type() const;

    static Let *from(const Anchor *anchor, const ASTBindings &bindings = {}, Block *body = nullptr);

    void move_constants_to_scope(Scope *scope);
    void flatten();
};

//------------------------------------------------------------------------------

struct Loop : LetLike {
    static bool classof(const ASTNode *T);

    Loop(const Anchor *anchor, const ASTBindings &bindings, Block *body);
    const Type *get_value_type() const;

    static Loop *from(const Anchor *anchor, const ASTBindings &bindings = {}, Block *body = nullptr);
};

//------------------------------------------------------------------------------

struct Const : ASTValue {
    static bool classof(const ASTNode *T);

    Const(const Anchor *anchor, Any value);
    const Type *get_value_type() const;

    static Const *from(const Anchor *anchor, Any value);

    Any value;
};

//------------------------------------------------------------------------------

struct Break : ASTNode {
    static bool classof(const ASTNode *T);

    Break(const Anchor *anchor, ASTArgumentList *args);
    const Type *get_value_type() const;

    static Break *from(const Anchor *anchor, ASTArgumentList *args = nullptr);

    ASTArgumentList *ensure_args();

    ASTArgumentList *args;
};

//------------------------------------------------------------------------------

struct Repeat : ASTNode {
    static bool classof(const ASTNode *T);

    Repeat(const Anchor *anchor, ASTArgumentList *args);
    const Type *get_value_type() const;

    static Repeat *from(const Anchor *anchor, ASTArgumentList *args = nullptr);

    ASTArgumentList *ensure_args();

    ASTArgumentList *args;
};

//------------------------------------------------------------------------------

struct Return : ASTNode {
    static bool classof(const ASTNode *T);

    Return(const Anchor *anchor, ASTArgumentList *args);
    const Type *get_value_type() const;

    static Return *from(const Anchor *anchor, ASTArgumentList *args = nullptr);

    ASTArgumentList *ensure_args();

    ASTArgumentList *args;
};

//------------------------------------------------------------------------------

struct SyntaxExtend : ASTNode {
    static bool classof(const ASTNode *T);

    SyntaxExtend(const Anchor *anchor, ASTFunction *func, const List *next, Scope *env);
    const Type *get_value_type() const;

    static SyntaxExtend *from(const Anchor *anchor, ASTFunction *func, const List *next, Scope *env);

    ASTFunction *func;
    const List *next;
    Scope *env;
};

//------------------------------------------------------------------------------

} // namespace scopes

#endif // SCOPES_AST_HPP
