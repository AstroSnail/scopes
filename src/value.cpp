
/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "value.hpp"
#include "error.hpp"
#include "scope.hpp"
#include "types.hpp"
#include "stream_ast.hpp"
#include "dyn_cast.inc"
#include "qualifier.inc"
#include "qualifiers.hpp"
#include "hash.hpp"

#include <assert.h>
#include <unordered_set>

namespace scopes {

const char *get_value_kind_name(ValueKind kind) {
    switch(kind) {
#define T(NAME, BNAME, CLASS) \
    case NAME: return BNAME;
SCOPES_VALUE_KIND()
#undef T
    default: return "???";
    }
}

const char *get_value_class_name(ValueKind kind) {
    switch(kind) {
#define T(NAME, BNAME, CLASS) \
    case NAME: return #CLASS;
SCOPES_VALUE_KIND()
#undef T
    default: return "???";
    }
}

#define T(CLASS) \
    bool CLASS::has_def_anchor() const { \
        return _def_anchor != nullptr; \
    } \
    const Anchor *CLASS::def_anchor() const { \
        return _def_anchor; \
    } \
    void CLASS::set_def_anchor(const Anchor *anchor) { \
        _def_anchor = anchor; \
    } \


SCOPES_DEFINED_VALUES()
#undef T

//------------------------------------------------------------------------------

std::size_t ValueIndex::Hash::operator()(const ValueIndex & s) const {
    return hash2(std::hash<Value *>{}(s.value), s.index);
}

ValueIndex::ValueIndex(TypedValue *_value, int _index)
    : value(_value), index(_index) {
repeat:
    switch(value->kind()) {
    case VK_Keyed: {
        value = cast<Keyed>(value)->value;
        goto repeat;
    } break;
    case VK_ArgumentList: {
        auto al = cast<ArgumentList>(value);
        if (index < al->values.size()) {
            value = al->values[index];
            index = 0;
            goto repeat;
        } else {
            assert(false);
        }
    } break;
    case VK_ExtractArgument: {
        auto ea = cast<ExtractArgument>(value);
        if (index == 0) {
            value = ea->value;
            index = ea->index;
            goto repeat;
        } else {
            assert(false);
        }
    } break;
    default: break;
    }
}

bool ValueIndex::operator ==(const ValueIndex &other) const {
    return value == other.value && index == other.index;
}

const Type *ValueIndex::get_type() const {
    return get_argument(value->get_type(), index);
}

//------------------------------------------------------------------------------

KeyedTemplate::KeyedTemplate(Symbol _key, const ValueRef &node)
    : UntypedValue(VK_KeyedTemplate), key(_key), value(node)
{
}

ValueRef KeyedTemplate::from(Symbol key, ValueRef node) {
    assert(node);
    if (node.isa<TypedValue>()) {
        return Keyed::from(anchor, key, node.cast<TypedValue>());
    } else {
        if (node.isa<KeyedTemplate>()) {
            node = node.cast<KeyedTemplate>()->value;
        }
        return ref(node.anchor(), new KeyedTemplate(anchor, key, node));
    }
}

//------------------------------------------------------------------------------

Keyed::Keyed(const Type *type, Symbol _key, const TypedValueRef &node)
    : TypedValue(VK_Keyed, type), key(_key), value(node)
{
}

TypedValueRef Keyed::from(Symbol key, TypedValueRef node) {
    assert(node);
    if (isa<Keyed>(node)) {
        node = node.cast<Keyed>()->value;
    }
    auto T = node->get_type();
    auto NT = key_type(key, T);
    if (T == NT)
        return node;
    if (node.isa<Pure>()) {
        return PureCast::from(anchor, NT, cast<Pure>(node));
    } else {
        return ref(node.anchor(), new Keyed(anchor, NT, key, node));
    }
}

//------------------------------------------------------------------------------

ArgumentListTemplate::ArgumentListTemplate(const Anchor *anchor, const Values &_values)
    : UntypedValue(VK_ArgumentListTemplate, anchor), values(_values) {
}

void ArgumentListTemplate::append(Value *node) {
    values.push_back(node);
}

void ArgumentListTemplate::append(Symbol key, Value *node) {
    assert(false); // todo: store key
    values.push_back(node);
}

bool ArgumentListTemplate::is_constant() const {
    for (auto val : values) {
        assert(val);
        if (!isa<Const>(val))
            return false;
    }
    return true;
}

Value *ArgumentListTemplate::empty_from(const Anchor *anchor) {
    return new ArgumentListTemplate(anchor, {});
}

Value *ArgumentListTemplate::from(const Anchor *anchor, const Values &values) {
    if (values.size() == 1) {
        return values[0];
    }
    for (auto value : values) {
        if (isa<UntypedValue>(value)) {
            return new ArgumentListTemplate(anchor, values);
        }
    }
    // all values are typed - promote to ArgumentList
    TypedValues typed_values;
    typed_values.reserve(values.size());
    for (auto value : values) {
        typed_values.push_back(cast<TypedValue>(value));
    }
    return ArgumentList::from(anchor, typed_values);
}

//------------------------------------------------------------------------------

const Type *arguments_type_from_typed_values(const TypedValues &_values) {
    Types types;
    for (auto value : _values) {
        types.push_back(value->get_type());
    }
    return arguments_type(types);
}

ArgumentList::ArgumentList(const Anchor *anchor, const TypedValues &_values)
    : TypedValue(VK_ArgumentList, anchor, arguments_type_from_typed_values(_values)),
        values(_values) {
}

bool ArgumentList::is_constant() const {
    for (auto val : values) {
        assert(val);
        if (!isa<Const>(val))
            return false;
    }
    return true;
}

TypedValue *ArgumentList::from(const Anchor *anchor, const TypedValues &values) {
    if (values.size() == 1) {
        return values[0];
    }
    return new ArgumentList(anchor, values);
}

//------------------------------------------------------------------------------

ExtractArgumentTemplate::ExtractArgumentTemplate(Value *_value, int _index, bool _vararg)
    : UntypedValue(VK_ExtractArgumentTemplate), index(_index), value(_value), vararg(_vararg) {
    assert(index >= 0);
}

ValueRef ExtractArgumentTemplate::from(
    const ValueRef &value, int index, bool vararg) {
    if (value.isa<TypedValue>()) {
        if (vararg) {
            return ExtractArgument::variadic_from(value.cast<TypedValue>(), index);
        } else {
            return ExtractArgument::from(value.cast<TypedValue>(), index);
        }
    } else {
        return ref(value.anchor(),
            new ExtractArgumentTemplate(anchor, value, index, vararg));
    }
}

//------------------------------------------------------------------------------

ExtractArgument::ExtractArgument(const Type *type, const TypedValueRef &_value, int _index)
    : TypedValue(VK_ExtractArgument, anchor, type),
        index(_index), value(_value) {
}

TypedValueRef ExtractArgument::variadic_from(const TypedValueRef &value, int index) {
    if (!index) return value;
    auto T = value->get_type();
    if (!is_returning(T)) return value;
    int count = get_argument_count(T);
    TypedValues values;
    if (isa<ArgumentList>(value)) {
        auto al = cast<ArgumentList>(value);
        for (int i = index; i < count; ++i) {
            values.push_back(al->values[i]);
        }
    } else {
        for (int i = index; i < count; ++i) {
            auto argT = get_argument(T, i);
            values.push_back(from(value, i));
        }
    }
    return ref(value.anchor(), ArgumentList::from(values));
}

TypedValueRef ExtractArgument::from(const TypedValueRef &value, int index) {
    auto T = value->get_type();
    if (!is_returning(T)) return value;
    int count = get_argument_count(T);
    if (index >= count)
        return ref(value.anchor(), ConstAggregate::none_from());
    if (isa<ArgumentList>(value)) {
        auto al = cast<ArgumentList>(value);
        assert (index < al->values.size());
        return al->values[index];
    } else if ((count == 1) && (index == 0)) {
        return value;
    } else {
        auto argT = get_argument(T, index);
        return ref(value.anchor(),
            new ExtractArgument(anchor, argT, value, index));
    }
}

//------------------------------------------------------------------------------

Template::Template(const Anchor *anchor, Symbol _name, const ParameterTemplates &_params, Value *_value)
    : UntypedValue(VK_Template, anchor),
        name(_name), params(_params), value(_value),
        _is_inline(false), docstring(nullptr),
        recursion(0) {
    int index = 0;
    for (auto param : params) {
        param->set_owner(this, index++);
    }
}

bool Template::is_forward_decl() const {
    return !value;
}

void Template::set_inline() {
    _is_inline = true;
}

bool Template::is_inline() const {
    return _is_inline;
}

void Template::append_param(ParameterTemplate *sym) {
    sym->set_owner(this, params.size());
    params.push_back(sym);
}

Template *Template::from(
    const Anchor *anchor, Symbol name,
    const ParameterTemplates &params, Value *value) {
    return new Template(anchor, name, params, value);
}

//------------------------------------------------------------------------------

Function::UniqueInfo::UniqueInfo(const ValueIndex& _value)
    : value(_value) {
}

int Function::UniqueInfo::get_depth() const {
    if (isa<Instruction>(value.value)) {
        auto instr = cast<Instruction>(value.value);
        const Block *block = instr->block;
        assert(block);
        return block->depth;
    } else if (isa<LoopLabelArguments>(value.value)) {
        auto lla = cast<LoopLabelArguments>(value.value);
        assert(lla->loop);
        return lla->loop->body.depth;
    } else {
        return 0;
    }
}

Function::Function(const Anchor *anchor, Symbol _name, const Parameters &_params)
    : Pure(VK_Function, anchor, TYPE_Unknown),
        name(_name), params(_params),
        docstring(nullptr),
        frame(nullptr), boundary(nullptr), original(nullptr), label(nullptr),
        complete(false),
        nextid(FirstUniquePrivate) {
    body.depth = 1;
    int index = 0;
    for (auto param : params) {
        param->set_owner(this, index++);
    }
}

const Function::UniqueInfo &Function::get_unique_info(int id) const {
    assert(id != 0);
    assert(id < nextid);
    auto it = uniques.find(id);
    assert(it != uniques.end());
    return it->second;
}

int Function::unique_id() {
    return nextid++;
}

void Function::try_bind_unique(TypedValue *value) {
    auto T = value->get_type();
    int count = get_argument_count(T);
    for (int i = 0; i < count; ++i) {
        auto argT = get_argument(T, i);
        if (is_unique(argT)) {
            bind_unique(UniqueInfo(ValueIndex(value, i)));
        }
    }
}

void Function::bind_unique(const UniqueInfo &info) {
    auto uq = get_unique(info.value.get_type());
    auto result = uniques.insert({uq->id, info});
    if (!result.second) {
        StyledStream ss;
        ss << "internal error: duplicate unique " << uq->id;
        ss << " (was " << result.first->second.value;
        ss << ", is " << info.value << ")" << std::endl;
    }
    assert(result.second);
}

bool Function::key_equal(const Function *other) const {
    return this == other;
}

std::size_t Function::hash() const {
    return std::hash<const Function *>{}(this);
}

bool Function::is_typed() const {
    assert(_type);
    return _type != TYPE_Unknown;
}

void Function::set_type(const Type *type) {
    assert(!is_typed());
    assert(type);
    _type = type;
}

void Function::change_type(const Type *type) {
    assert(is_typed());
    assert(type);
    _type = type;
}

void Function::build_valids() {
    assert(valid.empty());
    // add uniques
    for (auto sym : params) {
        auto T = sym->get_type();
        int count = get_argument_count(T);
        for (int i = 0; i < count; ++i) {
            auto argT = get_argument(T, i);
            auto uq = try_unique(argT);
            if (uq) {
                assert(uq->id);
                bind_unique(UniqueInfo(ValueIndex(sym, i)));
                valid.insert(uq->id);
            }
        }
    }
    // add views
    for (auto sym : params) {
        auto T = sym->get_type();
        int count = get_argument_count(T);
        for (int i = 0; i < count; ++i) {
            auto argT = get_argument(T, i);
            auto vq = try_view(argT);
            if (vq) {
                for (auto id : vq->ids) {
                    assert(id);
                    if (!valid.count(id)) {
                        auto result = uniques.insert({id,
                            UniqueInfo(ValueIndex(sym, i))});
                        assert(result.second);
                        valid.insert(id);
                    }
                }
            }
        }
    }
    original_valid = valid;
}

void Function::append_param(Parameter *sym) {
    sym->set_owner(this, params.size());
    params.push_back(sym);
}

TypedValue *Function::resolve_local(Value *node) const {
    auto it = map.find(node);
    if (it == map.end())
        return nullptr;
    return it->second;
}

TypedValue *Function::unsafe_resolve(Value *node) const {
    auto fn = this;
    while (fn) {
        auto val = fn->resolve_local(node);
        if (val) return val;
        fn = fn->frame;
    }
    return nullptr;
}

SCOPES_RESULT(TypedValue *) Function::resolve(Value *node, Function *_boundary) const {
    SCOPES_RESULT_TYPE(TypedValue *)
    auto fn = this;
    while (fn) {
        auto val = fn->resolve_local(node);
        if (val) {
            if ((fn->boundary != _boundary) && !val->is_accessible()) {
                SCOPES_EXPECT_ERROR(
                    error_value_inaccessible_from_closure(val, _boundary));
            }
            return val;
        }
        fn = fn->frame;
    }
    return nullptr;
}

void Function::bind(Value *oldnode, TypedValue *newnode) {
    auto it = map.insert({oldnode, newnode});
    if (!it.second) {
        it.first->second = newnode;
    }
}

Function *Function::from(
    const Anchor *anchor, Symbol name,
    const Parameters &params) {
    return new Function(anchor, name, params);
}

//------------------------------------------------------------------------------

static const Type *pointer_for_global_type(const Type *type, size_t flags, Symbol storage_class) {
    size_t ptrflags = required_flags_for_storage_class(storage_class);
    if (flags & GF_NonWritable)
        ptrflags |= PTF_NonWritable;
    else if (flags & GF_NonReadable)
        ptrflags |= PTF_NonReadable;
    return pointer_type(type, ptrflags, storage_class);
}

Global::Global(const Anchor *anchor, const Type *type, Symbol _name, size_t _flags, Symbol _storage_class, int _location, int _binding)
    : Pure(VK_Global, anchor, pointer_for_global_type(type, _flags, _storage_class)),
        element_type(type), name(_name), flags(_flags), storage_class(_storage_class), location(_location),
        binding(_binding) {
}

bool Global::key_equal(const Global *other) const {
    return this == other;
}

std::size_t Global::hash() const {
    return std::hash<const Global *>{}(this);
}

Global *Global::from(const Anchor *anchor, const Type *type, Symbol name, size_t flags, Symbol storage_class, int location, int binding) {
    if ((storage_class == SYM_SPIRV_StorageClassUniform)
        && !(flags & GF_BufferBlock)) {
        flags |= GF_Block;
    }
    return new Global(anchor, type, name, flags, storage_class, location, binding);
}

//------------------------------------------------------------------------------

PureCast::PureCast(const Anchor *anchor, const Type *type, Pure *_value)
    : Pure(VK_PureCast, anchor, type), value(_value) {}

bool PureCast::key_equal(const PureCast *other) const {
    return get_type() == other->get_type()
        && value == other->value;
}

std::size_t PureCast::hash() const {
    return value->hash();
}

PureRef PureCast::from(const Anchor *anchor, const Type *type, PureRef value) {
    if (value.isa<PureCast>()) {
        value = value.cast<PureCast>()->value;
    }
    if (value->get_type() == type)
        return value;
    return ref(value.anchor(), new PureCast(anchor, type, value));
}

//------------------------------------------------------------------------------

Block::Block()
    : depth(-1), insert_index(0), terminator(nullptr)
{}

bool Block::is_valid(const ValueIndex &value) const {
    auto T = value.get_type();
    auto vq = try_view(T);
    if (vq) {
        return is_valid(vq->ids);
    }
    auto uq = try_unique(T);
    if (uq) {
        return is_valid(uq->id);
    }
    return true;
}

bool Block::is_valid(const IDSet &ids) const {
    for (auto id : ids) {
        if (!valid.count(id))
            return false;
    }
    return true;
}

bool Block::is_valid(int id) const {
    return valid.count(id);
}

void Block::move(int id) {
    assert(valid.count(id));
    valid.erase(id);
}

bool Block::is_terminated() const {
    return terminator != nullptr;
}

void Block::set_parent(Block *_parent) {
    assert(depth < 0);
    assert(_parent);
    depth = _parent->depth + 1;
    // copy valids from parent
    if (valid.empty()) {
        valid = _parent->valid;
    } else {
        // block has some preconfigured values
        valid = union_idset(valid, _parent->valid);
    }
}

void Block::clear() {
    body.clear();
    valid.clear();
    terminator = nullptr;
}

void Block::migrate_from(Block &source) {
    for (auto arg : source.body) {
        arg->block = nullptr;
        append(arg);
    }
    if (source.terminator) {
        terminator = source.terminator;
        terminator->block = this;
    }
    // equivalent to removing all values no longer valid, and adding
    // all values that have since been added
    valid = source.valid;
    source.clear();
}

bool Block::empty() const {
    return body.empty() && !terminator;
}

void Block::insert_at(int index) {
    assert((index >= 0) && (index <= body.size()));
    insert_index = index;
}

void Block::insert_at_end() {
    insert_index = body.size();
}

int Block::append(TypedValue *node) {
    // ensure that whatever an argument list or extract argument is pointing at
    // is definitely inserted into a block
    switch(node->kind()) {
    case VK_ArgumentList: {
        auto al = cast<ArgumentList>(node);
        int count = al->values.size();
        int inserted = 0;
        for (int i = 0; i < count; ++i) {
            inserted += append(al->values[i]);
        }
    } break;
    case VK_ExtractArgument: {
        auto ea = cast<ExtractArgument>(node);
        return append(ea->value);
    } break;
    default: {
        if (isa<Instruction>(node)) {
            auto instr = cast<Instruction>(node);
            if (instr->block)
                return 0;
            instr->block = this;
            if (!is_returning(instr->get_type())) {
                assert(!terminator);
                assert(insert_index == body.size());
                terminator = instr;
            } else {
                auto T = instr->get_type();
                auto count = get_argument_count(T);
                for (int i = 0; i < count; ++i) {
                    auto uq = try_unique(get_argument(T, i));
                    if (uq) {
                        assert(uq->id);
                        valid.insert(uq->id);
                    }
                }
                body.insert(body.begin() + insert_index, instr);
                insert_index++;
            }
            return 1;
        }
    } break;
    }
    return 0;
}

//------------------------------------------------------------------------------

Expression::Expression(const Values &_body, const ValueRef &_value)
    : UntypedValue(VK_Expression, anchor), body(_body), value(_value), scoped(true) {
}

void Expression::append(const ValueRef &node) {
    assert(node);
    if (value && !value.isa<Pure>()) {
        body.push_back(value);
    }
    value = node;
}

Expression *Expression::scoped_from(const Values &nodes, const ValueRef &value) {
    return new Expression(nodes, value);
}

Expression *Expression::unscoped_from(const Values &nodes, const ValueRef &value) {
    auto expr = new Expression(nodes, value);
    expr->scoped = false;
    return expr;
}

//------------------------------------------------------------------------------

CondBr::CondBr(const Anchor *anchor, TypedValue *_cond)
    : Instruction(VK_CondBr, anchor, TYPE_NoReturn), cond(_cond)
{}

CondBr *CondBr::from(const Anchor *anchor, TypedValue *cond) {
    return new CondBr(anchor, cond);
}

//------------------------------------------------------------------------------

bool If::Clause::is_then() const {
    return cond != nullptr;
}

If::If(const Anchor *anchor, const Clauses &_clauses)
    : UntypedValue(VK_If, anchor), clauses(_clauses) {
}

If *If::from(const Anchor *anchor, const Clauses &_clauses) {
    return new If(anchor, _clauses);
}

void If::append_then(const Anchor *anchor, Value *cond, Value *value) {
    assert(anchor);
    assert(cond);
    assert(value);
    Clause clause;
    clause.anchor = anchor;
    clause.cond = cond;
    clause.value = value;
    clauses.push_back(clause);
}

void If::append_else(const Anchor *anchor, Value *value) {
    assert(anchor);
    assert(value);
    Clause clause;
    clause.anchor = anchor;
    clause.value = value;
    clauses.push_back(clause);
}

//------------------------------------------------------------------------------

SwitchTemplate::SwitchTemplate(const Anchor *anchor, Value *_expr, const Cases &_cases)
    : UntypedValue(VK_SwitchTemplate, anchor), expr(_expr), cases(_cases)
{}

SwitchTemplate *SwitchTemplate::from(const Anchor *anchor, Value *expr, const Cases &cases) {
    return new SwitchTemplate(anchor, expr, cases);
}

void SwitchTemplate::append_case(const Anchor *anchor, Value *literal, Value *value) {
    assert(anchor);
    assert(literal);
    assert(value);
    Case _case;
    _case.kind = CK_Case;
    _case.anchor = anchor;
    _case.literal = literal;
    _case.value = value;
    cases.push_back(_case);
}

void SwitchTemplate::append_pass(const Anchor *anchor, Value *literal, Value *value) {
    assert(anchor);
    assert(literal);
    assert(value);
    Case _case;
    _case.kind = CK_Pass;
    _case.anchor = anchor;
    _case.literal = literal;
    _case.value = value;
    cases.push_back(_case);
}

void SwitchTemplate::append_default(const Anchor *anchor, Value *value) {
    assert(anchor);
    assert(value);
    Case _case;
    _case.kind = CK_Default;
    _case.anchor = anchor;
    _case.value = value;
    cases.push_back(_case);
}

//------------------------------------------------------------------------------

Switch::Switch(const Anchor *anchor, TypedValue *_expr, const Cases &_cases)
    : Instruction(VK_Switch, anchor, TYPE_NoReturn), expr(_expr), cases(_cases)
{}

Switch *Switch::from(const Anchor *anchor, TypedValue *expr, const Cases &cases) {
    return new Switch(anchor, expr, cases);
}

Switch::Case &Switch::append_pass(const Anchor *anchor, ConstInt *literal) {
    assert(anchor);
    assert(literal);
    Case *_case = new Case();
    _case->kind = CK_Pass;
    _case->anchor = anchor;
    _case->literal = literal;
    cases.push_back(_case);
    return *cases.back();
}

Switch::Case &Switch::append_default(const Anchor *anchor) {
    assert(anchor);
    Case *_case = new Case();
    _case->kind = CK_Default;
    _case->anchor = anchor;
    cases.push_back(_case);
    return *cases.back();
}

//------------------------------------------------------------------------------

ParameterTemplate::ParameterTemplate(const Anchor *anchor, Symbol _name, bool _variadic)
    : UntypedValue(VK_ParameterTemplate, anchor), name(_name), variadic(_variadic),
        owner(nullptr), index(-1) {
}

ParameterTemplate *ParameterTemplate::from(const Anchor *anchor, Symbol name) {
    return new ParameterTemplate(anchor, name, false);
}

ParameterTemplate *ParameterTemplate::variadic_from(const Anchor *anchor, Symbol name) {
    return new ParameterTemplate(anchor, name, true);
}

bool ParameterTemplate::is_variadic() const {
    return variadic;
}

void ParameterTemplate::set_owner(Template *_owner, int _index) {
    assert(!owner);
    owner = _owner;
    index = _index;
}

//------------------------------------------------------------------------------

Parameter::Parameter(const Anchor *anchor, Symbol _name, const Type *_type)
    : TypedValue(VK_Parameter, anchor, _type), name(_name),
        owner(nullptr), block(nullptr), index(-1) {
}

Parameter *Parameter::from(const Anchor *anchor, Symbol name, const Type *type) {
    return new Parameter(anchor, name, type);
}

void Parameter::set_owner(Function *_owner, int _index) {
    assert(!owner);
    owner = _owner;
    index = _index;
}

void Parameter::retype(const Type *T) {
    _type = T;
}

//------------------------------------------------------------------------------

LoopArguments::LoopArguments(const Anchor *anchor, Loop *_loop)
    : UntypedValue(VK_LoopArguments, anchor), loop(_loop) {}

LoopArguments *LoopArguments::from(const Anchor *anchor, Loop *loop) {
    return new LoopArguments(anchor, loop);
}

//------------------------------------------------------------------------------

LoopLabelArguments::LoopLabelArguments(const Anchor *anchor, const Type *type)
    : TypedValue(VK_LoopLabelArguments, anchor, type), loop(nullptr) {}

LoopLabelArguments *LoopLabelArguments::from(const Anchor *anchor, const Type *type) {
    return new LoopLabelArguments(anchor, type);
}

//------------------------------------------------------------------------------

Exception::Exception(const Anchor *anchor, const Type *type)
    : TypedValue(VK_Exception, anchor, type) {
}

Exception *Exception::from(const Anchor *anchor, const Type *type) {
    return new Exception(anchor, type);
}

//------------------------------------------------------------------------------

CallTemplate::CallTemplate(const Anchor *anchor, Value *_callee, const Values &_args)
    : UntypedValue(VK_CallTemplate, anchor), callee(_callee), args(_args), flags(0) {
}

bool CallTemplate::is_rawcall() const {
    return flags & CF_RawCall;
}

void CallTemplate::set_rawcall() {
    flags |= CF_RawCall;
}

CallTemplate *CallTemplate::from(const Anchor *anchor, Value *callee, const Values &args) {
    return new CallTemplate(anchor, callee, args);
}

//------------------------------------------------------------------------------

Call::Call(const Anchor *anchor, const Type *type, TypedValue *_callee, const TypedValues &_args)
    : Instruction(VK_Call, anchor, type), callee(_callee), args(_args),
        except(nullptr) {
}

Call *Call::from(const Anchor *anchor, const Type *type, TypedValue *callee, const TypedValues &args) {
    return new Call(anchor, type, callee, args);
}

//------------------------------------------------------------------------------

LoopLabel::LoopLabel(const Anchor *anchor, const TypedValues &_init, LoopLabelArguments *_args)
    : Instruction(VK_LoopLabel, anchor, TYPE_NoReturn), init(_init), args(_args) {
    assert(args);
    args->loop = this;
}

LoopLabel *LoopLabel::from(const Anchor *anchor, const TypedValues &init, LoopLabelArguments *args) {
    return new LoopLabel(anchor, init, args);
}

//------------------------------------------------------------------------------

Loop::Loop(const Anchor *anchor, Value *_init, Value *_value)
    : UntypedValue(VK_Loop, anchor), init(_init), value(_value) {
    args = LoopArguments::from(anchor, this);
}

Loop *Loop::from(const Anchor *anchor, Value *init, Value *value) {
    return new Loop(anchor, init, value);
}

//------------------------------------------------------------------------------

Label::Label(const Anchor *anchor, LabelKind _kind, Symbol _name)
    : Instruction(VK_Label, anchor, empty_arguments_type()), name(_name), label_kind(_kind) {}

Label *Label::from(const Anchor *anchor, LabelKind kind, Symbol name) {
    return new Label(anchor, kind, name);
}

void Label::change_type(const Type *type) {
    assert(type);
    _type = type;
}

const char *get_label_kind_name(LabelKind kind) {
    switch(kind) {
    #define T(NAME, BNAME) \
        case NAME: return BNAME;
    SCOPES_LABEL_KIND()
    #undef T
    default: return "???";
    }
}

//------------------------------------------------------------------------------

LabelTemplate::LabelTemplate(const Anchor *anchor, LabelKind _kind, Symbol _name, Value *_value)
    : UntypedValue(VK_LabelTemplate, anchor), name(_name), value(_value), label_kind(_kind) {}

LabelTemplate *LabelTemplate::from(const Anchor *anchor, LabelKind kind, Symbol name, Value *value) {
    return new LabelTemplate(anchor, kind, name, value);
}

LabelTemplate *LabelTemplate::try_from(const Anchor *anchor,
    Value *value) {
    return new LabelTemplate(anchor, LK_Try, KW_Try, value);
}
LabelTemplate *LabelTemplate::except_from(const Anchor *anchor,
    Value *value) {
    return new LabelTemplate(anchor, LK_Except, KW_Except, value);
}

//------------------------------------------------------------------------------

bool Pure::classof(const Value *T) {
    switch(T->kind()) {
#define T(NAME, BNAME, CLASS) \
    case NAME:
SCOPES_PURE_VALUE_KIND()
#undef T
        return true;
    default: return false;
    }
}

Pure::Pure(ValueKind _kind, const Anchor *anchor, const Type *type)
    : TypedValue(_kind, anchor, type) {
}

bool Pure::key_equal(const Pure *other) const {
    if (kind() != other->kind())
        return false;
    switch(kind()) {
    #define T(KIND, NAME, CLASS) \
        case KIND: \
            return cast<CLASS>(this)->key_equal(cast<CLASS>(other));
    SCOPES_PURE_VALUE_KIND()
    #undef T
    default: assert(false); return false;
    }
}

std::size_t Pure::hash() const {
    switch(kind()) {
    #define T(KIND, NAME, CLASS) \
        case KIND: \
            return cast<CLASS>(this)->hash();
    SCOPES_PURE_VALUE_KIND()
    #undef T
    default: assert(false); return 0;
    }
}

//------------------------------------------------------------------------------

bool Const::classof(const Value *T) {
    switch(T->kind()) {
#define T(NAME, BNAME, CLASS) \
    case NAME:
SCOPES_CONST_VALUE_KIND()
#undef T
        return true;
    default: return false;
    }
}

Const::Const(ValueKind _kind, const Anchor *anchor, const Type *type)
    : Pure(_kind, anchor, type) {
}

//------------------------------------------------------------------------------

ConstInt::ConstInt(const Anchor *anchor, const Type *type, uint64_t _value)
    : Const(VK_ConstInt, anchor, type), value(_value) {
}

bool ConstInt::key_equal(const ConstInt *other) const {
    return get_type() == other->get_type()
        && value == other->value;
}

std::size_t ConstInt::hash() const {
    return std::hash<uint64_t>{}(value);
}

ConstInt *ConstInt::from(const Anchor *anchor, const Type *type, uint64_t value) {
    return new ConstInt(anchor, type, value);
}

ConstInt *ConstInt::symbol_from(const Anchor *anchor, Symbol value) {
    return new ConstInt(anchor, TYPE_Symbol, value.value());
}

ConstInt *ConstInt::builtin_from(const Anchor *anchor, Builtin value) {
    return new ConstInt(anchor, TYPE_Builtin, value.value());
}

//------------------------------------------------------------------------------

ConstReal::ConstReal(const Anchor *anchor, const Type *type, double _value)
    : Const(VK_ConstReal, anchor, type), value(_value) {}

bool ConstReal::key_equal(const ConstReal *other) const {
    return get_type() == other->get_type()
        && value == other->value;
}

std::size_t ConstReal::hash() const {
    return std::hash<double>{}(value);
}

ConstReal *ConstReal::from(const Anchor *anchor, const Type *type, double value) {
    return new ConstReal(anchor, type, value);
}

//------------------------------------------------------------------------------

ConstAggregate::ConstAggregate(const Anchor *anchor, const Type *type, const Constants &_fields)
    : Const(VK_ConstAggregate, anchor, type), values(_fields) {
}

bool ConstAggregate::key_equal(const ConstAggregate *other) const {
    if (get_type() != other->get_type())
        return false;
    for (int i = 0; i < values.size(); ++i) {
        auto a = values[i];
        auto b = other->values[i];
        if (!a->key_equal(b))
            return false;
    }
    return true;
}

std::size_t ConstAggregate::hash() const {
    uint64_t h = std::hash<const Type *>{}(get_type());
    for (int i = 0; i < values.size(); ++i) {
        h = hash2(h, values[i]->hash());
    }
    return h;
}

ConstAggregate *ConstAggregate::from(const Anchor *anchor, const Type *type, const Constants &fields) {
    return new ConstAggregate(anchor, type, fields);
}

ConstAggregate *ConstAggregate::none_from(const Anchor *anchor) {
    return from(anchor, TYPE_Nothing, {});
}

//------------------------------------------------------------------------------

ConstPointer::ConstPointer(const Anchor *anchor, const Type *type, const void *_pointer)
    : Const(VK_ConstPointer, anchor, type), value(_pointer) {}

bool ConstPointer::key_equal(const ConstPointer *other) const {
    if (get_type() != other->get_type())
        return false;
    if (get_type() == TYPE_List)
        return sc_list_compare((const List *)value, (const List *)other->value);
    return value == other->value;
}

std::size_t ConstPointer::hash() const {
    return std::hash<const void *>{}(value);
}

ConstPointer *ConstPointer::from(const Anchor *anchor, const Type *type, const void *pointer) {
    return new ConstPointer(anchor, type, pointer);
}

ConstPointer *ConstPointer::type_from(const Anchor *anchor, const Type *type) {
    return from(anchor, TYPE_Type, type);
}

ConstPointer *ConstPointer::closure_from(const Anchor *anchor, const Closure *closure) {
    return from(anchor, TYPE_Closure, closure);
}

ConstPointer *ConstPointer::string_from(const Anchor *anchor, const String *str) {
    return from(anchor, TYPE_String, str);
}

ConstPointer *ConstPointer::ast_from(const Anchor *anchor, Value *node) {
    return from(anchor, TYPE_Value, node);
}

ConstPointer *ConstPointer::list_from(const Anchor *anchor, const List *list) {
    return from(anchor, TYPE_List, list);
}

ConstPointer *ConstPointer::scope_from(const Anchor *anchor, Scope *scope) {
    return from(anchor, TYPE_Scope, scope);
}

ConstPointer *ConstPointer::anchor_from(const Anchor *anchor) {
    return from(anchor, TYPE_Anchor, anchor);
}

//------------------------------------------------------------------------------

Break::Break(const Anchor *anchor, Value *_value)
    : UntypedValue(VK_Break, anchor), value(_value) {
}

Break *Break::from(const Anchor *anchor, Value *value) {
    return new Break(anchor, value);
}

//------------------------------------------------------------------------------

RepeatTemplate::RepeatTemplate(const Anchor *anchor, Value *_value)
    : UntypedValue(VK_RepeatTemplate, anchor), value(_value) {}

RepeatTemplate *RepeatTemplate::from(const Anchor *anchor, Value *value) {
    return new RepeatTemplate(anchor, value);
}

//------------------------------------------------------------------------------

Repeat::Repeat(const Anchor *anchor, LoopLabel *_loop, const TypedValues &values)
    : Terminator(VK_Repeat, anchor, values), loop(_loop) {
}

Repeat *Repeat::from(const Anchor *anchor, LoopLabel *loop, const TypedValues &values) {
    return new Repeat(anchor, loop, values);
}

//------------------------------------------------------------------------------

ReturnTemplate::ReturnTemplate(const Anchor *anchor, Value *_value)
    : UntypedValue(VK_ReturnTemplate, anchor), value(_value) {}

ReturnTemplate *ReturnTemplate::from(const Anchor *anchor, Value *value) {
    return new ReturnTemplate(anchor, value);
}

//------------------------------------------------------------------------------

Return::Return(const Anchor *anchor, const TypedValues &values)
    : Terminator(VK_Return, anchor, values) {
}

Return *Return::from(const Anchor *anchor, const TypedValues &values) {
    return new Return(anchor, values);
}

//------------------------------------------------------------------------------

Merge::Merge(const Anchor *anchor, Label *_label, const TypedValues &values)
    : Terminator(VK_Merge, anchor, values), label(_label) {
}

Merge *Merge::from(const Anchor *anchor, Label *label, const TypedValues &values) {
    return new Merge(anchor, label, values);
}

//------------------------------------------------------------------------------

MergeTemplate::MergeTemplate(const Anchor *anchor, LabelTemplate *_label, Value *_value)
    : UntypedValue(VK_MergeTemplate, anchor), label(_label), value(_value) {}

MergeTemplate *MergeTemplate::from(const Anchor *anchor, LabelTemplate *label, Value *value) {
    return new MergeTemplate(anchor, label, value);
}

//------------------------------------------------------------------------------

RaiseTemplate::RaiseTemplate(const Anchor *anchor, Value *_value)
    : UntypedValue(VK_RaiseTemplate, anchor), value(_value) {}

RaiseTemplate *RaiseTemplate::from(const Anchor *anchor, Value *value) {
    return new RaiseTemplate(anchor, value);
}

//------------------------------------------------------------------------------

Raise::Raise(const Anchor *anchor, const TypedValues &values)
    : Terminator(VK_Raise, anchor, values) {
}

Raise *Raise::from(const Anchor *anchor, const TypedValues &values) {
    return new Raise(anchor, values);
}

//------------------------------------------------------------------------------

Quote::Quote(const Anchor *anchor, Value *_value)
    : UntypedValue(VK_Quote, anchor), value(_value) {
}

Quote *Quote::from(const Anchor *anchor, Value *value) {
    return new Quote(anchor, value);
}

//------------------------------------------------------------------------------

Unquote::Unquote(const Anchor *anchor, Value *_value)
    : UntypedValue(VK_Unquote, anchor), value(_value) {
}

Unquote *Unquote::from(const Anchor *anchor, Value *value) {
    return new Unquote(anchor, value);
}

//------------------------------------------------------------------------------

CompileStage::CompileStage(const Anchor *anchor, const List *_next, Scope *_env)
    : UntypedValue(VK_CompileStage, anchor), next(_next), env(_env) {
}

CompileStage *CompileStage::from(const Anchor *anchor, const List *next, Scope *env) {
    return new CompileStage(anchor, next, env);
}

//------------------------------------------------------------------------------

ValueKind Value::kind() const { return _kind; }

Value::Value(ValueKind kind)
    : _kind(kind),_origin(nullptr) {
}

bool Value::has_origin() const {
    return _origin != nullptr;
}

const Value *Value::origin() const {
    assert(_origin);
    return _origin;
}

void Value::set_origin(Value *origin) {
    assert(origin);
    _origin = origin;
}

bool Value::is_accessible() const {
    switch(kind()) {
    case VK_ArgumentList: {
        auto al = cast<ArgumentList>(this);
        for (auto val : al->values) {
            assert(val);
            if (!val->is_accessible())
                return false;
        }
        return true;
    } break;
    default: break;
    }
    return isa<Pure>(this);
}

int Value::get_depth() const {
    const Value *value = this;
    switch(value->kind()) {
    case VK_Parameter: {
        auto param = cast<Parameter>(value);
        assert(param->owner);
        if (param->block) {
            return param->block->depth;
        } if (isa<Function>(param->owner)) {
            // outside of function
            return 0;
        } else if (isa<Instruction>(param->owner)) {
            auto instr = cast<Instruction>(param->owner);
            if (!instr->block) {
                StyledStream ss;
                stream_ast(ss, instr, StreamASTFormat());
            }
            assert(instr->block);
            return instr->block->depth;
        } else {
            assert(false);
            return 0;
        }
    } break;
    case VK_LoopLabelArguments: {
        auto lla = cast<LoopLabelArguments>(value);
        assert(lla->loop);
        return lla->loop->body.depth;
    } break;
    default: {
        if (isa<Instruction>(value)) {
            //if (value->is_pure_in_function())
            //    return 0;
            auto instr = cast<Instruction>(value);
            assert(instr->block);
            return instr->block->depth;
        }
    } break;
    }
    return 0;
}

#define T(NAME, BNAME, CLASS) \
    bool CLASS::classof(const Value *T) { \
        return T->kind() == NAME; \
    }
SCOPES_VALUE_KIND()
#undef T

//------------------------------------------------------------------------------

UntypedValue::UntypedValue(ValueKind _kind, const Anchor *_anchor)
    : Value(_kind, _anchor)
{}

bool UntypedValue::classof(const Value *T) {
    switch(T->kind()) {
#define T(NAME, BNAME, CLASS) \
    case NAME:
SCOPES_UNTYPED_VALUE_KIND()
#undef T
        return true;
    default: return false;
    }
}

//------------------------------------------------------------------------------

bool TypedValue::classof(const Value *T) {
    switch(T->kind()) {
#define T(NAME, BNAME, CLASS) \
    case NAME: return true;
SCOPES_TYPED_VALUE_KIND()
#undef T
    default: return false;
    }
}

TypedValue::TypedValue(ValueKind _kind, const Anchor *_anchor, const Type *type)
    : Value(_kind, _anchor), _type(type) {
    assert(_type);
}

const Type *TypedValue::get_type() const {
    if (!_type) {
        StyledStream ss;
        ss << get_value_class_name(kind()) << std::endl;
    }
    assert(_type);
    return _type;
}

//------------------------------------------------------------------------------

Instruction::Instruction(ValueKind _kind, const Anchor *_anchor, const Type *type)
    : TypedValue(_kind, _anchor, type), name(SYM_Unnamed), block(nullptr) {
}

bool Instruction::classof(const Value *T) {
    switch(T->kind()) {
#define T(NAME, BNAME, CLASS) \
    case NAME:
SCOPES_INSTRUCTION_VALUE_KIND()
#undef T
        return true;
    default: return false;
    }
}

//------------------------------------------------------------------------------

bool Terminator::classof(const Value *T) {
    switch(T->kind()) {
#define T(NAME, BNAME, CLASS) \
    case NAME:
SCOPES_TERMINATOR_VALUE_KIND()
#undef T
        return true;
    default: return false;
    }
}

Terminator::Terminator(ValueKind _kind, const Anchor *_anchor, const TypedValues &_values)
    : Instruction(_kind, _anchor, TYPE_NoReturn), values(_values)
{}

//------------------------------------------------------------------------------

std::unordered_map<Closure, const Closure *, Closure::Hash> Closure::map;

Closure::Closure(Template *_func, Function *_frame) :
    Const(VK_Closure, TYPE_Closure), func(_func), frame(_frame) {}

std::size_t Closure::Hash::operator()(const Closure &k) const {
    return hash2(
        std::hash<Template *>{}(k.func),
        std::hash<Function *>{}(k.frame));
}

bool Closure::operator ==(const Closure &k) const {
    return (func == k.func)
        && (frame == k.frame);
}

const Closure *Closure::from(Template *func, Function *frame) {
    Closure cl(func, frame);
    auto it = map.find(cl);
    if (it != map.end()) {
        return it->second;
    }
    const Closure *result = new Closure(func, frame);
    map.insert({cl, result});
    return result;
}

StyledStream &Closure::stream(StyledStream &ost) const {
    ost << Style_Comment << "<" << Style_None;
    if (frame)
        ost << Style_Symbol << frame->name.name()->data << "λ" << (void *)frame << Style_None;
    ost << Style_Comment << "::" << Style_None
        << Style_Symbol << func->name.name()->data << "λ" << (void *)func << Style_None
        << Style_Comment << ">" << Style_None;
    return ost;
}

//------------------------------------------------------------------------------

StyledStream& operator<<(StyledStream& ss, const Closure *closure) {
    closure->stream(ss);
    return ss;
}

StyledStream& operator<<(StyledStream& ost, Value *node) {
    ost << const_cast<const Value *>(node);
    return ost;
}

StyledStream& operator<<(StyledStream& ost, const Value *node) {
    ost << Style_Keyword << get_value_class_name(node->kind()) << Style_None;
    ost << "$";
    stream_address(ost, node);
    if (isa<TypedValue>(node)) {
        ost << Style_Operator << ":" << Style_None << cast<TypedValue>(node)->get_type();
    }
    return ost;
}

StyledStream& operator<<(StyledStream& ost, TypedValue *node) {
    ost << (Value *)node;
    return ost;
}

StyledStream& operator<<(StyledStream& ost, const TypedValue *node) {
    ost << (const Value *)node;
    return ost;
}

StyledStream& operator<<(StyledStream& ost, const ValueIndex &arg) {
    ost << arg.value;
    if (get_argument_count(arg.value->get_type()) != 1) {
        ost << Style_Operator << "@" << Style_None << arg.index;
        ost << Style_Operator << ":" << Style_None << arg.get_type();
    }
    return ost;
}

StyledStream& operator<<(StyledStream& ost, ValueIndex &arg) {
    ost << const_cast<const ValueIndex &>(arg);
    return ost;
}

} // namespace scopes
