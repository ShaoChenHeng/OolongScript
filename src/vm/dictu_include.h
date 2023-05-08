#ifndef oolong_include_h
#define oolong_include_h

#include <stdbool.h>

typedef struct _vm DictuVM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} DictuInterpretResult;

DictuVM *dictuInitVM(bool repl, int argc, char *argv[]);

void dictuFreeVM(DictuVM *vm);

DictuInterpretResult dictuInterpret(DictuVM *vm, char *moduleName, char *source);

#endif
