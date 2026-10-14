#import "/tools/common.typ": *
= Scope <scope>
#para("1")[This document defines a #term("processor").]
#part("Constraints")
#para("2")[A word that takes a string cannot be given an integer.]
#ex("spec", "hello.pnx")
#ex("spec", "hello-refused.pnx")
#qa[What?][This.]
#diag(([types differ], "an argument has another type"),)
