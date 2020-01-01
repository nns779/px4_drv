// fwtool.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "tsv.h"
#include "crc32.h"

#define le32_load(p)	(*((uint8_t *)(p)) | *(((uint8_t *)(p)) + 1) << 8 | *(((uint8_t *)(p)) + 2) << 16 | *(((uint8_t *)(p)) + 3) << 24)

struct fwinfo {
	char *desc;
	unsigned long size;
	uint32_t crc32;
	uint8_t align;
	uint32_t code_ofs;
	uint32_t segment_ofs;
	uint32_t partition_ofs;
	uint32_t fw_crc32;
};

static const char *name[] = {
	"description",
	"size",
	"crc32",
	"align",
	"firmware_code",
	"firmware_segment",
	"firmware_partition",
	"firmware_crc32"
};

#define NAME_NUM	(sizeof(name) / sizeof(name[0]))

static int load_file(const char *path, uint8_t **buf, unsigned long *size)
{
	int ret = -1, fd = -1;
	FILE *fp = NULL;
	struct stat stbuf;
	off_t sz;
	uint8_t *b;

#if defined(_WIN32) || defined(_WIN64)
	if (_sopen_s(&fd, path, _O_RDONLY | O_BINARY, _SH_DENYWR, _S_IREAD) || fd == -1) {
#else
	fd = open(path, O_RDONLY);
	if (fd == -1) {
#endif
		fprintf(stderr, "Couldn't open file '%s' to read.\n", path);
		goto exit;
	}

#if defined(_WIN32) || defined(_WIN64)
	fp = _fdopen(fd, "rb");
	if (!fp) {
		fprintf(stderr, "_fdopen() failed.\n");
#else
	fp = fdopen(fd, "rb");
	if (!fp) {
		fprintf(stderr, "fdopen() failed.\n");
#endif
		goto exit;
	}

	if (fstat(fd, &stbuf) == -1) {
		fprintf(stderr, "fstat() failed.\n");
		goto exit;
	}

	sz = stbuf.st_size;
	if (sz < 0) {
		fprintf(stderr, "Invalid file size.\n");
		goto exit;
	}

	b = (uint8_t *)malloc(sz);
	if (b == NULL) {
		fprintf(stderr, "No enough memory.\n");
		goto exit;
	}

	if (fread(b, sz, 1, fp) < 1) {
		fprintf(stderr, "Failed to read from file '%s'.\n", path);
		free(b);
	} else {
		*buf = b;
		*size = sz;
		ret = 0;
	}

exit:
	if (fp)
		fclose(fp);
	else if (fd != -1)
#if defined(_WIN32) || defined(_WIN64)
		_close(fd);
#else
		close(fd);
#endif

	return ret;
}

static int load_tsv_file(struct tsv_data **tsv)
{
	int ret = -1;
	uint8_t *buf;
	unsigned long size;

	ret = load_file("fwinfo.tsv", &buf, &size);
	if (ret == -1)
		return ret;

	ret = tsv_load(buf, size, tsv);
	if (ret == -1)
		fprintf(stderr, "File 'fwinfo.tsv' is invalid.\n");

	free(buf);

	return ret;
}

static int load_fwinfo(struct tsv_data *tsv, struct fwinfo *fi, int num)
{
	int i, j;
	int col_num = tsv->col_num;
	int name_map[NAME_NUM];

	for (i = 0; i < NAME_NUM; i++) {
		name_map[i] = -1;

		for (j = 0; j < col_num; j++) {
			if (!strcmp(tsv->name[j], name[i])) {
				name_map[i] = j;
				break;
			}
		}

		if (name_map[i] == -1) {
			fprintf(stderr, "No enough columns in 'fwinfo.tsv'.\n");
			return -1;
		}
	}

	for (i = 0; i < num; i++) {
		fi[i].desc = tsv->field[i][name_map[0]];
		fi[i].size = strtoul(tsv->field[i][name_map[1]], NULL, 10);
		fi[i].crc32 = strtoul(tsv->field[i][name_map[2]], NULL, 16);
		fi[i].align = (uint8_t)strtoul(tsv->field[i][name_map[3]], NULL, 10);
		fi[i].code_ofs = strtoul(tsv->field[i][name_map[4]], NULL, 16);
		fi[i].segment_ofs = strtoul(tsv->field[i][name_map[5]], NULL, 16);
		fi[i].partition_ofs = strtoul(tsv->field[i][name_map[6]], NULL, 16);
		fi[i].fw_crc32 = strtoul(tsv->field[i][name_map[7]], NULL, 16);
	}

	return 0;
}

static int output_firmware(struct fwinfo *fi, const uint8_t *buf, unsigned long size, const char *path)
{
	uint8_t i, n;
	uint8_t align;
	uint32_t partition_ofs, segment_ofs, code_ofs, crc32;
	size_t code_len = 0;
	FILE *fp;

	align = fi->align;
	partition_ofs = fi->partition_ofs;
	segment_ofs = fi->segment_ofs;
	code_ofs = fi->code_ofs;

	if (align & (align - 1)) {
		fprintf(stderr, "Invalid alignment value.\n");
		return -1;
	}

	if (align < 4)
		align = 4;

	if (size < partition_ofs + 1) {
		fprintf(stderr, "Invalid file size.\n");
		return -1;
	}

	n = buf[partition_ofs];

	if (size < segment_ofs + (n * align * 2)) {
		fprintf(stderr, "Invalid file size.\n");
		return -1;
	}

	for (i = 0; i < n; i++) {
		uint32_t type;

		type = le32_load(&buf[segment_ofs + (i * align * 2)]);

		if (type != 0x01) {
			fprintf(stderr, "This driver's firmware is not supported.\n");
			return -1;
		}

		code_len += le32_load(&buf[segment_ofs + (i * align * 2) + (align * 1)]);
	}

	if (size < code_ofs + code_len) {
		fprintf(stderr, "Invalid file size.\n");
		return -1;
	}

	crc32 = crc32_calc(&buf[code_ofs], code_len);

	fprintf(stderr, "Firmware length: %zu %s\n", code_len, (code_len == 1) ? "byte" : "bytes");
	fprintf(stderr, "Firmware CRC32: %08x\n", crc32);

	if (fi->fw_crc32 && crc32 != fi->fw_crc32) {
		fprintf(stderr, "Incorrect CRC32 checksum!\n");
		return -1;
	}

#if defined(_WIN32) || defined(_WIN64)
	if (fopen_s(&fp, path, "wb") || !fp) {
#else
	fp = fopen(path, "wb");
	if (!fp) {
#endif
		fprintf(stderr, "Couldn't open file '%s' to write.\n", path);
		return -1;
	}

	if (fwrite(&buf[code_ofs], code_len, 1, fp) < 1) {
		fprintf(stderr, "Failed to write to file '%s'.\n", path);
		fclose(fp);
		return -1;
	}

	fclose(fp);

	return 0;
}

static void usage()
{
	fprintf(stderr, "usage: fwtool <driver binary> <output>\n");
}

int main(int argc, char *argv[])
{
	int ret, num, i;
	struct tsv_data *tsv = NULL;
	struct fwinfo *fi = NULL;
	uint8_t *buf = NULL;
	unsigned long size;
	uint32_t crc32;

	fprintf(stderr, "fwtool for px4 drivers\n\n");

	if (argc < 3) {
		usage();
		return 0;
	}

	fprintf(stderr, "Driver file (in)    : %s\n", argv[1]);
	fprintf(stderr, "Firmware file (out) : %s\n\n", argv[2]);

	ret = load_tsv_file(&tsv);
	if (ret) {
		fprintf(stderr, "Failed to load firmware information file.\n");
		goto fail;
	}

	num = tsv->row_num;

	if (!num) {
		fprintf(stderr, "No rows in 'fwinfo.tsv'.\n");
		goto fail;
	}

	fi = (struct fwinfo *)malloc(sizeof(struct fwinfo) * num);
	if (!fi) {
		fprintf(stderr, "No enough memory.\n");
		goto fail;
	}

	ret = load_fwinfo(tsv, fi, num);
	if (ret) {
		fprintf(stderr, "Failed to load firmware information.\n");
		goto fail;
	}

	ret = load_file(argv[1], &buf, &size);
	if (ret == -1) {
		fprintf(stderr, "Failed to load driver file.\n");
		goto fail;
	}

	crc32 = crc32_calc(buf, size);

	for (i = 0; i < num; i++) {
		if (size == fi[i].size && crc32 == fi[i].crc32) {
			fprintf(stderr, "Driver description: %s\n", fi[i].desc);

			ret = output_firmware(&fi[i], buf, size, argv[2]);
			if (!ret)
				fprintf(stderr, "OK.\n");

			break;
		}
	}

	if (i >= num) {
		fprintf(stderr, "Unknown driver file.\n");
		goto fail;
	}

	free(buf);
	free(fi);
	tsv_free(tsv);

	return 0;

fail:
	if (buf)
		free(buf);

	if (fi)
		free(fi);

	tsv_free(tsv);

	return 1;
}
