#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "vm.h"
#include "error.h"

#ifdef DEBUG_PRINT_CODE

#include "debug.h"

#endif

static Chunk *currentChunk(Compiler *compiler) {
  return &compiler->function->chunk;
}

static void errorAt(Parser *parser, Token *token, const char *message) {
  if (parser->panicMode) return;
  parser->panicMode = true;

  log_error("File '%s', {bold}line %d{reset}", parser->module->name->chars, token->line);

  switch (token->type) {
  case TOKEN_EOF:
    log_padln("Error at end: %s", message);
    break;
  case TOKEN_ERROR:
    log_padln("Error: %s", message);
    break;
  default:
    log_padln("%zu %s %.*s", token->line, "|", token->length, token->start);
    log_padln("%s", message);
    break;
  }

  fputc('\n', stderr);
  parser->hadError = true;
}

static void error(Parser *parser, const char *message) {
  errorAt(parser, &parser->previous, message);
}

static void errorAtCurrent(Parser *parser, const char *message) {
  errorAt(parser, &parser->current, message);
}

static void advance(Parser *parser) {
  parser->previous = parser->current;

  for (;;) {
    parser->current = scanToken(&parser->scanner);
    if (parser->current.type != TOKEN_ERROR) break;

    errorAtCurrent(parser, parser->current.start);
  }
}


static void consume(Compiler *compiler, TokenType type, const char *message) {
  if (compiler->parser->current.type == type) {
    advance(compiler->parser);
    return;
  }

  errorAtCurrent(compiler->parser, message);
}

static bool check(Compiler *compiler, TokenType type) {
  return compiler->parser->current.type == type;
}

static bool match(Compiler *compiler, TokenType type) {
  if (!check(compiler, type)) return false;
  advance(compiler->parser);
  return true;
}

static void emitByte(Compiler *compiler, uint8_t byte) {
  writeChunk(compiler->parser->vm, currentChunk(compiler), byte, compiler->parser->previous.line);
}

static void emitBytes(Compiler *compiler, uint8_t byte1, uint8_t byte2) {
  emitByte(compiler, byte1);
  emitByte(compiler, byte2);
}

static void emitLoop(Compiler *compiler, int loopStart) {
  emitByte(compiler, OP_LOOP);

  int offset = currentChunk(compiler)->count - loopStart + 2;
  if (offset > UINT16_MAX) error(compiler->parser, "Loop body too large.");

  emitByte(compiler, (offset >> 8) & 0xff);
  emitByte(compiler, offset & 0xff);
}

// Emits [instruction] followed by a placeholder for a jump offset. The
// placeholder can be patched by calling [jumpPatch]. Returns the index
// of the placeholder.
static int emitJump(Compiler *compiler, uint8_t instruction) {
  emitByte(compiler, instruction);
  emitByte(compiler, 0xff);
  emitByte(compiler, 0xff);
  return currentChunk(compiler)->count - 2;
}

static void emitReturn(Compiler *compiler) {
  // An initializer automatically returns "this".
  if (compiler->type == TYPE_INITIALIZER) {
    emitBytes(compiler, OP_GET_LOCAL, 0);
  } else {
    emitByte(compiler, OP_NIL);
  }

  emitByte(compiler, OP_RETURN);
}

static uint8_t makeConstant(Compiler *compiler, Value value) {
  int constant = addConstant(compiler->parser->vm, currentChunk(compiler), value);
  if (constant > UINT8_MAX) {
    error(compiler->parser, "Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t) constant;
}

static void emitConstant(Compiler *compiler, Value value) {
  emitBytes(compiler, OP_CONSTANT, makeConstant(compiler, value));
}

// Replaces the placeholder argument for a previous CODE_JUMP or
// CODE_JUMP_IF instruction with an offset that jumps to the current
// end of bytecode.
static void patchJump(Compiler *compiler, int offset) {
  // -2 to adjust for the bytecode for the jump offset itself.
  int jump = currentChunk(compiler)->count - offset - 2;

  if (jump > UINT16_MAX) {
    error(compiler->parser, "Too much code to jump over.");
  }

  currentChunk(compiler)->code[offset] = (jump >> 8) & 0xff;
  currentChunk(compiler)->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Parser *parser, Compiler *compiler, Compiler *parent, FunctionType type, AccessLevel level) {
  compiler->parser = parser;
  compiler->enclosing = parent;
  initTable(&compiler->stringConstants);
  compiler->function = NULL;
  compiler->class = NULL;
  compiler->loop = NULL;
  compiler->withBlock = false;
  compiler->classAnnotations = NULL;
  compiler->methodAnnotations = NULL;

  if (parent != NULL) {
    compiler->class = parent->class;
  }

  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;

  parser->vm->compiler = compiler;

  compiler->function = newFunction(parser->vm, parser->module, type, level);

  switch (type) {
  case TYPE_INITIALIZER:
  case TYPE_METHOD:
  case TYPE_STATIC:
  case TYPE_FUNCTION: {
    compiler->function->name = copyString(
					parser->vm,
					parser->previous.start,
					parser->previous.length
					);
    break;
  }
  case TYPE_ARROW_FUNCTION: {
    compiler->function->name = copyString(
					parser->vm,
					"<anonymous>",
					11
					);
    break;
  }
  case TYPE_TOP_LEVEL: {
    compiler->function->name = NULL;
    break;
  }
  }

  Local *local = &compiler->locals[compiler->localCount++];
  local->depth = compiler->scopeDepth;
  local->isUpvalue = false;
  if (type == TYPE_METHOD || type == TYPE_INITIALIZER) {
    // In a method, it holds the receiver, "this".
    local->name.start = "this";
    local->name.length = 4;
  } else {
    // In a function, it holds the function, but cannot be referenced,
    // so has no name.
    local->name.start = "";
    local->name.length = 0;
  }
}

static ObjFunction *endCompiler(Compiler *compiler) {
  emitReturn(compiler);

  ObjFunction *function = compiler->function;
#ifdef DEBUG_PRINT_CODE
  if (!compiler->parser->hadError) {

    disassembleChunk(currentChunk(compiler),
		     function->name != NULL ? function->name->chars
		     : function->module->name->chars);
  }
#endif
  if (compiler->enclosing != NULL) {
    // Capture the upvalues in the new closure object.
    emitBytes(compiler->enclosing, OP_CLOSURE, makeConstant(compiler->enclosing, OBJ_VAL(function)));

    // Emit arguments for each upvalue to know whether to capture a local
    // or an upvalue.
    for (int i = 0; i < function->upvalueCount; i++) {
      emitByte(compiler->enclosing, compiler->upvalues[i].isLocal ? 1 : 0);
      emitByte(compiler->enclosing, compiler->upvalues[i].index);
    }
  }

  freeTable(compiler->parser->vm, &compiler->stringConstants);
  compiler->parser->vm->compiler = compiler->enclosing;
  return function;
}

static void beginScope(Compiler *compiler) {
  compiler->scopeDepth++;
}

static void endScope(Compiler *compiler) {
  compiler->scopeDepth--;

  while (compiler->localCount > 0 &&
	 compiler->locals[compiler->localCount - 1].depth >
	 compiler->scopeDepth) {

    if (compiler->locals[compiler->localCount - 1].isUpvalue) {
      emitByte(compiler, OP_CLOSE_UPVALUE);
    } else {
      emitByte(compiler, OP_POP);
    }
    compiler->localCount--;
  }
}

static void expression(Compiler *compiler);

static void statement(Compiler *compiler);

static void declaration(Compiler *compiler);

static ParseRule *getRule(TokenType type);

static void parsePrecedence(Compiler *compiler, Precedence precedence);

static uint8_t identifierConstant(Compiler *compiler, Token *name) {
  ObjString *string = copyString(compiler->parser->vm, name->start, name->length);
  Value indexValue;
  if (tableGet(&compiler->stringConstants, string, &indexValue)) {
    return (uint8_t) AS_NUMBER(indexValue);
  }

  uint8_t index = makeConstant(compiler, OBJ_VAL(string));
  tableSet(compiler->parser->vm, &compiler->stringConstants, string, NUMBER_VAL((double) index));
  return index;
}

static bool identifiersEqual(Token *a, Token *b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name, bool inFunction) {
  // Look it up in the local scopes. Look in reverse order so that the
  // most nested variable is found first and shadows outer ones.
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local *local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      if (!inFunction && local->depth == -1) {
	error(compiler->parser, "Cannot read local variable in its own initializer.");
      }
      return i;
    }
  }

  return -1;
}

// Adds an upvalue to [compiler]'s function with the given properties.
// Does not add one if an upvalue for that variable is already in the
// list. Returns the index of the upvalue.
static int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal, bool constant) {
  // Look for an existing one.
  int upvalueCount = compiler->function->upvalueCount;
  for (int i = 0; i < upvalueCount; i++) {
    Upvalue *upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  // If we got here, it's a new upvalue.
  if (upvalueCount == UINT8_COUNT) {
    error(compiler->parser, "Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  compiler->upvalues[upvalueCount].constant = constant;
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler *compiler, Token *name) {
    // If we are at the top level, we didn't find it.
    if (compiler->enclosing == NULL) return -1;

    // See if it's a local variable in the immediately enclosing function.
    int local = resolveLocal(compiler->enclosing, name, true);
    if (local != -1) {
        // Mark the local as an upvalue so we know to close it when it goes
        // out of scope.
        compiler->enclosing->locals[local].isUpvalue = true;
        return addUpvalue(compiler, (uint8_t) local, true, compiler->enclosing->locals[local].constant);
    }

    // See if it's an upvalue in the immediately enclosing function. In
    // other words, if it's a local variable in a non-immediately
    // enclosing function. This "flattens" closures automatically: it
    // adds upvalues to all of the intermediate functions to get from the
    // function where a local is declared all the way into the possibly
    // deeply nested function that is closing over it.
    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t) upvalue, false, compiler->enclosing->upvalues[upvalue].constant);
    }

    // If we got here, we walked all the way up the parent chain and
    // couldn't find it.
    return -1;
}

static void addLocal(Compiler *compiler, Token name) {
  if (compiler->localCount == UINT8_COUNT) {
    error(compiler->parser, "Too many local variables in function.");
    return;
  }

  Local *local = &compiler->locals[compiler->localCount];
  local->name = name;

  // The local is declared but not yet defined.
  local->depth = -1;
  local->isUpvalue = false;
  local->constant = false;
  compiler->localCount++;
}


// Allocates a local slot for the value currently on the stack, if
// we're in a local scope.
static void declareVariable(Compiler *compiler, Token *name) {
  // Global variables are implicitly declared.
  if (compiler->scopeDepth == 0) return;

  // See if a local variable with this name is already declared in this
  // scope.
  // Token *name = &compiler->parser->previous;
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local *local = &compiler->locals[i];
    if (local->depth != -1 && local->depth < compiler->scopeDepth) break;
    if (identifiersEqual(name, &local->name)) {
      errorAt(compiler->parser, name, "Variable with this name already declared in this scope.");
    }
  }

  addLocal(compiler, *name);
}

static uint8_t parseVariable(Compiler *compiler, const char *errorMessage, bool constant) {
  UNUSED(constant);

  consume(compiler, TOKEN_IDENTIFIER, errorMessage);

  // If it's a global variable, create a string constant for it.
  if (compiler->scopeDepth == 0) {
    return identifierConstant(compiler, &compiler->parser->previous);
  }

  declareVariable(compiler, &compiler->parser->previous);
  return 0;
}

static void defineVariable(Compiler *compiler, uint8_t global, bool constant) {
  if (compiler->scopeDepth == 0) {
    if (constant) {
      tableSet(compiler->parser->vm, &compiler->parser->vm->constants,
	       AS_STRING(currentChunk(compiler)->constants.values[global]), NIL_VAL);
    }

    emitBytes(compiler, OP_DEFINE_MODULE, global);
  } else {
    // Mark the local as defined now.
    compiler->locals[compiler->localCount - 1].depth = compiler->scopeDepth;
    compiler->locals[compiler->localCount - 1].constant = constant;
  }
}

static int argumentList(Compiler *compiler, bool *unpack) {
  int argCount = 0;

  if (!check(compiler, TOKEN_RIGHT_PAREN)) {
    do {
      if (*unpack) {
	errorAtCurrent(compiler->parser, "Value unpacking must be the last argument.");
      }

      if (match(compiler, TOKEN_DOT_DOT_DOT)) {
	*unpack = true;
      }

      expression(compiler);
      argCount++;

      if (argCount > 255) {
	error(compiler->parser, "Cannot have more than 255 arguments.");
      }
    } while (match(compiler, TOKEN_COMMA));
  }

  consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");

  return argCount;
}

static void and_(Compiler *compiler, Token previousToken, bool canAssign) {
  UNUSED(previousToken);
  UNUSED(canAssign);

  // left operand...
  // OP_JUMP_IF       ------.
  // OP_POP // left operand |
  // right operand...       |
  //   <--------------------'
  // ...

  // Short circuit if the left operand is false.
  int endJump = emitJump(compiler, OP_JUMP_IF_FALSE);

  // Compile the right operand.
  emitByte(compiler, OP_POP); // Left operand.
  parsePrecedence(compiler, PREC_AND);

  patchJump(compiler, endJump);
}

static bool foldBinary(Compiler *compiler, TokenType operatorType) {
#define FOLD(operator)							\
  do {									\
    Chunk *chunk = currentChunk(compiler);				\
    uint8_t index = chunk->code[chunk->count - 1];			\
    uint8_t constant = chunk->code[chunk->count - 3];			\
    if (chunk->code[chunk->count - 2] != OP_CONSTANT) return false;	\
    if (chunk->code[chunk->count - 4] != OP_CONSTANT) return false;	\
    chunk->constants.values[constant] = NUMBER_VAL(			\
									  AS_NUMBER(chunk->constants.values[constant]) operator \
									  AS_NUMBER(chunk->constants.values[index]) \
									  ); \
    chunk->constants.count--;						\
    chunk->count -= 2;							\
    return true;							\
  } while (false)

#define FOLD_FUNC(func)                                                 \
  do {                                                                  \
    Chunk *chunk = currentChunk(compiler);                              \
    uint8_t index = chunk->code[chunk->count - 1];                        \
    uint8_t constant = chunk->code[chunk->count - 3];                     \
    if (chunk->code[chunk->count - 2] != OP_CONSTANT) return false;       \
    if (chunk->code[chunk->count - 4] != OP_CONSTANT) return false;       \
    chunk->constants.values[constant] = NUMBER_VAL(                      \
                                                        func(           \
                                                              AS_NUMBER(chunk->constants.values[constant]), \
                                                              AS_NUMBER(chunk->constants.values[index]) \
                                                              )         \
                                                        );              \
    chunk->constants.count--;                                            \
    chunk->count -= 2;                                                   \
    return true;                                                        \
  } while (false)

  switch (operatorType) {
  case TOKEN_PLUS: {
    FOLD(+);
    return false;
  }

  case TOKEN_MINUS: {
    FOLD(-);
    return false;
  }

  case TOKEN_STAR: {
    FOLD(*);
    return false;
  }

  case TOKEN_SLASH: {
    FOLD(/);
    return false;
  }
  default: {
    return false;
  }
  }
#undef FOLD
#undef FOLD_FUNC
}


static void binary(Compiler *compiler, Token previousToken, bool canAssign) {
  UNUSED(canAssign);

  TokenType operatorType = compiler->parser->previous.type;

  ParseRule *rule = getRule(operatorType);
  parsePrecedence(compiler, (Precedence) (rule->precedence + 1));

  TokenType currentToken = compiler->parser->previous.type;

  // Attempt constant fold.
  if ((previousToken.type == TOKEN_NUMBER) &&
      (currentToken == TOKEN_NUMBER || currentToken == TOKEN_LEFT_PAREN) &&
      foldBinary(compiler, operatorType)
      ) {
    return;
  }

  switch (operatorType) {
  case TOKEN_BANG_EQUAL:
    emitBytes(compiler, OP_EQUAL, OP_NOT);
    break;
  case TOKEN_EQUAL_EQUAL:
    emitByte(compiler, OP_EQUAL);
    break;
  case TOKEN_GREATER:
    emitByte(compiler, OP_GREATER);
    break;
  case TOKEN_GREATER_EQUAL:
    emitBytes(compiler, OP_LESS, OP_NOT);
    break;
  case TOKEN_LESS:
    emitByte(compiler, OP_LESS);
    break;
  case TOKEN_LESS_EQUAL:
    emitBytes(compiler, OP_GREATER, OP_NOT);
    break;
  case TOKEN_PLUS:
    emitByte(compiler, OP_ADD);
    break;
  case TOKEN_MINUS:
    emitByte(compiler, OP_SUBTRACT);
    break;
  case TOKEN_STAR:
    emitByte(compiler, OP_MULTIPLY);
    break;
  case TOKEN_SLASH:
    emitByte(compiler, OP_DIVIDE);
    break;
  case TOKEN_AMPERSAND:
    emitByte(compiler, OP_BITWISE_AND);
    break;
  case TOKEN_CARET:
    emitByte(compiler, OP_BITWISE_XOR);
    break;
  case TOKEN_PIPE:
    emitByte(compiler, OP_BITWISE_OR);
    break;
  default:
    return;
  }
}

static void call(Compiler *compiler, Token previousToken, bool canAssign) {
  UNUSED(previousToken);
  UNUSED(canAssign);
  bool unpack = false;

  int argCount = argumentList(compiler, &unpack);

  emitBytes(compiler, OP_CALL, argCount);
  emitByte(compiler, unpack);
}

static void dot(Compiler *compiler, Token previousToken, bool canAssign) {
  UNUSED(previousToken);

  consume(compiler, TOKEN_IDENTIFIER, "Expect property name after '.'.");
  uint8_t name = identifierConstant(compiler, &compiler->parser->previous);

  Token identifier = compiler->parser->previous;

  if (match(compiler, TOKEN_LEFT_PAREN)) {
    bool unpack = false;

    int argCount = argumentList(compiler, &unpack);
    if (compiler->class != NULL && (previousToken.type == TOKEN_THIS || identifiersEqual(&previousToken, &compiler->class->name))) {
      emitBytes(compiler, OP_INVOKE_INTERNAL, argCount);
    } else {
      emitBytes(compiler, OP_INVOKE, argCount);
    }

    emitBytes(compiler, name, unpack);
    return;
  }
  if (compiler->class != NULL && (previousToken.type == TOKEN_THIS && 0)) {}
  else {
    if (canAssign && match(compiler, TOKEN_EQUAL)) {
      expression(compiler);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_PLUS_EQUALS)) {
      emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
      expression(compiler);
      emitByte(compiler, OP_ADD);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_MINUS_EQUALS)) {
      emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
      expression(compiler);
      emitByte(compiler, OP_SUBTRACT);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_MULTIPLY_EQUALS)) {
      emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
      expression(compiler);
      emitByte(compiler, OP_MULTIPLY);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_DIVIDE_EQUALS)) {
      emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
      expression(compiler);
      emitByte(compiler, OP_DIVIDE);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_AMPERSAND_EQUALS)) {
      emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
      expression(compiler);
      emitByte(compiler, OP_BITWISE_AND);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_CARET_EQUALS)) {
      emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
      expression(compiler);
      emitByte(compiler, OP_BITWISE_XOR);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else if (canAssign && match(compiler, TOKEN_PIPE_EQUALS)) {
      emitBytes(compiler, OP_GET_PROPERTY_NO_POP, name);
      expression(compiler);
      emitByte(compiler, OP_BITWISE_OR);
      emitBytes(compiler, OP_SET_PROPERTY, name);
    } else {
      emitBytes(compiler, OP_GET_PROPERTY, name);
    }
  }
}

static void literal(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);

  switch (compiler->parser->previous.type) {
  case TOKEN_FALSE:
    emitByte(compiler, OP_FALSE);
    break;
  case TOKEN_NIL:
    emitByte(compiler, OP_NIL);
    break;
  case TOKEN_TRUE:
    emitByte(compiler, OP_TRUE);
    break;
  default:
    return; // Unreachable.
  }
}

static void block(Compiler *compiler) {
  while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
    declaration(compiler);
  }

  consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}




static void beginFunction(Compiler *compiler, Compiler *fnCompiler, FunctionType type, AccessLevel level) {
  initCompiler(compiler->parser, fnCompiler, compiler, type, level);
    
  beginScope(fnCompiler);
  // Compile the parameter list.
  consume(fnCompiler, TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    
  if (!check(fnCompiler, TOKEN_RIGHT_PAREN)) {
    bool optional = false;
    bool isSpreadParam = false;
    int index = 0;
    uint8_t identifiers[255];
    int indexes[255];

    uint8_t privateIdentifiers[255];
    int privateIndexes[255];
    do {
      // spread must come last
      if (isSpreadParam) {
        error(fnCompiler->parser, "spread parameter must be last in the parameter list");
      }
      bool varKeyword = match(compiler, TOKEN_VAR);
      isSpreadParam = match(compiler, TOKEN_DOT_DOT_DOT);
      consume(compiler, TOKEN_IDENTIFIER, "Expect parameter name.");
      uint8_t paramConstant = identifierConstant(fnCompiler, &fnCompiler->parser->previous);
      declareVariable(fnCompiler, &fnCompiler->parser->previous);
      defineVariable(fnCompiler, paramConstant, false);

      if (type == TYPE_INITIALIZER && varKeyword) {
        identifiers[fnCompiler->function->propertyCount] = paramConstant;
        indexes[fnCompiler->function->propertyCount++] = index;
      } else if (varKeyword) {
        error(fnCompiler->parser, "var keyword in a function definition that is not a class constructor");
      }
      if (isSpreadParam) {
        if (type == TYPE_INITIALIZER) {
          error(fnCompiler->parser, "spread parameter cannot be used in a class constructor");
        }
        fnCompiler->function->isVariadic = isSpreadParam;
      }

      if (match(fnCompiler, TOKEN_EQUAL)) {
        if (isSpreadParam) {
          error(fnCompiler->parser, "spread parameter cannot have an optional value");
        }
        fnCompiler->function->arityOptional++;
        optional = true;
        expression(fnCompiler);
      } else {
        fnCompiler->function->arity++;

        if (optional) {
          error(fnCompiler->parser, "Cannot have non-optional parameter after optional.");
        }
      }

      if (fnCompiler->function->arity + fnCompiler->function->arityOptional > 255) {
        error(fnCompiler->parser, "Cannot have more than 255 parameters.");
      }
      index++;
    } while (match(fnCompiler, TOKEN_COMMA));

    if (fnCompiler->function->arityOptional > 0) {
      emitByte(fnCompiler, OP_DEFINE_OPTIONAL);
      emitBytes(fnCompiler, fnCompiler->function->arity, fnCompiler->function->arityOptional);
    }

  }

  consume(fnCompiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
}

static void grouping(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);

  expression(compiler);
  consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static Value parseNumber(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);

  // We allocate the whole range for the worst case.
  // Also account for the null-byte.
  char *buffer = ALLOCATE(compiler->parser->vm, char, compiler->parser->previous.length + 1);
  char *current = buffer;

  // Strip it of any underscores.
  for (int i = 0; i < compiler->parser->previous.length; i++) {
    char c = compiler->parser->previous.start[i];

    if (c != '_') {
      *(current++) = c;
    }
  }

  // Terminate the string with a null character.
  *current = '\0';

  // Parse the string.
  double value = strtod(buffer, NULL);
  // Free the malloc'd buffer.
  FREE_ARRAY(compiler->parser->vm, char, buffer, compiler->parser->previous.length + 1);

  return NUMBER_VAL(value);
}

static void number(Compiler *compiler, bool canAssign) {
  emitConstant(compiler, parseNumber(compiler, canAssign));
}

static void or_(Compiler *compiler, Token previousToken, bool canAssign) {
  UNUSED(previousToken);
  UNUSED(canAssign);

  // left operand...
  // OP_JUMP_IF       ---.
  // OP_JUMP          ---+--.
  //   <-----------------'  |
  // OP_POP // left operand |
  // right operand...       |
  //   <--------------------'
  // ...

  // If the operand is *true* we want to keep it, so when it's false,
  // jump to the code to evaluate the right operand.
  int elseJump = emitJump(compiler, OP_JUMP_IF_FALSE);

  // If we get here, the operand is true, so jump to the end to keep it.
  int endJump = emitJump(compiler, OP_JUMP);

  // Compile the right operand.
  patchJump(compiler, elseJump);
  emitByte(compiler, OP_POP); // Left operand.

  parsePrecedence(compiler, PREC_OR);
  patchJump(compiler, endJump);
}

int parseEscapeSequences(char *string, int length) {
  for (int i = 0; i < length - 1; i++) {
    if (string[i] == '\\') {
      switch (string[i + 1]) {
      case 'n': {
	string[i + 1] = '\n';
	break;
      }
      case 't': {
	string[i + 1] = '\t';
	break;
      }
      case 'r': {
	string[i + 1] = '\r';
	break;
      }
      case 'v': {
	string[i + 1] = '\v';
	break;
      }
      case '\\': {
	string[i + 1] = '\\';
	break;
      }
      case '\'':
      case '"': {
	break;
      }
      default: {
	continue;
      }
      }
      memmove(&string[i], &string[i + 1], length - i);
      length -= 1;
    }
  }

  return length;
}


static void rString(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);

  if (match(compiler, TOKEN_STRING)) {
    Parser *parser = compiler->parser;
    emitConstant(compiler,
                 OBJ_VAL(copyString(parser->vm, parser->previous.start + 1,
                 parser->previous.length - 2)));
    return;
  }

  consume(compiler, TOKEN_STRING, "Expected string after r delimiter");
}


static Value parseString(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);

  Parser *parser = compiler->parser;
  int stringLength = parser->previous.length - 2;

  char *string = ALLOCATE(parser->vm, char, stringLength + 1);

  memcpy(string, parser->previous.start + 1, stringLength);
  int length = parseEscapeSequences(string, stringLength);

  // If there were escape chars and the string shrank, resize the buffer
  if (length != stringLength) {
    string = SHRINK_ARRAY(parser->vm, string, char, stringLength + 1, length + 1);
  }
  string[length] = '\0';

  return OBJ_VAL(takeString(parser->vm, string, length));
}

static void string(Compiler *compiler, bool canAssign) {
  emitConstant(compiler, parseString(compiler, canAssign));
}

static void checkConst(Compiler *compiler, uint8_t setOp, int arg) {
  if (setOp == OP_SET_LOCAL) {
    if (compiler->locals[arg].constant) {
      error(compiler->parser, "Cannot assign to a constant.");
    }
  } else if (setOp == OP_SET_UPVALUE) {
    Upvalue upvalue = compiler->upvalues[arg];

    if (upvalue.constant) {
      error(compiler->parser, "Cannot assign to a constant.");
    }
  } else if (setOp == OP_SET_MODULE) {
    Value _;
    if (tableGet(&compiler->parser->vm->constants, AS_STRING(currentChunk(compiler)->constants.values[arg]), &_)) {
      error(compiler->parser, "Cannot assign to a constant.");
    }
  }
}

static void namedVariable(Compiler *compiler, Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int arg = resolveLocal(compiler, &name, false);
  
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(compiler, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg = identifierConstant(compiler, &name);
    ObjString *string = copyString(compiler->parser->vm, name.start, name.length);
    Value value;
    if (tableGet(&compiler->parser->vm->globals, string, &value)) {
      getOp = OP_GET_GLOBAL;
      canAssign = false;
    } else {
      getOp = OP_GET_MODULE;
      setOp = OP_SET_MODULE;
    }
  }

  if (canAssign && match(compiler, TOKEN_EQUAL)) {
    checkConst(compiler, setOp, arg);
    expression(compiler);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else if (canAssign && match(compiler, TOKEN_PLUS_EQUALS)) {
    checkConst(compiler, setOp, arg);
    namedVariable(compiler, name, false);
    expression(compiler);
    emitByte(compiler, OP_ADD);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else if (canAssign && match(compiler, TOKEN_MINUS_EQUALS)) {
    checkConst(compiler, setOp, arg);
    namedVariable(compiler, name, false);
    expression(compiler);
    emitByte(compiler, OP_SUBTRACT);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else if (canAssign && match(compiler, TOKEN_MULTIPLY_EQUALS)) {
    checkConst(compiler, setOp, arg);
    namedVariable(compiler, name, false);
    expression(compiler);
    emitByte(compiler, OP_MULTIPLY);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else if (canAssign && match(compiler, TOKEN_DIVIDE_EQUALS)) {
    checkConst(compiler, setOp, arg);
    namedVariable(compiler, name, false);
    expression(compiler);
    emitByte(compiler, OP_DIVIDE);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else if (canAssign && match(compiler, TOKEN_AMPERSAND_EQUALS)) {
    checkConst(compiler, setOp, arg);
    namedVariable(compiler, name, false);
    expression(compiler);
    emitByte(compiler, OP_BITWISE_AND);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else if (canAssign && match(compiler, TOKEN_CARET_EQUALS)) {
    checkConst(compiler, setOp, arg);
    namedVariable(compiler, name, false);
    expression(compiler);
    emitByte(compiler, OP_BITWISE_XOR);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else if (canAssign && match(compiler, TOKEN_PIPE_EQUALS)) {
    checkConst(compiler, setOp, arg);
    namedVariable(compiler, name, false);
    expression(compiler);
    emitByte(compiler, OP_BITWISE_OR);
    emitBytes(compiler, setOp, (uint8_t) arg);
  } else {
    emitBytes(compiler, getOp, (uint8_t) arg);
  }
}

static void variable(Compiler *compiler, bool canAssign) {
  namedVariable(compiler, compiler->parser->previous, canAssign);
}

static Token syntheticToken(const char *text) {
  Token token;
  token.start = text;
  token.length = (int) strlen(text);
  return token;
}

static void pushSuperclass(Compiler *compiler) {
  if (compiler->class == NULL) return;
  namedVariable(compiler, syntheticToken("super"), false);
}

static void super_(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);

  if (compiler->class == NULL) {
    error(compiler->parser, "Cannot utilise 'super' outside of a class.");
  } else if (!compiler->class->hasSuperclass) {
    error(compiler->parser, "Cannot utilise 'super' in a class with no superclass.");
  }

  consume(compiler, TOKEN_DOT, "Expect '.' after 'super'.");
  consume(compiler, TOKEN_IDENTIFIER, "Expect superclass method name.");
  uint8_t name = identifierConstant(compiler, &compiler->parser->previous);

  // Push the receiver.
  namedVariable(compiler, syntheticToken("this"), false);

  if (match(compiler, TOKEN_LEFT_PAREN)) {
    bool unpack = false;
    int argCount = argumentList(compiler, &unpack);

    pushSuperclass(compiler);
    emitBytes(compiler, OP_SUPER, argCount);
    emitBytes(compiler, name, unpack);
  } else {
    pushSuperclass(compiler);
    emitBytes(compiler, OP_GET_SUPER, name);
  }
}


static void this_(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);
  if (compiler->class == NULL) {
    error(compiler->parser, "Cannot utilise 'this' outside of a class.");
  } else if (compiler->class->staticMethod) {
    error(compiler->parser, "Cannot utilise 'this' inside a static method.");
  } else {
    variable(compiler, false);
  }
}

static bool foldUnary(Compiler *compiler, TokenType operatorType) {
  TokenType valueToken = compiler->parser->previous.type;

  switch (operatorType) {
  case TOKEN_NOT: {
    if (valueToken == TOKEN_TRUE) {
      Chunk *chunk = currentChunk(compiler);
      chunk->code[chunk->count - 1] = OP_FALSE;
      return true;
    } else if (valueToken == TOKEN_FALSE) {
      Chunk *chunk = currentChunk(compiler);
      chunk->code[chunk->count - 1] = OP_TRUE;
      return true;
    }

    return false;
  }

  case TOKEN_MINUS: {
    if (valueToken == TOKEN_NUMBER) {
      Chunk *chunk = currentChunk(compiler);
      uint8_t constant = chunk->code[chunk->count - 1];
      chunk->constants.values[constant] = NUMBER_VAL(-AS_NUMBER(chunk->constants.values[constant]));
      return true;
    }

    return false;
  }

  default: {
    return false;
  }
  }
}


static void unary(Compiler *compiler, bool canAssign) {
  UNUSED(canAssign);

  TokenType operatorType = compiler->parser->previous.type;
  parsePrecedence(compiler, PREC_UNARY);

  // Constant fold.
  if (foldUnary(compiler, operatorType)) {
    return;
  }

  switch (operatorType) {
  case TOKEN_NOT:
    emitByte(compiler, OP_NOT);
    break;
  case TOKEN_MINUS:
    emitByte(compiler, OP_NEGATE);
    break;
  default:
    return;
  }
}

ParseRule rules[] = {
/* Compiling Expressions rules < Calls and Functions infix-left-paren
  [TOKEN_LEFT_PAREN]    = {grouping, NULL,   PREC_NONE},
*/
//> Calls and Functions infix-left-paren
  [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
//< Calls and Functions infix-left-paren
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE}, // [big]
  [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
/* Compiling Expressions rules < Classes and Instances table-dot
  [TOKEN_DOT]           = {NULL,     NULL,   PREC_NONE},
*/
//> Classes and Instances table-dot
  [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
//< Classes and Instances table-dot
  [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
  [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
/* Compiling Expressions rules < Types of Values table-not
  [TOKEN_BANG]          = {NULL,     NULL,   PREC_NONE},
*/
//> Types of Values table-not
  [TOKEN_NOT]          = {unary,    NULL,   PREC_NONE},
//< Types of Values table-not
/* Compiling Expressions rules < Types of Values table-equal
  [TOKEN_BANG_EQUAL]    = {NULL,     NULL,   PREC_NONE},
*/
//> Types of Values table-equal
  [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
//< Types of Values table-equal
  [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
/* Compiling Expressions rules < Types of Values table-comparisons
  [TOKEN_EQUAL_EQUAL]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_GREATER]       = {NULL,     NULL,   PREC_NONE},
  [TOKEN_GREATER_EQUAL] = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LESS]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LESS_EQUAL]    = {NULL,     NULL,   PREC_NONE},
*/
//> Types of Values table-comparisons
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
//< Types of Values table-comparisons
/* Compiling Expressions rules < Global Variables table-identifier
  [TOKEN_IDENTIFIER]    = {NULL,     NULL,   PREC_NONE},
*/
//> Global Variables table-identifier
  [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
//< Global Variables table-identifier
/* Compiling Expressions rules < Strings table-string
  [TOKEN_STRING]        = {NULL,     NULL,   PREC_NONE},
*/
//> Strings table-string
  [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
//< Strings table-string
  [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
/* Compiling Expressions rules < Jumping Back and Forth table-and
  [TOKEN_AND]           = {NULL,     NULL,   PREC_NONE},
*/
//> Jumping Back and Forth table-and
  [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
//< Jumping Back and Forth table-and
  [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
/* Compiling Expressions rules < Types of Values table-false
  [TOKEN_FALSE]         = {NULL,     NULL,   PREC_NONE},
*/
//> Types of Values table-false
  [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
//< Types of Values table-false
  [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DEF]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
/* Compiling Expressions rules < Types of Values table-nil
  [TOKEN_NIL]           = {NULL,     NULL,   PREC_NONE},
*/
//> Types of Values table-nil
  [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
//< Types of Values table-nil
/* Compiling Expressions rules < Jumping Back and Forth table-or
  [TOKEN_OR]            = {NULL,     NULL,   PREC_NONE},
*/
//> Jumping Back and Forth table-or
  [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
//< Jumping Back and Forth table-or

  [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
/* Compiling Expressions rules < Superclasses table-super
  [TOKEN_SUPER]         = {NULL,     NULL,   PREC_NONE},
*/
//> Superclasses table-super
  [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
//< Superclasses table-super
/* Compiling Expressions rules < Methods and Initializers table-this
  [TOKEN_THIS]          = {NULL,     NULL,   PREC_NONE},
*/
//> Methods and Initializers table-this
  [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
//< Methods and Initializers table-this
/* Compiling Expressions rules < Types of Values table-true
  [TOKEN_TRUE]          = {NULL,     NULL,   PREC_NONE},
*/
//> Types of Values table-true
  [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
//< Types of Values table-true
  [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_BREAK]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CONTINUE]      = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IMPORT]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FROM]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_PERCENT]       = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR_STAR]     = {NULL,     binary, PREC_INDICES},
 
};

static void parsePrecedence(Compiler *compiler, Precedence precedence) {
  Parser *parser = compiler->parser;
  advance(parser);
  ParsePrefixFn prefixRule = getRule(parser->previous.type)->prefix;
  if (prefixRule == NULL) {
    error(parser, "Expect expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(compiler, canAssign);

  while (precedence <= getRule(parser->current.type)->precedence) {
    Token token = compiler->parser->previous;
    advance(parser);
    ParseInfixFn infixRule = getRule(parser->previous.type)->infix;
    infixRule(compiler, token, canAssign);
  }

  if (canAssign && match(compiler, TOKEN_EQUAL)) {
    // If we get here, we didn't parse the "=" even though we could
    // have, so the LHS must not be a valid lvalue.
    error(parser, "Invalid assignment target.");
  }
}

static ParseRule *getRule(TokenType type) {
  return &rules[type];
}

void expression(Compiler *compiler) {
  parsePrecedence(compiler, PREC_ASSIGNMENT);
}

static void function(Compiler *compiler, FunctionType type, AccessLevel level) {
  Compiler fnCompiler;

  // Setup function and parse parameters
  beginFunction(compiler, &fnCompiler, type, level);
  // The body.
  consume(&fnCompiler, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  block(&fnCompiler);
  /**
   * No need to explicitly reduce the scope here as endCompiler does
   * it for us.
   **/
  endCompiler(&fnCompiler);
}

static void method(Compiler *compiler, bool private, Token *identifier, bool *hasAnnotation) {
  AccessLevel level = ACCESS_PUBLIC;
  FunctionType type;

  compiler->class->staticMethod = false;
  type = TYPE_METHOD;
 
  
  if (identifier == NULL) {
    consume(compiler, TOKEN_IDENTIFIER, "Expect method name.");
    identifier = &compiler->parser->previous;
  }
  uint8_t constant = identifierConstant(compiler, identifier);

  // If the method is named "init", it's an initializer.
  if (compiler->parser->previous.length == 4 &&
      memcmp(compiler->parser->previous.start, "init", 4) == 0) {
    type = TYPE_INITIALIZER;
  }

  if (type != TYPE_ABSTRACT) {
    function(compiler, type, level);
  } else {
    Compiler fnCompiler;

    // Setup function and parse parameters
    beginFunction(compiler, &fnCompiler, TYPE_ABSTRACT, ACCESS_PUBLIC);
    endCompiler(&fnCompiler);

    if (check(compiler, TOKEN_LEFT_BRACE)) {
      error(compiler->parser, "Abstract methods can not have an implementation.");
      return;
    }
  }

  emitBytes(compiler, OP_METHOD, constant);
}

static void setupClassCompiler(Compiler *compiler, ClassCompiler *classCompiler, bool abstract) {
  classCompiler->name = compiler->parser->previous;
  classCompiler->hasSuperclass = false;
  classCompiler->enclosing = compiler->class;
  classCompiler->staticMethod = false;
  classCompiler->abstractClass = abstract; 
  classCompiler->classAnnotations = NULL;
  classCompiler->methodAnnotations = NULL;
  initTable(&classCompiler->privateVariables);
  compiler->class = classCompiler;
}

static void endClassCompiler(Compiler *compiler, ClassCompiler *classCompiler) {
  freeTable(compiler->parser->vm, &classCompiler->privateVariables);
  compiler->class = compiler->class->enclosing;
}

static void parseClassBody(Compiler *compiler) {
  bool methodHasAnnotation = false;
    
  while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
    if (match(compiler, TOKEN_VAR)) {
      if (methodHasAnnotation) {
        consume(compiler, TOKEN_VAR, "Annotations not allowed on `var` statements");
      }

      consume(compiler, TOKEN_IDENTIFIER, "Expect class variable name.");
      uint8_t name = identifierConstant(compiler, &compiler->parser->previous);
      consume(compiler, TOKEN_EQUAL, "Expect '=' after class variable identifier.");
      expression(compiler);
      emitBytes(compiler, OP_SET_CLASS_VAR, name);
      emitByte(compiler, false);

      consume(compiler, TOKEN_SEMICOLON, "Expect ';' after class variable declaration.");
    }  else {
      method(compiler, false, NULL, &methodHasAnnotation);
    }  
  }
    
}


static void classDeclaration(Compiler *compiler) {
  consume(compiler, TOKEN_IDENTIFIER, "Expect class name.");
  uint8_t nameConstant = identifierConstant(compiler, &compiler->parser->previous);
  
  declareVariable(compiler, &compiler->parser->previous);

  ClassCompiler classCompiler;
  setupClassCompiler(compiler, &classCompiler, false);

  if (match(compiler, TOKEN_LESS)) {
    expression(compiler);
    classCompiler.hasSuperclass = true;

    beginScope(compiler);

    // Store the superclass in a local variable named "super".
    addLocal(compiler, syntheticToken("super"));

    emitBytes(compiler, OP_SUBCLASS, CLASS_DEFAULT);
  } else {
    emitBytes(compiler, OP_CLASS, CLASS_DEFAULT);
  }
  
  emitByte(compiler, nameConstant);

  consume(compiler, TOKEN_LEFT_BRACE, "Expect '{' before class body.");
  parseClassBody(compiler);
  consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
  if (classCompiler.hasSuperclass) {
    endScope(compiler);
    // If there's a super class, check abstract methods have been defined
    emitByte(compiler, OP_END_CLASS);
  }
  
  endClassCompiler(compiler, &classCompiler);
  defineVariable(compiler, nameConstant, false);
}


static void funDeclaration(Compiler *compiler) {
  uint8_t global = parseVariable(compiler, "Expect function name.", false);
  function(compiler, TYPE_FUNCTION, ACCESS_PUBLIC);
  defineVariable(compiler, global, false);
}

static void varDeclaration(Compiler *compiler, bool constant) {
  
  do {
    uint8_t global = parseVariable(compiler, "Expect variable name.", constant);

    if (match(compiler, TOKEN_EQUAL) || constant) {
      
      // Compile the initializer.
      expression(compiler);
    } else {
      // Default to nil.
      emitByte(compiler, OP_NIL);
    }

    defineVariable(compiler, global, constant);
  } while (match(compiler, TOKEN_COMMA));
  

  consume(compiler, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
}


static void expressionStatement(Compiler *compiler) {
  Token previous = compiler->parser->previous;
  advance(compiler->parser);
  TokenType t = compiler->parser->current.type;
  for (int i = 0; i < compiler->parser->current.length; ++i) {
    backTrack(&compiler->parser->scanner);
  }
  compiler->parser->current = compiler->parser->previous;
  compiler->parser->previous = previous;
  
  expression(compiler);
  consume(compiler, TOKEN_SEMICOLON, "Expect ';' after expression.");
  if (compiler->parser->vm->repl && t != TOKEN_EQUAL && compiler->type == TYPE_TOP_LEVEL) {
    emitByte(compiler, OP_POP_REPL);
  } else {
    emitByte(compiler, OP_POP);
  }
}

static int getArgCount(uint8_t *code, const ValueArray constants, int ip) {
  switch (code[ip]) {
  case OP_NIL:
  case OP_TRUE:
  case OP_FALSE:
  case OP_POP:
  case OP_EQUAL:
  case OP_GREATER:
  case OP_LESS:
  case OP_ADD:
  case OP_MULTIPLY:
  case OP_NOT:
  case OP_POW:
  case OP_MOD:
  case OP_NEGATE:
  case OP_CLOSE_UPVALUE:
  case OP_RETURN:
  case OP_EMPTY:
  case OP_END_CLASS:
  case OP_IMPORT_VARIABLE:
  case OP_IMPORT_END:
  case OP_BREAK:
  case OP_BITWISE_AND:
  case OP_BITWISE_XOR:
  case OP_BITWISE_OR:
  case OP_POP_REPL:
    return 0;

  case OP_CONSTANT:
  case OP_GET_LOCAL:
  case OP_SET_LOCAL:
  case OP_GET_GLOBAL:
  case OP_GET_MODULE:
  case OP_DEFINE_MODULE:
  case OP_SET_MODULE:
  case OP_GET_UPVALUE:
  case OP_SET_UPVALUE:
  case OP_GET_PROPERTY:
  case OP_SET_CLASS_VAR:
  case OP_GET_SUPER:
  case OP_METHOD:
  case OP_IMPORT:
  case OP_DEFINE_OPTIONAL:
  case OP_JUMP:
  case OP_JUMP_IF_FALSE:
  case OP_LOOP:
  case OP_CLASS:
  case OP_CALL:
    return 2;

  case OP_INVOKE:
  case OP_SUPER:
    return 3;

    /*case OP_IMPORT_BUILTIN_VARIABLE: {
    int argCount = code[ip + 2];

    return 2 + argCount;
  }
    */

  case OP_CLOSURE: {
    int constant = code[ip + 1];
    ObjFunction* loadedFn = AS_FUNCTION(constants.values[constant]);

    // There is one byte for the constant, then two for each upvalue.
    return 1 + (loadedFn->upvalueCount * 2);
  }

  case OP_IMPORT_FROM: {
    // 1 + amount of variables imported
    return 1 + code[ip + 1];
  }
  }

  return 0;
}

static void endLoop(Compiler *compiler) {
    if (compiler->loop->end != -1) {
        patchJump(compiler, compiler->loop->end);
        emitByte(compiler, OP_POP); // Condition.
    }

    int i = compiler->loop->body;
    while (i < compiler->function->chunk.count) {
        if (compiler->function->chunk.code[i] == OP_BREAK) {
            compiler->function->chunk.code[i] = OP_JUMP;
            patchJump(compiler, i + 1);
            i += 3;
        } else {
            i += 1 + getArgCount(compiler->function->chunk.code, compiler->function->chunk.constants, i);
        }
    }

    compiler->loop = compiler->loop->enclosing;
}

static void forStatement(Compiler *compiler) {
  // for (var i = 0; i < 10; i = i + 1) print i;
  //
  //   var i = 0;
  // start:                      <--.
  //   if (i < 10) goto exit;  --.  |
  //   goto body;  -----------.  |  |
  // increment:            <--+--+--+--.
  //   i = i + 1;             |  |  |  |
  //   goto start;  ----------+--+--'  |
  // body:                 <--'  |     |
  //   print i;                  |     |
  //   goto increment;  ---------+-----'
  // exit:                    <--'

  // Create a scope for the loop variable.
  beginScope(compiler);

  // The initialization clause.
  consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
  if (match(compiler, TOKEN_VAR)) {
    varDeclaration(compiler, false);
  } else if (match(compiler, TOKEN_SEMICOLON)) {
    // No initializer.
  } else {
    expressionStatement(compiler);
  }

  Loop loop;
  loop.start = currentChunk(compiler)->count;
  loop.scopeDepth = compiler->scopeDepth;
  loop.enclosing = compiler->loop;
  compiler->loop = &loop;

  // The exit condition.
  compiler->loop->end = -1;

  if (!match(compiler, TOKEN_SEMICOLON)) {
    expression(compiler);
    consume(compiler, TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    // Jump out of the loop if the condition is false.
    compiler->loop->end = emitJump(compiler, OP_JUMP_IF_FALSE);
    emitByte(compiler, OP_POP); // Condition.
  }

  // Increment step.
  if (!match(compiler, TOKEN_RIGHT_PAREN)) {
    // We don't want to execute the increment before the body, so jump
    // over it.
    int bodyJump = emitJump(compiler, OP_JUMP);

    int incrementStart = currentChunk(compiler)->count;
    expression(compiler);
    emitByte(compiler, OP_POP);
    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(compiler, compiler->loop->start);
    compiler->loop->start = incrementStart;

    patchJump(compiler, bodyJump);
  }

  // Compile the body.
  compiler->loop->body = compiler->function->chunk.count;
  statement(compiler);

  // Jump back to the beginning (or the increment).
  emitLoop(compiler, compiler->loop->start);

  endLoop(compiler);
  endScope(compiler); // Loop variable.
}

static void breakStatement(Compiler *compiler) {
  if (compiler->loop == NULL) {
    error(compiler->parser, "Cannot utilise 'break' outside of a loop.");
    return;
  }

  consume(compiler, TOKEN_SEMICOLON, "Expected semicolon after break");

  // Discard any locals created inside the loop.
  for (int i = compiler->localCount - 1;
       i >= 0 && compiler->locals[i].depth > compiler->loop->scopeDepth;
       i--) {
    if(compiler->locals[i].isUpvalue) {
      emitByte(compiler, OP_CLOSE_UPVALUE);
    } else {
      emitByte(compiler, OP_POP);
    }
  }
  emitJump(compiler, OP_BREAK);
}

static void continueStatement(Compiler *compiler) {
  if (compiler->loop == NULL) {
    error(compiler->parser, "Cannot utilise 'continue' outside of a loop.");
  }

  consume(compiler, TOKEN_SEMICOLON, "Expect ';' after 'continue'.");

  // Discard any locals created inside the loop.
  for (int i = compiler->localCount - 1;
       i >= 0 && compiler->locals[i].depth > compiler->loop->scopeDepth;
       i--) {
    if(compiler->locals[i].isUpvalue)
      emitByte(compiler, OP_CLOSE_UPVALUE);
    else
      emitByte(compiler, OP_POP);
  }

  // Jump to top of current innermost loop.
  emitLoop(compiler, compiler->loop->start);
}

static void ifStatement(Compiler *compiler) {
  consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression(compiler);
  consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  // Jump to the else branch if the condition is false.
  int elseJump = emitJump(compiler, OP_JUMP_IF_FALSE);

  // Compile the then branch.
  emitByte(compiler, OP_POP); // Condition.
  statement(compiler);

  // Jump over the else branch when the if branch is taken.
  int endJump = emitJump(compiler, OP_JUMP);

  // Compile the else branch.
  patchJump(compiler, elseJump);
  emitByte(compiler, OP_POP); // Condition.

  if (match(compiler, TOKEN_ELSE)) statement(compiler);

  patchJump(compiler, endJump);
}

static void returnStatement(Compiler *compiler) {
  if (compiler->type == TYPE_TOP_LEVEL) {
    error(compiler->parser, "Cannot return from top-level code.");
  }

  if (match(compiler, TOKEN_SEMICOLON)) {
    emitReturn(compiler);
  } else {
    if (compiler->type == TYPE_INITIALIZER) {
      error(compiler->parser, "Cannot return a value from an initializer.");
    }

    expression(compiler);
    consume(compiler, TOKEN_SEMICOLON, "Expect ';' after return value.");

    emitByte(compiler, OP_RETURN);
  }
}


static void importStatement(Compiler *compiler) {
  if (match(compiler, TOKEN_STRING)) {
    int importConstant = makeConstant(compiler, OBJ_VAL(copyString(
								   compiler->parser->vm,
								   compiler->parser->previous.start + 1,
								   compiler->parser->previous.length - 2)));

    emitBytes(compiler, OP_IMPORT, importConstant);
    emitByte(compiler, OP_POP);

    if (match(compiler, TOKEN_AS)) {
      uint8_t importName = parseVariable(compiler, "Expect import alias.", false);
      emitByte(compiler, OP_IMPORT_VARIABLE);
      defineVariable(compiler, importName, false);
    }
  }

  emitByte(compiler, OP_IMPORT_END);
  consume(compiler, TOKEN_SEMICOLON, "Expect ';' after import.");
}

static void fromImportStatement(Compiler *compiler) {
  if (match(compiler, TOKEN_STRING)) {
    int importConstant = makeConstant(compiler, OBJ_VAL(copyString(
								   compiler->parser->vm,
								   compiler->parser->previous.start + 1,
								   compiler->parser->previous.length - 2)));

    consume(compiler, TOKEN_IMPORT, "Expect 'import' after import path.");
    emitBytes(compiler, OP_IMPORT, importConstant);
    emitByte(compiler, OP_POP);

    uint8_t variables[255];
    Token tokens[255];
    int varCount = 0;

    do {
      consume(compiler, TOKEN_IDENTIFIER, "Expect variable name.");
      tokens[varCount] = compiler->parser->previous;
      variables[varCount] = identifierConstant(compiler, &compiler->parser->previous);
      varCount++;

      if (varCount > 255) {
        error(compiler->parser, "Cannot have more than 255 variables.");
      }
    } while (match(compiler, TOKEN_COMMA));

    emitBytes(compiler, OP_IMPORT_FROM, varCount);

    for (int i = 0; i < varCount; ++i) {
      emitByte(compiler, variables[i]);
    }

    // This needs to be two separate loops as we need
    // all the variables popped before defining.
    if (compiler->scopeDepth == 0) {
      for (int i = varCount - 1; i >= 0; --i) {
        defineVariable(compiler, variables[i], false);
      }
    } else {
      for (int i = 0; i < varCount; ++i) {
        declareVariable(compiler, &tokens[i]);
        defineVariable(compiler, 0, false);
      }
    }
   
  }

  emitByte(compiler, OP_IMPORT_END);
  consume(compiler, TOKEN_SEMICOLON, "Expect ';' after import.");
}

static void whileStatement(Compiler *compiler) {
    Loop loop;
    loop.start = currentChunk(compiler)->count;
    loop.scopeDepth = compiler->scopeDepth;
    loop.enclosing = compiler->loop;
    compiler->loop = &loop;

    if (check(compiler, TOKEN_LEFT_BRACE)) {
        emitByte(compiler, OP_TRUE);
    } else {
        consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
        expression(compiler);
        consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
    }

    // Jump out of the loop if the condition is false.
    compiler->loop->end = emitJump(compiler, OP_JUMP_IF_FALSE);

    // Compile the body.
    emitByte(compiler, OP_POP); // Condition.
    compiler->loop->body = compiler->function->chunk.count;
    statement(compiler);

    // Loop back to the start.
    emitLoop(compiler, loop.start);
    endLoop(compiler);
}


static void synchronize(Parser *parser) {
  parser->panicMode = false;

  while (parser->current.type != TOKEN_EOF) {
    if (parser->previous.type == TOKEN_SEMICOLON) return;

    switch (parser->current.type) {
    case TOKEN_CLASS:
    case TOKEN_DEF:
    case TOKEN_VAR:
    case TOKEN_FOR:
    case TOKEN_IF:
    case TOKEN_WHILE:
    case TOKEN_BREAK:
    case TOKEN_RETURN:
    case TOKEN_IMPORT:
    default:
      // Do nothing.
      ;
    }

    advance(parser);
  }
}

static void declaration(Compiler *compiler) {
  if (match(compiler, TOKEN_CLASS)) {
    classDeclaration(compiler);
    if (compiler->parser->panicMode) synchronize(compiler->parser);
    return;
  }

  if (match(compiler, TOKEN_DEF)) {
    funDeclaration(compiler);
  } else if (match(compiler, TOKEN_VAR)) {
    varDeclaration(compiler, false);
  } else {
    statement(compiler);
  }

  if (compiler->parser->panicMode) synchronize(compiler->parser);
}

static void statement(Compiler *compiler) {
  if (match(compiler, TOKEN_FOR)) {
    forStatement(compiler);
  } else if (match(compiler, TOKEN_IF)) {
    ifStatement(compiler);
  } else if (match(compiler, TOKEN_RETURN)) {
    returnStatement(compiler);
  } else if (match(compiler, TOKEN_IMPORT)) {
    importStatement(compiler);
  } else if (match(compiler, TOKEN_FROM)) {
    fromImportStatement(compiler);
  } else if (match(compiler, TOKEN_BREAK)) {
    breakStatement(compiler);
  } else if (match(compiler, TOKEN_WHILE)) {
    whileStatement(compiler);
  } else if (match(compiler, TOKEN_LEFT_BRACE)) {
    
    Parser *parser = compiler->parser;
    Token previous = parser->previous;
    Token curtok = parser->current;

    // Advance the parser to the next token
    advance(parser);

    if (check(compiler, TOKEN_RIGHT_BRACE)) {
      if (check(compiler, TOKEN_SEMICOLON)) {
        backTrack(&parser->scanner);
        backTrack(&parser->scanner);
        parser->current = previous;
        expressionStatement(compiler);
        return;
      }
    }

    // Reset the scanner to the previous position
    for (int i = 0; i < parser->current.length; ++i) {
      backTrack(&parser->scanner);
    }

    // Reset the parser
    parser->previous = previous;
    parser->current = curtok;

    beginScope(compiler);
    block(compiler);
    endScope(compiler);
  } else if (match(compiler, TOKEN_CONTINUE)) {
    continueStatement(compiler);
  } else {
    expressionStatement(compiler);
  }
}

ObjFunction *compile(DictuVM *vm, ObjModule *module, const char *source) {
  Parser parser;
  parser.vm = vm;
  parser.hadError = false;
  parser.panicMode = false;
  parser.module = module;

  Scanner scanner;
  initScanner(&scanner, source);
  parser.scanner = scanner;

  Compiler compiler;
  initCompiler(&parser, &compiler, NULL, TYPE_TOP_LEVEL, ACCESS_PUBLIC);

  advance(compiler.parser);

  if (!match(&compiler, TOKEN_EOF)) {
    do {
      declaration(&compiler);
    } while (!match(&compiler, TOKEN_EOF));
  }

  ObjFunction *function = endCompiler(&compiler);

  // If we're in the repl we need the constants to live for the entirety of the execution
  if (!vm->repl) {
    freeTable(vm, &vm->constants);
  }

  // If there was a compile error, the code is not valid, so don't
  // create a function.
  return parser.hadError ? NULL : function;
}

void grayCompilerRoots(DictuVM *vm) {
  Compiler *compiler = vm->compiler;

  while (compiler != NULL) {
    ClassCompiler *classCompiler = vm->compiler->class;

    while (classCompiler != NULL) {
      classCompiler = classCompiler->enclosing;
    }

    grayObject(vm, (Obj *) compiler->function);
    grayTable(vm, &compiler->stringConstants);
    compiler = compiler->enclosing;
  }
}
