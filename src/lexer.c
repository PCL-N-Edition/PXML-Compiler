#include "pxml_internal.h"

#include <ctype.h>

static bool lexer_matches(const PxmlLexer *lexer, const char *value)
{
    size_t length = strlen(value);
    return lexer->offset + length <= lexer->length &&
           memcmp(lexer->source + lexer->offset, value, length) == 0;
}

static void lexer_advance(PxmlLexer *lexer, size_t count)
{
    size_t index;
    for (index = 0U; index < count && lexer->offset < lexer->length; ++index) {
        char value = lexer->source[lexer->offset++];
        if (value == '\n') {
            lexer->line++;
            lexer->column = 1U;
        } else {
            lexer->column++;
        }
    }
}

static void lexer_advance_span(PxmlLexer *lexer, size_t count)
{
    size_t consumed = 0U;
    size_t final_column = lexer->column;
    size_t line_count = 0U;
    while (consumed < count) {
        size_t relative = pxml_scan_byte(
            lexer->source + lexer->offset + consumed,
            count - consumed,
            (unsigned char)'\n');
        if (relative == count - consumed) {
            final_column += relative;
            consumed = count;
        } else {
            consumed += relative + 1U;
            line_count++;
            final_column = 1U;
        }
    }
    lexer->offset += count;
    lexer->line += line_count;
    lexer->column = final_column;
}

static PxmlToken make_token(
    PxmlLexer *lexer,
    PxmlTokenKind kind,
    size_t offset,
    size_t line,
    size_t column,
    const char *start,
    size_t length)
{
    PxmlToken token;
    PXML_UNUSED(lexer);
    memset(&token, 0, sizeof(token));
    token.kind = kind;
    token.start = start;
    token.length = length;
    token.span.offset = offset;
    token.span.length = length;
    token.span.line = line;
    token.span.column = column;
    return token;
}

static bool is_name_start(unsigned char value)
{
    return isalpha((int)value) != 0 || value == (unsigned char)'_' || value >= 0x80U;
}

static bool is_name_continue(unsigned char value)
{
    return is_name_start(value) || isdigit((int)value) != 0 ||
           value == (unsigned char)'-' || value == (unsigned char)'.' ||
           value == (unsigned char)':';
}

void pxml_lexer_init(
    PxmlLexer *lexer,
    const char *path,
    const char *source,
    size_t length,
    PxmlDiagnosticList *diagnostics)
{
    memset(lexer, 0, sizeof(*lexer));
    lexer->path = path;
    lexer->source = source;
    lexer->length = length;
    lexer->line = 1U;
    lexer->column = 1U;
    lexer->diagnostics = diagnostics;
    if (length >= 3U &&
        (unsigned char)source[0] == 0xEFU &&
        (unsigned char)source[1] == 0xBBU &&
        (unsigned char)source[2] == 0xBFU) {
        lexer_advance(lexer, 3U);
    }
}

PxmlToken pxml_lexer_next(PxmlLexer *lexer)
{
    size_t start_offset;
    size_t start_line;
    size_t start_column;
    const char *start;
    if (lexer->in_tag) {
        while (lexer->offset < lexer->length) {
            char value = lexer->source[lexer->offset];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            lexer_advance(lexer, 1U);
        }
    }
    start_offset = lexer->offset;
    start_line = lexer->line;
    start_column = lexer->column;
    start = lexer->source + lexer->offset;
    if (lexer->offset >= lexer->length) {
        return make_token(
            lexer,
            PXML_TOKEN_EOF,
            start_offset,
            start_line,
            start_column,
            start,
            0U);
    }

    if (!lexer->in_tag) {
        if (lexer_matches(lexer, "<!--")) {
            size_t content_offset;
            size_t content_line;
            size_t content_column;
            lexer_advance(lexer, 4U);
            content_offset = lexer->offset;
            content_line = lexer->line;
            content_column = lexer->column;
            while (lexer->offset < lexer->length && !lexer_matches(lexer, "-->")) {
                lexer_advance(lexer, 1U);
            }
            if (lexer->offset >= lexer->length) {
                PxmlSourceSpan span = {
                    start_offset,
                    lexer->offset - start_offset,
                    start_line,
                    start_column};
                pxml_add_diagnostic(
                    lexer->diagnostics,
                    "PXML1004",
                    PXML_DIAGNOSTIC_ERROR,
                    lexer->path,
                    span,
                    "unterminated comment");
                return make_token(
                    lexer,
                    PXML_TOKEN_INVALID,
                    start_offset,
                    start_line,
                    start_column,
                    start,
                    lexer->offset - start_offset);
            }
            {
                PxmlToken token = make_token(
                    lexer,
                    PXML_TOKEN_COMMENT,
                    content_offset,
                    content_line,
                    content_column,
                    lexer->source + content_offset,
                    lexer->offset - content_offset);
                lexer_advance(lexer, 3U);
                return token;
            }
        }
        if (lexer_matches(lexer, "<?pxml")) {
            lexer_advance(lexer, 6U);
            lexer->in_tag = true;
            return make_token(
                lexer,
                PXML_TOKEN_PROLOG_START,
                start_offset,
                start_line,
                start_column,
                start,
                6U);
        }
        if (lexer_matches(lexer, "<?") || lexer_matches(lexer, "<!DOCTYPE") ||
            lexer_matches(lexer, "<!ENTITY")) {
            PxmlSourceSpan span = {start_offset, 2U, start_line, start_column};
            pxml_add_diagnostic(
                lexer->diagnostics,
                "PXML9001",
                PXML_DIAGNOSTIC_ERROR,
                lexer->path,
                span,
                "processing instructions, DTD and entity declarations are forbidden");
            lexer_advance(lexer, 2U);
            return make_token(
                lexer,
                PXML_TOKEN_INVALID,
                start_offset,
                start_line,
                start_column,
                start,
                2U);
        }
        if (lexer_matches(lexer, "</")) {
            lexer_advance(lexer, 2U);
            lexer->in_tag = true;
            return make_token(
                lexer,
                PXML_TOKEN_LT_SLASH,
                start_offset,
                start_line,
                start_column,
                start,
                2U);
        }
        if (*start == '<') {
            lexer_advance(lexer, 1U);
            lexer->in_tag = true;
            return make_token(
                lexer,
                PXML_TOKEN_LT,
                start_offset,
                start_line,
                start_column,
                start,
                1U);
        }
        lexer_advance_span(
            lexer,
            pxml_scan_byte(
                lexer->source + lexer->offset,
                lexer->length - lexer->offset,
                (unsigned char)'<'));
        return make_token(
            lexer,
            PXML_TOKEN_TEXT,
            start_offset,
            start_line,
            start_column,
            start,
            lexer->offset - start_offset);
    }

    if (lexer_matches(lexer, "?>")) {
        lexer_advance(lexer, 2U);
        lexer->in_tag = false;
        return make_token(
            lexer,
            PXML_TOKEN_PROLOG_END,
            start_offset,
            start_line,
            start_column,
            start,
            2U);
    }
    if (lexer_matches(lexer, "/>")) {
        lexer_advance(lexer, 2U);
        lexer->in_tag = false;
        return make_token(
            lexer,
            PXML_TOKEN_SLASH_GT,
            start_offset,
            start_line,
            start_column,
            start,
            2U);
    }
    if (*start == '>') {
        lexer_advance(lexer, 1U);
        lexer->in_tag = false;
        return make_token(
            lexer,
            PXML_TOKEN_GT,
            start_offset,
            start_line,
            start_column,
            start,
            1U);
    }
    if (*start == '=') {
        lexer_advance(lexer, 1U);
        return make_token(
            lexer,
            PXML_TOKEN_EQUAL,
            start_offset,
            start_line,
            start_column,
            start,
            1U);
    }
    if (*start == '"' || *start == '\'') {
        char quote = *start;
        PxmlToken token;
        lexer_advance(lexer, 1U);
        start_offset = lexer->offset;
        start_line = lexer->line;
        start_column = lexer->column;
        start = lexer->source + lexer->offset;
        lexer_advance_span(
            lexer,
            pxml_scan_byte(
                lexer->source + lexer->offset,
                lexer->length - lexer->offset,
                (unsigned char)quote));
        if (lexer->offset >= lexer->length) {
            PxmlSourceSpan span = {
                start_offset,
                lexer->offset - start_offset,
                start_line,
                start_column};
            pxml_add_diagnostic(
                lexer->diagnostics,
                "PXML1005",
                PXML_DIAGNOSTIC_ERROR,
                lexer->path,
                span,
                "unterminated attribute string");
            return make_token(
                lexer,
                PXML_TOKEN_INVALID,
                start_offset,
                start_line,
                start_column,
                start,
                lexer->offset - start_offset);
        }
        token = make_token(
            lexer,
            PXML_TOKEN_STRING,
            start_offset,
            start_line,
            start_column,
            start,
            lexer->offset - start_offset);
        token.quote = quote;
        lexer_advance(lexer, 1U);
        return token;
    }
    if (is_name_start((unsigned char)*start)) {
        lexer_advance(lexer, 1U);
        while (lexer->offset < lexer->length &&
               is_name_continue((unsigned char)lexer->source[lexer->offset])) {
            lexer_advance(lexer, 1U);
        }
        return make_token(
            lexer,
            PXML_TOKEN_NAME,
            start_offset,
            start_line,
            start_column,
            start,
            lexer->offset - start_offset);
    }

    {
        PxmlSourceSpan span = {start_offset, 1U, start_line, start_column};
        pxml_add_diagnostic(
            lexer->diagnostics,
            "PXML1006",
            PXML_DIAGNOSTIC_ERROR,
            lexer->path,
            span,
            "unexpected character '%c' in tag",
            *start);
    }
    lexer_advance(lexer, 1U);
    return make_token(
        lexer,
        PXML_TOKEN_INVALID,
        start_offset,
        start_line,
        start_column,
        start,
        1U);
}
