#
    Bangra Interpreter
    Copyright (c) 2017 Leonard Ritter

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

    This is the bangra boot script. It implements the remaining standard
    functions and macros, parses the command-line and optionally enters
    the REPL.

fn/cc type? (_ T)
    icmp== (ptrtoint type size_t) (ptrtoint (typeof T) size_t)

fn/cc type== (_ a b)
    fn/cc assert-type (_ T)
        branch (type? T)
            fn/cc (_)
            fn/cc (_)
                compiler-error
                    string-join "type expected, not " (Any-repr (Any-wrap T))
    assert-type a
    assert-type b
    icmp== (ptrtoint a size_t) (ptrtoint b size_t)

fn/cc assert-typeof (_ a T)
    branch (type== T (typeof a))
        fn/cc (_)
        fn/cc (_)
            compiler-error
                string-join "type "
                    string-join (Any-repr (Any-wrap T))
                        string-join " expected, not "
                            Any-repr (Any-wrap (typeof a))

fn/cc string->rawstring (_ s)
    assert-typeof s string
    getelementptr s 0 1 0

fn/cc Any-typeof (_ val)
    assert-typeof val Any
    extractvalue val 0
fn/cc Any-payload (_ val)
    assert-typeof val Any
    extractvalue val 1

fn/cc Any-extract-list (_ val)
    assert-typeof val Any
    inttoptr (Any-payload val) list

fn/cc Any-extract-Syntax (_ val)
    assert-typeof val Any
    inttoptr (Any-payload val) Syntax

fn/cc Any-extract-Symbol (_ val)
    assert-typeof val Any
    bitcast (Any-payload val) Symbol

fn/cc Any-extract-i32 (_ val)
    assert-typeof val Any
    trunc (Any-payload val) i32

fn/cc list-empty? (_ l)
    assert-typeof l list
    icmp== (ptrtoint l size_t) 0:usize
fn/cc list-at (_ l)
    assert-typeof l list
    branch (list-empty? l)
        fn/cc (_) (Any-wrap none)
        fn/cc (_) (extractvalue (load l) 0)
fn/cc list-next (_ l)
    assert-typeof l list
    branch (list-empty? l)
        fn/cc (_) eol
        fn/cc (_)
            bitcast (extractvalue (load l) 1) list
fn/cc list-at-next (_ l)
    assert-typeof l list
    branch (list-empty? l)
        fn/cc (_)
            _ (Any-wrap none) eol
        fn/cc (_)
            _
                extractvalue (load l) 0
                bitcast (extractvalue (load l) 1) list
fn/cc list-countof (_ l)
    assert-typeof l list
    branch (list-empty? l)
        fn/cc (_)
            _ 0:u64
        fn/cc (_)
            extractvalue (load l) 2

fn/cc Any-list? (_ val)
    assert-typeof val Any
    type== (Any-typeof val) list

fn/cc maybe-unsyntax (_ val)
    branch (type== (Any-typeof val) Syntax)
        fn/cc (_)
            extractvalue (load (Any-extract-Syntax val)) 1
        fn/cc (_) val

fn/cc Any-dispatch (return val)
    assert-typeof val Any
    call
        fn/cc try0 (_ T)
            fn/cc failed (_)
                return none
            fn/cc try3 (_)
                branch (type== T i32)
                    fn/cc (_)
                        return (Any-extract-i32 val)
                    \ failed
            fn/cc try2 (_)
                branch (type== T Symbol)
                    fn/cc (_)
                        return (Any-extract-Symbol val)
                    \ try3
            fn/cc try1 (_)
                branch (type== T Syntax)
                    fn/cc (_)
                        return (Any-extract-Syntax val)
                    \ try2
            branch (type== T list)
                fn/cc (_)
                    return (Any-extract-list val)
                \ try1
        Any-typeof val

fn/cc list-reverse (_ l)
    assert-typeof l list
    fn/cc loop (_ l next)
        branch (list-empty? l)
            fn/cc (_) next
            fn/cc (_)
                loop (list-next l) (list-cons (list-at l) next)
    loop l eol

fn/cc integer-type? (_ T)
    icmp== (type-kind T) type-kind-integer
fn/cc real-type? (_ T)
    icmp== (type-kind T) type-kind-real
fn/cc pointer-type? (_ T)
    icmp== (type-kind T) type-kind-pointer
fn/cc integer? (_ val)
    integer-type? (typeof val)
fn/cc real? (_ val)
    real-type? (typeof val)
fn/cc pointer? (_ val)
    pointer-type? (typeof val)

fn/cc powi (_ base exponent)
    assert-typeof base i32
    assert-typeof exponent i32
    fn/cc loop (_ result cur exponent)
        branch (icmp== exponent 0)
            fn/cc (_) result
            fn/cc (_)
                loop
                    branch (icmp== (band exponent 1) 0)
                        fn/cc (_) result
                        fn/cc (_)
                            mul result cur
                    mul cur cur
                    lshr exponent 1
    branch (constant? exponent)
        fn/cc (_)
            loop 1 base exponent
        fn/cc (_)
            loop (unconst 1) (unconst base) exponent

fn/cc Any-new (_ val)
    fn/cc construct (_ outval)
        insertvalue (insertvalue (undef Any) (typeof val) 0) outval 1

    fn/cc wrap-unknown (_)
        call
            fn/cc (_ val)
                fn/cc failed (_)
                    compiler-error
                        string-join "unable to wrap value of storage type "
                            Any-repr (Any-wrap (typeof val))
                fn/cc try-wrap-real (_)
                    branch (real? val)
                        fn/cc (_)
                            construct
                                bitcast (fpext val f64) u64
                        \ failed
                fn/cc try-wrap-integer (_)
                    branch (integer? val)
                        fn/cc (_)
                            construct
                                branch (signed? (typeof val))
                                    fn/cc (_)
                                        sext val u64
                                    fn/cc (_)
                                        zext val u64
                        \ try-wrap-real
                branch (pointer? val)
                    fn/cc (_)
                        compiler-message "wrapping pointer"
                        construct
                            ptrtoint val u64
                    \ try-wrap-integer
            bitcast val
                type-storage (typeof val)

    branch (constant? val)
        fn/cc (_)
            Any-wrap val
        \ wrap-unknown


fn/cc list-new (_ ...)
    fn/cc loop (_ i tail)
        branch (icmp== i 0)
            fn/cc (_) tail
            fn/cc (_)
                loop (sub i 1)
                    list-cons (Any-new (va@ (sub i 1) ...)) tail
    loop (va-countof ...) eol

#compile (typify Any-new string) 'dump-module

# calling polymorphic function
    Any-dispatch (Any-wrap 10)

# importing C code
#call
    fn/cc (_ lib)
        call
            fn/cc (_ sinf printf)
                printf
                    string->rawstring "test: %f\n"
                    sinf 0.5235987755982989
                #printf
                    string->rawstring "fac: %i\n"
                    fac
                        unconst 5
            purify
                Any-extract
                    Scope@ lib 'sinf
            Any-extract
                Scope@ lib 'printf

    import-c "testdata.c" "
        float sinf(float);
        int printf( const char* format, ... );
        "
        \ eol

# print function
fn/cc print (return ...)
    fn/cc load-printf (_)
        #compiler-message "loading printf..."
        call
            fn/cc (_ lib)
                call
                    fn/cc (_ printf)
                        fn/cc (_ fmt ...)
                            printf (string->rawstring fmt) ...
                    Any-extract
                        Scope@ lib 'stb_printf
            import-c "printf.c" "
                int stb_printf(const char *fmt, ...);
                "
                \ eol

    call
        fn/cc (() printf)
            fn/cc print-element (return val)
                call
                    fn/cc (() T)
                        fn/cc fail ()
                            io-write "<value of type "
                            io-write (Any-repr (Any-wrap (typeof val)))
                            io-write ">"
                            return

                        fn/cc try-f32 ()
                            branch (type== T f32)
                                fn/cc ()
                                    printf "%g" val
                                    return
                                \ fail
                        fn/cc try-i32 ()
                            branch (type== T i32)
                                fn/cc ()
                                    printf "%i" val
                                    return
                                \ try-f32
                        branch (type== T string)
                            fn/cc ()
                                io-write val
                                return
                            \ try-i32
                    typeof val
            call
                fn/cc loop (() i)
                    branch (icmp<s i (va-countof ...))
                        fn/cc ()
                            branch (icmp>s i 0)
                                fn/cc (_)
                                    io-write " "
                                fn/cc (_)
                            print-element (unconst (va@ i ...))
                            cc/call loop none (add i 1)
                        fn/cc ()
                            io-write "\n"
                            return
                \ 0
        load-printf

fn/cc print-spaces (_ depth)
    assert-typeof depth i32
    branch (icmp== depth 0)
        fn/cc (_)
        fn/cc (_)
            io-write "    "
            print-spaces (sub depth 1)

fn/cc walk-list (_ on-leaf l depth)
    call
        fn/cc loop (_ l)
            branch (list-empty? l)
                fn/cc (_) true
                fn/cc (_)
                    call
                        fn/cc (_ at next)
                            call
                                fn/cc (_ value)
                                    branch (Any-list? value)
                                        fn/cc (_)
                                            print-spaces depth
                                            io-write ";\n"
                                            walk-list on-leaf
                                                Any-extract-list value
                                                add depth 1
                                        fn/cc (_)
                                            on-leaf value depth
                                            \ true
                                maybe-unsyntax at
                            loop next
                        list-at-next l
        \ l

# deferring remaining expressions to bootstrap parser
syntax-apply-block
    fn/cc (_ anchor exprs env)
        walk-list
            fn/cc on-leaf (_ value depth)
                print-spaces depth
                #Any-dispatch value
                io-write
                    Any-repr value
                io-write "\n"
            unconst exprs
            unconst 0

# static assertion
    branch (type== (typeof 1) i32)
        fn/cc (_)
        fn/cc (_)
            compiler-error "static assertion failed: argument not i32"
    branch (constant? (add 1 2))
        fn/cc (_)
        fn/cc (_)
            compiler-error "static assertion failed: argument not constant"

# naive factorial
    fn/cc fac (_ n)
        branch (icmp<=s n 1)
            fn/cc (_) 1
            fn/cc (_)
                mul n
                    call
                        fn/cc (_ n-1)
                            fac n-1
                        sub n 1

    fac
        unconst 5

# importing C code
    call
        fn/cc (_ lib)
            call
                fn/cc (_ sinf printf)
                    printf
                        string->rawstring "test: %f\n"
                        sinf 0.5235987755982989
                    printf
                        string->rawstring "fac: %i\n"
                        fac
                            unconst 5
                purify
                    Any-extract
                        Scope@ lib 'sinf
                Any-extract
                    Scope@ lib 'printf

        import-c "testdata.c" "
            float sinf(float);
            int printf( const char* format, ... );
            "
            \ eol

# continuation skip
    fn/cc mainf (return x)
        io-write "doin stuff...\n"
        call
            fn/cc subf (_ x)
                branch (icmp== x 0)
                    fn/cc (_) true
                    fn/cc (_)
                        io-write "terminated early!\n"
                        return false
            \ x
        io-write "done stuff.\n"
        \ true

    mainf
        unconst 0
    mainf
        unconst 1


# mutual recursion
    fn/cc even? (eret ei)
        fn/cc odd? (oret oi)
            branch (icmp>s oi 0)
                fn/cc (_)
                    even? (sub oi 1)
                fn/cc (_)
                    eret false

        branch (icmp>s ei 0)
            fn/cc (_)
                odd? (sub ei 1)
            fn/cc (_)
                _ true

    branch
        even?
            unconst 30
        fn/cc (_)
            io-write "even\n"
        fn/cc (_)
            io-write "odd\n"


# tail-recursive program that avoids closures
    fn/cc puts (return s n)
        fn/cc loop (() i n)
            branch (icmp== i n)
                fn/cc ()
                    return
                fn/cc ()
                    io-write s
                    io-write "\n"
                    cc/call loop none (add i 1) n
        cc/call loop none
            branch (constant? n)
                fn/cc (_) 0
                fn/cc (_) (unconst 0)
            \ n

    puts "hello world"
        unconst 8

# tail-recursive program with closures
    fn/cc puts (return s n)
        fn/cc loop (_ i n)
            branch (icmp== i n)
                fn/cc (_)
                    return
                fn/cc (_)
                    io-write s
                    io-write "\n"
                    loop (add i 1) n
        loop
            branch (constant? n)
                fn/cc (_) 0
                fn/cc (_) (unconst 0)
            \ n

    puts "hello world" (unconst 8)

# explicit instantiation
fn/cc test-explicit-instantiation (_)
    fn/cc test-add (return x1 y1 z1 w1 x2 y2 z2 w2)
        return
            fadd x1 x2
            fadd y1 y2
            fadd z1 z2
            fadd w1 w2

    dump-label test-add
    dump-label
        typify test-add f32 f32 f32 f32 f32 f32 f32 f32
    call
        fn/cc (_ f)
            dump f
            print
                f 1. 2. 3. 4. 5. 6. 7. 8.
        compile
            typify test-add f32 f32 f32 f32 f32 f32 f32 f32
            \ 'dump-disassembly 'dump-module

#test-explicit-instantiation

fn/cc test-select-optimization (_)
    fn/cc conditional-select (return opt i)
        branch opt
            fn/cc (_)
                return
                    add i 5
            fn/cc (_)
                return
                    mul i 5

    dump-label
        typify conditional-select bool i32

    compile
        typify conditional-select bool i32
        \ 'dump-module 'dump-disassembly #'skip-opts

# return function dynamically
fn/cc test-dynamic-function-return (_)
    fn/cc square-brackets (_ s)
        io-write "["; io-write s; io-write "]"
    fn/cc round-brackets (_ s)
        io-write "("; io-write s; io-write ")"
    fn/cc bracket (_ use-square?)
        branch (unconst use-square?)
            fn/cc (_) square-brackets
            fn/cc (_) round-brackets
    fn/cc apply-brackets (_ f s)
        f s
        io-write "\n"

    apply-brackets (bracket true) "hello"
    apply-brackets (bracket true) "world"
    apply-brackets (bracket false) "hello"
    apply-brackets (bracket false) "world"

# polymorphic return type and inlined type checking
fn/cc print-value (_ value)
    call
        fn/cc (_ value-type)
            branch (type== value-type i32)
                fn/cc (_)
                    io-write "<number>\n"
                    \ "hello"
                fn/cc (_)
                    branch (type== value-type string)
                        fn/cc (_)
                            io-write value
                            io-write "\n"
                            \ false
                        fn/cc (_)
                            io-write "???\n"
        typeof value
fn/cc test-polymorphic-return-type (_)
    print-value
        print-value
            print-value 3

\ true





