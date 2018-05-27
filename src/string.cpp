/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "string.hpp"
#include "gc.hpp"
#include "utils.hpp"
#include "hash.hpp"

#define STB_SPRINTF_DECORATE(name) stb_##name
#define STB_SPRINTF_NOUNALIGNED
#include "stb_sprintf.h"

#include <assert.h>
#include <memory.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wvla-extension"

namespace scopes {

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

int unescape_string(char *buf) {
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

#define B_SNFORMAT 512 // how many characters per callback
typedef char *(*vsformatcb_t)(const char *buf, void *user, int len);

struct vsformat_cb_ctx {
    int count;
    char *dest;
    char tmp[B_SNFORMAT];
};

static char *vsformat_cb(const char *buf, void *user, int len) {
    vsformat_cb_ctx *ctx = (vsformat_cb_ctx *)user;
    if (buf != ctx->dest) {
        char *d = ctx->dest;
        char *e = d + len;
        while (d != e) {
            *d++ = *buf++;
        }
    }
    ctx->dest += len;
    return ctx->tmp;
}

static char *vsformat_cb_null(const char *buf, void *user, int len) {
    vsformat_cb_ctx *ctx = (vsformat_cb_ctx *)user;
    ctx->count += len;
    return ctx->tmp;
}

static int escapestrcb(vsformatcb_t cb, void *user, char *buf,
    const char *str, int strcount,
    const char *quote_chars = nullptr) {
    assert(buf);
    const char *fmt_start = str;
    const char *fmt = fmt_start;
    char *p = buf;
#define VSFCB_CHECKWRITE(N) \
    if (((p - buf) + (N)) > B_SNFORMAT) { buf = p = cb(buf, user, p - buf); }
#define VSFCB_PRINT(MAXCOUNT, FMT, SRC) { \
        VSFCB_CHECKWRITE(MAXCOUNT+1); \
        p += snprintf(p, B_SNFORMAT - (p - buf), FMT, SRC); }
    for(;;) {
        char c = *fmt;
        switch(c) {
        case '\n': VSFCB_CHECKWRITE(2); *p++ = '\\'; *p++ = 'n'; break;
        case '\r': VSFCB_CHECKWRITE(2); *p++ = '\\'; *p++ = 'r'; break;
        case '\t': VSFCB_CHECKWRITE(2); *p++ = '\\'; *p++ = 't'; break;
        case 0: if ((fmt - fmt_start) == strcount) goto done;
            // otherwise, fall through
        default:
            if ((c < 32) || (c >= 127)) {
                VSFCB_PRINT(4, "\\x%02x", (unsigned char)c);
            } else {
                if ((c == '\\') || (quote_chars && strchr(quote_chars, c))) {
                    VSFCB_CHECKWRITE(1);
                    *p++ = '\\';
                }
                *p++ = c;
            }
            break;
        }
        fmt++;
    }
done:
    VSFCB_CHECKWRITE(B_SNFORMAT); // force flush if non-empty
    return 0;
#undef VSFCB_CHECKWRITE
#undef VSFCB_PRINT
}

int escape_string(char *buf, const char *str, int strcount, const char *quote_chars) {
    vsformat_cb_ctx ctx;
    if (buf) {
        ctx.dest = buf;
        escapestrcb(vsformat_cb, &ctx, ctx.tmp, str, strcount, quote_chars);
        int l = ctx.dest - buf;
        buf[l] = 0;
        return l;
    } else {
        ctx.count = 0;
        escapestrcb(vsformat_cb_null, &ctx, ctx.tmp, str, strcount, quote_chars);
        return ctx.count + 1;
    }
}

//------------------------------------------------------------------------------
// STRING
//------------------------------------------------------------------------------

std::size_t String::Hash::operator()(const String *s) const {
    return hash_bytes(s->data, s->count);
}

//------------------------------------------------------------------------------

bool String::operator ==(const String &other) const {
    if (count == other.count) {
        return !memcmp(data, other.data, count);
    }
    return false;
}

String *String::alloc(size_t count) {
    String *str = (String *)tracked_malloc(
        sizeof(size_t) + sizeof(char) * (count + 1));
    str->count = count;
    return str;
}

const String *String::from(const char *s, size_t count) {
    String *str = (String *)tracked_malloc(
        sizeof(size_t) + sizeof(char) * (count + 1));
    str->count = count;
    memcpy(str->data, s, sizeof(char) * count);
    str->data[count] = 0;
    return str;
}

const String *String::from_cstr(const char *s) {
    return from(s, strlen(s));
}

const String *String::join(const String *a, const String *b) {
    size_t ac = a->count;
    size_t bc = b->count;
    size_t cc = ac + bc;
    String *str = alloc(cc);
    memcpy(str->data, a->data, sizeof(char) * ac);
    memcpy(str->data + ac, b->data, sizeof(char) * bc);
    str->data[cc] = 0;
    return str;
}

const String *String::from_stdstring(const std::string &s) {
    return from(s.c_str(), s.size());
}

StyledStream& String::stream(StyledStream& ost, const char *escape_chars) const {
    auto c = escape_string(nullptr, data, count, escape_chars);
    char deststr[c + 1];
    escape_string(deststr, data, count, escape_chars);
    ost << deststr;
    return ost;
}

const String *String::substr(int64_t i0, int64_t i1) const {
    assert(i1 >= i0);
    return from(data + i0, (size_t)(i1 - i0));
}

StyledStream& operator<<(StyledStream& ost, const String *s) {
    ost << Style_String << "\"";
    s->stream(ost, "\"");
    ost << "\"" << Style_None;
    return ost;
}

//------------------------------------------------------------------------------

StyledString::StyledString() :
    out(_ss) {
}

StyledString::StyledString(StreamStyleFunction ssf) :
    out(_ss, ssf) {
}

StyledString StyledString::plain() {
    return StyledString(stream_plain_style);
}

const String *StyledString::str() const {
    return String::from_stdstring(_ss.str());
}

//------------------------------------------------------------------------------

const String *vformat( const char *fmt, va_list va ) {
    va_list va2;
    va_copy(va2, va);
    size_t size = stb_vsnprintf( nullptr, 0, fmt, va2 );
    va_end(va2);
    String *str = String::alloc(size);
    stb_vsnprintf( str->data, size + 1, fmt, va );
    return str;
}

const String *format( const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    const String *result = vformat(fmt, va);
    va_end(va);
    return result;
}

// computes the levenshtein distance between two strings
size_t distance(const String *_s, const String *_t) {
    const char *s = _s->data;
    const char *t = _t->data;
    const size_t n = _s->count;
    const size_t m = _t->count;
    if (!m) return n;
    if (!n) return m;

    size_t _v0[m + 1];
    size_t _v1[m + 1];

    size_t *v0 = _v0;
    size_t *v1 = _v1;
    for (size_t i = 0; i <= m; ++i) {
        v0[i] = i;
    }

    for (size_t i = 0; i < n; ++i) {
        v1[0] = i + 1;

        for (size_t j = 0; j < m; ++j) {
            size_t cost = (s[i] == t[j])?0:1;
            v1[j + 1] = std::min(v1[j] + 1,
                std::min(v0[j + 1] + 1, v0[j] + cost));
        }

        size_t *tmp = v0;
        v0 = v1;
        v1 = tmp;
    }

    //std::cout << "lev(" << s << ", " << t << ") = " << v0[m] << std::endl;

    return v0[m];
}

} // namespace scopes
