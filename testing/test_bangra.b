IR

"

the typed bangra expression format is
    (: expression type)

at this point it is assumed that all expressions nested in <expression> are
typed.

a root expression that is not a typed expression is expanded / resolved before
expansion continues.






"

include "../api.b"
include "../macros.b"
include "../libc.b"

################################################################################
# build and install the preprocessor hook function.

deftype &opaque (pointer opaque)

define wrap-pointer (head ptr)
    function Value Value &opaque
    ret
        qquote
            unquote-splice head;
                unquote
                    call new-handle ptr

deftype MacroFunction
    function Value Value Value

defvalue key-symbols
    quote "symbols"
defvalue key-ir-env
    quote "ir-env"

define get-ir-env (env)
    function Environment Value
    ret
        bitcast
            call handle-value
                call get-key! env key-ir-env
            Environment

define get-symbols (env)
    function Value Value
    ret
        call get-key! env key-symbols

define get-handler (env head)
    function (pointer MacroFunction) Value Value
    ret
        bitcast
            call handle-value
                call get-key!
                    call get-symbols env
                    head
            pointer MacroFunction

define single (value)
    function Value Value
    ret
        call set-next value (null Value)

define typed? (value)
    function i1 Value
    ret
        call value== (call at value) (quote :)

define get-expr (value)
    function Value Value
    ret
        call next
            call at value

define replace (fromvalue tovalue)
    function Value Value Value
    ret
        call set-next tovalue
            call next fromvalue

define get-type (value)
    function Value Value
    ret
        call next
            call next
                call at value

declare expand-expression
    MacroFunction

# expression list is expanded chain-aware
define expand-expression-list (value env f)
    function Value Value Value (pointer MacroFunction)
    ret
        ?
            icmp == value
                null Value
            null Value
            splice
                defvalue head-expr
                    alloca Value
                store (null Value) head-expr
                defvalue prev-expr
                    alloca Value
                store (null Value) prev-expr
                loop expr
                    value
                    icmp != expr (null Value)
                    call next expanded-expr
                    defvalue expanded-expr
                        call f expr env
                    defvalue @prev-expr
                        load prev-expr
                    ? (icmp == @prev-expr (null Value))
                        splice
                            store expanded-expr head-expr
                            false
                        splice
                            call set-next!
                                @prev-expr
                                expanded-expr
                            false
                    store expanded-expr prev-expr
                load head-expr

define expand-expression (value env)
    MacroFunction
    defvalue tail
        call next value
    ret
        ?
            call atom? value
            call replace value
                qquote
                    error
                        unquote value
                        "unknown atom"
            splice
                defvalue head
                    call at value
                ?
                    call typed? value
                    value
                    splice
                        defvalue handler
                            call get-key!
                                call get-symbols env
                                head
                        if
                            call handle? handler;
                                call expand-expression
                                    call
                                        bitcast
                                            call handle-value handler
                                            pointer MacroFunction
                                        value
                                        env
                                    env
                            else
                                qquote
                                    error
                                        unquote value
                                        "unable to resolve symbol"

define expand-untype-expression (value env)
    MacroFunction
    defvalue expanded-value
        call expand-expression value env
    ret
        ? (call typed? expanded-value)
            call replace value
                call get-expr expanded-value
            expanded-value

global global-env
    null Value

define set-global (key value)
    function void Value Value
    call set-key!
        call get-symbols
            load global-env
        key
        value
    ret;

define set-global-syntax (head handler)
    function void Value (pointer MacroFunction)
    call set-global
        head
        call new-handle
            bitcast
                handler
                pointer opaque
    ret;

# the top level expression goes through the preprocessor, which then descends
# the expression tree and translates it to bangra IR.
define global-preprocessor (ir-env value)
    preprocessor-func
    call set-key!
        load global-env
        key-ir-env
        call new-handle
            bitcast
                ir-env
                pointer opaque

    defvalue result
        call expand-expression-list
            call next
                call at value
            load global-env
            expand-untype-expression

    ret
        call dump-value
            qquote
                IR
                    #include "../libc.b"

                    define ::ret:: ()
                        function void
                        unquote-splice result
                        ret;
                    dump-module;
                    execute ::ret::

# install bangra preprocessor
run
    store
        call new-table
        global-env
    defvalue symtable
        call new-table
    call set-key!
        load global-env
        key-symbols
        symtable

    call set-global-syntax
        quote let
        define "" (value env)
            MacroFunction
            defvalue expr
                call next
                    defvalue name
                        call next
                            call at value
            ret
                call replace value
                    qquote
                        :
                            defvalue
                                unquote name
                                unquote
                                    call get-expr expr

    call set-preprocessor
        &str "bangra"
        global-preprocessor

module test-bangra bangra

    let printf
        :
            declare "printf"
                function i32 rawstring ...
            function i32 rawstring ...

    :
        call printf
            bitcast (global "" "hello world\n") rawstring
        i32


