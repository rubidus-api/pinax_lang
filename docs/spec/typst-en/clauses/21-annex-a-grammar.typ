#import "/tools/common.typ": *
// shape: the grammar collected; each rule has its examples in its clause

#set heading(numbering: (..n) => if n.pos().len() == 1 { "Annex A" } else { "A." + n.pos().slice(1).map(str).join(".") })
= The whole grammar (normative) <annex-grammar>

#para("1")[This annex collects the syntax rules of the clauses in one place. The notation is that of ISO/IEC 14977:
`a b` is sequence, `a | b` choice, `[ a ]` optional, `{ a }` zero or more times, `(* … *)` a remark, `? … ?` a rule
stated in words. Names in capitals are the tokens of clause 6. Any number of spaces and remarks may stand between
tokens.]

#para("2")[The grammar is read with one token of lookahead. The exception is a parameter of a signature, where the token
after it tells whether it is a name or a type. What a name refers to (a local, a global, a word) and the types on the
stack are settled by the declarations and checks, not by the grammar.]

```ebnf
(* 6: tokens *)
NAME        = letter { letter | digit } ;                (* not a reserved word *)
PATH        = NAME "." member { "." member } ;           (* member: a run of letters and digits *)
INT         = [ "+" | "-" ] ( digits | "0x" hex-digits | "0b" bit-digits ) ;
CHAR        = [ "U" ] "'" ( char-byte | escape ) "'" ;
STRING      = '"' { string-byte | escape } '"' ;
RAWSTRING   = "r" '"' { raw-byte } '"' ;
TEXT        = "text" TAG newline { text-line newline } TAG ;
BOOL        = "true" | "false" ;
literal     = INT | CHAR | STRING | RAWSTRING | TEXT | BOOL ;

(* 6.7, 13: a source *)
source      = [ "pinax" INT ] { item } ;
item        = import | forward | declaration | definition | row-item | assign-item | body ;
import      = "use" NAME [ "as" NAME ] ;
forward     = "forward" ( "array" | "list" | "pool" ) NAME
            | "forward" "word" NAME "(" effect ")" ;

(* 8: declarations *)
declaration = [ "export" ] ( [ "bound" ] bindable | decl ) ;
bindable    = "global" NAME type | "array" NAME type size | "buffer" NAME size ;
decl        = "const" NAME literal
            | "list" NAME type size | "queue" NAME type size | "pool" NAME NAME size
            | "record" NAME { field | "big_endian" } "end"
            | "type" NAME ( "int" | "bool" | "str" )
            | "index" NAME NAME PATH
            | "tasks" NAME size [ size [ size ] ]
            | "extern" c-name "(" effect ")" "from" STRING [ "as" NAME ]
            | "test" NAME { body } "end" ;
size        = INT | NAME ;
field       = NAME NAME [ "(" size ")" ] ;
c-name      = NAME | reserved-word | STRING ;
row-item    = [ "export" ] "row" NAME [ NAME ] "(" value { "," value } ")" ;
assign-item = "assign" NAME "(" value { "," value } ")" ;
value       = literal | NAME ;

(* 9: words *)
definition  = [ "export" ] "word" NAME "(" effect ")" { body } "end" ;
effect      = [ params ] [ "gives" params ] [ "or" "fail" ] ;
params      = param { "," param } ;
param       = [ NAME ] type ;
type        = [ "owned" ] ( NAME [ "(" size ")" ] | "word" "(" effect ")" ) ;

(* 9 to 11: the body *)
body        = literal | NAME | PATH | store | block | cat | guard | control | defer
            | "return" | "fail" | ( "while" | "until" | "break" ) [ "(" NAME ")" ] ;
store       = "to" ( NAME | PATH | "(" NAME { "," NAME } ")" ) ;
block       = "do" [ "(" effect ")" ] { body } "end" ;
cat         = "cat" "(" phrase { "," phrase } ")" ;
phrase      = body { body } ;
guard       = "or_fail" | "or_return" | "or_else" [ "(" NAME ")" ] { body } "end" ;
control     = "if" { body } [ "else" { body } ] "end"
            | "loop" [ "(" NAME ")" ] { body } "end"
            | "case" { branch } [ "otherwise" { body } "end" ] "end" ;
defer       = "defer" { body } "end" ;             (* 10.7 *)
branch      = ( case-value | "(" case-value { "," case-value } ")" ) "of" { body } "end" ;
case-value  = literal | NAME ;
```

#para("3")[A source read by data load follows `data-source` of 13.4.]
