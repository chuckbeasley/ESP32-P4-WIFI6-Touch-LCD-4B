/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loads vendor_db.bin (see scripts/gen_vendor_table.py for the layout) into PSRAM and
 * serves lookups out of it with a binary search. The file can be refreshed on the SD
 * card and reloaded on the next boot -- no firmware rebuild.
 */
#include "vendor_lookup.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "VendorLookup";

#define VENDOR_SEC_OUI        0
#define VENDOR_SEC_COMPANY    1
#define VENDOR_SEC_SERVICE    2
#define VENDOR_SEC_APPEARANCE 3
#define VENDOR_SEC_FASTPAIR   4
#define VENDOR_SEC_COUNT      5
#define VENDOR_TOTAL_SECTIONS 6

#define VENDOR_MAGIC          0x42444C56u  /* "VLDB" little-endian */
#define VENDOR_VERSION        1

typedef struct {
    uint32_t key;
    uint32_t name_off;
} vendor_entry_t;

typedef struct {
    const vendor_entry_t *entries;
    uint32_t count;
    const char *pool;
} vendor_section_t;

static vendor_section_t s_sec[VENDOR_SEC_COUNT];
static const uint8_t *s_rules;
static uint32_t s_rule_count;
static uint8_t *s_buf;
static bool s_loaded;

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool vendor_lookup_is_loaded(void)
{
    return s_loaded;
}

esp_err_t vendor_lookup_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "no database at %s; lookups return NULL", path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "fseek failed on %s", path);
        fclose(f);
        return ESP_FAIL;
    }
    const long sz = ftell(f);
    rewind(f);
    ESP_LOGI(TAG, "reading %ld bytes from %s", sz, path);
    if (sz < 8) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "out of memory for %ld-byte database", sz);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        ESP_LOGE(TAG, "short read from %s", path);
        fclose(f);
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    fclose(f);

    if (rd_u32(buf) != VENDOR_MAGIC || rd_u16(buf + 4) != VENDOR_VERSION ||
        rd_u16(buf + 6) != VENDOR_TOTAL_SECTIONS) {
        ESP_LOGE(TAG, "bad database header");
        heap_caps_free(buf);
        return ESP_ERR_INVALID_VERSION;
    }

    size_t off = 8;
    for (int i = 0; i < VENDOR_SEC_COUNT; i++) {
        if (off + 4 > (size_t)sz) {
            heap_caps_free(buf);
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t count = rd_u32(buf + off);
        off += 4;
        if (off + (size_t)count * 8 + 4 > (size_t)sz) {
            heap_caps_free(buf);
            return ESP_ERR_INVALID_SIZE;
        }
        s_sec[i].entries = (const vendor_entry_t *)(buf + off);
        s_sec[i].count = count;
        off += (size_t)count * 8;
        const uint32_t pool_size = rd_u32(buf + off);
        off += 4;
        if (off + pool_size > (size_t)sz) {
            heap_caps_free(buf);
            return ESP_ERR_INVALID_SIZE;
        }
        s_sec[i].pool = (const char *)(buf + off);
        off += pool_size;
    }

    /* Section 5: classification rules (count, then 12 bytes per rule). */
    if (off + 4 > (size_t)sz) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    s_rule_count = rd_u32(buf + off);
    off += 4;
    if (off + (size_t)s_rule_count * 12 > (size_t)sz) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    s_rules = buf + off;
    off += (size_t)s_rule_count * 12;

    if (s_buf != NULL) {
        heap_caps_free(s_buf);
    }
    s_buf = buf;
    s_loaded = true;

    ESP_LOGI(TAG, "loaded %u entries from %s (%ld bytes)",
             (unsigned)(s_sec[0].count + s_sec[1].count + s_sec[2].count +
                        s_sec[3].count + s_sec[4].count),
             path, sz);
    return ESP_OK;
}

static const char *lookup_section(int section, uint32_t key)
{
    if (!s_loaded) {
        return NULL;
    }
    const vendor_section_t *sec = &s_sec[section];
    uint32_t lo = 0, hi = sec->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (sec->entries[mid].key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < sec->count && sec->entries[lo].key == key) {
        return sec->pool + sec->entries[lo].name_off;
    }
    return NULL;
}

const char *vendor_lookup_oui(const uint8_t mac[6])
{
    if (mac == NULL) {
        return NULL;
    }
    const uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) |
                         (uint32_t)mac[2];
    return lookup_section(VENDOR_SEC_OUI, key);
}

const char *vendor_lookup_company(uint16_t company_id)
{
    return lookup_section(VENDOR_SEC_COMPANY, (uint32_t)company_id);
}

const char *vendor_lookup_service(uint16_t uuid)
{
    return lookup_section(VENDOR_SEC_SERVICE, (uint32_t)uuid);
}

const char *vendor_lookup_appearance(uint16_t appearance)
{
    return lookup_section(VENDOR_SEC_APPEARANCE, (uint32_t)appearance);
}

const char *vendor_lookup_fastpair(uint32_t model_id)
{
    return lookup_section(VENDOR_SEC_FASTPAIR, model_id);
}

bool vendor_rule_eval(uint8_t ad_type, const uint8_t *body, uint8_t body_len,
                      uint32_t *flags, uint8_t *kind)
{
    if (!s_loaded || body == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < s_rule_count; i++) {
        const uint8_t *r = s_rules + (size_t)i * 12;
        if (r[0] != ad_type) {
            continue;
        }
        const uint16_t key = (uint16_t)(r[1] | (r[2] << 8));
        if (body_len < 2 || body[0] != (key & 0xFF) || body[1] != ((key >> 8) & 0xFF)) {
            continue;
        }
        if (body_len < r[9]) {               /* min_len */
            continue;
        }
        const uint8_t pat_off = r[3];
        const uint8_t pat_len = r[4];
        if (pat_len > 0) {
            if (body_len < 2 + pat_off + pat_len ||
                memcmp(body + 2 + pat_off, r + 5, pat_len) != 0) {
                continue;
            }
        }
        if (r[10] == 0) {                    /* assign kind */
            if (kind != NULL) {
                *kind = r[11];
            }
            return true;
        }
        if (flags != NULL) {                 /* set flag bit */
            *flags |= (uint32_t)1 << r[11];
        }
    }
    return false;
}
