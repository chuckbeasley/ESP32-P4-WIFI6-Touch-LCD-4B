/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loads vendor_db.json (see scripts/gen_vendor_table.py for the layout) into PSRAM and
 * serves lookups out of it with a binary search. The file can be refreshed on the SD
 * card and reloaded on the next boot -- no firmware rebuild, no binary conversion.
 */
#include "vendor_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "VendorLookup";

#define VENDOR_SEC_OUI        0
#define VENDOR_SEC_COMPANY    1
#define VENDOR_SEC_SERVICE    2
#define VENDOR_SEC_APPEARANCE 3
#define VENDOR_SEC_FASTPAIR   4
#define VENDOR_SEC_COUNT      5

typedef struct {
    uint32_t key;
    uint32_t name_off;
} vendor_entry_t;

typedef struct {
    vendor_entry_t *entries;
    uint32_t count;
    char *pool;
} vendor_section_t;

typedef struct {
    uint8_t ad_type;
    uint16_t key;
    uint8_t pat_off;
    uint8_t pat_len;
    uint8_t pat[4];
    uint8_t min_len;
    uint8_t action;   /* 0 = assign kind, 1 = set flag */
    uint8_t arg;      /* kind id, or flag bit index */
} vendor_rule_t;

static vendor_section_t s_sec[VENDOR_SEC_COUNT];
static vendor_rule_t *s_rules;
static uint32_t s_rule_count;
static bool s_loaded;

static const char *const s_table_keys[VENDOR_SEC_COUNT] = {
    "oui", "company", "service", "appearance", "fastpair",
};

static void free_tables(void)
{
    for (int i = 0; i < VENDOR_SEC_COUNT; i++) {
        if (s_sec[i].entries != NULL) {
            heap_caps_free(s_sec[i].entries);
            s_sec[i].entries = NULL;
        }
        if (s_sec[i].pool != NULL) {
            heap_caps_free(s_sec[i].pool);
            s_sec[i].pool = NULL;
        }
        s_sec[i].count = 0;
    }
    if (s_rules != NULL) {
        heap_caps_free(s_rules);
        s_rules = NULL;
    }
    s_rule_count = 0;
}

static void *alloc_psram(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    }
    return p;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decode a hex string ("0215") into up to max bytes; returns the byte count. */
static uint8_t hex_decode(const char *s, uint8_t *out, uint8_t max)
{
    uint8_t n = 0;
    while (s[0] != '\0' && s[1] != '\0' && n < max) {
        const int hi = hex_nibble(s[0]);
        const int lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0) {
            break;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
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

    char *text = (char *)alloc_psram((size_t)sz + 1);
    if (text == NULL) {
        ESP_LOGE(TAG, "out of memory for %ld-byte database", sz);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(text, 1, (size_t)sz, f) != (size_t)sz) {
        ESP_LOGE(TAG, "short read from %s", path);
        fclose(f);
        heap_caps_free(text);
        return ESP_FAIL;
    }
    fclose(f);
    text[sz] = '\0';

    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        ESP_LOGE(TAG, "invalid JSON in %s", path);
        heap_caps_free(text);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    for (int i = 0; i < VENDOR_SEC_COUNT; i++) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, s_table_keys[i]);
        if (!cJSON_IsArray(arr)) {
            ESP_LOGE(TAG, "missing %s array", s_table_keys[i]);
            err = ESP_ERR_INVALID_ARG;
            goto out;
        }
        const int count = cJSON_GetArraySize(arr);

        size_t pool_sz = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(it, 1));
            pool_sz += (name != NULL ? strlen(name) : 0) + 1;
        }

        s_sec[i].entries = (vendor_entry_t *)alloc_psram(
            (size_t)count * sizeof(vendor_entry_t));
        s_sec[i].pool = (char *)alloc_psram(pool_sz);
        if (s_sec[i].entries == NULL || s_sec[i].pool == NULL) {
            ESP_LOGE(TAG, "out of memory building %s", s_table_keys[i]);
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        s_sec[i].count = (uint32_t)count;

        uint32_t idx = 0;
        size_t off = 0;
        cJSON_ArrayForEach(it, arr) {
            cJSON *key = cJSON_GetArrayItem(it, 0);
            const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(it, 1));
            const size_t len = (name != NULL ? strlen(name) : 0) + 1;
            memcpy(s_sec[i].pool + off, name != NULL ? name : "", len);
            s_sec[i].entries[idx].key = (uint32_t)cJSON_GetNumberValue(key);
            s_sec[i].entries[idx].name_off = (uint32_t)off;
            off += len;
            idx++;
        }
    }

    cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (cJSON_IsArray(rules)) {
        const int n = cJSON_GetArraySize(rules);
        s_rules = (vendor_rule_t *)alloc_psram((size_t)n * sizeof(vendor_rule_t));
        if (s_rules == NULL) {
            ESP_LOGE(TAG, "out of memory building rules");
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        s_rule_count = (uint32_t)n;

        uint32_t idx = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, rules) {
            vendor_rule_t *r = &s_rules[idx++];
            memset(r, 0, sizeof(*r));
            r->ad_type = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "type"));
            r->key = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "key"));
            r->pat_off = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "off"));
            r->min_len = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "min"));
            const char *pat = cJSON_GetStringValue(cJSON_GetObjectItem(it, "pattern"));
            r->pat_len = hex_decode(pat != NULL ? pat : "", r->pat, sizeof(r->pat));
            cJSON *kind = cJSON_GetObjectItem(it, "kind");
            cJSON *flag = cJSON_GetObjectItem(it, "flag");
            if (cJSON_IsNumber(kind)) {
                r->action = 0;
                r->arg = (uint8_t)cJSON_GetNumberValue(kind);
            } else if (cJSON_IsNumber(flag)) {
                r->action = 1;
                r->arg = (uint8_t)cJSON_GetNumberValue(flag);
            }
        }
    }

    ESP_LOGI(TAG, "loaded %u entries and %u rules from %s",
             (unsigned)(s_sec[0].count + s_sec[1].count + s_sec[2].count +
                        s_sec[3].count + s_sec[4].count),
             (unsigned)s_rule_count, path);

out:
    cJSON_Delete(root);
    heap_caps_free(text);
    if (err != ESP_OK) {
        free_tables();
    } else {
        s_loaded = true;
    }
    return err;
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
        const vendor_rule_t *r = &s_rules[i];
        if (r->ad_type != ad_type) {
            continue;
        }
        if (body_len < 2 || body[0] != (r->key & 0xFF) || body[1] != ((r->key >> 8) & 0xFF)) {
            continue;
        }
        if (body_len < r->min_len) {
            continue;
        }
        if (r->pat_len > 0) {
            if (body_len < 2 + r->pat_off + r->pat_len ||
                memcmp(body + 2 + r->pat_off, r->pat, r->pat_len) != 0) {
                continue;
            }
        }
        if (r->action == 0) {
            if (kind != NULL) {
                *kind = r->arg;
            }
            return true;
        }
        if (flags != NULL) {
            *flags |= (uint32_t)1 << r->arg;
        }
    }
    return false;
}
