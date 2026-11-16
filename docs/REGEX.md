# Regex and lexer contract

The lexer implements maximal munch: longest matched byte sequence first, then higher priority, then earlier declaration. Keywords and anonymous literals win ties with general regex tokens. `context do` uses the same competition rules; use lexer modes when text and identifier patterns should apply in different contexts.

## Supported byte dialect

- Literal UTF-8 byte sequences; escape regex metacharacters when meant literally.
- Concatenation, `|`, `(...)`, `(?:...)`, `*`, `+`, `?`, `{n}`, `{n,}`, `{n,m}`.
- ASCII byte classes/ranges such as `[a-z]`, negation `[^x]`, literal `]` in the first class position and literal `-` at the end.
- `.` matches one byte except CR/LF; it is not a Unicode-codepoint matcher.
- `\n`, `\r`, `\t`, `\f`, `\v`, `\0`, exactly two hex digits in `\xHH`.
- ASCII `\d`, `\w`, `\s` and complements `\D`, `\W`, `\S`; inside a class `\b` means backspace.
- POSIX classes `space alpha alnum digit upper lower xdigit blank punct print cntrl`. These are ASCII and do not depend on the process locale.

Unsupported syntax is an error, including anchors `^/$` outside classes, word boundaries outside classes, backreferences, lookaround, named groups, Unicode escapes/properties, unknown escapes/classes, lazy/possessive/repeated quantifiers, malformed/unclosed groups/classes, trailing patterns and reversed ranges. Raw non-ASCII characters in a class are rejected instead of splitting Unicode characters into unrelated byte alternatives. Use literal alternatives, e.g. `/(а|б|в)/`, for a small explicit set.

Limits: pattern 8192 bytes, group nesting 64, numeric repetition bound 1024, estimated compiled NFA budget 4096 states. Nested repetitions are checked before expansion. Empty patterns can be represented internally; lexer tokens still must consume input, and repetition parsers stop on zero progress.

## Grammar string escapes

DSL quoted strings support `\n \r \t \f \v \b \0 \" \\' \\` and `\xHH`. Unknown escapes are diagnosed. UTF-8 text can be written literally. C17 constants use fixed-width octal escaping and escaped question marks; long constants use byte-array compound literals, avoiding C's hex-escape ambiguity, trigraphs and overlength-string limits.