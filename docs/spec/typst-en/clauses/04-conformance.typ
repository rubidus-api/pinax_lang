#import "/tools/common.typ": *
// shape: conformance has no example of its own; every later clause gives them

= Conformance <conformance>

#para("1")[A #term("conforming program") is a set of sources that keeps all the syntax rules and constraints of this
document. A conforming program that stops with a fault while running is still conforming: a fault is part of what a
program means (11.3).]

#para("2")[A #term("conforming processor"):
+ accepts conforming programs and runs them as this document says;
+ refuses at load a source that breaks a syntax rule or a constraint, runs none of its code, and leaves the state as it
  was before the load;
+ reports the place of the breach as line and column; it may report only the first breach of a source;
+ reports the kind of a fault (annex B) and the line where it happened;
+ accepts the minimum sizes of annex C.]

#para("3")[The sentences of the diagnostics (the English messages in the tables of this document) are those of this
implementation. Another processor may word them differently. What it refuses shall be the same, and the place it points
at shall lie inside the offending token (this implementation points at the start of the token, or at the byte inside it
where the error shows). Lines and columns count from 1; columns count bytes.]

#para("4")[A processor may add words, modules and host functions not in this document. What it adds shall not change
the meaning of a conforming program, and shall not use the reserved words (6.6) or the names this document defines.]

#para("5")[What this document says it does not define (speed, the shape of instructions) is up to the processor. What
it says differs between processors (default capacities, for example) the processor shall document.]

#note[The line between refusing at load and faulting while running is the centre of the language. Types, the depth of
the stack, names, whether a failure is handled and whether an `owned` value is handed over exactly once are all settled
at load. Only what can be known while running --- does a value overflow, is an index in range --- becomes a fault.]
