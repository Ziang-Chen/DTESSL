# Canonical Value Format v1

This format is a backend-neutral, length-delimited encoding for the value kinds
implemented in DTESSL `v0.1.0`. It is not the future canonical module format.

## Common rules

- One value occupies the entire byte sequence; trailing bytes are invalid.
- Lengths and counts use unsigned base-128 varints, least-significant group
  first. Redundant leading zero groups are invalid.
- Strings are length-prefixed byte sequences. The surface language intends
  UTF-8; strict UTF-8 validation is a later lexer/schema gate.
- Sets are strictly sorted by bytewise string order and contain no duplicates.
- Decoding is bounded by explicit byte, string and set-cardinality limits.

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

Unknown tags are invalid.

## Golden vectors

```text
false             00 00
true              00 01
42                01 00 00 00 00 00 00 00 2a
"a"               02 01 61
{"a", "b"}        03 02 01 61 01 62
```

## Canonicality

The encoder maps one supported `Value` to exactly one byte sequence. The
decoder rejects alternative spellings such as non-minimal lengths and unsorted
sets. Therefore `encode(decode(bytes)) == bytes` for every accepted sequence,
and `decode(encode(value)) == value` for every supported value.

Future kinds receive new tags. Existing tag meaning is immutable within format
v1. Records and exact rationals are introduced in later `v0.1.x` releases with
additional golden vectors. Recursive decoding is bounded by maximum depth,
collection cardinality, string size and total bytes.
