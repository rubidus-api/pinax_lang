#import "/tools/common.typ": *
// shape: no rules a program can break

= Normative references <references>

#para("1")[The following documents are part of this one. Where an edition is given, only that edition applies;
otherwise the latest.]

#para("2")[ISO/IEC 9899:2024, _Programming languages --- C_ (C23). The host interface of clause 19 is written in C types
and calling conventions. The header `pinax.h` itself also reads as C99.]

#para("3")[The Unicode Standard and its definition of UTF-8. Source is UTF-8 (6.1); character literals and escapes
write Unicode code points (6.5).]

#para("4")[ISO/IEC 14977:1996, _Extended BNF_. The grammar follows its notation (6.2, annex A).]

#para("5")[RFC 4648 (Base16, Base64), RFC 3986 (percent-encoding of URIs), RFC 8259 (JSON), RFC 4180 (CSV). The
standard modules of clause 16 read and write these formats; each of those clauses states how far it follows them.]
