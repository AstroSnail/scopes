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

#ifndef SCOPES_UTILS_HPP
#define SCOPES_UTILS_HPP

#include <stddef.h>
#include <stdio.h>

#include <functional>
#include <map>

inline size_t align(size_t offset, size_t align) {
    return (offset + align - 1) & ~(align - 1);
}

int stb_fprintf(FILE *out, const char *fmt, ...);

namespace scopes {

template <typename R, typename... Args>
inline std::function<R (Args...)> memoize(R (*fn)(Args...)) {
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

}

#endif // SCOPES_UTILS_HPP