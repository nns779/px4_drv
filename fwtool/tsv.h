// tsv.h

#pragma once

#include <stdint.h>

struct tsv_data {
	int col_num;
	int row_num;
	char **name;
	char ***field;
};

int tsv_load(const uint8_t *buf, size_t len, struct tsv_data **tsv);
void tsv_free(struct tsv_data *tsv);
