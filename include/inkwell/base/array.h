#pragma once

/*
 * Element count of a C array. Only valid for a real array - on a pointer it silently yields
 * the ratio of two pointer sizes, so never hand it a function parameter.
 *
 * A macro rather than the small static helper the style guide prefers, because there is no way
 * to write this as a function in C17: the type is different at every call site.
 */
#define INKWELL_ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
