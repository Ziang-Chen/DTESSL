# Canonical Value Format v1

This format is a backend-neutral, length-delimited encoding for the value kinds
implemented since DTESSL `v0.1.0` and extended additively in `v0.1.x`. It is
not the future canonical module format.

## Common rules

- One value occupies the entire byte sequence; trailing bytes are invalid.
- Lengths and counts use unsigned base-128 varints, least-significant group
  first. Redundant leading zero groups are invalid.
- Strings are length-prefixed byte sequences. The surface language intends
  UTF-8; strict UTF-8 validation is a later lexer/schema gate.
- Sets are strictly sorted by bytewise string order and contain no duplicates.
- Decoding is bounded by explicit byte, string and set-cardinality limits.
- Record fields are strictly sorted by field name. Nominal type identities and
  variant constructor identities are encoded as strings and cannot be empty.

## Tags

| Tag | Value | Payload |
| --- | --- | --- |
| `00` | bool | exactly one byte: `00` or `01` |
| `01` | signed int64 | exactly eight bytes, big-endian two's complement |
| `02` | string | varuint byte length followed by bytes |
| `03` | set<string> | varuint count followed by count string payloads without string tags |
| `04` | list | varuint count followed by canonical values in semantic order |
| `05` | generic set | varuint count followed by strictly canonical-sorted values |
| `06` | map | varuint count followed by strictly key-sorted key/value canonical pairs |
| `07` | bag | varuint distinct-item count followed by sorted value/positive-varuint-count pairs |
| `08` | record | type identity, field count, then strictly name-sorted name/value pairs |
| `09` | variant/enum/option/result | type identity, constructor identity, payload count (zero or one), then payload |
| `0a` | newtype | type identity followed by exactly one canonical value |
| `0b` | big integer | sign byte, magnitude-byte count, minimal unsigned big-endian magnitude |
| `0c` | rational | canonical integer numerator followed by canonical positive integer denominator |
| `0d` | tuple | positive arity followed by that many canonical values |
| `0e` | relation | positive arity, row count, then strictly tuple-sorted row fields |

Unknown tags are invalid.

## Golden vectors

```text
false             00 00
true              00 01
42                01 00 00 00 00 00 00 00 2a
"a"               02 01 61
{"a", "b"}        03 02 01 61 01 62
SessionId("x")    0a 09 53 65 73 73 69 6f 6e 49 64 02 01 78
Phase.Running     09 05 50 68 61 73 65 07 52 75 6e 6e 69 6e 67 00
9223372036854775808  0b 00 08 80 00 00 00 00 00 00 00
1/2               0c 01 00 00 00 00 00 00 00 01 01 00 00 00 00 00 00 00 02
("edge", 7)       0d 02 02 04 65 64 67 65 01 00 00 00 00 00 00 00 07
{("a", 1)}        0e 02 01 02 01 61 01 00 00 00 00 00 00 00 01
```

## Canonicality

The encoder maps one supported `Value` to exactly one byte sequence. The
decoder rejects alternative spellings such as non-minimal lengths and unsorted
sets. Therefore `encode(decode(bytes)) == bytes` for every accepted sequence,
and `decode(encode(value)) == value` for every supported value.

Future kinds receive new tags. Existing tag meaning is immutable within format
v1. Big integers outside int64 must use tag `0b`; values inside int64 must use
the original tag `01`. Rational components must already be gcd-normalized with
a positive denominator, so decoding never accepts bytes whose re-encoding would
change. Recursive decoding is bounded by maximum depth,
collection cardinality, string size and total bytes.
