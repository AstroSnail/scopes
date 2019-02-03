
using import testing

inline print_stuff (x)
    print "line"
    inline ()
        print x

fn main ()
    let link = (print_stuff "hello")
    link;

main;

do
    define ascope
        syntax-eval (Scope)
    fn print_stuff2 (x)
        print "line"
        'set-symbol ascope 'somefunc
            fn ()
                print x

    fn main2 ()
        print_stuff2 "hello"
    main2;

    compile-stage;

    # this case is illegal
    # error: non-constant value of type String is inaccessible from function
    assert-compiler-error
        ascope.somefunc;

true