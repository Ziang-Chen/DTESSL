#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dtessl {

// Deterministic signed arbitrary-precision integer. Limbs are an implementation
// detail; source and canonical encodings never depend on host word size.
class ExactInt {
 public:
  ExactInt() = default;
  ExactInt(std::int64_t value);

  [[nodiscard]] static ExactInt parse(std::string_view decimal,
                                      std::size_t max_digits = 4096);
  [[nodiscard]] static ExactInt from_magnitude_bytes(
      bool negative, std::span<const std::uint8_t> magnitude);

  [[nodiscard]] std::string text() const;
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] bool is_negative() const noexcept;
  [[nodiscard]] bool fits_int64() const noexcept;
  [[nodiscard]] std::int64_t to_int64() const;
  [[nodiscard]] std::vector<std::uint8_t> magnitude_bytes() const;

  friend bool operator==(const ExactInt&, const ExactInt&) = default;
  friend int compare(const ExactInt& left, const ExactInt& right) noexcept;
  friend ExactInt operator-(const ExactInt& value);
  friend ExactInt operator+(const ExactInt& left, const ExactInt& right);
  friend ExactInt operator-(const ExactInt& left, const ExactInt& right);
  friend ExactInt operator*(const ExactInt& left, const ExactInt& right);
  friend ExactInt operator/(const ExactInt& left, const ExactInt& right);
  friend ExactInt operator%(const ExactInt& left, const ExactInt& right);

 private:
  friend struct ExactIntAccess;
  static constexpr std::uint32_t base = 1'000'000'000U;
  int sign_{0};
  std::vector<std::uint32_t> limbs_;

  void normalize() noexcept;
};

class Rational {
 public:
  Rational();
  Rational(ExactInt numerator, ExactInt denominator);
  Rational(std::int64_t numerator, std::int64_t denominator);

  [[nodiscard]] const ExactInt& numerator() const noexcept { return numerator_; }
  [[nodiscard]] const ExactInt& denominator() const noexcept { return denominator_; }
  [[nodiscard]] std::string text() const;

  friend bool operator==(const Rational&, const Rational&) = default;
  friend int compare(const Rational& left, const Rational& right);
  friend Rational operator-(const Rational& value);
  friend Rational operator+(const Rational& left, const Rational& right);
  friend Rational operator-(const Rational& left, const Rational& right);
  friend Rational operator*(const Rational& left, const Rational& right);
  friend Rational operator/(const Rational& left, const Rational& right);

 private:
  ExactInt numerator_{0};
  ExactInt denominator_{1};
};

}  // namespace dtessl
