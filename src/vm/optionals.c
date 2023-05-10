#include "optionals.h"

BuiltinModules modules[] = {
  {"Math", &createMathsModule, false},
  {"Time", &createTimeModule, false},
  {"Random", &createRandomModule, false},
  /*
    #ifndef DISABLE_UUID
    {"UUID", &createUuidModule, false},
    #endif
    {"UnitTest", &createUnitTestModule, true},
    {"Inspect", &createInspectModule, false},
    {"Object", &createObjectModule, true},
    #ifndef DISABLE_HTTP
    {"HTTP", &createHTTPModule, true},
    #endif
  */
  {NULL, NULL, false}
};

Value importBuiltinModule(DictuVM *vm, int index) {
  return modules[index].module(vm);
}

int findBuiltinModule(char *name, int length, bool *dictuSource) {
  for (int i = 0; modules[i].module != NULL; ++i) {
    if (strncmp(modules[i].name, name, length) == 0) {
      *dictuSource = modules[i].dictuSource;

      return i;
    }
  }

  return -1;
}
