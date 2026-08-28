# Exact Numeric Profile v1

This profile is the executable and cross-backend contract for DTESSL `int` and
`rational`. It deliberately contains no binary floating-point operation.

## Integer semantics

- `int` is a signed mathematical integer, not a host `long`, pointer-sized word
  or silently wrapping machine integer.
- Decimal source literals accept at most 4096 digits, excluding an optional
  sign.
- An executing value may use at most 1024 base-1,000,000,000 limbs (at most
  9216 decimal digits). An operation exceeding this deterministic profile limit
  fails the candidate round before commit.
- `+`, `-`, `*`, comparison and negation are exact.
- The public C++ integer `/` and `%` operations truncate toward zero and give a
  remainder with the dividend sign. Surface expression `a / b` produces an
  exact rational instead of discarding a remainder.
- Division by zero is an evaluation error; it never produces infinity, NaN or a
  partially committed state.

## Rational semantics

`rational` is a pair of arbitrary integers normalized at construction:

- denominator is strictly positive;
- numerator and denominator are divided by their positive gcd;
- zero has the single representation `0/1`;
- `+`, `-`, `*`, `/`, comparison and negation are exact;
- an int used with a rational is promoted to `int/1`;
- dividing by a zero rational rejects the round.

The surface literal is `numerator/denominator`. Whitespace and parentheses are
source details and do not affect the value. For example, `2/4`, `-1/-2` and
`1/2` all evaluate to the same value, whose text is `1/2`.

## Canonical encoding

- Values fitting signed int64 retain tag `01` and the original eight-byte
  two's-complement payload.
- Larger magnitudes use tag `0b`, a `00` positive or `01` negative sign byte,
  and a minimal unsigned big-endian magnitude. Using `0b` for an int64 value is
  rejected.
- Rational tag `0c` contains the canonical integer numerator and denominator.
  A non-positive denominator or reducible pair is rejected.

Golden vectors are listed in `CANONICAL_VALUE_V1.md` and asserted in the C++
test corpus. These rules are independent of C++ limb representation and must be
matched by VM/provider backends that advertise `exact-numeric`.

## Failure and resource boundary

Numeric errors are deterministic logical evaluation failures. The engine
computes candidate assignments before changing `round`, current state or
causal-writer metadata, so division by zero and magnitude exhaustion cannot
partially commit. A later gas system may impose a tighter run budget but cannot
change accepted arithmetic results.
