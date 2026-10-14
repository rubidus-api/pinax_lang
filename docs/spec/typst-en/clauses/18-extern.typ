#import "/tools/common.typ": *

= Calling C functions <extern>

#part("Syntax")
```ebnf
extern = [ "export" ] "extern" c-name "(" effect ")" "from" STRING [ "as" NAME ] ;
c-name = NAME | reserved-word | STRING ;
```

#para("1")[`extern NAME (signature) from "LIBRARY"` makes one C function a word. The processor does not open libraries
itself. At load it asks the host's #term("resolver") for (library, name) and calls the function the host gives. When
the host has set no resolver, or gives no function, the load is refused. So the C functions a script can call are the
host's choice.]

#para("2")[With `as OTHER` at the end the script calls it by that name; the name asked of the host stays the same. When
the name of the C function is a core word or a reserved word, or holds characters that are not those of a name (it is
then written in quotes), `as` is required.]

#para("3")[The signature may have: as arguments `int` (and `bool`, declared types and other values represented as
`int`), handles, `str` and `cstr`; as the result one `int` or handle, or none. `or fail` goes only with a handle result:
a null pointer from the function is a failure. `cstr` is a type of this signature only.]

#para("4")[How the function is called: every `int` and every handle's object pointer is one `intptr_t` argument; a
`str` is two arguments, `(const char *, size_t)`, not NUL-terminated; a `cstr` is one `const char *` to a
NUL-terminated copy. At most eight C arguments. The result is read as one `intptr_t`. Functions of other shapes
(floating point arguments, an `int` result, structs, variable arguments) are wrapped in host words.]

#ex("spec", "18-extern.c")

#para("5")[A signature that does not match the C function is undefined behaviour in C, and the processor cannot know.
A host that runs scripts it does not trust sets no resolver.]

#part("Constraints")

#para("6")[A function whose name exists already (a core word, for instance) shall be given another name with `as`.]
#ex("spec", "18-extern-as.pnx")

#para("7")[A signature that (3) does not allow is refused.]
#ex("spec", "18-extern-result.pnx")

#part("Diagnostics")
#diag(
  ([A name that exists, without `as`], "this name is already defined: give the function another with `as`"),
  ([A signature not allowed], "extern: int, handle, str and cstr arguments (eight for C at most), one int or handle result; or fail only with a handle result"),
  ([The host gives no such function], "extern: the host does not give this function"),
)
