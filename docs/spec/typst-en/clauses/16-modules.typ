#import "/tools/common.typ": *
// shape: an introduction; the modules are the files it includes

= Standard modules <std>

#para("1")[The standard modules come with the processor. They are opened with `use MODULE` and written `MODULE.WORD`
(13.1). Words of standard modules cannot reach outside without the host --- they use no files, clocks or environment.]

#para("2")[Words of standard modules make no new storage. Their results are one of:
- a view of the bytes they were given; nothing is copied;
- a view of new bytes written in the region (12.2);
- bytes appended to a buffer the caller gave. Such words end in `_into` or take a buffer, and fail with `err.full`
  when there is not enough room.]

#include "16-1-str.typ"
#include "16-2-fmt.typ"
#include "16-3-hash.typ"
#include "16-4-encode.typ"
#include "16-5-bytes.typ"
#include "16-6-ptn.typ"
#include "16-7-rex.typ"
#include "16-8-json.typ"
#include "16-9-csv.typ"
