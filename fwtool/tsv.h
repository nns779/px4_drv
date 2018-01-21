// tsv.h

#pragma once

struct tsv_data {
	int col_num;
	int row_num;
	char **name;
	char ***field;
};

int tsv_load(const char *buf, size_t len, struct tsv_data **tsv);
void tsv_free(struct tsv_data *tsv);
