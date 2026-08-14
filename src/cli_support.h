#ifndef PXML_CLI_SUPPORT_H
#define PXML_CLI_SUPPORT_H

#include "pxml/pxml.h"

bool pxml_cli_read_file(const char *path, char **data, size_t *length);
bool pxml_cli_write_file(const char *path, const void *data, size_t length);
void pxml_cli_print_diagnostics(const PxmlDiagnosticList *diagnostics);

#endif
