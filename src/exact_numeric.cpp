#include "dtessl/exact_numeric.hpp"

#include "dtessl/dtessl.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace dtessl {

struct ExactIntAccess {
  static int sign(const ExactInt& value) noexcept { return value.sign_; }
  static int& sign(ExactInt& value) noexcept { return value.sign_; }
  static const auto& limbs(const ExactInt& value) noexcept { return value.limbs_; }
  static auto& limbs(ExactInt& value) noexcept { return value.limbs_; }
  static constexpr std::uint32_t base() noexcept { return ExactInt::base; }
  static void normalize(ExactInt& value) noexcept { value.normalize(); }
};

namespace {

constexpr std::size_t max_exact_limbs = 1024;

void enforce_budget(const ExactInt& value) {
  if (ExactIntAccess::limbs(value).size() > max_exact_limbs) {
    throw Error("exact integer exceeds limb budget");
  }
}

int compare_abs(const ExactInt& left, const ExactInt& right) noexcept {
  const auto& lhs = ExactIntAccess::limbs(left);
  const auto& rhs = ExactIntAccess::limbs(right);
  if (lhs.size() != rhs.size()) return lhs.size() < rhs.size() ? -1 : 1;
  for (std::size_t index = lhs.size(); index > 0; --index) {
    if (lhs[index - 1U] != rhs[index - 1U]) {
      return lhs[index - 1U] < rhs[index - 1U] ? -1 : 1;
    }
  }
  return 0;
}

ExactInt abs_value(ExactInt value) {
  if (!value.is_zero()) ExactIntAccess::sign(value) = 1;
  return value;
}

ExactInt add_abs(const ExactInt& left, const ExactInt& right) {
  ExactInt result;
  ExactIntAccess::sign(result) = 1;
  auto& output = ExactIntAccess::limbs(result);
  const auto& lhs = ExactIntAccess::limbs(left);
  const auto& rhs = ExactIntAccess::limbs(right);
  output.resize(std::max(lhs.size(), rhs.size()));
  std::uint64_t carry = 0;
  for (std::size_t index = 0; index < output.size(); ++index) {
    const std::uint64_t sum = carry + (index < lhs.size() ? lhs[index] : 0U) +
                              (index < rhs.size() ? rhs[index] : 0U);
    output[index] = static_cast<std::uint32_t>(sum % ExactIntAccess::base());
    carry = sum / ExactIntAccess::base();
  }
  if (carry != 0) output.push_back(static_cast<std::uint32_t>(carry));
  ExactIntAccess::normalize(result);
  enforce_budget(result);
  return result;
}

ExactInt subtract_abs(const ExactInt& larger, const ExactInt& smaller) {
  ExactInt result;
  ExactIntAccess::sign(result) = 1;
  auto& output = ExactIntAccess::limbs(result);
  const auto& lhs = ExactIntAccess::limbs(larger);
  const auto& rhs = ExactIntAccess::limbs(smaller);
  output.resize(lhs.size());
  std::int64_t borrow = 0;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    std::int64_t value = static_cast<std::int64_t>(lhs[index]) - borrow -
                         (index < rhs.size() ? static_cast<std::int64_t>(rhs[index]) : 0);
    if (value < 0) {
      value += ExactIntAccess::base();
      borrow = 1;
    } else {
      borrow = 0;
    }
    output[index] = static_cast<std::uint32_t>(value);
  }
  ExactIntAccess::normalize(result);
  enforce_budget(result);
  return result;
}

ExactInt multiply_small(const ExactInt& value, std::uint32_t factor) {
  if (factor == 0 || value.is_zero()) return ExactInt{};
  ExactInt result;
  ExactIntAccess::sign(result) = 1;
  auto& output = ExactIntAccess::limbs(result);
  const auto& input = ExactIntAccess::limbs(value);
  output.resize(input.size());
  std::uint64_t carry = 0;
  for (std::size_t index = 0; index < input.size(); ++index) {
    const std::uint64_t product = static_cast<std::uint64_t>(input[index]) * factor + carry;
    output[index] = static_cast<std::uint32_t>(product % ExactIntAccess::base());
    carry = product / ExactIntAccess::base();
  }
  if (carry != 0) output.push_back(static_cast<std::uint32_t>(carry));
  return result;
}

void multiply_add_small(ExactInt& value, std::uint32_t factor, std::uint32_t addend) {
  auto& limbs = ExactIntAccess::limbs(value);
  std::uint64_t carry = addend;
  for (std::uint32_t& limb : limbs) {
    const std::uint64_t product = static_cast<std::uint64_t>(limb) * factor + carry;
    limb = static_cast<std::uint32_t>(product % ExactIntAccess::base());
    carry = product / ExactIntAccess::base();
  }
  if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
  if (!limbs.empty()) ExactIntAccess::sign(value) = 1;
  ExactIntAccess::normalize(value);
  enforce_budget(value);
}

std::pair<ExactInt, ExactInt> divmod_abs(const ExactInt& dividend,
                                        const ExactInt& divisor) {
  if (divisor.is_zero()) throw Error("integer division by zero");
  if (compare_abs(dividend, divisor) < 0) return {ExactInt{}, dividend};
  ExactInt quotient;
  ExactIntAccess::sign(quotient) = 1;
  ExactIntAccess::limbs(quotient).assign(ExactIntAccess::limbs(dividend).size(), 0U);
  ExactInt remainder;
  const auto& input = ExactIntAccess::limbs(dividend);
  for (std::size_t index = input.size(); index > 0; --index) {
    auto& remainder_limbs = ExactIntAccess::limbs(remainder);
    remainder_limbs.insert(remainder_limbs.begin(), input[index - 1U]);
    ExactIntAccess::sign(remainder) = 1;
    ExactIntAccess::normalize(remainder);

    std::uint32_t low = 0;
    std::uint32_t high = ExactIntAccess::base() - 1U;
    std::uint32_t digit = 0;
    while (low <= high) {
      const std::uint32_t middle = low + (high - low) / 2U;
      const ExactInt product = multiply_small(divisor, middle);
      const int order = compare_abs(product, remainder);
      if (order <= 0) {
        digit = middle;
        if (middle == ExactIntAccess::base() - 1U) break;
        low = middle + 1U;
      } else {
        if (middle == 0) break;
        high = middle - 1U;
      }
    }
    if (digit != 0) remainder = subtract_abs(remainder, multiply_small(divisor, digit));
    ExactIntAccess::limbs(quotient)[index - 1U] = digit;
  }
  ExactIntAccess::normalize(quotient);
  ExactIntAccess::normalize(remainder);
  enforce_budget(quotient);
  return {std::move(quotient), std::move(remainder)};
}

ExactInt gcd(ExactInt left, ExactInt right) {
  left = abs_value(std::move(left));
  right = abs_value(std::move(right));
  while (!right.is_zero()) {
    ExactInt remainder = left % right;
    left = std::move(right);
    right = std::move(remainder);
  }
  return left;
}

}  // namespace

ExactInt::ExactInt(std::int64_t value) {
  if (value == 0) return;
  sign_ = value < 0 ? -1 : 1;
  std::uint64_t magnitude = value < 0
                                ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                                : static_cast<std::uint64_t>(value);
  while (magnitude != 0) {
    limbs_.push_back(static_cast<std::uint32_t>(magnitude % base));
    magnitude /= base;
  }
}

ExactInt ExactInt::parse(std::string_view decimal, std::size_t max_digits) {
  if (decimal.empty()) throw Error("empty integer literal");
  bool negative = false;
  if (decimal.front() == '-' || decimal.front() == '+') {
    negative = decimal.front() == '-';
    decimal.remove_prefix(1);
  }
  if (decimal.empty() || decimal.size() > max_digits) {
    throw Error("integer literal exceeds digit limit");
  }
  ExactInt result;
  for (const char character : decimal) {
    if (character < '0' || character > '9') throw Error("invalid integer literal");
    multiply_add_small(result, 10U, static_cast<std::uint32_t>(character - '0'));
  }
  if (negative && !result.is_zero()) result.sign_ = -1;
  return result;
}

ExactInt ExactInt::from_magnitude_bytes(bool negative,
                                        std::span<const std::uint8_t> magnitude) {
  if (magnitude.empty() || magnitude.front() == 0U) {
    throw Error("non-canonical integer magnitude");
  }
  ExactInt result;
  for (const std::uint8_t byte : magnitude) multiply_add_small(result, 256U, byte);
  if (negative) result.sign_ = -1;
  return result;
}

std::string ExactInt::text() const {
  if (is_zero()) return "0";
  std::string result = sign_ < 0 ? "-" : "";
  result += std::to_string(limbs_.back());
  for (std::size_t index = limbs_.size() - 1U; index > 0; --index) {
    std::string limb = std::to_string(limbs_[index - 1U]);
    result.append(9U - limb.size(), '0');
    result += limb;
  }
  return result;
}

bool ExactInt::is_zero() const noexcept { return sign_ == 0; }
bool ExactInt::is_negative() const noexcept { return sign_ < 0; }

bool ExactInt::fits_int64() const noexcept {
  std::uint64_t magnitude = 0;
  for (std::size_t index = limbs_.size(); index > 0; --index) {
    if (magnitude > (std::numeric_limits<std::uint64_t>::max() - limbs_[index - 1U]) / base) {
      return false;
    }
    magnitude = magnitude * base + limbs_[index - 1U];
  }
  const std::uint64_t limit = sign_ < 0
                                  ? (std::uint64_t{1} << 63U)
                                  : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return magnitude <= limit;
}

std::int64_t ExactInt::to_int64() const {
  if (!fits_int64()) throw Error("integer does not fit int64");
  std::uint64_t magnitude = 0;
  for (std::size_t index = limbs_.size(); index > 0; --index) {
    magnitude = magnitude * base + limbs_[index - 1U];
  }
  if (sign_ >= 0) return static_cast<std::int64_t>(magnitude);
  if (magnitude == (std::uint64_t{1} << 63U)) return std::numeric_limits<std::int64_t>::min();
  return -static_cast<std::int64_t>(magnitude);
}

std::vector<std::uint8_t> ExactInt::magnitude_bytes() const {
  ExactInt copy = abs_value(*this);
  std::vector<std::uint8_t> reversed;
  while (!copy.is_zero()) {
    std::uint64_t remainder = 0;
    auto& limbs = ExactIntAccess::limbs(copy);
    for (std::size_t index = limbs.size(); index > 0; --index) {
      const std::uint64_t current = remainder * base + limbs[index - 1U];
      limbs[index - 1U] = static_cast<std::uint32_t>(current / 256U);
      remainder = current % 256U;
    }
    reversed.push_back(static_cast<std::uint8_t>(remainder));
    ExactIntAccess::normalize(copy);
  }
  return {reversed.rbegin(), reversed.rend()};
}

void ExactInt::normalize() noexcept {
  while (!limbs_.empty() && limbs_.back() == 0U) limbs_.pop_back();
  if (limbs_.empty()) sign_ = 0;
}

int compare(const ExactInt& left, const ExactInt& right) noexcept {
  if (ExactIntAccess::sign(left) != ExactIntAccess::sign(right)) {
    return ExactIntAccess::sign(left) < ExactIntAccess::sign(right) ? -1 : 1;
  }
  if (left.is_zero()) return 0;
  const int order = compare_abs(left, right);
  return left.is_negative() ? -order : order;
}

ExactInt operator-(const ExactInt& value) {
  ExactInt result = value;
  ExactIntAccess::sign(result) = -ExactIntAccess::sign(result);
  return result;
}

ExactInt operator+(const ExactInt& left, const ExactInt& right) {
  if (left.is_zero()) return right;
  if (right.is_zero()) return left;
  if (ExactIntAccess::sign(left) == ExactIntAccess::sign(right)) {
    ExactInt result = add_abs(left, right);
    ExactIntAccess::sign(result) = ExactIntAccess::sign(left);
    return result;
  }
  const int order = compare_abs(left, right);
  if (order == 0) return ExactInt{};
  ExactInt result = order > 0 ? subtract_abs(left, right) : subtract_abs(right, left);
  ExactIntAccess::sign(result) = order > 0 ? ExactIntAccess::sign(left)
                                           : ExactIntAccess::sign(right);
  return result;
}

ExactInt operator-(const ExactInt& left, const ExactInt& right) { return left + (-right); }

ExactInt operator*(const ExactInt& left, const ExactInt& right) {
  if (left.is_zero() || right.is_zero()) return ExactInt{};
  ExactInt result;
  ExactIntAccess::sign(result) = ExactIntAccess::sign(left) * ExactIntAccess::sign(right);
  const auto& lhs = ExactIntAccess::limbs(left);
  const auto& rhs = ExactIntAccess::limbs(right);
  auto& output = ExactIntAccess::limbs(result);
  output.assign(lhs.size() + rhs.size(), 0U);
  for (std::size_t left_index = 0; left_index < lhs.size(); ++left_index) {
    std::uint64_t carry = 0;
    for (std::size_t right_index = 0; right_index < rhs.size(); ++right_index) {
      const std::size_t target = left_index + right_index;
      const std::uint64_t current = output[target] + carry +
                                    static_cast<std::uint64_t>(lhs[left_index]) * rhs[right_index];
      output[target] = static_cast<std::uint32_t>(current % ExactIntAccess::base());
      carry = current / ExactIntAccess::base();
    }
    std::size_t target = left_index + rhs.size();
    while (carry != 0) {
      const std::uint64_t current = output[target] + carry;
      output[target] = static_cast<std::uint32_t>(current % ExactIntAccess::base());
      carry = current / ExactIntAccess::base();
      ++target;
      if (target == output.size() && carry != 0) output.push_back(0U);
    }
  }
  ExactIntAccess::normalize(result);
  enforce_budget(result);
  return result;
}

ExactInt operator/(const ExactInt& left, const ExactInt& right) {
  auto [quotient, remainder] = divmod_abs(abs_value(left), abs_value(right));
  static_cast<void>(remainder);
  if (!quotient.is_zero()) {
    ExactIntAccess::sign(quotient) = ExactIntAccess::sign(left) * ExactIntAccess::sign(right);
  }
  return quotient;
}

ExactInt operator%(const ExactInt& left, const ExactInt& right) {
  auto [quotient, remainder] = divmod_abs(abs_value(left), abs_value(right));
  static_cast<void>(quotient);
  if (!remainder.is_zero()) ExactIntAccess::sign(remainder) = ExactIntAccess::sign(left);
  return remainder;
}

Rational::Rational() = default;
Rational::Rational(std::int64_t numerator, std::int64_t denominator)
    : Rational(ExactInt(numerator), ExactInt(denominator)) {}

Rational::Rational(ExactInt numerator, ExactInt denominator) {
  if (denominator.is_zero()) throw Error("rational denominator must not be zero");
  if (denominator.is_negative()) {
    numerator = -numerator;
    denominator = -denominator;
  }
  const ExactInt divisor = gcd(numerator, denominator);
  numerator_ = numerator / divisor;
  denominator_ = denominator / divisor;
}

std::string Rational::text() const {
  return numerator_.text() + "/" + denominator_.text();
}

int compare(const Rational& left, const Rational& right) {
  return compare(left.numerator_ * right.denominator_,
                 right.numerator_ * left.denominator_);
}

Rational operator-(const Rational& value) {
  return Rational(-value.numerator_, value.denominator_);
}

Rational operator+(const Rational& left, const Rational& right) {
  return Rational(left.numerator_ * right.denominator_ +
                      right.numerator_ * left.denominator_,
                  left.denominator_ * right.denominator_);
}

Rational operator-(const Rational& left, const Rational& right) { return left + (-right); }

Rational operator*(const Rational& left, const Rational& right) {
  return Rational(left.numerator_ * right.numerator_,
                  left.denominator_ * right.denominator_);
}

Rational operator/(const Rational& left, const Rational& right) {
  if (right.numerator_.is_zero()) throw Error("rational division by zero");
  return Rational(left.numerator_ * right.denominator_,
                  left.denominator_ * right.numerator_);
}

}  // namespace dtessl
