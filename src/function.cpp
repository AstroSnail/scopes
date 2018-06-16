/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "function.hpp"
#include "return.hpp"
#include "tuple.hpp"
#include "pointer.hpp"
#include "extern.hpp"
#include "error.hpp"
#include "dyn_cast.inc"
#include "hash.hpp"

#include <assert.h>

namespace scopes {

//------------------------------------------------------------------------------
// FUNCTION TYPE
//------------------------------------------------------------------------------

bool FunctionType::classof(const Type *T) {
    return T->kind() == TK_Function;
}

void FunctionType::stream_name(StyledStream &ss) const {
    if (divergent()) {
        ss << "?<-";
    } else {
        stream_type_name(ss, return_type);
        ss << "<-";
    }
    ss << "(";
    for (size_t i = 0; i < argument_types.size(); ++i) {
        if (i > 0) {
            ss << " ";
        }
        stream_type_name(ss, argument_types[i]);
    }
    if (vararg()) {
        ss << " ...";
    }
    ss << ")";
}

FunctionType::FunctionType(
    const Type *_return_type, const ArgTypes &_argument_types, uint32_t _flags) :
    Type(TK_Function),
    return_type(_return_type),
    argument_types(_argument_types),
    flags(_flags) {

    assert(!(flags & FF_Divergent) || argument_types.empty());
}

bool FunctionType::vararg() const {
    return flags & FF_Variadic;
}
bool FunctionType::divergent() const {
    return flags & FF_Divergent;
}

SCOPES_RESULT(const Type *) FunctionType::type_at_index(size_t i) const {
    SCOPES_RESULT_TYPE(const Type *);
    SCOPES_CHECK_RESULT(verify_range(i, argument_types.size() + 1));
    if (i == 0)
        return return_type;
    else
        return argument_types[i - 1];
}

//------------------------------------------------------------------------------

const Type *Function(const Type *return_type,
    const ArgTypes &argument_types, uint32_t flags) {

    struct TypeArgs {
        const Type *return_type;
        ArgTypes argtypes;
        uint32_t flags;

        TypeArgs() {}
        TypeArgs(const Type *_return_type,
            const ArgTypes &_argument_types,
            uint32_t _flags = 0) :
            return_type(_return_type),
            argtypes(_argument_types),
            flags(_flags)
        {}

        bool operator==(const TypeArgs &other) const {
            if (return_type != other.return_type) return false;
            if (flags != other.flags) return false;
            if (argtypes.size() != other.argtypes.size()) return false;
            for (size_t i = 0; i < argtypes.size(); ++i) {
                if (argtypes[i] != other.argtypes[i])
                    return false;
            }
            return true;
        }

        struct Hash {
            std::size_t operator()(const TypeArgs& s) const {
                std::size_t h = std::hash<const Type *>{}(s.return_type);
                h = hash2(h, std::hash<uint32_t>{}(s.flags));
                for (auto arg : s.argtypes) {
                    h = hash2(h, std::hash<const Type *>{}(arg));
                }
                return h;
            }
        };
    };

    typedef std::unordered_map<TypeArgs, FunctionType *, typename TypeArgs::Hash> ArgMap;

    static ArgMap map;

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

    TypeArgs ta(return_type, argument_types, flags);
    typename ArgMap::iterator it = map.find(ta);
    if (it == map.end()) {
        FunctionType *t = new FunctionType(return_type, argument_types, flags);
        map.insert({ta, t});
        return t;
    } else {
        return it->second;
    }
}

//------------------------------------------------------------------------------

bool is_function_pointer(const Type *type) {
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

const FunctionType *extract_function_type(const Type *T) {
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

SCOPES_RESULT(void) verify_function_pointer(const Type *type) {
    SCOPES_RESULT_TYPE(void);
    if (!is_function_pointer(type)) {
        StyledString ss;
        ss.out << "function pointer expected, got " << type;
        SCOPES_LOCATION_ERROR(ss.str());
    }
    return true;
}

} // namespace scopes
