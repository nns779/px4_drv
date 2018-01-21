// crc32.h

#pragma once

#include <stdint.h>
#include <stddef.h>

uint32_t crc32_calc(const void *buf, size_t size);
