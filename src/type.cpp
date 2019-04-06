/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "types.hpp"
#include "error.hpp"
#include "gc.hpp"
#include "anchor.hpp"
#include "list.hpp"
#include "hash.hpp"
#include "type/qualify_type.hpp"
#include "dyn_cast.inc"
#include "qualifier.inc"

#include <memory.h>
#include <algorithm>

namespace scopes {

#define T(NAME, BNAME, CLASS) \
    bool CLASS::classof(const Type *T) { \
        return T->kind() == NAME; \
    }
B_TYPE_KIND()
#undef T

//------------------------------------------------------------------------------

TypeKind Type::kind() const { return _kind; } // for this codebase

Type::Type(TypeKind kind) : _kind(kind) {}

StyledStream& Type::stream(StyledStream& ost) const {
    StyledString ss = StyledString::plain();
    stream_type_name(ss.out, this);
    ost << Style_Type;
    ost << ss.str()->data;
    ost << Style_None;
    return ost;
}

void Type::bind(Symbol name, const ValueRef &value) {
    auto ret = symbols.insert({ name, value });
    if (!ret.second) {
        ret.first->second = value;
    }
}

void Type::del(Symbol name) {
    auto it = symbols.find(name);
    if (it != symbols.end()) {
        symbols.erase(it);
    }
}

bool Type::lookup(Symbol name, ValueRef &dest) const {
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

bool Type::lookup_local(Symbol name, ValueRef &dest) const {
    auto it = symbols.find(name);
    if (it != symbols.end()) {
        dest = it->second;
        return true;
    }
    return false;
}

bool Type::lookup_call_handler(ValueRef &dest) const {
    return lookup(SYM_CallHandler, dest);
}

bool Type::lookup_return_handler(ValueRef &dest) const {
    return lookup(SYM_ReturnHandler, dest);
}

const Type::Map &Type::get_symbols() const {
    return symbols;
}

std::vector<Symbol> Type::find_closest_match(Symbol name) const {
    const String *s = name.name();
    std::unordered_set<Symbol, Symbol::Hash> done;
    std::vector<Symbol> best_syms;
    size_t best_dist = (size_t)-1;
    const Type *self = this;
    do {
        auto &&map = self->symbols;
        for (auto &&k : map) {
            Symbol sym = k.first;
            if (done.count(sym))
                continue;
            size_t dist = distance(s, sym.name());
            if (dist == best_dist) {
                best_syms.push_back(sym);
            } else if (dist < best_dist) {
                best_dist = dist;
                best_syms = { sym };
            }
            done.insert(sym);
        }
        if (self == TYPE_Typename)
            break;
        self = superof(self);
    } while (true);
    std::sort(best_syms.begin(), best_syms.end());
    return best_syms;
}

//------------------------------------------------------------------------------

StyledStream& operator<<(StyledStream& ost, const Type *type) {
    if (!type) {
        ost << Style_Error;
        ost << "<null type>";
        ost << Style_None;
        return ost;
    } else {
        return type->stream(ost);
    }
}

//------------------------------------------------------------------------------

#define T(TYPE, TYPENAME) \
    const Type *TYPE = nullptr;
B_TYPES()
#undef T

//------------------------------------------------------------------------------
// TYPE INQUIRIES
//------------------------------------------------------------------------------

void stream_type_name(StyledStream &ss, const Type *T) {
    switch (T->kind()) {
#define T(TYPE, TYPENAME, CLASS) \
    case TYPE: cast<CLASS>(T)->stream_name(ss); break;
B_TYPE_KIND()
#undef T
        default:
            ss << "???"; break;
    }
}

bool all_plain(const Types &types) {
    for (auto type : types) {
        if (!is_plain(type))
            return false;
    }
    return true;
}

bool is_plain(const Type *T) {
    switch(T->kind()) {
    case TK_Qualify:
        return is_plain(cast<QualifyType>(T)->type);
    case TK_Pointer:
        //return is_plain(cast<PointerType>(T)->element_type);
    case TK_Integer:
    case TK_Real:
    case TK_Image:
    case TK_SampledImage:
        return true;
    case TK_Array:
    case TK_Vector:
        return is_plain(cast<ArrayLikeType>(T)->element_type);
    case TK_Tuple:
    case TK_Union:
        return cast<TupleLikeType>(T)->is_plain();
    case TK_Arguments:
        return all_plain(cast<ArgumentsType>(T)->values);
    case TK_Typename:
        return cast<TypenameType>(T)->is_plain();
    case TK_Function:
        return false;
    }
    return false;
}

TypeKind storage_kind(const Type *T) {
    if (is_opaque(T))
        return T->kind();
    return storage_type(T).assert_ok()->kind();
}

bool is_opaque(const Type *T) {
    switch(T->kind()) {
    case TK_Qualify:
        return is_opaque(cast<QualifyType>(T)->type);
    case TK_Typename: {
        const TypenameType *tt = cast<TypenameType>(T);
        if (!tt->finalized()) {
            return true;
        } else {
            return is_opaque(tt->storage_type);
        }
    } break;
    case TK_Arguments:
    case TK_Image:
    case TK_SampledImage:
    case TK_Function: return true;
    default: break;
    }
    return false;
}

SCOPES_RESULT(size_t) size_of(const Type *T) {
    SCOPES_RESULT_TYPE(size_t);
    switch(T->kind()) {
    case TK_Qualify:
        return size_of(cast<QualifyType>(T)->type);
    case TK_Integer: {
        const IntegerType *it = cast<IntegerType>(T);
        return (it->width + 7) / 8;
    }
    case TK_Real: {
        const RealType *rt = cast<RealType>(T);
        return (rt->width + 7) / 8;
    }
    case TK_Pointer: return PointerType::size();
    case TK_Array: return cast<ArrayType>(T)->size;
    case TK_Vector: return cast<VectorType>(T)->size;
    case TK_Tuple: return cast<TupleType>(T)->size;
    case TK_Union: return cast<UnionType>(T)->size;
    case TK_Typename: return size_of(SCOPES_GET_RESULT(storage_type(T)));
    default: break;
    }

    SCOPES_ERROR(OpaqueType, T);
}

SCOPES_RESULT(size_t) align_of(const Type *T) {
    SCOPES_RESULT_TYPE(size_t);
    switch(T->kind()) {
    case TK_Qualify:
        return align_of(cast<QualifyType>(T)->type);
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
    case TK_Pointer: return PointerType::size();
    case TK_Array: return cast<ArrayType>(T)->align;
    case TK_Vector: return cast<VectorType>(T)->align;
    case TK_Tuple: return cast<TupleType>(T)->align;
    case TK_Union: return cast<UnionType>(T)->align;
    case TK_Typename: return align_of(SCOPES_GET_RESULT(storage_type(T)));
    default: break;
    }

    SCOPES_ERROR(OpaqueType, T);
}

const Type *superof(const Type *T) {
    switch(T->kind()) {
    case TK_Qualify: return TYPE_Qualify;
    case TK_Arguments: return TYPE_Arguments;
    case TK_Integer: return TYPE_Integer;
    case TK_Real: return TYPE_Real;
    case TK_Pointer: return TYPE_Pointer;
    case TK_Array: return TYPE_Array;
    case TK_Vector: return TYPE_Vector;
    case TK_Tuple: return TYPE_Tuple;
    case TK_Union: return TYPE_Union;
    case TK_Typename: return cast<TypenameType>(T)->super();
    case TK_Function: return TYPE_Function;
    case TK_Image: return TYPE_Image;
    case TK_SampledImage: return TYPE_SampledImage;
    }
    assert(false && "unhandled type kind; corrupt pointer?");
    return nullptr;
}

bool is_returning(const Type *T) {
    return (T != TYPE_NoReturn);
}

bool is_returning_value(const Type *T) {
    return is_returning(T) && (T != empty_arguments_type());
}

SCOPES_RESULT(bool) types_compatible(const Type *paramT, const Type *argT) {
    SCOPES_RESULT_TYPE(bool);
    if (paramT == argT)
        return true;
    if (!is_opaque(argT)) {
        argT = SCOPES_GET_RESULT(storage_type(argT));
    }
    if (!is_opaque(paramT)) {
        paramT = SCOPES_GET_RESULT(storage_type(paramT));
    }
    if (isa<PointerType>(paramT) && isa<PointerType>(argT)) {
        auto pa = cast<PointerType>(argT);
        auto pb = cast<PointerType>(paramT);
        auto scls = pb->storage_class;
        if (scls == SYM_Unnamed) {
            scls = pa->storage_class;
        }
        if (SCOPES_GET_RESULT(types_compatible(pb->element_type, pa->element_type))
            && pointer_flags_compatible(pb->flags, pa->flags)
            && pointer_storage_classes_compatible(pb->storage_class, pa->storage_class))
            return true;
    }
    return false;
}

//------------------------------------------------------------------------------
// TYPE CHECK PREDICATES
//------------------------------------------------------------------------------

SCOPES_RESULT(void) verify(const Type *typea, const Type *typeb) {
    SCOPES_RESULT_TYPE(void);
    if (strip_lifetime(typea) != strip_lifetime(typeb)) {
        SCOPES_ERROR(ParameterTypeMismatch, typea, typeb);
    }
    return {};
}

SCOPES_RESULT(void) verify_integer(const Type *type) {
    SCOPES_RESULT_TYPE(void);
    if (type->kind() != TK_Integer) {
        SCOPES_ERROR(ParameterTypeMismatch, TYPE_Integer, type);
    }
    return {};
}

SCOPES_RESULT(void) verify_real(const Type *type) {
    SCOPES_RESULT_TYPE(void);
    if (type->kind() != TK_Real) {
        SCOPES_ERROR(ParameterTypeMismatch, TYPE_Real, type);
    }
    return {};
}

SCOPES_RESULT(void) verify_range(size_t idx, size_t count) {
    SCOPES_RESULT_TYPE(void);
    if (idx >= count) {
        SCOPES_ERROR(IndexOutOfRange, idx, count);
    }
    return {};
}

//------------------------------------------------------------------------------

#define DEFINE_TYPENAME(NAME, T) \
    T = typename_type(String::from(NAME));

#define DEFINE_BASIC_TYPE(NAME, CT, T, BODY) { \
        T = typename_type(String::from(NAME)); \
        auto tn = cast<TypenameType>(const_cast<Type *>(T)); \
        tn->finalize(BODY, TNF_Plain).assert_ok(); \
        assert(sizeof(CT) == size_of(T).assert_ok()); \
    }

#define DEFINE_OPAQUE_HANDLE_TYPE(NAME, CT, T) { \
        T = typename_type(String::from(NAME)); \
        auto tn = cast<TypenameType>(const_cast<Type *>(T)); \
        tn->finalize(native_ro_pointer_type(typename_type(String::from("_" NAME))), TNF_Plain).assert_ok(); \
    }

void init_types() {
    DEFINE_TYPENAME("typename", TYPE_Typename);

    DEFINE_TYPENAME("Nothing", TYPE_Nothing);
    DEFINE_TYPENAME("noreturn", TYPE_NoReturn);

    DEFINE_TYPENAME("Sampler", TYPE_Sampler);

    DEFINE_TYPENAME("integer", TYPE_Integer);
    DEFINE_TYPENAME("real", TYPE_Real);
    DEFINE_TYPENAME("pointer", TYPE_Pointer);
    DEFINE_TYPENAME("array", TYPE_Array);
    DEFINE_TYPENAME("vector", TYPE_Vector);
    DEFINE_TYPENAME("tuple", TYPE_Tuple);
    DEFINE_TYPENAME("union", TYPE_Union);
    DEFINE_TYPENAME("Qualify", TYPE_Qualify);
    DEFINE_TYPENAME("Arguments", TYPE_Arguments);
    DEFINE_TYPENAME("Raises", TYPE_Raises);
    DEFINE_TYPENAME("constant", TYPE_Constant);
    DEFINE_TYPENAME("function", TYPE_Function);
    DEFINE_TYPENAME("Image", TYPE_Image);
    DEFINE_TYPENAME("SampledImage", TYPE_SampledImage);
    DEFINE_TYPENAME("CStruct", TYPE_CStruct);
    DEFINE_TYPENAME("CUnion", TYPE_CUnion);
    DEFINE_TYPENAME("CEnum", TYPE_CEnum);

    TYPE_Bool = integer_type(1, false);

    TYPE_I8 = integer_type(8, true);
    TYPE_I16 = integer_type(16, true);
    TYPE_I32 = integer_type(32, true);
    TYPE_I64 = integer_type(64, true);

    TYPE_U8 = integer_type(8, false);
    TYPE_U16 = integer_type(16, false);
    TYPE_U32 = integer_type(32, false);
    TYPE_U64 = integer_type(64, false);

    TYPE_F16 = real_type(16);
    TYPE_F32 = real_type(32);
    TYPE_F64 = real_type(64);
    TYPE_F80 = real_type(80);

    DEFINE_BASIC_TYPE("usize", size_t, TYPE_USize, TYPE_U64);

    TYPE_Type = typename_type(String::from("type"));
    TYPE_Unknown = typename_type(String::from("Unknown"));
    TYPE_Variadic = typename_type(String::from("..."));
    const Type *_TypePtr = native_ro_pointer_type(typename_type(String::from("_type")));
    cast<TypenameType>(const_cast<Type *>(TYPE_Type))->finalize(_TypePtr, TNF_Plain).assert_ok();
    cast<TypenameType>(const_cast<Type *>(TYPE_Unknown))->finalize(_TypePtr, TNF_Plain).assert_ok();

    cast<TypenameType>(const_cast<Type *>(TYPE_Nothing))->finalize(tuple_type({}).assert_ok(), TNF_Plain).assert_ok();

    DEFINE_BASIC_TYPE("Symbol", Symbol, TYPE_Symbol, TYPE_U64);
    DEFINE_BASIC_TYPE("Builtin", Builtin, TYPE_Builtin, TYPE_U64);

    DEFINE_OPAQUE_HANDLE_TYPE("_Value", Value, TYPE__Value);

    DEFINE_OPAQUE_HANDLE_TYPE("SourceFile", SourceFile, TYPE_SourceFile);
    DEFINE_OPAQUE_HANDLE_TYPE("Closure", Closure, TYPE_Closure);
    DEFINE_OPAQUE_HANDLE_TYPE("Scope", Scope, TYPE_Scope);
    DEFINE_OPAQUE_HANDLE_TYPE("String", String, TYPE_String);
    DEFINE_OPAQUE_HANDLE_TYPE("List", List, TYPE_List);
    DEFINE_OPAQUE_HANDLE_TYPE("Error", Error, TYPE_Error);

    DEFINE_OPAQUE_HANDLE_TYPE("Anchor", Anchor, TYPE_Anchor);

    DEFINE_TYPENAME("Value", TYPE_ValueRef);
    {
        cast<TypenameType>(const_cast<Type *>(TYPE_ValueRef))
            ->finalize(
                tuple_type({
                    TYPE__Value,
                    TYPE_Anchor
                }).assert_ok(),
                TNF_Plain).assert_ok();
    }

    DEFINE_BASIC_TYPE("CompileStage", ValueRef, TYPE_CompileStage,
        storage_type(TYPE_ValueRef).assert_ok());


    DEFINE_TYPENAME("SpiceMacro", TYPE_ASTMacro);
    {
        cast<TypenameType>(const_cast<Type *>(TYPE_ASTMacro))
            ->finalize(
                native_ro_pointer_type(
                    raising_function_type(
                        TYPE_ValueRef, { TYPE_ValueRef })
                ),
                TNF_Plain).assert_ok();
    }

#define T(TYPE, TYPENAME) \
    assert(TYPE);
    B_TYPES()
#undef T
}

#undef DEFINE_TYPENAME
#undef DEFINE_BASIC_TYPE
#undef DEFINE_OPAQUE_HANDLE_TYPE

} // namespace scopes

