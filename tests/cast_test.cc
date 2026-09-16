// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/cxx_cast.c: across classes with multiple, repeated and virtual
// bases, casts through the remembering __dynamic_cast give what libstdc++'s
// gives, the second time as the first, for objects of the same type at other
// addresses and while an object is being constructed.
//
//   make tests/cast_test && tests/cast_test

#include <cxxabi.h>

#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <typeinfo>

extern "C" void* __darwin___dynamic_cast(const void* source,
                                         const void* source_type,
                                         const void* destination_type,
                                         std::ptrdiff_t hint);

static int failures;

static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

struct Base {
  virtual ~Base() {}
  int b = 1;
};
struct Left : virtual Base {
  int l = 2;
};
struct Right : virtual Base {
  int r = 3;
};
struct Diamond : Left, Right {
  int d = 4;
};
struct Other {
  virtual ~Other() {}
  int o = 5;
};
struct Mixed : Other, Left {
  int m = 6;
};
struct RepeatA : Other {};
struct RepeatB : Other {};
struct Repeated : RepeatA, RepeatB {};  // Other twice: ambiguous

// Casts |source| (of static type |S|) to |D| both ways, twice.
template <typename S, typename D>
static void compare(S* source, const char* what) {
  auto s = static_cast<const abi::__class_type_info*>(&typeid(S));
  auto d = static_cast<const abi::__class_type_info*>(&typeid(D));
  for (int round = 0; round < 2; round++) {
    void* real = abi::__dynamic_cast(source, s, d, -1);
    void* cached = __darwin___dynamic_cast(source, s, d, -1);
    check(real == cached, what);
  }
}

struct Constructing : Left {
  Constructing() {
    // The object's vtable is Left's construction vtable here.
    Left* self = this;
    compare<Left, Constructing>(self, "during construction");
    compare<Left, Base>(self, "a virtual base during construction");
  }
};

int main() {
  Diamond d1, d2;
  Mixed m1, m2;
  Repeated r;
  for (Diamond* d : { &d1, &d2 }) {
    compare<Left, Right>(d, "Left to Right across a diamond");
    compare<Right, Diamond>(d, "Right down to the diamond");
    compare<Base, Left>(d, "a virtual base down to Left");
    compare<Base, Other>(d, "to an unrelated class");
  }
  for (Mixed* m : { &m1, &m2 }) {
    compare<Other, Left>(m, "Other to Left in a second base");
    compare<Left, Other>(m, "Left to Other");
    compare<Base, Mixed>(m, "a virtual base down to the object");
  }
  compare<RepeatA, Other>(&r, "a base the object has once from here");
  compare<RepeatB, Repeated>(&r, "the object from its second base");
  RepeatA* a = &r;
  compare<Other, Repeated>(a, "down from one of two copies of a base");
  Constructing c;
  compare<Left, Constructing>(&c, "after construction");
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
