
fn main ()
    fn testf (a b)
        + a b

    # force a translation to a C function pointer
    let f = (typify testf i32 i32)
    assert ((f 2 3) == 5)
    true

dump-ast
    typify main

#fn testfunc (x y)
    x * y
let lib =
    import-c "callback.c"
        """"
            typedef int (*testfunc)(int x, int y);
            int call_testfunc(testfunc f, int x, int y) {
                return f(x,y);
            }
        '()

compile-stage;

fn testf (x y)
    + x y

let z =
    lib.call_testfunc testf 2 3
print z
assert (z == 5)


