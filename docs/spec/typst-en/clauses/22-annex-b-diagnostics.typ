#import "/tools/common.typ": *
// shape: collected from the clauses

#set heading(numbering: (..n) => if n.pos().len() == 1 { "Annex B" } else { "B." + n.pos().slice(1).map(str).join(".") })
= Diagnostics (informative) <annex-diagnostics>

#para("1")[This annex collects the messages of the diagnostics tables of the clauses. They are the texts this
implementation prints (4 (3)). The left column is the clause that lists the message.]

#para("2")[A refusal is printed as `file:line:column: message`. The line and column are those of the token at which the
processor knew: something missing is known at the token after it, a value left over at the end of the definition. In
a message, `name`, `s`, `i` and `buf` stand for places, not words, and the short code after a colon shows the right
form.]

#all-diagnostics()
