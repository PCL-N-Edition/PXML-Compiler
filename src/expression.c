#include "pxml_internal.h"

#include <ctype.h>

typedef enum ExprTokenKind {
    EXPR_EOF = 0,
    EXPR_IDENTIFIER,
    EXPR_NUMBER,
    EXPR_STRING,
    EXPR_LEFT_PAREN,
    EXPR_RIGHT_PAREN,
    EXPR_LEFT_BRACKET,
    EXPR_RIGHT_BRACKET,
    EXPR_COMMA,
    EXPR_DOT,
    EXPR_QUESTION,
    EXPR_COLON,
    EXPR_PLUS,
    EXPR_MINUS,
    EXPR_STAR,
    EXPR_SLASH,
    EXPR_PERCENT,
    EXPR_BANG,
    EXPR_EQUAL_EQUAL,
    EXPR_BANG_EQUAL,
    EXPR_LESS,
    EXPR_LESS_EQUAL,
    EXPR_GREATER,
    EXPR_GREATER_EQUAL,
    EXPR_AND_AND,
    EXPR_OR_OR,
    EXPR_COALESCE,
    EXPR_INVALID
} ExprTokenKind;

typedef struct ExprToken {
    ExprTokenKind kind;
    const char *start;
    size_t length;
    size_t offset;
} ExprToken;

typedef struct ExprParser {
    const char *path;
    PxmlSourceSpan source_span;
    const char *source;
    size_t length;
    size_t offset;
    ExprToken current;
    PxmlDependencyList *dependencies;
    PxmlDiagnosticList *diagnostics;
} ExprParser;

static bool expr_is_identifier_start(unsigned char value)
{
    return isalpha((int)value) != 0 || value == (unsigned char)'_' || value >= 0x80U;
}

static bool expr_is_identifier_continue(unsigned char value)
{
    return expr_is_identifier_start(value) || isdigit((int)value) != 0 ||
           value == (unsigned char)'-';
}

static ExprToken make_expr_token(
    ExprTokenKind kind,
    const char *start,
    size_t length,
    size_t offset)
{
    ExprToken token;
    token.kind = kind;
    token.start = start;
    token.length = length;
    token.offset = offset;
    return token;
}

static ExprToken lex_expression(ExprParser *parser)
{
    const char *start;
    size_t start_offset;
    while (parser->offset < parser->length &&
           isspace((unsigned char)parser->source[parser->offset]) != 0) {
        parser->offset++;
    }
    start_offset = parser->offset;
    start = parser->source + start_offset;
    if (start_offset >= parser->length) {
        return make_expr_token(EXPR_EOF, start, 0U, start_offset);
    }
    if (expr_is_identifier_start((unsigned char)*start)) {
        parser->offset++;
        while (parser->offset < parser->length &&
               expr_is_identifier_continue((unsigned char)parser->source[parser->offset])) {
            parser->offset++;
        }
        return make_expr_token(
            EXPR_IDENTIFIER,
            start,
            parser->offset - start_offset,
            start_offset);
    }
    if (isdigit((unsigned char)*start) != 0 ||
        (*start == '.' && start_offset + 1U < parser->length &&
         isdigit((unsigned char)parser->source[start_offset + 1U]) != 0)) {
        bool dot = *start == '.';
        parser->offset++;
        while (parser->offset < parser->length) {
            char value = parser->source[parser->offset];
            if (isdigit((unsigned char)value) != 0) {
                parser->offset++;
            } else if (value == '.' && !dot) {
                dot = true;
                parser->offset++;
            } else {
                break;
            }
        }
        return make_expr_token(EXPR_NUMBER, start, parser->offset - start_offset, start_offset);
    }
    if (*start == '\'' || *start == '"') {
        char quote = *start;
        bool escaped = false;
        parser->offset++;
        while (parser->offset < parser->length) {
            char value = parser->source[parser->offset++];
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == quote) {
                return make_expr_token(
                    EXPR_STRING,
                    start,
                    parser->offset - start_offset,
                    start_offset);
            }
        }
        return make_expr_token(
            EXPR_INVALID,
            start,
            parser->offset - start_offset,
            start_offset);
    }
    parser->offset++;
    switch (*start) {
        case '(': return make_expr_token(EXPR_LEFT_PAREN, start, 1U, start_offset);
        case ')': return make_expr_token(EXPR_RIGHT_PAREN, start, 1U, start_offset);
        case '[': return make_expr_token(EXPR_LEFT_BRACKET, start, 1U, start_offset);
        case ']': return make_expr_token(EXPR_RIGHT_BRACKET, start, 1U, start_offset);
        case ',': return make_expr_token(EXPR_COMMA, start, 1U, start_offset);
        case '.': return make_expr_token(EXPR_DOT, start, 1U, start_offset);
        case ':': return make_expr_token(EXPR_COLON, start, 1U, start_offset);
        case '+': return make_expr_token(EXPR_PLUS, start, 1U, start_offset);
        case '-': return make_expr_token(EXPR_MINUS, start, 1U, start_offset);
        case '*': return make_expr_token(EXPR_STAR, start, 1U, start_offset);
        case '/': return make_expr_token(EXPR_SLASH, start, 1U, start_offset);
        case '%': return make_expr_token(EXPR_PERCENT, start, 1U, start_offset);
        case '!':
            if (parser->offset < parser->length && parser->source[parser->offset] == '=') {
                parser->offset++;
                return make_expr_token(EXPR_BANG_EQUAL, start, 2U, start_offset);
            }
            return make_expr_token(EXPR_BANG, start, 1U, start_offset);
        case '=':
            if (parser->offset < parser->length && parser->source[parser->offset] == '=') {
                parser->offset++;
                return make_expr_token(EXPR_EQUAL_EQUAL, start, 2U, start_offset);
            }
            break;
        case '<':
            if (parser->offset < parser->length && parser->source[parser->offset] == '=') {
                parser->offset++;
                return make_expr_token(EXPR_LESS_EQUAL, start, 2U, start_offset);
            }
            return make_expr_token(EXPR_LESS, start, 1U, start_offset);
        case '>':
            if (parser->offset < parser->length && parser->source[parser->offset] == '=') {
                parser->offset++;
                return make_expr_token(EXPR_GREATER_EQUAL, start, 2U, start_offset);
            }
            return make_expr_token(EXPR_GREATER, start, 1U, start_offset);
        case '&':
            if (parser->offset < parser->length && parser->source[parser->offset] == '&') {
                parser->offset++;
                return make_expr_token(EXPR_AND_AND, start, 2U, start_offset);
            }
            break;
        case '|':
            if (parser->offset < parser->length && parser->source[parser->offset] == '|') {
                parser->offset++;
                return make_expr_token(EXPR_OR_OR, start, 2U, start_offset);
            }
            break;
        case '?':
            if (parser->offset < parser->length && parser->source[parser->offset] == '?') {
                parser->offset++;
                return make_expr_token(EXPR_COALESCE, start, 2U, start_offset);
            }
            return make_expr_token(EXPR_QUESTION, start, 1U, start_offset);
        default: break;
    }
    return make_expr_token(EXPR_INVALID, start, parser->offset - start_offset, start_offset);
}

static void expr_advance(ExprParser *parser)
{
    parser->current = lex_expression(parser);
}

static PxmlSourceSpan expression_span(const ExprParser *parser, const ExprToken *token)
{
    PxmlSourceSpan span = parser->source_span;
    span.offset += token->offset;
    span.column += token->offset;
    span.length = token->length == 0U ? 1U : token->length;
    return span;
}

static bool expr_error(ExprParser *parser, const char *message)
{
    pxml_add_diagnostic(
        parser->diagnostics,
        "PXML3101",
        PXML_DIAGNOSTIC_ERROR,
        parser->path,
        expression_span(parser, &parser->current),
        "%s",
        message);
    return false;
}

static bool expr_accept(ExprParser *parser, ExprTokenKind kind)
{
    if (parser->current.kind != kind) {
        return false;
    }
    expr_advance(parser);
    return true;
}

static bool expr_expect(ExprParser *parser, ExprTokenKind kind, const char *message)
{
    return expr_accept(parser, kind) || expr_error(parser, message);
}

static bool dependency_exists(const PxmlDependencyList *dependencies, uint64_t value)
{
    size_t index;
    for (index = 0U; index < dependencies->count; ++index) {
        if (dependencies->items[index] == value) {
            return true;
        }
    }
    return false;
}

static bool add_dependency(ExprParser *parser, const ExprToken *identifier)
{
    char *name = pxml_strndup(identifier->start, identifier->length);
    uint64_t id;
    if (name == NULL) {
        return false;
    }
    if (pxml_string_equal(name, "true") || pxml_string_equal(name, "false") ||
        pxml_string_equal(name, "null") || pxml_string_equal(name, "item") ||
        pxml_string_equal(name, "local") || pxml_string_equal(name, "component")) {
        free(name);
        return true;
    }
    id = pxml_hash64(name, strlen(name), UINT64_C(0x44455053));
    free(name);
    if (dependency_exists(parser->dependencies, id)) {
        return true;
    }
    if (!pxml_reserve_array(
            (void **)&parser->dependencies->items,
            &parser->dependencies->capacity,
            parser->dependencies->count + 1U,
            sizeof(uint64_t))) {
        return false;
    }
    parser->dependencies->items[parser->dependencies->count++] = id;
    return true;
}

static bool function_is_allowed(const ExprToken *identifier)
{
    static const char *const functions[] = {
        "format", "clamp", "min", "max", "round", "floor", "ceil",
        "upper", "lower", "trim", "is-null", "not-null", "rgb", "rgba"
    };
    size_t index;
    for (index = 0U; index < sizeof(functions) / sizeof(functions[0]); ++index) {
        if (strlen(functions[index]) == identifier->length &&
            strncmp(functions[index], identifier->start, identifier->length) == 0) {
            return true;
        }
    }
    return false;
}

static bool parse_conditional(ExprParser *parser);

static bool parse_primary(ExprParser *parser)
{
    if (parser->current.kind == EXPR_NUMBER || parser->current.kind == EXPR_STRING) {
        expr_advance(parser);
        return true;
    }
    if (parser->current.kind == EXPR_LEFT_PAREN) {
        expr_advance(parser);
        return parse_conditional(parser) &&
               expr_expect(parser, EXPR_RIGHT_PAREN, "expected ')' after expression");
    }
    if (parser->current.kind == EXPR_IDENTIFIER) {
        ExprToken identifier = parser->current;
        expr_advance(parser);
        if (parser->current.kind == EXPR_LEFT_PAREN) {
            if (!function_is_allowed(&identifier)) {
                pxml_add_diagnostic(
                    parser->diagnostics,
                    "PXML3102",
                    PXML_DIAGNOSTIC_ERROR,
                    parser->path,
                    expression_span(parser, &identifier),
                    "function is not in the PXML pure-function whitelist");
                return false;
            }
            expr_advance(parser);
            if (parser->current.kind != EXPR_RIGHT_PAREN) {
                do {
                    if (!parse_conditional(parser)) {
                        return false;
                    }
                } while (expr_accept(parser, EXPR_COMMA));
            }
            return expr_expect(parser, EXPR_RIGHT_PAREN, "expected ')' after function arguments");
        }
        if (!add_dependency(parser, &identifier)) {
            return false;
        }
        while (parser->current.kind == EXPR_DOT ||
               parser->current.kind == EXPR_LEFT_BRACKET) {
            if (expr_accept(parser, EXPR_DOT)) {
                if (!expr_expect(parser, EXPR_IDENTIFIER, "expected path segment after '.'")) {
                    return false;
                }
            } else {
                expr_advance(parser);
                if (!parse_conditional(parser) ||
                    !expr_expect(parser, EXPR_RIGHT_BRACKET, "expected ']' after index")) {
                    return false;
                }
            }
        }
        return true;
    }
    if (parser->current.kind == EXPR_INVALID) {
        return expr_error(parser, "invalid or unterminated expression token");
    }
    return expr_error(parser, "expected literal, path, pure function call or parenthesized expression");
}

static bool parse_unary(ExprParser *parser)
{
    if (parser->current.kind == EXPR_BANG ||
        parser->current.kind == EXPR_MINUS ||
        parser->current.kind == EXPR_PLUS) {
        expr_advance(parser);
        return parse_unary(parser);
    }
    return parse_primary(parser);
}

static bool parse_binary(
    ExprParser *parser,
    bool (*operand)(ExprParser *),
    const ExprTokenKind *operators,
    size_t operator_count)
{
    size_t index;
    if (!operand(parser)) {
        return false;
    }
    for (;;) {
        bool matched = false;
        for (index = 0U; index < operator_count; ++index) {
            if (parser->current.kind == operators[index]) {
                expr_advance(parser);
                if (!operand(parser)) {
                    return false;
                }
                matched = true;
                break;
            }
        }
        if (!matched) {
            return true;
        }
    }
}

static bool parse_multiplicative(ExprParser *parser)
{
    static const ExprTokenKind operators[] = {EXPR_STAR, EXPR_SLASH, EXPR_PERCENT};
    return parse_binary(parser, parse_unary, operators, 3U);
}

static bool parse_additive(ExprParser *parser)
{
    static const ExprTokenKind operators[] = {EXPR_PLUS, EXPR_MINUS};
    return parse_binary(parser, parse_multiplicative, operators, 2U);
}

static bool parse_relational(ExprParser *parser)
{
    static const ExprTokenKind operators[] = {
        EXPR_LESS, EXPR_LESS_EQUAL, EXPR_GREATER, EXPR_GREATER_EQUAL};
    return parse_binary(parser, parse_additive, operators, 4U);
}

static bool parse_equality(ExprParser *parser)
{
    static const ExprTokenKind operators[] = {EXPR_EQUAL_EQUAL, EXPR_BANG_EQUAL};
    return parse_binary(parser, parse_relational, operators, 2U);
}

static bool parse_logical_and(ExprParser *parser)
{
    static const ExprTokenKind operators[] = {EXPR_AND_AND};
    return parse_binary(parser, parse_equality, operators, 1U);
}

static bool parse_logical_or(ExprParser *parser)
{
    static const ExprTokenKind operators[] = {EXPR_OR_OR};
    return parse_binary(parser, parse_logical_and, operators, 1U);
}

static bool parse_coalesce(ExprParser *parser)
{
    static const ExprTokenKind operators[] = {EXPR_COALESCE};
    return parse_binary(parser, parse_logical_or, operators, 1U);
}

static bool parse_conditional(ExprParser *parser)
{
    if (!parse_coalesce(parser)) {
        return false;
    }
    if (expr_accept(parser, EXPR_QUESTION)) {
        return parse_conditional(parser) &&
               expr_expect(parser, EXPR_COLON, "expected ':' in conditional expression") &&
               parse_conditional(parser);
    }
    return true;
}

bool pxml_validate_expression(
    const char *path,
    PxmlSourceSpan span,
    const char *expression,
    PxmlDependencyList *dependencies,
    PxmlDiagnosticList *diagnostics)
{
    ExprParser parser;
    bool result;
    memset(&parser, 0, sizeof(parser));
    parser.path = path;
    parser.source_span = span;
    parser.source = expression;
    parser.length = strlen(expression);
    parser.dependencies = dependencies;
    parser.diagnostics = diagnostics;
    expr_advance(&parser);
    result = parse_conditional(&parser);
    if (result && parser.current.kind != EXPR_EOF) {
        result = expr_error(&parser, "unexpected token after expression");
    }
    return result;
}
