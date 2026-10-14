#import "/tools/common.typ": *
// shape: collected from the clauses

#set heading(numbering: (..n) => if n.pos().len() == 1 { "Annex B" } else { "B." + n.pos().slice(1).map(str).join(".") })
= Diagnostics (informative) <annex-diagnostics>

#para("1")[This annex collects the messages of the diagnostics tables of the clauses. They are the texts this
implementation prints (4 (3)). The left column is the clause that lists the message.]

#all-diagnostics()
