/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#ifndef SCOPES_SYMBOL_HPP
#define SCOPES_SYMBOL_HPP

#include "symbol_enum.hpp"

#include <cstddef>
#include <vector>
#include <string>

namespace scopes {

struct StyledStream;

//------------------------------------------------------------------------------
// SYMBOL
//------------------------------------------------------------------------------

const char SYMBOL_ESCAPE_CHARS[] = " []{}()\"";

//------------------------------------------------------------------------------
// SYMBOL TYPE
//------------------------------------------------------------------------------

struct Symbol {
    typedef KnownSymbol EnumT;

    struct Hash {
        std::size_t operator()(const scopes::Symbol & s) const;
    };

protected:
    static void verify_unmapped(Symbol id, const std::string &name);

    static void map_symbol(Symbol id, const std::string &name);

    static void map_known_symbol(Symbol id, const std::string &name);

    static Symbol get_symbol(const std::string &name);

    static const std::string &get_symbol_name(Symbol id);

    uint64_t _value;

    Symbol(uint64_t tid);

public:
    static Symbol wrap(uint64_t value);

    Symbol();

    Symbol(EnumT id);

    template<unsigned N>
    Symbol(const char (&str)[N]) :
        _value(get_symbol(str)._value) {
    }

    Symbol(const std::string &str);

    bool is_known() const;
    EnumT known_value() const;

    // for std::map support
    bool operator < (Symbol b) const;
    bool operator ==(Symbol b) const;
    bool operator !=(Symbol b) const;
    bool operator ==(EnumT b) const;
    bool operator !=(EnumT b) const;

    std::size_t hash() const;
    uint64_t value() const;

    const std::string &name() const;

    static void _init_symbols();
    static size_t symbol_count();

    StyledStream& stream(StyledStream& ost) const;

};

bool ends_with_parenthesis(Symbol sym);

typedef std::vector<Symbol> Symbols;

} // namespace scopes

#endif // SCOPES_SYMBOL_HPP
