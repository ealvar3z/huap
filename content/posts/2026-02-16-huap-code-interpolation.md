# Using `$code` Interpolation in huap

huap can pull source files directly into a Markdown page before rendering.

## Full file include

This includes the full file (with snippet marker lines removed):

$code ./examples/demo.c

## Named snippet include

This includes only one named snippet from the same file:

$code ./examples/demo.c [sum]

And this includes another snippet without brackets:

$code ./examples/demo.c greet
