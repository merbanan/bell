// Minimal stand-ins for the Arduino runtime, just enough to compile the real
// src/log.cpp on a workstation. The source under test is a byte-identical copy
// of the firmware file; only its #includes are shadowed.
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

class String {
public:
    String() : _s() {}
    String(const char *s) : _s(s ? s : "") {}
    String(char c) : _s(1, c) {}
    const char *c_str() const { return _s.c_str(); }
    String &operator+=(const String &o) { _s += o._s; return *this; }
    String &operator+=(const char *o) { _s += o; return *this; }
private:
    std::string _s;
};

inline String operator+(String a, const String &b) { a += b; return a; }

uint32_t millis();

class EspClass {
public:
    uint32_t getFreeHeap() const { return 40000; }
};
extern EspClass ESP;
