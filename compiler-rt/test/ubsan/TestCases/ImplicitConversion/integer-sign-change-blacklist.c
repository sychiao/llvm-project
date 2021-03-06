// FIXME: https://code.google.com/p/address-sanitizer/issues/detail?id=316
// I'm not sure this is actually *that* issue, but this seems oddly similar to the other XFAIL'ed cases.
// XFAIL: android
// UNSUPPORTED: ios

// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change                           -O0 %s -o %t && not %run %t 2>&1 | FileCheck %s --implicit-check-not="implicit conversion"
// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change                           -O1 %s -o %t && not %run %t 2>&1 | FileCheck %s --implicit-check-not="implicit conversion"
// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change                           -O2 %s -o %t && not %run %t 2>&1 | FileCheck %s --implicit-check-not="implicit conversion"
// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change                           -O3 %s -o %t && not %run %t 2>&1 | FileCheck %s --implicit-check-not="implicit conversion"

// RUN: rm -f %tmp
// RUN: echo "[implicit-integer-sign-change]" >> %tmp
// RUN: echo "fun:implicitSignChange" >> %tmp
// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change -fsanitize-blacklist=%tmp -O0 %s -o %t && not %run %t 2>&1 | not FileCheck %s
// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change -fsanitize-blacklist=%tmp -O1 %s -o %t && not %run %t 2>&1 | not FileCheck %s
// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change -fsanitize-blacklist=%tmp -O2 %s -o %t && not %run %t 2>&1 | not FileCheck %s
// RUN: %clang -fsanitize=implicit-integer-sign-change -fno-sanitize-recover=implicit-integer-sign-change -fsanitize-blacklist=%tmp -O3 %s -o %t && not %run %t 2>&1 | not FileCheck %s

#include <stdint.h>

int32_t implicitSignChange(uint32_t argc) {
  return argc; // BOOM
// CHECK: {{.*}}integer-sign-change-blacklist.c:[[@LINE-1]]:10: runtime error: implicit conversion from type '{{.*}}' (aka 'unsigned int') of value 4294967295 (32-bit, unsigned) to type '{{.*}}' (aka 'int') changed the value to -1 (32-bit, signed)
}

int main(int argc, char **argv) {
  return implicitSignChange(~0U);
}
