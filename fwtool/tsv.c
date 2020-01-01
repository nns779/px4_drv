// tsv.c

#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "tsv.h"

int load(const uint8_t *buf, size_t buf_len, struct tsv_data *tsv, void *str_pool, size_t *str_poolsize)
{
	const uint8_t *p = buf;
	uint8_t *pool_p = (uint8_t *)str_pool;
	size_t l = buf_len, pool_size = 0, pool_remain = 0;
	int col_num = 0, row_num = 0;

	if (pool_p && str_poolsize)
		pool_remain = *str_poolsize;

	while (l && *p != '\0') {
		int num = 0;	// number of columns (current line)
		int newline = 0;

		do {
			const uint8_t *pb = p, *pp = NULL;

			while(l && *p != '\0') {
				if (*p == '\t') {
					// TAB
					pp = p;
					p++;
					l--;
					break;
				} else if (*p == '\r') {
					// CR
					pp = p;
					p++;
					l--;
					newline = 1;
					if (l && *p == '\n') {
						// CRLF
						p++;
						l--;
					}
					break;
				} else if (*p == '\n') {
					// LF
					pp = p;
					p++;
					l--;
					newline = 1;
					break;
				}
				p++;
				l--;
			}

			if (pp && pp > pb) {
				size_t sl = pp - pb + 1;

				if (pool_p) {
					memcpy(pool_p, pb, sl - 1);
					pool_p[sl - 1] = '\0';

					if (!col_num)
						tsv->name[num] = (char *)pool_p;
					else
						tsv->field[row_num][num] = (char *)pool_p;

					pool_p += sl;
					pool_remain -= sl;
				}

				pool_size += sl;
				num++;
			}
		} while (!newline && l && *p != '\0');

		if (num) {
			if (!col_num) {
				// first line
				col_num = num;
			} else {
				if (col_num != num) {
					errno = EBADMSG;
					return -1;
				}

				row_num++;
			}
		}
	}

	tsv->col_num = col_num;
	tsv->row_num = row_num;

	if (!str_pool && str_poolsize)
		*str_poolsize = pool_size;

	return 0;
}

int tsv_load(const uint8_t *buf, size_t len, struct tsv_data **tsv)
{
	int ret = 0, i;
	struct tsv_data tsv_tmp, *tsv_ret;
	char *pool;
	size_t array_poolsize, str_poolsize;

	if (!buf || !len || !tsv) {
		errno = EINVAL;
		return -1;
	}

	ret = load(buf, len, &tsv_tmp, NULL, &str_poolsize);
	if (ret)
		return ret;

	array_poolsize = /* name */(sizeof(char **) * tsv_tmp.col_num) + /* field */((sizeof(char ***) * tsv_tmp.row_num) + (sizeof(char **) * tsv_tmp.col_num * tsv_tmp.row_num));

	tsv_ret = (struct tsv_data *)malloc(sizeof(*tsv_ret) + array_poolsize + str_poolsize);
	if (!tsv_ret)
		return -1;

	pool = (char *)(tsv_ret + 1);

	tsv_ret->name = (char **)pool;
	pool += sizeof(char **) * tsv_tmp.col_num;

	tsv_ret->field = (char ***)pool;
	pool += sizeof(char ***) * tsv_tmp.row_num;

	for (i = 0; i < tsv_tmp.row_num; i++) {
		tsv_ret->field[i] = (char **)pool;
		pool += sizeof(char **) * tsv_tmp.col_num;
	}

	ret = load(buf, len, tsv_ret, pool, &str_poolsize);
	if (ret) {
		free(tsv_ret);
		return ret;
	}

	*tsv = tsv_ret;

	return ret;
}

void tsv_free(struct tsv_data *tsv)
{
	if (tsv)
		free(tsv);
}
