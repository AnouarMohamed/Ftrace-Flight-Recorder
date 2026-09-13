#include "fdr.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct fdr_runtime fdr;

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint64_t parsed_size = 0;
	char *input;

	input = malloc(size + 1);
	if (input == NULL)
		return 0;

	memcpy(input, data, size);
	input[size] = '\0';

	(void)fdr_parse_size(input, &parsed_size);

	free(input);
	return 0;
}
