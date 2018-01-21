// tsv.c

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "tsv.h"

int load(const char *buf, size_t buf_len, struct tsv_data *tsv, void *str_pool, size_t *str_poolsize)
{
	const char *p = buf;
	char *sp = str_pool;
	size_t l = buf_len, npl = 0, pl = 0;
	int col_num = 0, row_num = 0;

	if (sp && str_poolsize) {
		pl = *str_poolsize;
	}

	while (l && *p != '\0') {
		int n = 0;	// number of columns (current line)
		int newline = 0;

		if (*p == '#') {
			// comment
			p++;
			l--;
			while (l && *p != '\0') {
				p++;
				l--;
				if (*(p - 1) == '\r') {
					if (l && *p == '\n') {
						p++;
						l--;
					}
					break;
				} else if (*(p - 1) == '\n') {
					break;
				}
			}
			continue;
		}

		do {
			const char *pb, *pp = NULL;

			pb = p;

			while(l && *p != '\0') {
				if (*p == '\t') {
					pp = p;
					while(--l && *++p == '\t')
						;
					break;
				} else if (*p == '\r') {
					pp = p;
					p++;
					l--;
					newline = 1;
					if (l && *p == '\n') {
						p++;
						l--;
					}
					break;
				} else if (*p == '\n') {
					pp = p;
					p++;
					l--;
					newline = 1;
					break;
				}
				p++;
				l--;
			}

			if (!pp) {
				pp = p;
			}

			if (pp > pb) {
				size_t sl;

				sl = pp - pb;

				if (pl) {
					strncpy(sp, pb, sl);
					sp[sl] = '\0';

					if (!col_num) {
						tsv->name[n] = sp;
					} else {
						tsv->field[row_num - 1][n] = sp;
					}
					sp += sl + 1;
					pl -= sl + 1;
				}
				npl += sl + 1;
				n++;
			}
		} while(!newline && l && *p != '\0');

		if (n) {
			if (!col_num) {
				col_num = n;
			} else if (col_num != n) {
				errno = EBADMSG;
				return -1;
			}

			row_num++;
		}
	}

	tsv->col_num = col_num;
	tsv->row_num = row_num - 1;

	if (!str_pool && str_poolsize) {
		*str_poolsize = npl;
	}

	return 0;
}

int tsv_load(const char *buf, size_t len, struct tsv_data **tsv)
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
	if (ret) {
		return ret;
	}

	array_poolsize = /* name */(sizeof(char **) * tsv_tmp.col_num) + /* field */((sizeof(char ***) * tsv_tmp.row_num) + (sizeof(char **) * tsv_tmp.col_num * tsv_tmp.row_num));

	tsv_ret = (struct tsv_data *)malloc(sizeof(*tsv_ret) + array_poolsize + str_poolsize);
	if (!tsv_ret) {
		return -1;
	}

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
