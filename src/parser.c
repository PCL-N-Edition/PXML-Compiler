#include "pxml_internal.h"

#include <stdio.h>

typedef struct PxmlParser {
    PxmlLexer lexer;
    PxmlToken current;
    PxmlDiagnosticList *diagnostics;
    const char *path;
    PxmlDocument *document;
} PxmlParser;

static bool token_is_whitespace(const PxmlToken *token)
{
    size_t index;
    for (index = 0U; index < token->length; ++index) {
        char value = token->start[index];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            return false;
        }
    }
    return true;
}

static void parser_advance(PxmlParser *parser)
{
    parser->current = pxml_lexer_next(&parser->lexer);
}

static bool parser_expect(PxmlParser *parser, PxmlTokenKind kind, const char *description)
{
    if (parser->current.kind == kind) {
        return true;
    }
    pxml_add_diagnostic(
        parser->diagnostics,
        "PXML1101",
        PXML_DIAGNOSTIC_ERROR,
        parser->path,
        parser->current.span,
        "expected %s",
        description);
    return false;
}

static char *decode_value(
    PxmlParser *parser,
    const PxmlToken *token,
    bool attribute)
{
    PxmlStringBuilder builder;
    size_t index = 0U;
    pxml_sb_init(&builder);
    while (index < token->length) {
        char value = token->start[index];
        if (value != '&') {
            if (!pxml_sb_append_char(&builder, value)) {
                pxml_sb_destroy(&builder);
                return NULL;
            }
            ++index;
            continue;
        }
        {
            static const struct Entity {
                const char *encoded;
                char decoded;
            } entities[] = {
                {"&amp;", '&'},
                {"&lt;", '<'},
                {"&gt;", '>'},
                {"&quot;", '"'},
                {"&apos;", '\''}
            };
            size_t entity_index;
            bool matched = false;
            for (entity_index = 0U;
                 entity_index < sizeof(entities) / sizeof(entities[0]);
                 ++entity_index) {
                size_t encoded_length = strlen(entities[entity_index].encoded);
                if (index + encoded_length <= token->length &&
                    memcmp(token->start + index, entities[entity_index].encoded, encoded_length) == 0) {
                    if (!pxml_sb_append_char(&builder, entities[entity_index].decoded)) {
                        pxml_sb_destroy(&builder);
                        return NULL;
                    }
                    index += encoded_length;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                PxmlSourceSpan span = token->span;
                span.offset += index;
                span.column += index;
                span.length = 1U;
                pxml_add_diagnostic(
                    parser->diagnostics,
                    "PXML9002",
                    PXML_DIAGNOSTIC_ERROR,
                    parser->path,
                    span,
                    "only the five predefined XML escapes are allowed; custom entities are forbidden");
                pxml_sb_destroy(&builder);
                return NULL;
            }
        }
    }
    PXML_UNUSED(attribute);
    return pxml_sb_take(&builder, NULL);
}

static bool parse_attribute(PxmlParser *parser, PxmlNode *node)
{
    PxmlToken name_token = parser->current;
    char *name;
    char *value;
    if (!parser_expect(parser, PXML_TOKEN_NAME, "attribute name")) {
        return false;
    }
    name = pxml_document_intern_n(
        parser->document, name_token.start, name_token.length);
    if (name == NULL) {
        return false;
    }
    parser_advance(parser);
    if (!parser_expect(parser, PXML_TOKEN_EQUAL, "'=' after attribute name")) {
        return false;
    }
    parser_advance(parser);
    if (!parser_expect(parser, PXML_TOKEN_STRING, "quoted attribute value")) {
        return false;
    }
    value = decode_value(parser, &parser->current, true);
    if (value == NULL) {
        return false;
    }
    if (pxml_node_find_attribute(node, name) != NULL) {
        pxml_add_diagnostic(
            parser->diagnostics,
            "PXML1102",
            PXML_DIAGNOSTIC_ERROR,
            parser->path,
            name_token.span,
            "duplicate attribute '%s'",
            name);
        free(value);
        return false;
    }
    if (!pxml_node_add_attribute(node, name, value, name_token.span)) {
        free(value);
        return false;
    }
    free(value);
    parser_advance(parser);
    return true;
}

static PxmlNode *parse_element(PxmlParser *parser, size_t depth)
{
    PxmlToken opening;
    PxmlToken name_token;
    PxmlNode *node;
    char *name;
    if (depth > 1024U) {
        pxml_add_diagnostic(
            parser->diagnostics,
            "PXML9003",
            PXML_DIAGNOSTIC_ERROR,
            parser->path,
            parser->current.span,
            "element nesting exceeds the parser safety limit");
        return NULL;
    }
    opening = parser->current;
    if (!parser_expect(parser, PXML_TOKEN_LT, "'<'")) {
        return NULL;
    }
    parser_advance(parser);
    if (!parser_expect(parser, PXML_TOKEN_NAME, "element name")) {
        return NULL;
    }
    name_token = parser->current;
    name = pxml_document_intern_n(
        parser->document, name_token.start, name_token.length);
    if (name == NULL) {
        return NULL;
    }
    node = pxml_node_create(parser->document, PXML_SYNTAX_ELEMENT, name, NULL);
    if (node == NULL) {
        return NULL;
    }
    node->span = opening.span;
    parser_advance(parser);
    while (parser->current.kind == PXML_TOKEN_NAME) {
        if (!parse_attribute(parser, node)) {
            pxml_node_destroy(node);
            return NULL;
        }
    }
    if (parser->current.kind == PXML_TOKEN_SLASH_GT) {
        node->span.length = parser->current.span.offset + parser->current.span.length - node->span.offset;
        parser_advance(parser);
        return node;
    }
    if (!parser_expect(parser, PXML_TOKEN_GT, "'>' or '/>'")) {
        pxml_node_destroy(node);
        return NULL;
    }
    parser_advance(parser);
    while (parser->current.kind != PXML_TOKEN_EOF &&
           parser->current.kind != PXML_TOKEN_LT_SLASH) {
        PxmlNode *child = NULL;
        if (parser->current.kind == PXML_TOKEN_LT) {
            child = parse_element(parser, depth + 1U);
        } else if (parser->current.kind == PXML_TOKEN_TEXT ||
                   parser->current.kind == PXML_TOKEN_COMMENT) {
            PxmlSyntaxKind kind = parser->current.kind == PXML_TOKEN_TEXT
                ? PXML_SYNTAX_TEXT
                : PXML_SYNTAX_COMMENT;
            char *text = decode_value(parser, &parser->current, false);
            if (text != NULL) {
                child = pxml_node_create(parser->document, kind, NULL, text);
                free(text);
                if (child != NULL) {
                    child->span = parser->current.span;
                }
            }
            parser_advance(parser);
        } else {
            pxml_add_diagnostic(
                parser->diagnostics,
                "PXML1103",
                PXML_DIAGNOSTIC_ERROR,
                parser->path,
                parser->current.span,
                "unexpected token inside element '%s'",
                node->name);
            parser_advance(parser);
        }
        if (child == NULL || !pxml_node_add_child(node, child)) {
            pxml_node_destroy(child);
            pxml_node_destroy(node);
            return NULL;
        }
    }
    if (!parser_expect(parser, PXML_TOKEN_LT_SLASH, "closing tag")) {
        pxml_node_destroy(node);
        return NULL;
    }
    parser_advance(parser);
    if (!parser_expect(parser, PXML_TOKEN_NAME, "closing element name")) {
        pxml_node_destroy(node);
        return NULL;
    }
    if (strlen(node->name) != parser->current.length ||
        memcmp(parser->current.start, node->name, parser->current.length) != 0) {
        pxml_add_diagnostic(
            parser->diagnostics,
            "PXML1104",
            PXML_DIAGNOSTIC_ERROR,
            parser->path,
            parser->current.span,
            "closing tag '%.*s' does not match '%s'",
            (int)parser->current.length,
            parser->current.start,
            node->name);
        pxml_node_destroy(node);
        return NULL;
    }
    parser_advance(parser);
    if (!parser_expect(parser, PXML_TOKEN_GT, "'>' after closing element name")) {
        pxml_node_destroy(node);
        return NULL;
    }
    node->span.length = parser->current.span.offset + parser->current.span.length - node->span.offset;
    parser_advance(parser);
    return node;
}

static bool parse_prolog(PxmlParser *parser, PxmlDocument *document)
{
    PxmlNode pseudo;
    const PxmlAttribute *version;
    const PxmlAttribute *strict;
    memset(&pseudo, 0, sizeof(pseudo));
    pseudo._owner = document;
    if (parser->current.kind != PXML_TOKEN_PROLOG_START) {
        return true;
    }
    parser_advance(parser);
    while (parser->current.kind == PXML_TOKEN_NAME) {
        if (!parse_attribute(parser, &pseudo)) {
            goto cleanup_failure;
        }
    }
    if (!parser_expect(parser, PXML_TOKEN_PROLOG_END, "'?>'")) {
        goto cleanup_failure;
    }
    parser_advance(parser);
    version = pxml_node_find_attribute(&pseudo, "version");
    strict = pxml_node_find_attribute(&pseudo, "strict");
    if (version != NULL) {
        document->language_version = pxml_document_intern(document, version->value);
    }
    if (strict != NULL) {
        if (pxml_string_equal(strict->value, "true")) {
            document->strict = true;
        } else if (!pxml_string_equal(strict->value, "false")) {
            pxml_add_diagnostic(
                parser->diagnostics,
                "PXML2101",
                PXML_DIAGNOSTIC_ERROR,
                parser->path,
                strict->span,
                "prolog attribute 'strict' expects true or false");
        }
    }
    return document->language_version != NULL;

cleanup_failure:
    return false;
}

PxmlDocument *pxml_parse_text(
    const char *path,
    const char *source,
    size_t source_length,
    PxmlDiagnosticList *diagnostics)
{
    PxmlDocument *document;
    PxmlParser parser;
    if (source == NULL || diagnostics == NULL) {
        return NULL;
    }
    document = pxml_document_create(path, source, source_length);
    if (document == NULL) return NULL;
    memset(&parser, 0, sizeof(parser));
    parser.path = document->path;
    parser.diagnostics = diagnostics;
    parser.document = document;
    pxml_lexer_init(
        &parser.lexer,
        document->path,
        document->source,
        document->source_length,
        diagnostics);
    parser_advance(&parser);
    if (!parse_prolog(&parser, document)) {
        pxml_document_destroy(document);
        return NULL;
    }
    while (parser.current.kind == PXML_TOKEN_COMMENT ||
           (parser.current.kind == PXML_TOKEN_TEXT &&
            token_is_whitespace(&parser.current))) {
        parser_advance(&parser);
    }
    if (parser.current.kind != PXML_TOKEN_LT) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML1105",
            PXML_DIAGNOSTIC_ERROR,
            document->path,
            parser.current.span,
            "document must contain exactly one root element");
        pxml_document_destroy(document);
        return NULL;
    }
    document->root = parse_element(&parser, 0U);
    while (parser.current.kind == PXML_TOKEN_COMMENT ||
           parser.current.kind == PXML_TOKEN_TEXT) {
        if (parser.current.kind == PXML_TOKEN_TEXT) {
            char *text = pxml_strndup(parser.current.start, parser.current.length);
            bool whitespace = text != NULL && pxml_is_whitespace_text(text);
            free(text);
            if (!whitespace) {
                break;
            }
        }
        parser_advance(&parser);
    }
    if (document->root == NULL || parser.current.kind != PXML_TOKEN_EOF) {
        if (parser.current.kind != PXML_TOKEN_EOF) {
            pxml_add_diagnostic(
                diagnostics,
                "PXML1106",
                PXML_DIAGNOSTIC_ERROR,
                document->path,
                parser.current.span,
                "unexpected content after the root element");
        }
        pxml_document_destroy(document);
        return NULL;
    }
    if (!pxml_string_equal(document->language_version, "1.0")) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML8001",
            PXML_DIAGNOSTIC_ERROR,
            document->path,
            document->root->span,
            "unsupported PXML language version '%s'",
            document->language_version);
    }
    return document;
}

PxmlDocument *pxml_parse_file(const char *path, PxmlDiagnosticList *diagnostics)
{
    char *text;
    size_t length;
    PxmlDocument *document;
    if (!pxml_read_file_text(path, &text, &length, diagnostics)) {
        return NULL;
    }
    document = pxml_parse_text(path, text, length, diagnostics);
    free(text);
    return document;
}
