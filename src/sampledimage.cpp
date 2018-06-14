/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "sampledimage.hpp"
#include "image.hpp"
#include "dyn_cast.inc"
#include "hash.hpp"

#include <unordered_set>

namespace scopes {

namespace SampledImageSet {
    struct Hash {
        std::size_t operator()(const SampledImageType *s) const {
            return std::hash<const ImageType *>{}(s->type);
        }
    };

    struct KeyEqual {
        bool operator()( const SampledImageType *lhs, const SampledImageType *rhs ) const {
            return lhs->type == rhs->type;
        }
    };
} // namespace SampledImageSet

static std::unordered_set<const SampledImageType *, SampledImageSet::Hash, SampledImageSet::KeyEqual> sampled_images;

//------------------------------------------------------------------------------
// SAMPLED IMAGE TYPE
//------------------------------------------------------------------------------

bool SampledImageType::classof(const Type *T) {
    return T->kind() == TK_SampledImage;
}

SampledImageType::SampledImageType(const ImageType *_type) :
    Type(TK_SampledImage), type(_type) {
    auto ss = StyledString::plain();
    ss.out << "<SampledImage " <<  _type->name()->data << ">";
    _name = ss.str();
}

const Type *SampledImage(const ImageType *_type) {
    SCOPES_TYPE_KEY(SampledImageType, key);
    key->type = _type;
    auto it = sampled_images.find(key);
    if (it != sampled_images.end())
        return *it;
    auto result = new SampledImageType(_type);
    sampled_images.insert(result);
    return result;
}


} // namespace scopes
