"""C11 tokenizer for the cfw static analyzer.

Produces a flat token stream. Comments are discarded, string and character
literals are single tokens, and preprocessor lines are returned as single
``pp`` tokens so callers can treat them as opaque text (no macro expansion).
"""

from __future__ import annotations

from dataclasses import dataclass

KEYWORDS = frozenset(
    """
    auto break case char const continue default do double else enum extern
    float for goto if inline int long register restrict return short signed
    sizeof static struct switch typedef union unsigned void volatile while
    _Bool _Complex _Imaginary _Alignas _Alignof _Atomic _Generic _Noreturn
    _Static_assert _Thread_local
    """.split()
)

# Multi-character punctuators, longest first.
PUNCTS = [
    "<<=", ">>=", "...", "->", "++", "--", "<=", ">=", "==", "!=",
    "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
    "##",
    "(", ")", "[", "]", "{", "}", ";", ",", ".", "?", ":", "~", "!",
    "^", "%", "&", "*", "+", "-", "/", "=", "<", ">", "|", "#",
]
_PUNCT_SET = set(PUNCTS)


@dataclass(frozen=True)
class Token:
    kind: str  # 'id', 'kw', 'num', 'str', 'chr', 'punct', 'pp', 'eof'
    text: str
    line: int
    col: int


def _is_id_start(c: str) -> bool:
    return c == "_" or c.isalpha()


def _is_id_char(c: str) -> bool:
    return c == "_" or c.isalnum()


def _skip_block_comment(src: str, i: int, n: int) -> int:
    # src[i:i+2] == '/*'
    j = i + 2
    while j + 1 < n:
        if src[j] == "*" and src[j + 1] == "/":
            return j + 2
        j += 1
    return n


def _skip_line_comment(src: str, i: int, n: int) -> int:
    # src[i:i+2] == '//'
    j = i + 2
    while j < n and src[j] != "\n":
        j += 1
    return j


def _lex_string(src: str, i: int, n: int, quote: str):
    """Lex a string/char literal starting at i (src[i] == quote)."""
    j = i + 1
    while j < n:
        c = src[j]
        if c == "\\":
            j += 2
            continue
        if c == "\n":
            break  # unterminated; stop at newline
        if c == quote:
            return src[i : j + 1], j + 1
        j += 1
    return src[i:j], j


def _literal_prefix(src: str, i: int, n: int):
    """Return (prefix_len, quote) for u8/u/U/L string and char literals."""
    for prefix in ("u8", "u", "U", "L"):
        plen = len(prefix)
        if src.startswith(prefix, i) and i + plen < n \
                and src[i + plen] in "\"'":
            return plen, src[i + plen]
    return None


def _lex_number(src: str, i: int, n: int):
    j = i
    # consume mantissa/exponent loosely: alnum, '_', '.', '\'',
    # plus '+'/'-' right after e/E/p/P
    while j < n:
        c = src[j]
        if c.isalnum() or c in "_.'":
            j += 1
            continue
        if c in "+-" and j > i and src[j - 1] in "eEpP":
            j += 1
            continue
        break
    return src[i:j], j


def _line_start(src: str, i: int) -> bool:
    """True if position i is the first non-whitespace char of a logical line."""
    j = i - 1
    while j >= 0 and src[j] in " \t":
        j -= 1
    return j < 0 or src[j] == "\n"


def _lex_pp(src: str, i: int, n: int):
    """Consume a preprocessor line starting at '#' through its newline,
    honoring backslash-newline continuations."""
    j = i
    while j < n:
        c = src[j]
        if c == "\\" and j + 1 < n and src[j + 1] == "\n":
            j += 2
            continue
        if c == "\n":
            return src[i : j + 1], j + 1
        j += 1
    return src[i:j], j


def tokenize(src: str):
    """Return a list of Tokens for C source text."""
    tokens = []
    n = len(src)
    i = 0
    line = 1
    col = 1

    def advance(s: str):
        nonlocal line, col
        for ch in s:
            if ch == "\n":
                line += 1
                col = 1
            else:
                col += 1

    while i < n:
        c = src[i]

        # whitespace and line splicing
        if c in " \t\r\v\f":
            i += 1
            col += 1
            continue
        if c == "\n":
            i += 1
            line += 1
            col = 1
            continue
        if c == "\\" and i + 1 < n and src[i + 1] == "\n":
            i += 2
            line += 1
            col = 1
            continue

        # comments
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = _skip_line_comment(src, i, n)
            advance(src[i:j])
            i = j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = _skip_block_comment(src, i, n)
            advance(src[i:j])
            i = j
            continue

        # preprocessor line: '#' as first non-ws char of a logical line
        if c == "#" and _line_start(src, i):
            text, j = _lex_pp(src, i, n)
            tokens.append(Token("pp", text, line, col))
            advance(text)
            i = j
            continue

        # encoding-prefixed string/char literals (u8"...", L'x', ...)
        prefix = _literal_prefix(src, i, n)
        if prefix is not None:
            plen, quote = prefix
            lit, j = _lex_string(src, i + plen, n, quote)
            text = src[i:i + plen] + lit
            kind = "str" if quote == '"' else "chr"
            tokens.append(Token(kind, text, line, col))
            advance(text)
            i = i + plen + len(lit)
            continue

        # identifiers / keywords
        if _is_id_start(c):
            j = i + 1
            while j < n and _is_id_char(src[j]):
                j += 1
            text = src[i:j]
            kind = "kw" if text in KEYWORDS else "id"
            tokens.append(Token(kind, text, line, col))
            advance(text)
            i = j
            continue

        # numbers
        if c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            text, j = _lex_number(src, i, n)
            tokens.append(Token("num", text, line, col))
            advance(text)
            i = j
            continue

        # string / char literal
        if c in "\"'":
            text, j = _lex_string(src, i, n, c)
            kind = "str" if c == '"' else "chr"
            tokens.append(Token(kind, text, line, col))
            advance(text)
            i = j
            continue

        # punctuation (longest match)
        matched = None
        for p in PUNCTS:
            if src.startswith(p, i):
                matched = p
                break
        if matched is not None:
            tokens.append(Token("punct", matched, line, col))
            advance(matched)
            i += len(matched)
            continue

        # unknown char: emit as punct to keep positions, skip
        tokens.append(Token("punct", c, line, col))
        i += 1
        col += 1

    tokens.append(Token("eof", "", line, col))
    return tokens
