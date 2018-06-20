/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#ifndef SCOPES_FRAME_HPP
#define SCOPES_FRAME_HPP

#include "argument.hpp"

#include <stddef.h>

#include <unordered_map>

namespace scopes {

struct Label;

struct Frame {
    Frame();
    Frame(Frame *_parent, Label *_label, Label *_instance = nullptr, size_t _loop_count = 0);

    Args args;
    Frame *parent;
    Label *label;
    size_t loop_count;
    bool inline_merge;

    Frame *find_parent_frame(Label *label);

    static Frame *from(Frame *parent, Label *label, Label *instance, size_t loop_count);

    bool all_args_constant() const;

    struct ArgsKey {
        Label *label;
        scopes::Args args;

        ArgsKey();

        bool operator==(const ArgsKey &other) const;

        struct Hash {
            std::size_t operator()(const ArgsKey& s) const;
        };

    };

    Frame *find_frame(const ArgsKey &key) const;

    Frame *find_any_frame(Label *label, ArgsKey &key) const;

    void insert_frame(const ArgsKey &key, Frame *frame);

    Label *get_instance() const;

    static Frame *root;
protected:
    std::unordered_map<ArgsKey, Frame *, ArgsKey::Hash> frames;
    Label *instance;
};

//------------------------------------------------------------------------------

} // namespace scopes

#endif // SCOPES_FRAME_HPP