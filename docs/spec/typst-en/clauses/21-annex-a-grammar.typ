#import "/tools/common.typ": *
// shape: the grammar collected; each rule has its examples in its clause

#set heading(numbering: (..n) => if n.pos().len() == 1 { "Annex A" } else { "A." + n.pos().slice(1).map(str).join(".") })
= The whole grammar (normative) <annex-grammar>

#para("1")[This annex collects the syntax rules of the clauses in one place. The notation is that of ISO/IEC 14977:
`a b` is sequence, `a | b` choice, `[ a ]` optional, `{ a }` zero or more times, `(* … *)` a remark, `? … ?` a rule
stated in words. Names in capitals are the tokens of clause 6. Any number of spaces and remarks may stand between
tokens, except right before a new name (6.3 (4)).]

#para("2")[The grammar is read with one token of lookahead. The exception is a parameter of a signature, where the token
after it tells whether it is a name or a type. What a name refers to (a local, a global, a procedure) and the types on the
stack are settled by the declarations and checks, not by the grammar.]

```ebnf
(* 6: tokens *)
NAME        = letter { letter | digit } ;                (* not a reserved word *)
PATH        = NAME "." member { "." member } ;           (* member: a run of letters and digits *)
INT         = [ "+" | "-" ] ( digits | "0x" hex-digits | "0b" bit-digits ) ;
DEC         = [ "+" | "-" ] digits "." digits ;              (* 7.5 *)
FLOAT       = [ "+" | "-" ] digits [ "." digits ] "e" [ "+" | "-" ] digits
            | [ "+" | "-" ] "0x" hex-digits [ "." hex-digits ] "p" [ "+" | "-" ] digits ;   (* 7.7 *)
CHAR        = [ "U" ] "'" ( char-byte | escape ) "'" ;
STRING      = '"' { string-byte | escape } '"' ;
RAWSTRING   = "r" '"' { raw-byte } '"' ;
TEXT        = "text" TAG newline { text-line newline } TAG ;
BOOL        = "true" | "false" ;
literal     = INT | DEC | FLOAT | CHAR | STRING | RAWSTRING | TEXT | BOOL ;

(* 6.7, 13: a source *)
source      = [ "pinax" INT ] { item } ;
item        = import | order | forward | declaration | definition | row-item | assign-item | body ;
import      = "use" NAME [ "as" NAME ] "." ;
order       = "set_order" { order-one } "." | "get_order" ;   (* 13.5 *)
order-one   = NAME | "get_order" ;
forward     = "forward" ( "array" | "list" | "pool" ) NAME
            | "forward" NAME effect "end" ;

(* 8: declarations *)
declaration = [ "export" ] ( [ "bound" ] bindable | "shared" shareable | decl ) ;
bindable    = "global" NAME type | "array" NAME type capacity | "buffer" NAME capacity ;
shareable   = bindable | "list" NAME type capacity | "index" NAME NAME PATH ;
decl        = "const" NAME literal
            | "err" NAME
            | "list" NAME type capacity | "queue" NAME type capacity | "pool" NAME NAME capacity
            | "record" NAME { field | "big_endian" } "end"
            | "type" NAME ( "int" | "bool" | "str" | "dec" INT | "proc" effect "end" )
            | "index" NAME NAME PATH
            | "tasks" NAME capacity [ size [ size [ size ] ] ] [ "apart" ]
            | "extern" c-name effect "from" STRING [ "as" NAME ] "end"
            | "test" NAME "do" { body } "end" ;
capacity    = size | "host" ;
size        = INT | NAME ;
field       = NAME NAME [ size ] ;
c-name      = NAME | reserved-procedure | STRING ;
row-item    = [ "export" ] "row" NAME [ "as" NAME ] { value } "." ;
assign-item = "assign" NAME "(" value { "," value } ")" ;
value       = literal | NAME ;

(* 9: procedures *)
definition  = [ "export" ] "def" NAME effect "do" { body } "end" ;
effect      = [ params ] [ "gives" params ] [ "or" "fail" ] ;
params      = param { "," param } ;
param       = [ NAME ] ( type | coll-type | "args" ) ;          (* args: an input only, 16.13 *)
type        = [ "owned" ] ( NAME [ size ] ) ;      (* a proc type is declared by a type, 7.4 *)
coll-type   = ( "array" | "list" | "queue" ) NAME [ size ] | "buffer" ;   (* an input, 9.9 *)

(* 9 to 11: the body *)
body        = literal | NAME | PATH | store | set | fits | block | cat | args-value | guard | control | defer
            | "," | "."                                       (* the end of a statement, 10.8 *)
            | "return" | "fail" | ( "break" | "continue" ) [ INT ] "." ;
store       = "to" ( NAME | PATH ) ;   (* a local, or a field *)
set         = "set" ( NAME | PATH ) ;                         (* a global, 9.4 *)
fits        = "fits" ( NAME | PATH ) ;                        (* a str N place, 7.3 *)
block       = "do" { "to" NAME } { body } "end"
            | "def" effect "do" { body } "end"              (* a block value, 10.5 *)
            | "ref" NAME ;
cat         = "args" { body } "." "cat" | "cat" ;           (* 9.8; a bare cat takes an args parameter, 16.13 *)
args-value  = "args" { body } "." ;                           (* 16.13 *)
guard       = "or_fail" | "or_return" | "or_else" [ "to" NAME ] { body } "end" ;
control     = "if" { body } [ "else" { body } ] "end"
            | "loop" { body } "end"
            | "match" { branch } [ "otherwise" { body } "end" ] "end" ;
defer       = "defer" { body } "end" ;             (* 10.7 *)
branch      = "when" case-value { case-value } "do" { body } "end" ;
case-value  = literal | NAME | PATH ;
```

#para("3")[A source read by data load follows `data-source` of 13.4.]
