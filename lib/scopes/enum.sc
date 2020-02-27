#
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.

""""enum
    ====

    Support for defining tagged unions and classical enums through the `enum`
    sugar.

# typechecking-time function
fn _extract-payload (enum-value extractT)
    extractT as:= type
    # don't bother if it's a unit tag
    if (extractT == Nothing)
        return (spice-quote none)

    let qcls = ('qualified-typeof enum-value)
    let cls = ('strip-qualifiers qcls)
    let payload-cls = ('element@ cls 1)
    let raw-payload = `(extractvalue enum-value 1)
    let refer = ('refer? qcls)
    let ptrT = ('refer->pointer-type qcls)

    let extracted =
        if refer
            let ptrET = ('change-element-type ptrT extractT)
            spice-quote
                let ptr = (reftoptr raw-payload)
                let ptr = (bitcast ptr ptrET)
                ptrtoref ptr
        else
            let ptrET = ('change-storage-class
                ('mutable (pointer.type extractT)) 'Function)
            spice-quote
                let ptr = (alloca payload-cls)
                store raw-payload ptr
                let ptr = (bitcast ptr ptrET)
                load ptr
    let ET = ('strip-qualifiers extractT)
    let extracted =
        if (ET < tuple)
            `(unpack extracted)
        else extracted

# tagged union / sum type
typedef Enum
    spice __dispatch (self handlers...)
        let qcls = ('qualified-typeof self)
        let cls = ('strip-qualifiers qcls)
        let fields = (('@ cls '__fields) as type)
        let field-types = ('@ cls '__fields__)
        let field-type-args = ('args field-types)
        let tag = `(extractvalue self 0)
        let sw = (sc_switch_new tag)
        for arg in ('args handlers...)
            let anchor = ('anchor arg)
            let key arg = ('dekey arg)
            if (key == unnamed)
                sc_switch_append_default sw `(arg)
            else
                let i = (sc_type_field_index fields key)
                let ET = ('key-type ('element@ fields i) unnamed)
                let field = (('getarg field-types i) as type)
                let lit = ('@ field 'Literal)
                let extractT = ('@ field 'Type)
                let payload = (_extract-payload self extractT)
                sc_switch_append_case sw lit ('tag `(arg payload) anchor)
        sw

fn define-field-runtime (T name field-type index-value)
    let fields = ('@ T '__fields__)
    let index = (('@ T '__index__) as u64)
    let index-anchor = ('anchor index-value)
    let index-value =
        if (('typeof index-value) == Nothing) index
        else (extract-integer index-value)
    let next-index = (index-value + 1)
    let FT = (typename.type
        (.. "enum-field<" (name as Symbol as string) ":"
            (tostring (field-type as type)) "=" (tostring index-value) ">")
        typename)
    'set-opaque FT
    'set-symbols FT
        Name = name
        Type = field-type
        Index = index-value
    let FT = `FT
    let fields =
        sc_argument_list_join_values fields FT
    sc_type_set_symbol T '__fields__ fields
    sc_type_set_symbol T '__index__ next-index
    index-value

spice define-field (enum-type name opts...)
    if (not ('constant? enum-type))
        error "enum-type must be constant"
    let T = (enum-type as type)
    let argc = ('argcount opts...)
    let field-type index-value =
        if (argc == 0) (_ `Nothing `none)
        elseif (argc == 1) (_ ('getarg opts... 0) `none)
        else (_ ('getarg opts... 0) ('getarglist opts... 1))
    define-field-runtime T name field-type index-value

fn shabbysort (buf sz)
    for i in (range sz)
        for j in (range (i + 1) sz)
            let a = (load (getelementptr buf i))
            let ak = (extractvalue a 0)
            let b = (load (getelementptr buf j))
            let bk = (extractvalue b 0)
            if (ak > bk)
                # swap
                store b (getelementptr buf i)
                store a (getelementptr buf j)

fn build-repr-switch-case (litT self field-types allow-dupes?)
    let sw = (sc_switch_new self)
    let numfields = ('argcount field-types)
    let sorted = (alloca-array (tuple u64 type) numfields)
    for i in (range numfields)
        let field = (('getarg field-types i) as type)
        let field = (field as type)
        let index = (('@ field 'Index) as u64)
        store (tupleof index field) (getelementptr sorted i)
    # sort array so duplicates are next to each other and merge names
    shabbysort sorted numfields
    loop (i = 0)
        if (i == numfields)
            break;
        let index field = (unpack (load (getelementptr sorted i)))
        let lit = (sc_const_int_new litT index)
        let name = (('@ field 'Name) as Symbol as string)
        # accumulate all fields with the same index
        let i name =
            loop (i name = (i + 1) name)
                if (i == numfields)
                    break i name
                let index2 field2 = (unpack (load (getelementptr sorted i)))
                if (index2 == index)
                    if (not allow-dupes?)
                        error "duplicate tags not permitted for tagged unions"
                    let name2 = (('@ field2 'Name) as Symbol as string)
                    repeat (i + 1) (.. name "|" name2)
                else
                    break i name
        let name =
            sc_default_styler style-number name
        sc_switch_append_case sw lit name
        i
    sc_switch_append_default sw "?invalid?"
    sw

# constructor for unit tags without payload
inline unit-tag-constructor (enum-type index-value payload-type)
    let value = (undef enum-type)
    let value = (insertvalue value index-value 0)
    insertvalue value (nullof payload-type) 1

inline tag-constructor (
    enum-type index-value payload-type field-type field-pointer-type ...)
    let payload = (alloca payload-type)
    let PT = (typeof payload)
    let destptr = (bitcast payload field-pointer-type)
    let payload = (field-type ...)
    store payload destptr
    let payload = (load (bitcast destptr PT))
    let value = (undef enum-type)
    let value = (insertvalue value index-value 0)
    insertvalue value payload 1

fn finalize-enum-runtime (T storage)
    let field-types = ('@ T '__fields__)
    let field-type-args = ('args field-types)
    # figure out integer bit width and signedness for the enumerator
    let width signed =
        fold (width signed = 0 false) for field in field-type-args
            let field = (field as type)
            let index = (('@ field 'Index) as u64 as i64)
            let signed = (signed | (index < 0))
            let iwidth =
                if signed
                    if ((index >= -0x80) and (index < 0x80)) 8
                    elseif ((index >= -0x8000) and (index < 0x8000)) 16
                    elseif ((index >= -0x80000000) and (index < 0x80000000)) 32
                    else 64
                else
                    if (index <= 1) 1
                    if (index <= 0xff) 8
                    elseif (index <= 0xffff) 16
                    elseif (index <= 0xffffffff) 32
                    else 64
            _ (max width iwidth) signed

    inline check-field-redefinition (name finalized-fieldT)
        # if field has already been seen, its "last type" is finalized-fieldT
        # ie. this checks if we defined the same tag twice.
        let previously-definedT =
            try
                'typeof ('@ T (name as Symbol))
            except (ex)
                return;
        if (previously-definedT == finalized-fieldT)
            hide-traceback;
            error (.. "Duplicate enum fields aren't allowed: " (repr (name as Symbol)) ":" (repr T))

    let classic? =
        if (T < CEnum) true
        elseif (T < Enum) false
        else
            error
                .. "type " (repr T) " must have Enum or CEnum supertype"
                    \ " but has supertype " (repr ('superof T))
    local using-scope = (Scope)
    if classic?
        # no storage definition means `plain` was used.
        let storage =
            if (('typeof storage) == Nothing) i32
            else
                if (('typeof storage) == type)
                    storage as type
                else
                    hide-traceback;
                    error (.. "type expected, got " (tostring ('typeof storage)) ".")
        if (not (storage < integer))
            error "enum storage must be of integer type."
        'set-plain-storage T storage
        # enum is integer
        for field in field-type-args
            let field = (field as type)
            let name = (('@ field 'Name) as Symbol)
            check-field-redefinition name T
            let index = (('@ field 'Index) as u64)
            let field-type = (('@ field 'Type) as type)
            if (field-type != Nothing)
                error "plain enums can't have tagged fields"
            let value = (sc_const_int_new T index)
            'set-symbol T name value
            using-scope =
                'bind using-scope name value
        # build repr function
        spice-quote
            inline __repr (self)
                spice-unquote
                    build-repr-switch-case T self field-types true
        'set-symbol T '__repr __repr
    else
        let index-type = (sc_integer_type width signed)
        let numfields = ('argcount field-types)
        inline drop-default ()
        inline... drop-any
        case (arg : Nothing,)
        case (args...)
            va-map __drop args...
            ;
        # build repr function
        spice-quote
            inline __repr (self)
                let val = (extractvalue self 0)
                spice-unquote
                    build-repr-switch-case index-type val field-types false
            inline __drop (self)
                #print "dropping option" self
                '__dispatch self
                    spice-unquote
                        sc_argument_list_map_new (numfields + 1)
                            inline (i)
                                if (i == numfields)
                                    # default field
                                    `drop-default
                                else
                                    let field = (('getarg field-types i) as type)
                                    let name = (('@ field 'Name) as Symbol)
                                    sc_keyed_new name drop-any
        spice-quote
        'set-symbols T
            __repr = __repr
            __drop = __drop
        let fields = (alloca-array type numfields)
        for i in (range numfields)
            let field = (('getarg field-types i) as type)
            let name = (('@ field 'Name) as Symbol)
            let field-type = (('@ field 'Type) as type)
            let field-type = ('key-type field-type name)
            store field-type (getelementptr fields i)
        'set-symbols T
            __fields = (sc_tuple_type numfields fields)
        let payload-type = (sc_union_storage_type numfields fields)
        'set-storage T
            tuple.type index-type payload-type
        let consts = (alloca-array Value 2)
        # build value constructors
        for i _field in (enumerate field-type-args)
            let field = (_field as type)
            let name = (('@ field 'Name) as Symbol)
            check-field-redefinition name type
            let index = (('@ field 'Index) as u64)
            let field-type = (('@ field 'Type) as type)
            let index-value = (sc_const_int_new index-type index)
            'set-symbol field 'Literal index-value
            let constructor =
                if (field-type == Nothing)
                    spice-quote
                        inline constructor (cls)
                            unit-tag-constructor T index-value payload-type
                    sc_template_set_name constructor name
                    constructor
                else
                    let TT = ('change-storage-class
                            ('mutable (pointer.type field-type)) 'Function)
                    spice-quote
                        inline constructor (cls ...)
                            tag-constructor T index-value payload-type field-type TT ...
                    sc_template_set_name constructor name
                    constructor
            'set-symbol field '__typecall constructor
            'set-symbol T name field
            using-scope =
                'bind using-scope name field
    'set-symbol T '__using (deref using-scope)
    ;

spice finalize-enum (T storage)
    if ('constant? T)
        finalize-enum-runtime (T as type) storage
        `()
    else
        `(finalize-enum-runtime T storage)

sugar enum (name body...)

    let supertype body has-supertype? storage =
        sugar-match body...
        case ('plain rest...)
            _ `CEnum rest... true `none
        case (': storage rest...)
            _ `CEnum rest... true `storage
        case ('< supertype rest...)
            _ supertype rest... true `none
        default
            _ `Enum body... false `none

    let has-fwd-decl =
        if (('typeof name) == Symbol)
            if (empty? body)
                # forward declaration
                return
                    qq [typedef] [name] < [supertype] do

            let symname = (name as Symbol)
            # see if we can find a forward declaration in the local scope
            try ('@ sugar-scope symname) true
            except (err) false
        else false

    # detect and rewrite top level field forms
    let body =
        loop (result body = '() body)
            if (empty? body)
                break ('reverse result)
            let expr next = (decons body)
            let anchor = ('anchor expr)
            let this-type =
                'tag `'this-type anchor
            let exprT = ('typeof expr)
            let expr =
                if (exprT == Symbol)
                    let newexpr =
                        qq [let] [expr] =
                            [define-field] [this-type] '[expr] [Nothing]
                    `newexpr
                elseif (exprT == list)
                    sugar-match (expr as list)
                    case ((name is Symbol) ': T...)
                        let newexpr =
                            qq [let] [name] =
                                [define-field] [this-type] '[name] ([tuple] (unquote-splice T...))
                        `newexpr
                    case ((name is Symbol) '= index...)
                        let newexpr =
                            qq [let] [name] =
                                [define-field] [this-type] '[name] [Nothing]
                                    unquote-splice index...
                        `newexpr
                    default expr
                else expr
            repeat (cons expr result) next

    spice init-fields (enum-type)
        fn init-fields-runtime (T)
            sc_type_set_symbol T '__fields__ (sc_argument_list_new 0 null)
            sc_type_set_symbol T '__index__ (sc_const_int_new u64 0:u64)

        if ('constant? enum-type)
            init-fields-runtime (enum-type as type)
            `()
        else
            `(init-fields-runtime enum-type)
    qq
        unquote-splice
            if has-fwd-decl
                if has-supertype?
                    hide-traceback;
                    error "completing enum declaration must not define a supertype"
                qq [typedef+] [name]
            else
                qq [typedef] [name] < [supertype] do
        [init-fields] this-type
        inline tag (...)
            [define-field] this-type ...
        [do]
            unquote-splice body
            [fold-locals] this-type [append-to-type]
        [finalize-enum] this-type [storage]
        this-type

sugar dispatch (value)
    loop (next outp = next-expr '())
        sugar-match next
        case (('case (name is Symbol) (args...) body...) rest...)
            let anchor = ('anchor ('@ next))
            let _inline = ('tag `inline anchor)
            repeat rest...
                cons
                    qq [name] =
                        [_inline] "#hidden" [args...] (unquote-splice body...)
                    outp
        case (('default body...) rest...)
            let anchor = ('anchor ('@ next))
            let _inline = ('tag `inline anchor)
            return
                qq '__dispatch [value] (unquote-splice ('reverse outp))
                    [_inline] "#hidden" () (unquote-splice body...)
                rest...
        default
            error "missing default case"

# wrap for usage outside spices
spice extract-payload (enum-value extractT)
    _extract-payload enum-value extractT

run-stage;

typedef UnwrapError : (tuple)
    inline __typecall (cls)
        bitcast none this-type

@@ memo
inline Option (T)
    enum (.. "Option<" (tostring T) ">")
        None
        Some : T

        inline... __typecall
        case (cls : type,)
            this-type.None;
        case (cls : type, value)
            # follow same rules as assignment
            imply value this-type

        inline __tobool (self)
            dispatch self
            case None () false
            default true

        inline swap (self newvalue)
            let value = (deref (dupe self))
            assign (imply newvalue this-type) self
            value

        inline __imply (cls other-cls)
            static-if (T == bool)
                __tobool

        inline __rimply (other-cls cls)
            static-if (other-cls == Nothing)
                inline ()
                    this-type.None;
            else (imply? other-cls T)
                inline (self)
                    this-type.Some self

        inline unwrap (self)
            if self
                extract-payload self T
            else
                raise (UnwrapError)


do
    let enum dispatch Enum Option
    locals;
