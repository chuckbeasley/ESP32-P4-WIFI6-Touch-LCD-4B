/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loads vendor_db.json (see scripts/gen_vendor_table.py for the layout) into PSRAM and
 * serves lookups out of it with a binary search. The file can be refreshed on the SD
 * card and reloaded on the next boot -- no firmware rebuild, no binary conversion.
 *
 * The JSON is parsed by a small hand-written streaming tokenizer rather than cJSON, so
 * the ~44k table entries go straight into the lookup structures instead of through a
 * throw-away DOM tree. The schema is fixed, so the parser only understands what the
 * generator emits: a top-level object of arrays of [number, "name"] and a rules array.
 */
#include "vendor_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- Streaming JSON tokenizer --------------------------------------------- */

typedef struct {
    const char *p;
    const char *end;
    bool err;
} jp_t;

static void jp_ws(jp_t *jp)
{
    while (jp->p < jp->end &&
           (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) {
        jp->p++;
    }
}

static char jp_peek(jp_t *jp)
{
    jp_ws(jp);
    return (jp->p < jp->end) ? *jp->p : '\0';
}

static void jp_expect(jp_t *jp, char c)
{
    jp_ws(jp);
    if (jp->p >= jp->end || *jp->p != c) {
        jp->err = true;
        return;
    }
    jp->p++;
}

static uint32_t jp_number(jp_t *jp)
{
    jp_ws(jp);
    uint32_t v = 0;
    if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') {
        jp->err = true;
        return 0;
    }
    while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') {
        v = v * 10 + (uint32_t)(*jp->p - '0');
        jp->p++;
    }
    return v;
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

/* Parse a JSON string into `out` (NUL-terminated); returns the byte length, or -1. */
static int jp_string(jp_t *jp, char *out, size_t max)
{
    jp_ws(jp);
    if (jp->p >= jp->end || *jp->p != '"') {
        jp->err = true;
        return -1;
    }
    jp->p++;

    size_t n = 0;
    while (jp->p < jp->end) {
        const char c = *jp->p++;
        if (c == '"') {
            out[n] = '\0';
            return (int)n;
        }
        if (c != '\\') {
            out[n++] = c;
        } else {
            if (jp->p >= jp->end) {
                jp->err = true;
                return -1;
            }
            const char e = *jp->p++;
            switch (e) {
            case '"':  out[n++] = '"';  break;
            case '\\': out[n++] = '\\'; break;
            case '/':  out[n++] = '/';  break;
            case 'b':  out[n++] = '\b'; break;
            case 'f':  out[n++] = '\f'; break;
            case 'n':  out[n++] = '\n'; break;
            case 'r':  out[n++] = '\r'; break;
            case 't':  out[n++] = '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    if (jp->p >= jp->end) {
                        jp->err = true;
                        return -1;
                    }
                    const int h = hex_nibble(*jp->p++);
                    if (h < 0) {
                        jp->err = true;
                        return -1;
                    }
                    cp = (cp << 4) | (uint32_t)h;
                }
                if (cp < 0x80) {
                    out[n++] = (char)cp;
                } else if (cp < 0x800) {
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                jp->err = true;
                return -1;
            }
        }
        if (n >= max) {
            jp->err = true;
            return -1;
        }
    }
    jp->err = true;
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

/* Grow a heap_caps (PSRAM) allocation to hold at least `need` elements of `elem` bytes. */
static void *grow(void *p, size_t *cap, size_t elem, size_t need)
{
    if (need <= *cap) {
        return p;
    }
    size_t nc = (*cap != 0) ? *cap : (elem > 1 ? 1024 : 65536);
    while (nc < need) {
        nc *= 2;
    }
    void *np = heap_caps_realloc(p, nc * elem, MALLOC_CAP_SPIRAM);
    if (np != NULL) {
        *cap = nc;
    }
    return np;
}

/* ---- Parse helpers -------------------------------------------------------- */

typedef struct {
    vendor_entry_t *entries;
    uint32_t count;
    uint32_t cap;
    char *pool;
    size_t pool_len;
    size_t pool_cap;
} table_builder_t;

typedef struct {
    vendor_rule_t *rules;
    uint32_t count;
    uint32_t cap;
} rule_builder_t;

static int table_index(const char *key)
{
    for (int i = 0; i < VENDOR_SEC_COUNT; i++) {
        if (strcmp(key, s_table_keys[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void parse_table(jp_t *jp, table_builder_t *tb)
{
    jp_expect(jp, '[');
    while (!jp->err && jp_peek(jp) != ']') {
        jp_expect(jp, '[');
        const uint32_t key = jp_number(jp);
        jp_expect(jp, ',');
        char name[256];
        const int len = jp_string(jp, name, sizeof(name));
        jp_expect(jp, ']');

        tb->entries = (vendor_entry_t *)grow(tb->entries, (size_t *)&tb->cap,
                                             sizeof(vendor_entry_t), (size_t)tb->count + 1);
        if (tb->entries == NULL) {
            jp->err = true;
            return;
        }
        while (tb->pool_len + (size_t)len + 1 > tb->pool_cap) {
            tb->pool = (char *)grow(tb->pool, &tb->pool_cap, 1,
                                    tb->pool_len + (size_t)len + 1);
            if (tb->pool == NULL) {
                jp->err = true;
                return;
            }
        }
        memcpy(tb->pool + tb->pool_len, name, (size_t)len + 1);
        tb->entries[tb->count].key = key;
        tb->entries[tb->count].name_off = (uint32_t)tb->pool_len;
        tb->pool_len += (size_t)len + 1;
        tb->count++;

        if (jp_peek(jp) == ',') {
            jp->p++;
        } else if (jp_peek(jp) != ']') {
            jp->err = true;
        }
    }
    jp_expect(jp, ']');
}

static void parse_rules(jp_t *jp, rule_builder_t *rb)
{
    jp_expect(jp, '[');
    while (!jp->err && jp_peek(jp) != ']') {
        rb->rules = (vendor_rule_t *)grow(rb->rules, (size_t *)&rb->cap,
                                          sizeof(vendor_rule_t), (size_t)rb->count + 1);
        if (rb->rules == NULL) {
            jp->err = true;
            return;
        }
        vendor_rule_t *r = &rb->rules[rb->count];
        memset(r, 0, sizeof(*r));

        jp_expect(jp, '{');
        while (!jp->err && jp_peek(jp) != '}') {
            char field[16];
            jp_string(jp, field, sizeof(field));
            jp_expect(jp, ':');
            if (strcmp(field, "type") == 0) {
                r->ad_type = (uint8_t)jp_number(jp);
            } else if (strcmp(field, "key") == 0) {
                r->key = (uint16_t)jp_number(jp);
            } else if (strcmp(field, "off") == 0) {
                r->pat_off = (uint8_t)jp_number(jp);
            } else if (strcmp(field, "min") == 0) {
                r->min_len = (uint8_t)jp_number(jp);
            } else if (strcmp(field, "pattern") == 0) {
                char pat[16];
                jp_string(jp, pat, sizeof(pat));
                r->pat_len = hex_decode(pat, r->pat, sizeof(r->pat));
            } else if (strcmp(field, "kind") == 0) {
                r->action = 0;
                r->arg = (uint8_t)jp_number(jp);
            } else if (strcmp(field, "flag") == 0) {
                r->action = 1;
                r->arg = (uint8_t)jp_number(jp);
            } else {
                jp->err = true;   /* unknown rule field */
            }

            if (jp_peek(jp) == ',') {
                jp->p++;
            } else if (jp_peek(jp) != '}') {
                jp->err = true;
            }
        }
        jp_expect(jp, '}');
        rb->count++;

        if (jp_peek(jp) == ',') {
            jp->p++;
        } else if (jp_peek(jp) != ']') {
            jp->err = true;
        }
    }
    jp_expect(jp, ']');
}

/* ---- Lifecycle ------------------------------------------------------------ */

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

    char *text = (char *)heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (text == NULL) {
        text = (char *)heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_8BIT);
    }
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

    table_builder_t tb[VENDOR_SEC_COUNT];
    memset(tb, 0, sizeof(tb));
    rule_builder_t rb;
    memset(&rb, 0, sizeof(rb));

    jp_t jp = { text, text + sz, false };
    jp_expect(&jp, '{');
    while (!jp.err && jp_peek(&jp) != '}') {
        char key[16];
        jp_string(&jp, key, sizeof(key));
        jp_expect(&jp, ':');

        const int ti = table_index(key);
        if (ti >= 0) {
            parse_table(&jp, &tb[ti]);
        } else if (strcmp(key, "rules") == 0) {
            parse_rules(&jp, &rb);
        } else if (strcmp(key, "version") == 0) {
            (void)jp_number(&jp);
        } else {
            jp.err = true;
        }

        if (jp_peek(&jp) == ',') {
            jp.p++;
        } else if (jp_peek(&jp) != '}') {
            jp.err = true;
        }
    }
    jp_expect(&jp, '}');
    jp_ws(&jp);
    if (jp.p != jp.end) {
        jp.err = true;   /* trailing content */
    }

    if (jp.err) {
        ESP_LOGE(TAG, "invalid JSON in %s", path);
        for (int i = 0; i < VENDOR_SEC_COUNT; i++) {
            if (tb[i].entries != NULL) {
                heap_caps_free(tb[i].entries);
            }
            if (tb[i].pool != NULL) {
                heap_caps_free(tb[i].pool);
            }
        }
        if (rb.rules != NULL) {
            heap_caps_free(rb.rules);
        }
        heap_caps_free(text);
        return ESP_ERR_INVALID_ARG;
    }

    free_tables();
    for (int i = 0; i < VENDOR_SEC_COUNT; i++) {
        s_sec[i].entries = tb[i].entries;
        s_sec[i].count = tb[i].count;
        s_sec[i].pool = tb[i].pool;
    }
    s_rules = rb.rules;
    s_rule_count = rb.count;
    s_loaded = true;

    ESP_LOGI(TAG, "loaded %u entries and %u rules from %s",
             (unsigned)(s_sec[0].count + s_sec[1].count + s_sec[2].count +
                        s_sec[3].count + s_sec[4].count),
             (unsigned)s_rule_count, path);

    heap_caps_free(text);
    return ESP_OK;
}

/* ---- Lookups -------------------------------------------------------------- */

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
