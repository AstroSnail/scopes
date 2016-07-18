IR

# requires api.b to be included first
include "api.b"

define macro-quote (env expr)
    preprocessor-func
    ret
        call ref
            call join
                quote _Value escape
                call ref
                    call join
                        call at
                            quote _Value (quote _Value)
                        expr

define macro-@str (env expr)
    preprocessor-func
    ret
        call ref
            call join
                call join
                    quote _Value bitcast
                    call ref
                        call join
                            call at
                                quote _Value (global "")
                            expr
                quote _Value rawstring

execute
    define "" (env)
        function void Environment
        call set-macro env
            bitcast (global "" "quote") rawstring
            macro-quote
        call set-macro env
            bitcast (global "" "@str") rawstring
            macro-@str
        ret;

define qquote-1 (value)
    function Value Value
    cond-br
        call atom? value
        block $is-not-list
        block $is-list
    set-block $is-not-list
    ret
        call ref
            call set-next
                quote quote
                call set-next
                    value
                    null Value
    set-block $is-list
    cond-br
        call expression? value
            quote unquote
        block $is-unquote
        block $is-not-unquote
    set-block $is-unquote
    ret
        call next
            call at value
    set-block $is-not-unquote
    cond-br
        call expression? value
            quote qquote
        block $is-qquote
        block $is-not-qquote
    set-block $is-qquote
    ret
        call qquote-1
            call qquote-1
                call next
                    call at value
    set-block $is-not-qquote
    ret
        call ref
            call join
                call at
                    quote (call prepend)
                call join
                    call qquote-1
                        call at value
                    call qquote-1
                        call ref
                            call next
                                call at value

define macro-qquote (env expr)
    preprocessor-func
    ret
        call qquote-1 expr

define macro-run (env expr)
    preprocessor-func
    ret
        call ref
            call join
                quote execute
                call ref
                    call join
                        call at
                            quote (define "" (env))
                        call join
                            quote (function void Environment)
                            call join expr
                                quote (ret)

execute
    define "" (env)
        function void Environment
        call set-macro env
            @str "run"
            macro-run
        call set-macro env
            @str "qquote"
            macro-qquote
        ret;

define macro-? (env expr)
    preprocessor-func
    defvalue else-expr
        call next
            defvalue then-expr
                call next
                    defvalue condition expr
    defvalue label-then (call unique-symbol (@str "then"))
    defvalue label-else (call unique-symbol (@str "else"))
    defvalue label-finally (call unique-symbol (@str "if-end"))
    defvalue label-then-br (call unique-symbol (@str "then-br"))
    defvalue label-else-br (call unique-symbol (@str "else-br"))
    defvalue value-then (call unique-symbol (@str "value"))
    defvalue value-else (call unique-symbol (@str "value"))
    cond-br
        icmp == else-expr (null Value)
        block $no-else
        block $has-else
    block $finally
    set-block $no-else
    defvalue without-else-blockdef
        label-finally
    defvalue without-else-expr
        null Value
    defvalue without-else-phi
        null Value
    br $finally
    set-block $has-else
    defvalue with-else-blockdef
        qquote
            block
                unquote label-else
    defvalue with-else-expr
        call next
            call at
                qquote
                    splice
                        set-block
                            unquote label-else
                        defvalue
                            unquote value-else
                            unquote
                                call set-next else-expr (null Value)
                        defvalue
                            unquote label-else-br
                            this-block
                        br
                            unquote label-finally
    defvalue with-else-phi
        qquote
            phi
                typeof (unquote value-then)
                unquote value-then;
                    unquote label-then-br
                unquote value-else;
                    unquote label-else-br
    br $finally
    set-block $finally
    defvalue opt-else-blockdef
        phi Value
            without-else-blockdef $no-else
            with-else-blockdef $has-else
    defvalue opt-else-expr
        phi Value
            without-else-expr $no-else
            with-else-expr $has-else
    defvalue opt-else-phi
        phi Value
            without-else-phi $no-else
            with-else-phi $has-else
    ret
        qquote
            splice
                block
                    unquote label-finally
                cond-br
                    unquote
                        call set-next condition (null Value)
                    block
                        unquote label-then
                    unquote opt-else-blockdef
                set-block
                    unquote label-then
                defvalue
                    unquote value-then
                    unquote
                        call set-next then-expr (null Value)
                defvalue
                    unquote label-then-br
                    this-block
                br
                    unquote label-finally
                unquote opt-else-expr
                set-block
                    unquote label-finally
                unquote opt-else-phi

run
    call set-macro env
        @str "?"
        macro-?

define macro-if (env expr)
    preprocessor-func
    defvalue second-block
        call next
            defvalue first-block expr

    ret
        ?
            call value==
                quote else
                call at first-block
            qquote
                splice
                    unquote
                        call next
                            call at first-block
            qquote
                ?
                    unquote
                        call set-next
                            call at first-block
                            null Value
                    splice
                        unquote
                            call next
                                call at first-block
                    unquote
                        ? (icmp == second-block (null Value))
                            null Value
                            qquote
                                if
                                    unquote second-block


# (loop varname init-expr cond-expr next-expr body-expr ...)
define macro-loop (env expr)
    preprocessor-func
    defvalue body-expr
        call next
            defvalue next-expr
                call next
                    defvalue cond-expr
                        call next
                            defvalue init-expr
                                call next
                                    defvalue varname expr
    defvalue value-init (call unique-symbol (@str "init"))
    defvalue label-entry (call unique-symbol (@str "entry"))
    defvalue label-cond (call unique-symbol (@str "cond"))
    defvalue label-loop (call unique-symbol (@str "loop"))
    defvalue label-finally (call unique-symbol (@str "finally"))
    defvalue param-varname
        call set-next varname (null Value)
    ret
        qquote
            splice
                defvalue
                    unquote value-init
                    unquote
                        call set-next init-expr (null Value)
                defvalue
                    unquote label-entry
                    this-block
                br
                    block
                        unquote label-cond
                set-block
                    unquote label-cond
                defvalue
                    unquote param-varname
                    phi
                        typeof
                            unquote value-init
                        unquote value-init;
                            unquote label-entry
                cond-br
                    unquote
                        call set-next cond-expr (null Value)
                    block
                        unquote label-loop
                    block
                        unquote label-finally
                set-block
                    unquote label-loop
                unquote body-expr
                incoming
                    unquote param-varname
                    unquote (call set-next next-expr (null Value));
                        this-block
                br
                    unquote label-cond
                set-block
                    unquote label-finally

run
    call set-macro env
        @str "if"
        macro-if
    call set-macro env
        @str "loop"
        macro-loop
