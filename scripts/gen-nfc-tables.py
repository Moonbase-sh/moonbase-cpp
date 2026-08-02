#!/usr/bin/env python3
"""Generate the Unicode tables used by detail/unicode/nfc_ascii.hpp.

The device fingerprint spec canonicalizes every value as

    NFC  ->  keep only U+0020..U+007E  ->  truncate to 128  ->  trim spaces

and this SDK cannot depend on ICU. It does not need to: because the
printable-ASCII filter runs immediately after NFC, the only thing NFC can still
change about the output is *which printable-ASCII characters remain visible*.
That reduces to three enumerable effects, and this script derives all three from
the Python standard library's Unicode database.

    1. An ASCII starter is consumed by a following combining mark.
       "e" + U+0301 composes to a non-ASCII precomposed character, which the
       filter then drops whole, so the "e" must not survive either.
    2. A non-ASCII code point whose NFC form *is* a printable-ASCII character.
       Singleton decompositions are always composition-excluded, so the ASCII
       result stays exposed. A naive "strip everything non-ASCII" loses these.
    3. A combining mark that has its own canonical decomposition, which must be
       expanded before the blocking analysis in (1).

Usage:
    scripts/gen-nfc-tables.py                 # write the header
    scripts/gen-nfc-tables.py --stdout        # print it instead
    scripts/gen-nfc-tables.py --verify        # differential-test the algorithm
"""

from __future__ import annotations

import argparse
import itertools
import pathlib
import random
import sys
import unicodedata

MAX_CODE_POINT = 0x110000
PRINTABLE_MIN = 0x20
PRINTABLE_MAX = 0x7E

HEADER_PATH = (
    pathlib.Path(__file__).resolve().parent.parent
    / "include"
    / "moonbase"
    / "detail"
    / "unicode"
    / "nfc_tables.hpp"
)

# The blocks whose combining classes the shipped table carries.
#
# Deliberately not every combining mark in Unicode. The full table is 393 ranges
# and 8.5 KB of the header, five times these blocks, and it exists only to get the
# *blocking* rule right when an ASCII base carries marks from several scripts at
# once. Every composing mark and every self-decomposing mark lives in the first
# block below (asserted in collect()), so the behaviour the conformance vectors
# actually require is unaffected.
#
# The cost is one known divergence class, which --verify pins rather than ignores:
# a mark outside these blocks is treated as a starter, so it ends the cluster and a
# later composing mark never gets the chance to annihilate the base. That needs an
# ASCII base followed by a foreign-script combining mark followed by a Latin
# composing mark, which no IOKit UUID, machine-id, sysfs DMI string, SMBIOS string
# or host name contains. Every input of one or two code points is still exact.
COMBINING_BLOCKS = [
    (0x0300, 0x036F),  # Combining Diacritical Marks
    (0x1AB0, 0x1ACE),  # Combining Diacritical Marks Extended
    (0x1DC0, 0x1DFF),  # Combining Diacritical Marks Supplement
    (0x20D0, 0x20F0),  # Combining Diacritical Marks for Symbols
    (0xFE20, 0xFE2F),  # Combining Half Marks
]


def in_covered_blocks(code_point: int) -> bool:
    return any(low <= code_point <= high for low, high in COMBINING_BLOCKS)


def combining_class_ranges() -> list[tuple[int, int, int]]:
    """Non-zero combining classes within the covered blocks, run-collapsed."""
    ranges: list[tuple[int, int, int]] = []
    for low, high in COMBINING_BLOCKS:
        for cp in range(low, high + 1):
            ccc = unicodedata.combining(chr(cp))
            if ccc == 0:
                continue
            if ranges and ranges[-1][1] == cp - 1 and ranges[-1][2] == ccc:
                ranges[-1] = (ranges[-1][0], cp, ccc)
            else:
                ranges.append((cp, cp, ccc))
    return ranges


def ascii_compositions() -> dict[int, set[int]]:
    """base -> marks that form a primary composite with it, for printable-ASCII bases.

    A pair counts only if NFC actually recomposes it: that filters out the
    composition exclusions and any decomposition whose first element is not a
    starter, which are exactly the cases where the base would survive.
    """
    compositions: dict[int, set[int]] = {}
    for cp in range(MAX_CODE_POINT):
        decomposed = unicodedata.normalize("NFD", chr(cp))
        if len(decomposed) != 2:
            continue
        base, mark = ord(decomposed[0]), ord(decomposed[1])
        if not (PRINTABLE_MIN <= base <= PRINTABLE_MAX):
            continue
        if unicodedata.normalize("NFC", decomposed) != chr(cp):
            continue
        compositions.setdefault(base, set()).add(mark)
    return compositions


def ascii_singletons() -> list[tuple[int, int]]:
    """Non-ASCII code points whose NFC form is a single printable-ASCII character."""
    singletons: list[tuple[int, int]] = []
    for cp in range(MAX_CODE_POINT):
        if PRINTABLE_MIN <= cp <= PRINTABLE_MAX:
            continue
        composed = unicodedata.normalize("NFC", chr(cp))
        if len(composed) != 1:
            continue
        result = ord(composed)
        if result != cp and PRINTABLE_MIN <= result <= PRINTABLE_MAX:
            singletons.append((cp, result))
    return singletons


def mark_decompositions() -> list[tuple[int, tuple[int, ...]]]:
    """Combining marks that canonically decompose to other marks."""
    decompositions: list[tuple[int, tuple[int, ...]]] = []
    for cp in range(MAX_CODE_POINT):
        if unicodedata.combining(chr(cp)) == 0:
            continue
        decomposed = unicodedata.normalize("NFD", chr(cp))
        if decomposed != chr(cp):
            decompositions.append((cp, tuple(ord(c) for c in decomposed)))
    return decompositions


def collect() -> dict:
    compositions = ascii_compositions()
    marks = sorted({mark for marks in compositions.values() for mark in marks})

    # The C++ cluster walk assumes every composing mark is a non-starter, so that
    # a starter always terminates the preceding cluster. Verify rather than trust:
    # a ccc-0 composing partner would need a differently shaped algorithm.
    for mark in marks:
        assert unicodedata.combining(chr(mark)) != 0, f"U+{mark:04X} composes but has ccc 0"
        # And it must be inside the blocks the reduced table covers, or the
        # behaviour the vectors require would silently stop working.
        assert in_covered_blocks(mark), f"composing mark U+{mark:04X} is outside COMBINING_BLOCKS"

    for source, _ in mark_decompositions():
        assert in_covered_blocks(source), f"decomposable mark U+{source:04X} is outside COMBINING_BLOCKS"

    # And no composite of a printable-ASCII base is itself printable ASCII, so
    # "the base composed" always means "the result is filtered away".
    for base, base_marks in compositions.items():
        for mark in base_marks:
            composed = unicodedata.normalize("NFC", chr(base) + chr(mark))
            assert len(composed) == 1, f"U+{base:04X}+U+{mark:04X} did not compose"
            assert not (PRINTABLE_MIN <= ord(composed) <= PRINTABLE_MAX), (
                f"U+{base:04X}+U+{mark:04X} composes to printable ASCII"
            )

    assert len(marks) <= 32, f"{len(marks)} marks will not fit in a uint32_t mask"

    # A mark is blocked only by an earlier mark of the *same* combining class, so
    # a class no composing mark uses can neither compose nor block one and can be
    # ignored outright. Tracking only these classes turns "which classes have I
    # seen in this cluster" into a handful of bits.
    mark_classes = sorted({unicodedata.combining(chr(mark)) for mark in marks})
    assert len(mark_classes) <= 32, f"{len(mark_classes)} classes will not fit in a uint32_t mask"

    return {
        "unicode_version": unicodedata.unidata_version,
        "ccc_ranges": combining_class_ranges(),
        "compositions": compositions,
        "marks": marks,
        "mark_classes": mark_classes,
        "singletons": ascii_singletons(),
        "mark_decompositions": mark_decompositions(),
    }


def wrap(entries: list[str], per_line: int, indent: str = "    ") -> str:
    lines = []
    for chunk in range(0, len(entries), per_line):
        lines.append(indent + " ".join(entries[chunk : chunk + per_line]))
    return "\n".join(lines)


def render(data: dict) -> str:
    marks: list[int] = data["marks"]
    compositions: dict[int, set[int]] = data["compositions"]
    mark_bit = {mark: index for index, mark in enumerate(marks)}

    masks = []
    for base in range(PRINTABLE_MIN, PRINTABLE_MAX + 1):
        mask = 0
        for mark in compositions.get(base, ()):
            mask |= 1 << mark_bit[mark]
        masks.append(mask)

    mark_first, mark_last = marks[0], marks[-1]
    mark_index = [-1] * (mark_last - mark_first + 1)
    for mark in marks:
        mark_index[mark - mark_first] = mark_bit[mark]

    ccc_ranges = data["ccc_ranges"]
    singletons = data["singletons"]
    decompositions = data["mark_decompositions"]

    out = f"""#pragma once

// Generated by scripts/gen-nfc-tables.py from Unicode {data["unicode_version"]}. Do not edit by hand.
//
// Supporting data for detail/unicode/nfc_ascii.hpp, which answers exactly one
// question: after NFC, which printable-ASCII characters are still visible? See
// that header, and the generator, for why this is not a general NFC
// implementation and must never be reused as one.
//
// Stability: the Unicode Normalization Stability Policy freezes the composition
// table, the singleton mappings and the mark decompositions below, so those can
// never change. Only the combining-class ranges grow, as new scripts are
// encoded. A stale range can only matter for a value that mixes a printable
// ASCII base with a combining mark from a script this table predates, which no
// UUID, machine-id, DMI string, SMBIOS string or host name contains.

#include <cstddef>
#include <cstdint>

namespace moonbase::detail::unicode::tables {{

inline constexpr const char* unicode_version = "{data["unicode_version"]}";

// ---------------------------------------------------------------------------
// Canonical combining class, as run-collapsed ranges over the code points with
// a non-zero class. Needed both to tell a combining mark from a starter and to
// find the head of each equal-class run when testing whether a mark is blocked.

struct combining_class_range {{
    char32_t first;
    char32_t last;
    std::uint8_t combining_class;
}};

inline constexpr combining_class_range combining_class_ranges[] = {{
{wrap([f"{{0x{first:04X},0x{last:04X},{ccc}}}," for first, last, ccc in ccc_ranges], 6)}
}};

inline constexpr std::size_t combining_class_range_count =
    sizeof(combining_class_ranges) / sizeof(combining_class_ranges[0]);

// ---------------------------------------------------------------------------
// The {len(marks)} combining marks that can form a primary composite with a printable
// ASCII base, and a bitmask per base saying which. All of them fall in
// U+{mark_first:04X}..U+{mark_last:04X}, so a small index array resolves a mark to its bit in O(1).

inline constexpr char32_t composing_mark_first = 0x{mark_first:04X};
inline constexpr char32_t composing_mark_last = 0x{mark_last:04X};

inline constexpr std::int8_t composing_mark_index[] = {{
{wrap([f"{value}," for value in mark_index], 16)}
}};

// Indexed by (base - 0x20) for base in U+0020..U+007E. Bit i corresponds to the
// mark whose composing_mark_index value is i.
inline constexpr std::uint32_t composition_masks[] = {{
{wrap([f"0x{mask:08X}," for mask in masks], 6)}
}};

inline constexpr char32_t composition_mask_first = 0x{PRINTABLE_MIN:04X};
inline constexpr char32_t composition_mask_last = 0x{PRINTABLE_MAX:04X};

// The distinct combining classes those marks use. A mark is blocked only by an
// earlier mark of the same class, so a class absent from this list can neither
// compose with an ASCII base nor block something that would, and the cluster
// walk skips it. Position in this array is the bit used to remember that the
// class has already been seen in the current cluster.
inline constexpr std::uint8_t composing_mark_classes[] = {{
{wrap([f"{value}," for value in data["mark_classes"]], 8)}
}};

inline constexpr std::size_t composing_mark_class_count =
    sizeof(composing_mark_classes) / sizeof(composing_mark_classes[0]);

// ---------------------------------------------------------------------------
// Non-ASCII code points whose NFC form is a single printable-ASCII character.
// These are singleton decompositions, which are always composition-excluded, so
// NFC leaves the ASCII result exposed and it must survive the filter.

struct ascii_singleton {{
    char32_t from;
    char32_t to;
}};

inline constexpr ascii_singleton ascii_singletons[] = {{
{wrap([f"{{0x{source:04X},0x{target:04X}}}," for source, target in singletons], 4)}
}};

inline constexpr std::size_t ascii_singleton_count =
    sizeof(ascii_singletons) / sizeof(ascii_singletons[0]);

// ---------------------------------------------------------------------------
// Combining marks with a canonical decomposition of their own. They must be
// expanded in place before the blocking analysis, or a mark that decomposes to
// a composing one would fail to annihilate its base.

struct mark_decomposition {{
    char32_t from;
    char32_t to[2];
    std::uint8_t length;
}};

inline constexpr mark_decomposition mark_decompositions[] = {{
{wrap([
    "{{0x{:04X},{{0x{:04X},0x{:04X}}},{}}},".format(
        source, target[0], target[1] if len(target) > 1 else 0, len(target)
    )
    for source, target in decompositions
], 2)}
}};

inline constexpr std::size_t mark_decomposition_count =
    sizeof(mark_decompositions) / sizeof(mark_decompositions[0]);

}} // namespace moonbase::detail::unicode::tables
"""
    return out


# ---------------------------------------------------------------------------
# Verification: a Python transcription of the C++ algorithm, differential-tested
# against real NFC followed by the printable-ASCII filter.


class Reference:
    def __init__(self, data: dict) -> None:
        self.marks = data["marks"]
        self.mark_bit = {mark: index for index, mark in enumerate(self.marks)}
        self.compositions = data["compositions"]
        self.mark_classes = set(data["mark_classes"])
        self.singletons = dict(data["singletons"])
        self.decompositions = dict(data["mark_decompositions"])

    def combining(self, code_point: int) -> int:
        """Combining class as the *shipped table* reports it.

        Outside the covered blocks this is 0, which is exactly how the C++ lookup
        behaves, so this reference diverges from real NFC in the same places the
        SDK does.
        """
        if not in_covered_blocks(code_point):
            return 0
        return unicodedata.combining(chr(code_point))

    def composes(self, base: int, mark: int) -> bool:
        return mark in self.compositions.get(base, ())

    def canonicalize(self, text: str) -> str:
        points = [self.singletons.get(ord(c), ord(c)) for c in text]

        out: list[str] = []
        index = 0
        while index < len(points):
            starter = points[index]
            if self.combining(starter) != 0:
                index += 1
                continue

            marks: list[int] = []
            cursor = index + 1
            while cursor < len(points) and self.combining(points[cursor]) != 0:
                marks.extend(self.decompositions.get(points[cursor], (points[cursor],)))
                cursor += 1

            if PRINTABLE_MIN <= starter <= PRINTABLE_MAX:
                annihilated = False
                seen: set[int] = set()
                for mark in marks:
                    ccc = self.combining(mark)
                    # Mirrors the C++ walk exactly, including the restriction to
                    # classes a composing mark actually uses.
                    if ccc not in self.mark_classes or ccc in seen:
                        continue
                    seen.add(ccc)
                    if self.composes(starter, mark):
                        annihilated = True
                        break
                if not annihilated:
                    out.append(chr(starter))

            index = cursor

        return "".join(out)


def truth(text: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFC", text) if PRINTABLE_MIN <= ord(c) <= PRINTABLE_MAX)


def is_subsequence(needle: str, haystack: str) -> bool:
    """Is every character of `needle` present in `haystack`, in order?"""
    iterator = iter(haystack)
    return all(character in iterator for character in needle)


def has_uncovered_mark(text: str) -> bool:
    """Does the string contain a real combining mark the shipped table omits?

    This is the sole licence for diverging from real NFC. Anything else is a bug.
    """
    return any(unicodedata.combining(c) != 0 and not in_covered_blocks(ord(c)) for c in text)


def verify(data: dict) -> int:
    reference = Reference(data)
    failures = 0
    accepted = 0
    accepted_example: str | None = None
    checked = 0
    long_only = 0

    def check(text: str) -> None:
        nonlocal failures, accepted, accepted_example, checked, long_only
        checked += 1
        got, want = reference.canonicalize(text), truth(text)
        if got == want:
            return

        codes = " ".join(f"U+{ord(c):04X}" for c in text)

        # A divergence is only permitted where the reduced combining-class table
        # is the cause. Anything else means the algorithm itself is wrong.
        if not has_uncovered_mark(text):
            failures += 1
            if failures <= 20:
                print(f"  UNEXPECTED MISMATCH {codes}: got {got!r}, want {want!r}", file=sys.stderr)
            return

        # And it must fail in the conservative direction: bases survive that real
        # NFC would have removed, never the reverse. A *subsequence* test, not a
        # substring one: the surviving base can sit anywhere in the string, so
        # "98" vs "9x8" is the expected shape rather than a violation.
        if not is_subsequence(want, got):
            failures += 1
            if failures <= 20:
                print(f"  WRONG-DIRECTION DIVERGENCE {codes}: got {got!r}, want {want!r}", file=sys.stderr)
            return

        # The guarantee that makes this trade safe: no realistic fingerprint value
        # is affected, and every short input is exact.
        if len(text) <= 2:
            long_only += 1
            print(f"  SHORT-INPUT DIVERGENCE {codes}: got {got!r}, want {want!r}", file=sys.stderr)

        accepted += 1
        if accepted_example is None:
            accepted_example = f"{codes}: reduced {got!r}, real NFC {want!r}"

    print("verifying: every single code point")
    for cp in range(MAX_CODE_POINT):
        check(chr(cp))

    print("verifying: every printable-ASCII base x every combining mark")
    all_marks = [cp for cp in range(MAX_CODE_POINT) if unicodedata.combining(chr(cp)) != 0]
    for base in range(PRINTABLE_MIN, PRINTABLE_MAX + 1):
        for mark in all_marks:
            check(chr(base) + chr(mark))

    print("verifying: every code point x a spread of composing marks")
    probes = [0x0301, 0x0308, 0x0300, 0x0327, 0x0338, 0x0323, 0x0344, 0x0341, 0x0483, 0x0591, 0x1AB0]
    for cp in range(MAX_CODE_POINT):
        for mark in probes:
            check(chr(cp) + chr(mark))

    print("verifying: base x mark x mark, exhaustively for a sample of bases")
    for base in "aeiouAEIOUcnsyzCNSYZ=":
        for first, second in itertools.product(all_marks, repeat=2):
            check(base + chr(first) + chr(second))

    print("verifying: random strings")
    rng = random.Random(20260729)
    alphabet = (
        [chr(cp) for cp in range(PRINTABLE_MIN, PRINTABLE_MAX + 1)]
        + [chr(mark) for mark in all_marks[:200]]
        + [chr(cp) for cp, _ in data["singletons"]]
        + ["é", "中", " ", "퟿", "￿", "\U0001F600"]
    )
    for _ in range(400_000):
        length = rng.randint(1, 8)
        check("".join(rng.choice(alphabet) for _ in range(length)))

    print(f"\nchecked {checked:,} strings")
    print(f"  unexpected mismatches:     {failures}")
    print(f"  accepted divergences:      {accepted:,} ({accepted / checked:.4%})")
    print("    all of the form: ASCII base + a combining mark outside COMBINING_BLOCKS")
    print("    + a Latin composing mark, where the reduced table keeps the base.")
    if accepted_example:
        print(f"    example: {accepted_example}")
    print(f"  short-input divergences:   {long_only} (must be 0)")

    if long_only:
        print("\nFAILED: an input of one or two code points diverged. The claim that every"
              "\nrealistic fingerprint value is exact no longer holds.", file=sys.stderr)

    return 1 if (failures or long_only) else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdout", action="store_true", help="print the header instead of writing it")
    parser.add_argument("--verify", action="store_true", help="differential-test the algorithm against NFC")
    args = parser.parse_args()

    data = collect()

    if args.verify:
        return verify(data)

    header = render(data)
    if args.stdout:
        sys.stdout.write(header)
        return 0

    HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    HEADER_PATH.write_text(header, encoding="utf-8")
    print(f"wrote {HEADER_PATH} (Unicode {data['unicode_version']})")
    print(f"  {len(data['ccc_ranges'])} combining-class ranges")
    print(f"  {len(data['marks'])} composing marks over {len(data['compositions'])} ASCII bases")
    print(f"  {len(data['singletons'])} ASCII-exposing singletons")
    print(f"  {len(data['mark_decompositions'])} decomposable marks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
