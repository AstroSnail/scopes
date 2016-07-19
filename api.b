IR

################################################################################
# declare the bangra API as we don't have the means to comfortably import clang
# declarations yet.

# opaque declarations for the bangra compiler Environment and the Values of
# its S-Expression tree, which can be Pointer, String, Symbol, Integer, Real.
struct _Environment
struct _Value

deftype Environment (* _Environment)
deftype Value (* _Value)

defvalue value-type-none 0
defvalue value-type-pointer 1
defvalue value-type-string 2
defvalue value-type-symbol 3
defvalue value-type-integer 4
defvalue value-type-real 5

defvalue argc
    declare-global "bang_argc" i32
defvalue argv
    declare-global "bang_argv" (* rawstring)
defvalue executable-path
    declare-global "bang_executable_path" rawstring

# LLVM compatibility
#-------------------------------------------------------------------------------

include "llvm.b"

defvalue parent-env
    declare "bangra_parent_env" (function Environment Environment)
defvalue meta-env
    declare "bangra_meta_env" (function Environment Environment)
defvalue llvm-module
    declare "bangra_llvm_module" (function LLVMModuleRef Environment)
defvalue llvm-value
    declare "bangra_llvm_value" (function LLVMValueRef Environment)
defvalue llvm-type
    declare "bangra_llvm_type" (function LLVMTypeRef Environment)
defvalue llvm-engine
    declare "bangra_llvm_engine" (function LLVMExecutionEngineRef Environment)
defvalue import-c-module
    declare "bangra_import_c_module"
        function LLVMModuleRef Value rawstring rawstring (* rawstring) i32
defvalue import-c-string
    declare "bangra_import_c_string"
        function LLVMModuleRef Value rawstring rawstring rawstring (* rawstring) i32

# methods that apply to all types
#-------------------------------------------------------------------------------

defvalue kind-of
    declare "bangra_get_kind" (function i32 Value)
defvalue value==
    declare "bangra_eq" (function i1 Value Value)

defvalue next
    declare "bangra_next" (function Value Value)
defvalue set-next
    declare "bangra_set_next" (function Value Value Value)
defvalue set-next!
    declare "bangra_set_next_mutable" (function Value Value Value)

defvalue dump-value
    declare "bangra_dump_value" (function Value Value)

defvalue anchor-path
    declare "bangra_anchor_path" (function rawstring Value)
defvalue anchor-lineno
    declare "bangra_anchor_lineno" (function i32 Value)
defvalue anchor-column
    declare "bangra_anchor_column" (function i32 Value)
defvalue anchor-offset
    declare "bangra_anchor_offset" (function i32 Value)
defvalue set-anchor
    declare "bangra_set_anchor" (function Value Value rawstring i32 i32 i32)
defvalue set-anchor!
    declare "bangra_set_anchor_mutable" (function Value Value rawstring i32 i32 i32)

# pointer
#-------------------------------------------------------------------------------

defvalue ref
    declare "bangra_ref" (function Value Value)
defvalue at
    declare "bangra_at" (function Value Value)
defvalue set-at!
    declare "bangra_set_at_mutable" (function Value Value Value)

# string and symbol
#-------------------------------------------------------------------------------

defvalue new-string
    declare "bangra_string" (function Value rawstring)
defvalue new-symbol
    declare "bangra_symbol" (function Value rawstring)
defvalue string-value
    declare "bangra_string_value" (function rawstring Value)

# real
#-------------------------------------------------------------------------------

defvalue new-real
    declare "bangra_real" (function Value double)
defvalue real-value
    declare "bangra_real_value" (function double Value)

# integer
#-------------------------------------------------------------------------------

defvalue new-integer
    declare "bangra_integer" (function Value i64)
defvalue integer-value
    declare "bangra_integer_value" (function i64 Value)

# table
#-------------------------------------------------------------------------------

defvalue new-table
    declare "bangra_table" (function Value)
defvalue set-key
    declare "bangra_set_key" (function void Value Value Value)
defvalue get-key
    declare "bangra_get_key" (function Value Value Value)

# handle
#-------------------------------------------------------------------------------

defvalue new-handle
    declare "bangra_handle" (function Value (* opaque))
defvalue handle-value
    declare "bangra_handle_value" (function (* opaque) Value)

# metaprogramming
#-------------------------------------------------------------------------------

deftype preprocessor-func
    function Value Environment Value

defvalue error-message
    declare "bangra_error_message" (function void Environment Value rawstring ...)
defvalue set-preprocessor
    declare "bangra_set_preprocessor" (function void rawstring (* preprocessor-func))
defvalue get-preprocessor
    declare "bangra_get_preprocessor" (function (* preprocessor-func) rawstring)
defvalue set-macro
    declare "bangra_set_macro" (function void Environment rawstring (* preprocessor-func))
defvalue get-macro
    declare "bangra_get_macro" (function (* preprocessor-func) Environment rawstring)
defvalue unique-symbol
    declare "bangra_unique_symbol" (function Value rawstring)

# helpers
################################################################################

defvalue pointer?
    define "" (value)
        function i1 Value
        ret
            icmp ==
                call kind-of value
                value-type-pointer

# non-pointer or null pointer
defvalue atom?
    define "" (value)
        function i1 Value
        cond-br
            call pointer? value
            block $is-pointer
            block $is-not-pointer
        set-block $is-pointer
        ret
            icmp ==
                call at value
                null Value
        set-block $is-not-pointer
        ret (int i1 1)

defvalue symbol?
    define "" (value)
        function i1 Value
        ret
            icmp ==
                call kind-of value
                value-type-symbol

defvalue expression?
    define "" (value expected-head)
        function i1 Value Value
        cond-br
            call atom? value
            block $is-atom
            block $is-not-atom
        set-block $is-atom
        ret (int i1 0)
        set-block $is-not-atom
        ret
            call value==
                call at value
                expected-head

defvalue type-key
    quote _Value "#bangra-type"

defvalue set-type
    define "" (value value-type)
        function void Value Value
        call set-key
            value
            type-key
            value-type
        ret;

defvalue get-type
    define "" (value)
        function Value Value
        ret
            call get-key
                value
                type-key

defvalue ref-set-next
    define "" (lhs rhs)
        function Value Value Value
        ret
            call ref
                call set-next lhs rhs

# appends ys ... to xs ...
define join (xs ys)
    function Value Value Value
    cond-br
        icmp == xs
            null Value
        block $is-null
        block $is-not-null
    set-block $is-null
    ret ys
    set-block $is-not-null
    ret
        call set-next xs
            call join
                call next xs
                ys

define prepend (xs ys)
    function Value Value Value
    ret
        call ref
            call join xs (call at ys)