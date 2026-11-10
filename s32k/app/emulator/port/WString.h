/*
 * Copy of test/emul/WString.h with toFloat() added, which comm_nvm.cpp needs, and floats formatted
 * without streams: std::ostringstream and std::to_string(float) pulled the iostream, stringstream and
 * locale parts of libstdc++ into the image (about 100 KB), and the printf of this C library has no float
 * support anyway. Comes first in the include path of the S32K core.
 */
#include <stdlib.h>
#ifndef WSTRING_H
#define WSTRING_H

#include <math.h>
#include <stdint.h>
#include <string>

class String {
 private:
  std::string data;

  // Fixed point text like Arduino's String(float, decimals): "nan", "inf", "ovf" past 4294967040
  static std::string fixed(double value, unsigned int decimals) {
    if (isnan(value)) {
      return "nan";
    }
    if (isinf(value)) {
      return "inf";
    }
    if (value > 4294967040.0 || value < -4294967040.0) {
      return "ovf";
    }
    if (decimals > 9) {
      decimals = 9;
    }
    uint64_t scale = 1;
    for (unsigned int i = 0; i < decimals; i++) {
      scale *= 10;
    }
    const bool negative = value < 0;
    const uint64_t n = (uint64_t)((negative ? -value : value) * (double)scale + 0.5);
    std::string out = negative && n != 0 ? "-" : "";
    out += std::to_string(n / scale);
    if (decimals > 0) {
      std::string frac = std::to_string(n % scale);
      out += '.';
      out.append(decimals - frac.size(), '0');
      out += frac;
    }
    return out;
  }

 public:
  // Constructors
  String() = default;
  String(const char* s) : data(s) {}
  String(const std::string& s) : data(s) {}
  String(const String& other) = default;
  String(String&& other) = default;

  // Numeric constructors (Arduino-style)
  String(int value) { data = std::to_string(value); }
  String(unsigned int value) { data = std::to_string(value); }
  String(long value) { data = std::to_string(value); }
  String(unsigned long value) { data = std::to_string(value); }
  String(unsigned long long value) { data = std::to_string(value); }
  // Arduino's default is 2 decimals
  String(float value) { data = fixed(value, 2); }
  String(double value) { data = fixed(value, 2); }
  String(float value, int decimalPlaces) { data = fixed(value, decimalPlaces < 0 ? 0 : decimalPlaces); }
  String(double value, int decimalPlaces) { data = fixed(value, decimalPlaces < 0 ? 0 : decimalPlaces); }

  // Arduino's String(value, base), for example String(x, HEX): lower case digits, a sign only in base 10.
  // Before these the integer went to the float constructor and came out with "base" decimals.
  String(unsigned char value, int base) { data = in_base(value, base); }
  String(int value, int base) { data = signed_in_base(value, base); }
  String(unsigned int value, int base) { data = in_base(value, base); }
  String(long value, int base) { data = signed_in_base(value, base); }
  String(unsigned long value, int base) { data = in_base(value, base); }

 private:
  static std::string in_base(unsigned long long value, int base) {
    if (base < 2 || base > 36) {
      base = 10;
    }
    std::string out;
    do {
      const int digit = (int)(value % (unsigned)base);
      out.insert(out.begin(), (char)(digit < 10 ? '0' + digit : 'a' + digit - 10));
      value /= (unsigned)base;
    } while (value != 0);
    return out;
  }
  static std::string signed_in_base(long value, int base) {
    if (base == 10 && value < 0) {
      return "-" + in_base((unsigned long long)(-(long long)value), 10);
    }
    return in_base((unsigned long)value, base);
  }

 public:

  // Assignment operators
  String& operator=(const String& other) = default;
  String& operator=(String&& other) = default;

  // Conversion operator to std::string
  operator std::string() const { return data; }

  // This host shim does not simulate allocation failures; empty strings are valid too.
  explicit operator bool() const { return true; }

  // Accessor
  const std::string& str() const { return data; }

  // Comparison operators
  bool operator==(const String& rhs) const { return data == rhs.data; }

  // Concatenation
  String operator+(const String& rhs) const { return String(data + rhs.data); }

  String operator+(const std::string& rhs) const { return String(data + rhs); }

  String operator+(const char* rhs) const { return String(data + std::string(rhs)); }

  // Append
  bool concat(const char* rhs) {
    if (!rhs) {
      return false;
    }
    data += rhs;
    return true;
  }

  bool concat(const String& rhs) {
    data += rhs.data;
    return true;
  }

  bool concat(char rhs) {
    data += rhs;
    return true;
  }

  String& operator+=(const String& rhs) {
    data += rhs.data;
    return *this;
  }

  String& operator+=(const std::string& rhs) {
    data += rhs;
    return *this;
  }

  String& operator+=(const char* rhs) {
    data += rhs;
    return *this;
  }

  // Arduino-like methods (example)
  int length() const { return static_cast<int>(data.length()); }
  bool isEmpty() const { return data.empty(); }
  float toFloat() const { return strtof(c_str(), nullptr); }
  const char* c_str() const { return data.c_str(); }

  // Arduino String::reserve returns bool; pre-allocates capacity.
  bool reserve(unsigned int size) {
    data.reserve(size);
    return true;
  }

  // Friend functions to allow std::string + String
  friend String operator+(const std::string& lhs, const String& rhs) { return String(lhs + rhs.data); }

  friend String operator+(const char* lhs, const String& rhs) { return String(std::string(lhs) + rhs.data); }
};
#endif
