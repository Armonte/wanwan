# UI blueprint

`app.jsx` is the design-canvas prototype this Slint port reproduces -- the
"APP - END-TO-END NAVIGABLE - 640x448" mock. It is the SPEC, not code we
build: nothing here is compiled, and no build step references it.

It lives in the repo because it was previously only in a session scratch
directory, one wipe away from being unrecoverable, while every layout
decision in `../ui/*.slint` cites it. When a port detail is in question,
this file is the answer -- read it rather than inferring the design from
the ImGui prototype or from an earlier screenshot.
