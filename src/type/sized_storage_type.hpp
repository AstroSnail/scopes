/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#ifndef SCOPES_SIZED_STORAGE_HPP
#define SCOPES_SIZED_STORAGE_HPP

#include "../type.hpp"

namespace scopes {

struct CompositeType : Type {
    CompositeType(TypeKind kind);

    size_t size;
    size_t align;
};

//------------------------------------------------------------------------------

struct ArrayLikeType : CompositeType {
    static bool classof(const Type *T);

    ArrayLikeType(TypeKind kind, const Type *_element_type, size_t _count);

    SCOPES_RESULT(void *) getelementptr(void *src, size_t i) const;

    SCOPES_RESULT(const Type *) type_at_index(size_t i) const;

    const Type *element_type;
    size_t count;
    size_t stride;
};

//------------------------------------------------------------------------------

struct TupleLikeType : CompositeType {
    static bool classof(const Type *T);

    TupleLikeType(TypeKind kind, const Types &values);

    bool is_plain() const;

    Types values;
protected:
    bool _is_plain;
};

//------------------------------------------------------------------------------

} // namespace scopes

#endif // SCOPES_SIZED_STORAGE_HPP