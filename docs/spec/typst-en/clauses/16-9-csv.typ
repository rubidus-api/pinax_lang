#import "/tools/common.typ": *

== `csv` --- CSV <std-csv>

#para("1")[The module `csv` reads and writes CSV in the form of RFC 4180: fields are separated by commas; a field that
holds a comma, a quote or a line break is quoted, and quotes inside it are doubled.]

#para("2")[`s csv.fields` is a stem (10.6) of the fields of one line. A quoted field is given without its quotes and
with doubled quotes made single. An unclosed quote gives the rest of the text as the last field (a stem cannot report a
failure).]

#para("3")[The writing words append to a buffer (`or fail`, `err.full` when there is not enough room). `s buf csv.put`,
`n buf csv.put_int` and `b buf csv.put_bool` append one field (with the comma it needs); `buf csv.end_line` ends the
line. Booleans are written `true` and `false`.]

#para("4")[`row buf csv.pack` appends one row of a record as one line. `s row csv.unpack` reads one line into the fields
in order and gives what follows the line. The row is written only when every field is right (`err.format`).]

#ex("spec", "16-csv.pnx")

#part("Constraints")

#para("5")[`pack` and `unpack` take only rows of records.]
#ex("spec", "16-csv-pack-int.pnx")

#part("Diagnostics")
#diag(
  ([A value that is not a row of a record], "pack takes a row of a table of records and a buffer: row buf csv.pack"),
)
