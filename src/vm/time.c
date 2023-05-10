#include "time.h"

static Value timeNative(DictuVM *vm, int argCount, Value *args) {
    UNUSED(vm); UNUSED(argCount); UNUSED(args);

    return NUMBER_VAL((double) time(NULL));
}

static Value clockNative(DictuVM *vm, int argCount, Value *args) {
    UNUSED(vm); UNUSED(argCount); UNUSED(args);

    return NUMBER_VAL((double) clock() / CLOCKS_PER_SEC);
}


static Value sleepNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "sleep() takes 1 argument (%d given)", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "sleep() argument must be a number");
        return EMPTY_VAL;
    }

    double stopTime = AS_NUMBER(args[0]);

#ifdef _WIN32
    Sleep(stopTime * 1000);
#elif _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    ts.tv_sec = stopTime;
    ts.tv_nsec = fmod(stopTime, 1) * 1000000000;
    nanosleep(&ts, NULL);
#else
    if (stopTime >= 1)
      sleep(stopTime);

    // 1000000 = 1 second
    usleep(fmod(stopTime, 1) * 1000000);
#endif

    return NIL_VAL;
}


Value createTimeModule(DictuVM *vm) {
    ObjString *name = copyString(vm, "Time", 4);
    push(vm, OBJ_VAL(name));
    ObjModule *module = newModule(vm, name);
    push(vm, OBJ_VAL(module));

    /**
     * Define Time methods
     */
    defineNative(vm, &module->values, "time", timeNative);
    defineNative(vm, &module->values, "clock", clockNative);
    defineNative(vm, &module->values, "sleep", sleepNative);
    

    /**
     * Define time properties
     */
    /*
    if (!vm->repl) {
        // Set argv variable
        initArgv(vm, &module->values, vm->argc, vm->argv);
    }

    initPlatform(vm, &module->values);
    */

    pop(vm);
    pop(vm);

    return OBJ_VAL(module);
}

