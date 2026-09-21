#pragma once

/*
 * Just enough JSON to walk a document whose shape you already know.
 *
 * A cursor over the text and nothing else: no allocation, no tree, no ownership. You enter an
 * object, ask for its keys in the order they appear, read the value you wanted and skip the
 * ones you did not. Everything it can answer is a value copied into a buffer you named, so a
 * document can be read without ever holding a second copy of it.
 *
 * Why it walks structure rather than scanning for `"key":`, which is shorter and works on a
 * small document: the moment a document is large and carries free text, a scanner is wrong. A
 * 150 KB index whose every entry holds a page of prose written by whoever merged a change is
 * text that contains quotes, braces and, sooner or later, a `"zip_url":` of its own - and a
 * scanner finds that one. So a string is skipped as a string here, and a key is only a key when
 * it is one.
 *
 * Malformed input is a false return and a cursor left where it was, never a read past the end -
 * this reads bytes off the network, and `make fuzz` exists for exactly this shape of code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How deep a skipped value may nest before the reader gives up.
 *
 * Skipping is iterative and counts rather than recurses, so depth costs nothing but a number;
 * the limit is here so a document that is a megabyte of open brackets is refused rather than
 * walked. Upstream's deepest is four.
 */
#define INKWELL_JSON_MAX_DEPTH 32U

struct inkwell_json {
    const char *cursor;
    const char *end;
    /*
     * Whether the object or array being walked has yielded anything yet - which is the whole of
     * what it takes to *require* the separator between members rather than merely allow it.
     *
     * Set by entering a container and cleared by the first thing taken out of it, so a copy of
     * the cursor resumes with the same answer. Nesting needs no stack: an inner walk leaves it
     * false, and false is exactly right for the outer container, which by then is past its own
     * first member.
     */
    bool at_first;
};

/* `len` may be 0 for a NUL-terminated string. */
void inkwell_json_init(struct inkwell_json *json, const char *text, size_t len);

/*
 * Steps into the object or array the cursor is on. False when it is on something else, which
 * is how a caller tells "this document is not the shape I was told" from "this key is absent".
 */
bool inkwell_json_enter_object(struct inkwell_json *json);
bool inkwell_json_enter_array(struct inkwell_json *json);

/*
 * Reads the next key of the object being walked and leaves the cursor on its value. False at
 * the closing brace, which it consumes - so a loop over the keys ends with the cursor after
 * the object, ready for whatever follows it.
 *
 * The comma before every member but the first is **required**, not merely allowed: a document
 * missing one is malformed, and a reader that walked it anyway would be reporting the contents
 * of something it had already decided it could not trust.
 *
 * The caller must read or skip the value before asking for the next key.
 */
bool inkwell_json_next_key(struct inkwell_json *json, char *out, size_t out_len);

/*
 * True when the array being walked has another element, leaving the cursor on it. False at the
 * closing bracket, which it consumes.
 */
bool inkwell_json_next_element(struct inkwell_json *json);

/*
 * Reads the value the cursor is on and steps past it. A type that does not match is not
 * consumed, so a caller that guessed wrong can still skip it.
 *
 * Strings are unescaped, \uXXXX included; a lone surrogate becomes '?' rather than a broken
 * sequence, because what comes out of here goes on to be measured in cells by src/utils/text.c.
 * A string longer than the buffer is truncated at a whole character and still consumed - the
 * cursor's job is to stay in step with the document, not with the caller's storage.
 */
bool inkwell_json_read_string(struct inkwell_json *json, char *out, size_t out_len);
bool inkwell_json_read_u64(struct inkwell_json *json, uint64_t *out);
bool inkwell_json_read_bool(struct inkwell_json *json, bool *out);

/* Steps past whatever the cursor is on, however deeply nested. False on malformed input or on
   a document nested deeper than INKWELL_JSON_MAX_DEPTH. */
bool inkwell_json_skip_value(struct inkwell_json *json);

/*
 * Walks the object the cursor is on to the value of `key`, and leaves the cursor there.
 *
 * The common case in one call, and the one place order matters: it walks forward only, so a
 * caller reading several keys out of one object asks for them in the order the document lists
 * them - or keeps a copy of the cursor (the struct is a value; copying it is the idiom) and
 * searches again from there.
 */
bool inkwell_json_object_find(struct inkwell_json *json, const char *key);

#ifdef __cplusplus
}
#endif
