"""Rewrites of the game's preprocessed text for the ARM build, with libclang (like port/tools/ptr32.py, whose
helpers this uses).

shifts(): a shift by a count outside 0..31 is undefined in C, and the machines disagree: the PS2 and x86 use the low
5 bits of the count (6 for a 64-bit value), an ARM processor shifts everything out and gives 0. Whether the game
ever shifts out of range is not known, so the question is removed: every shift of the game's text whose count is
not a literal number gets the PS2's rule written out,
        a << n    ->    a << ((n) & 31)          (& 63 when the shifted value is 64 bits wide)
which changes nothing for counts in range and costs one instruction.

divs(): the game's float divisions with the PS2's answer for a zero divisor (the largest number, not an infinity or
a NaN; port/include/port_compat.h __port_fdiv):
        a / b     ->    __port_fdiv(a, b)              (a float division whose divisor is not a number written out)
        a /= b    ->    __port_fdiv_to(&(a), b)        (a float divided in place)
inside functions only (an initialiser of a static is left alone)."""
import ptr32
from clang.cindex import CursorKind as K, TokenKind, TypeKind

def shifts(data, args):
    """Returns (new text, number of shifts rewritten). data: bytes of a preprocessed file."""
    game = ptr32.game_test(data)
    tu = ptr32.parse(data, args)
    edits = []  # insertions: (offset, 0, text, order)

    def strip(c):
        while c.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR):
            ch = list(c.get_children())
            if len(ch) != 1:
                break
            c = ch[0]
        return c

    def walk(c, depth):
        for ch in c.get_children():
            walk(ch, depth + 1)
        if c.kind not in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR) or not game(c.extent.start.offset):
            return
        ch = list(c.get_children())
        if len(ch) != 2:
            return
        lo, hi = ch[0].extent.end.offset, ch[1].extent.start.offset
        if hi <= lo or hi - lo > 64:
            return
        op = data[lo:hi].strip(b" \t\r\n()")
        if op not in (b"<<", b">>", b"<<=", b">>="):
            return
        if strip(ch[1]).kind == K.INTEGER_LITERAL:
            return
        wide = (ch[0].type if c.kind == K.COMPOUND_ASSIGNMENT_OPERATOR else c.type).get_canonical().get_size() == 8
        s, e = ch[1].extent.start.offset, ch[1].extent.end.offset
        if e <= s:
            return
        edits.append((s, 0, "((", depth))                                 # of two at one place, the outer one first
        edits.append((e, 0, ") & 63)" if wide else ") & 31)", -depth))    # ... and the inner one closes first
    walk(tu.cursor, 0)
    return ptr32.apply(data, edits), len(edits) // 2


def divs(data, args):
    """Returns (new text, number of divisions rewritten). data: bytes of a preprocessed file."""
    game = ptr32.game_test(data)
    tu = ptr32.parse(data, args)
    edits = []

    def literal_nonzero(c):
        """a divisor written as a number other than 0 (in parentheses or not): nothing to rewrite"""
        toks = [t for t in c.get_tokens() if t.spelling not in ("(", ")")]
        if len(toks) != 1 or toks[0].kind != TokenKind.LITERAL:
            return False
        t = toks[0].spelling.lower()
        try:
            if t.startswith("0x") and "p" in t:  # (the float constants as port/tools/eeconst.py writes them)
                return float.fromhex(t.rstrip("fl")) != 0.0
            if any(ch in t for ch in ".e") and not t.startswith("0x"):
                return float(t.rstrip("fl")) != 0.0
            return int(t.rstrip("ul"), 0) != 0
        except ValueError:
            return False

    def walk(c, depth, infn):
        infn = infn or c.kind == K.FUNCTION_DECL
        for ch in c.get_children():
            walk(ch, depth + 1, infn)
        if not infn or c.kind not in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR) or not game(c.extent.start.offset):
            return
        ch = list(c.get_children())
        if len(ch) != 2:
            return
        lo, hi = ch[0].extent.end.offset, ch[1].extent.start.offset
        if hi <= lo or hi - lo > 16:
            return
        op = data[lo:hi].strip(b" \t\r\n")
        if op == b"/" and c.kind == K.BINARY_OPERATOR:
            if c.type.get_canonical().kind != TypeKind.FLOAT or literal_nonzero(ch[1]):
                return
            head, mid = "__port_fdiv(", ", "
        elif op == b"/=" and c.kind == K.COMPOUND_ASSIGNMENT_OPERATOR:
            if ch[0].type.get_canonical().kind != TypeKind.FLOAT or literal_nonzero(ch[1]):
                return
            head, mid = "__port_fdiv_to(&(", "), "
        else:
            return
        s0, e1 = ch[0].extent.start.offset, ch[1].extent.end.offset
        if s0 >= lo or e1 <= hi:
            return
        edits.append((s0, 0, head, depth))      # (of two at one place, the outer one first)
        edits.append((lo, hi - lo, mid, 0))
        edits.append((e1, 0, ")", -depth))      # (... and the inner one closes first)
    walk(tu.cursor, 0, False)
    return ptr32.apply(data, edits), len(edits) // 3
