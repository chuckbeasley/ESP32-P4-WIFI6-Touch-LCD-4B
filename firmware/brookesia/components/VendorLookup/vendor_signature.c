/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fieldwatch signature ingestion. Parses Fieldwatch's fieldwatch-signatures-v2.json at
 * runtime and matches BLE advertisements against its fleet catalog. This is a focused
 * BLE subset of Fieldwatch's SignatureEngine: it evaluates the OUI / MAC / name / UUID /
 * manufacturer / service-data rules, prefers the most specific match, and skips the
 * Wi-Fi-only rules (vendor IE, hidden SSID), clustering, and the decode field maps.
 */
#include "vendor_signature.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "VendorSignature";

/* ---- Streaming JSON tokenizer (same subset as vendor_lookup.c) ------------- */

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

static bool jp_bool(jp_t *jp)
{
    jp_ws(jp);
    if (jp->p + 4 <= jp->end && strncmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        return true;
    }
    if (jp->p + 5 <= jp->end && strncmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        return false;
    }
    jp->err = true;
    return false;
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

static void jp_skip_string(jp_t *jp)
{
    if (jp->p < jp->end && *jp->p == '"') {
        jp->p++;
        while (jp->p < jp->end && *jp->p != '"') {
            if (*jp->p == '\\' && jp->p + 1 < jp->end) {
                jp->p++;
            }
            jp->p++;
        }
        if (jp->p < jp->end) {
            jp->p++;
        }
    } else {
        jp->err = true;
    }
}

static void jp_skip_value(jp_t *jp)
{
    jp_ws(jp);
    if (jp->err || jp->p >= jp->end) {
        return;
    }
    const char c = *jp->p;
    if (c == '"') {
        jp_skip_string(jp);
    } else if (c == '{' || c == '[') {
        jp->p++;
        int depth = 1;
        while (jp->p < jp->end && depth > 0) {
            const char d = *jp->p;
            if (d == '{' || d == '[') {
                depth++;
                jp->p++;
            } else if (d == '}' || d == ']') {
                depth--;
                jp->p++;
            } else if (d == '"') {
                jp_skip_string(jp);
            } else {
                jp->p++;
            }
        }
    } else {
        while (jp->p < jp->end && *jp->p != ',' && *jp->p != '}' && *jp->p != ']') {
            jp->p++;
        }
    }
}

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

/* ---- Rule kinds / classes -------------------------------------------------- */

enum {
    RULE_OUI, RULE_MAC_PREFIX, RULE_NAME_CONTAINS, RULE_NAME_GLOB,
    RULE_SERVICE_UUID, RULE_SERVICE_DATA, RULE_MANUFACTURER_ID,
    RULE_MANUFACTURER_DATA, RULE_RADIO_KIND,
};

static int rule_kind_of(const char *s)
{
    if (strcmp(s, "OUI") == 0) return RULE_OUI;
    if (strcmp(s, "MAC_PREFIX") == 0) return RULE_MAC_PREFIX;
    if (strcmp(s, "NAME_CONTAINS") == 0) return RULE_NAME_CONTAINS;
    if (strcmp(s, "NAME_GLOB") == 0) return RULE_NAME_GLOB;
    if (strcmp(s, "SERVICE_UUID") == 0) return RULE_SERVICE_UUID;
    if (strcmp(s, "SERVICE_DATA") == 0) return RULE_SERVICE_DATA;
    if (strcmp(s, "MANUFACTURER_ID") == 0) return RULE_MANUFACTURER_ID;
    if (strcmp(s, "MANUFACTURER_DATA") == 0) return RULE_MANUFACTURER_DATA;
    if (strcmp(s, "RADIO_KIND") == 0) return RULE_RADIO_KIND;
    return -1;   /* HIDDEN_SSID / VENDOR_IE_OUI and anything else are Wi-Fi-only */
}

static int radio_of(const char *s)
{
    if (s == NULL) return 0;
    if (strcmp(s, "BLE") == 0) return 1;
    if (strcmp(s, "WIFI") == 0) return 2;
    return 0;
}

static ble_signature_class_t class_of(const char *s)
{
    static const struct { const char *n; ble_signature_class_t c; } map[] = {
        { "FINDER", SIG_CLASS_FINDER }, { "BEACON", SIG_CLASS_BEACON },
        { "SIGNAGE", SIG_CLASS_SIGNAGE }, { "WEARABLE", SIG_CLASS_WEARABLE },
        { "SURVEILLANCE", SIG_CLASS_SURVEILLANCE }, { "DRONE", SIG_CLASS_DRONE },
        { "HACKING", SIG_CLASS_HACKING }, { "BODYWORN", SIG_CLASS_BODYWORN },
        { "LAW_ENFORCEMENT", SIG_CLASS_LAW_ENFORCEMENT }, { "VEHICLE", SIG_CLASS_VEHICLE },
        { "GLASSES", SIG_CLASS_GLASSES }, { "AUDIO", SIG_CLASS_AUDIO },
        { "CAMERA", SIG_CLASS_CAMERA }, { "THERMOSTAT", SIG_CLASS_THERMOSTAT },
        { "LOCK", SIG_CLASS_LOCK }, { "HEALTH", SIG_CLASS_HEALTH },
        { "HOME", SIG_CLASS_HOME }, { "ISP", SIG_CLASS_ISP }, { "MESH", SIG_CLASS_MESH },
        { "PHONE", SIG_CLASS_PHONE }, { "OTHER", SIG_CLASS_OTHER },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(s, map[i].n) == 0) {
            return map[i].c;
        }
    }
    return SIG_CLASS_UNKNOWN;
}

/* ---- Parsed catalog -------------------------------------------------------- */

typedef struct {
    uint8_t kind;
    uint8_t radio;        /* 0 any, 1 BLE, 2 WIFI */
    uint16_t company_id;
    uint32_t text_off;          /* 0 = none */
    uint32_t data_prefix_off;   /* 0 = none */
} sig_rule_t;

typedef struct {
    uint32_t name_off;
    uint8_t cls;
    uint8_t match_any;
    uint8_t specific;           /* has a mfg/service rule: preferred over OUI/name-only */
    uint32_t rule_start;
    uint16_t rule_count;
} sig_fleet_t;

static sig_rule_t *s_rules;
static uint32_t s_rule_count, s_rule_cap;
static sig_fleet_t *s_fleets;
static uint32_t s_fleet_count, s_fleet_cap;
static char *s_arena;
static size_t s_arena_len, s_arena_cap;
static bool s_loaded;

static uint32_t arena_append(jp_t *jp, const char *s)
{
    const size_t len = strlen(s) + 1;
    if (s_arena_len + len > s_arena_cap) {
        char *np = (char *)grow(s_arena, &s_arena_cap, 1, s_arena_len + len);
        if (np == NULL) {
            jp->err = true;
            return 0;
        }
        s_arena = np;
    }
    memcpy(s_arena + s_arena_len, s, len);
    const uint32_t off = (uint32_t)s_arena_len;
    s_arena_len += len;
    return off;
}

/* ---- Parse ---------------------------------------------------------------- */

static void parse_rule(jp_t *jp)
{
    int kind = -1;               /* -1 = skip (Wi-Fi-only or unknown) */
    uint8_t radio = 0;
    uint16_t company_id = 0;
    bool enabled = true;
    char text[256];
    text[0] = '\0';
    char data_prefix[256];
    data_prefix[0] = '\0';

    jp_expect(jp, '{');
    while (!jp->err && jp_peek(jp) != '}') {
        char field[24];
        jp_string(jp, field, sizeof(field));
        jp_expect(jp, ':');
        if (strcmp(field, "kind") == 0) {
            char v[32];
            jp_string(jp, v, sizeof(v));
            kind = rule_kind_of(v);
        } else if (strcmp(field, "text") == 0) {
            jp_string(jp, text, sizeof(text));
        } else if (strcmp(field, "companyId") == 0) {
            company_id = (uint16_t)jp_number(jp);
        } else if (strcmp(field, "dataPrefixHex") == 0) {
            jp_string(jp, data_prefix, sizeof(data_prefix));
        } else if (strcmp(field, "radio") == 0) {
            char v[8];
            if (jp_peek(jp) == '"') {
                jp_string(jp, v, sizeof(v));
                radio = (uint8_t)radio_of(v);
            } else {
                (void)jp_skip_value(jp);   /* null */
            }
        } else if (strcmp(field, "enabled") == 0) {
            enabled = jp_bool(jp);
        } else {
            (void)jp_skip_value(jp);
        }

        if (jp_peek(jp) == ',') {
            jp->p++;
        } else if (jp_peek(jp) != '}') {
            jp->err = true;
        }
    }
    jp_expect(jp, '}');

    if (kind < 0 || radio == 2 || !enabled) {
        return;   /* Wi-Fi-only, disabled, or unknown rule */
    }

    s_rules = (sig_rule_t *)grow(s_rules, (size_t *)&s_rule_cap,
                                 sizeof(sig_rule_t), (size_t)s_rule_count + 1);
    if (s_rules == NULL) {
        jp->err = true;
        return;
    }
    sig_rule_t *r = &s_rules[s_rule_count++];
    memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)kind;
    r->radio = radio;
    r->company_id = company_id;
    if (text[0] != '\0') {
        r->text_off = arena_append(jp, text);
    }
    if (data_prefix[0] != '\0') {
        r->data_prefix_off = arena_append(jp, data_prefix);
    }
}

static void parse_rules_array(jp_t *jp, uint32_t *start, uint16_t *count, uint8_t *specific)
{
    const uint32_t begin = s_rule_count;
    jp_expect(jp, '[');
    while (!jp->err && jp_peek(jp) != ']') {
        parse_rule(jp);
        if (jp_peek(jp) == ',') {
            jp->p++;
        } else if (jp_peek(jp) != ']') {
            jp->err = true;
        }
    }
    jp_expect(jp, ']');

    *start = begin;
    *count = (uint16_t)(s_rule_count - begin);

    for (uint32_t i = begin; i < s_rule_count; i++) {
        const uint8_t k = s_rules[i].kind;
        if (k == RULE_MANUFACTURER_ID || k == RULE_MANUFACTURER_DATA ||
            k == RULE_SERVICE_UUID || k == RULE_SERVICE_DATA) {
            *specific = 1;
            break;
        }
    }
}

static void parse_fleet(jp_t *jp)
{
    uint32_t name_off = 0;
    uint8_t cls = SIG_CLASS_OTHER;
    uint8_t match_any = 1;
    uint32_t rule_start = 0;
    uint16_t rule_count = 0;
    uint8_t specific = 0;

    jp_expect(jp, '{');
    while (!jp->err && jp_peek(jp) != '}') {
        char field[24];
        jp_string(jp, field, sizeof(field));
        jp_expect(jp, ':');
        if (strcmp(field, "name") == 0) {
            char name[256];
            jp_string(jp, name, sizeof(name));
            name_off = arena_append(jp, name);
        } else if (strcmp(field, "kind") == 0) {
            char v[32];
            jp_string(jp, v, sizeof(v));
            cls = (uint8_t)class_of(v);
        } else if (strcmp(field, "matchAny") == 0) {
            match_any = jp_bool(jp) ? 1 : 0;
        } else if (strcmp(field, "rules") == 0) {
            parse_rules_array(jp, &rule_start, &rule_count, &specific);
        } else {
            (void)jp_skip_value(jp);
        }

        if (jp_peek(jp) == ',') {
            jp->p++;
        } else if (jp_peek(jp) != '}') {
            jp->err = true;
        }
    }
    jp_expect(jp, '}');

    if (rule_count == 0 || name_off == 0) {
        return;   /* nothing matchable */
    }

    s_fleets = (sig_fleet_t *)grow(s_fleets, (size_t *)&s_fleet_cap,
                                   sizeof(sig_fleet_t), (size_t)s_fleet_count + 1);
    if (s_fleets == NULL) {
        jp->err = true;
        return;
    }
    sig_fleet_t *f = &s_fleets[s_fleet_count++];
    f->name_off = name_off;
    f->cls = cls;
    f->match_any = match_any;
    f->specific = specific;
    f->rule_start = rule_start;
    f->rule_count = rule_count;
}

/* ---- Lifecycle ------------------------------------------------------------ */

static void free_all(void)
{
    if (s_rules != NULL) {
        heap_caps_free(s_rules);
        s_rules = NULL;
    }
    if (s_fleets != NULL) {
        heap_caps_free(s_fleets);
        s_fleets = NULL;
    }
    if (s_arena != NULL) {
        heap_caps_free(s_arena);
        s_arena = NULL;
    }
    s_rule_count = s_rule_cap = 0;
    s_fleet_count = s_fleet_cap = 0;
    s_arena_len = s_arena_cap = 0;
    s_loaded = false;
}

bool vendor_signature_is_loaded(void)
{
    return s_loaded;
}

esp_err_t vendor_signature_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "no signature catalog at %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    const long sz = ftell(f);
    rewind(f);

    char *text = (char *)heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (text == NULL) {
        text = (char *)heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_8BIT);
    }
    if (text == NULL) {
        ESP_LOGE(TAG, "out of memory for %ld-byte catalog", sz);
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

    jp_t jp = { text, text + sz, false };
    jp_expect(&jp, '{');
    while (!jp.err && jp_peek(&jp) != '}') {
        char key[24];
        jp_string(&jp, key, sizeof(key));
        jp_expect(&jp, ':');
        if (strcmp(key, "fleets") == 0) {
            jp_expect(&jp, '[');
            while (!jp.err && jp_peek(&jp) != ']') {
                parse_fleet(&jp);
                if (jp_peek(&jp) == ',') {
                    jp.p++;
                } else if (jp_peek(&jp) != ']') {
                    jp.err = true;
                }
            }
            jp_expect(&jp, ']');
        } else {
            (void)jp_skip_value(&jp);
        }
        if (jp_peek(&jp) == ',') {
            jp.p++;
        } else if (jp_peek(&jp) != '}') {
            jp.err = true;
        }
    }
    jp_expect(&jp, '}');

    if (jp.err) {
        ESP_LOGE(TAG, "invalid signature catalog %s", path);
        free_all();
        heap_caps_free(text);
        return ESP_ERR_INVALID_ARG;
    }

    s_loaded = true;
    ESP_LOGI(TAG, "loaded %u fleets / %u rules from %s",
             (unsigned)s_fleet_count, (unsigned)s_rule_count, path);
    heap_caps_free(text);
    return ESP_OK;
}

/* ---- Matching -------------------------------------------------------------- */

static void hex_compact(const char *in, char *out, size_t max)
{
    size_t n = 0;
    for (; *in != '\0' && n + 1 < max; in++) {
        if (hex_nibble(*in) >= 0) {
            out[n++] = (*in >= 'a' && *in <= 'f') ? (char)(*in - 'a' + 'A') : *in;
        }
    }
    out[n] = '\0';
}

static bool contains_ci(const char *hay, const char *needle)
{
    if (needle[0] == '\0') {
        return false;
    }
    for (const char *h = hay; *h != '\0'; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a != '\0' && *b != '\0') {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
            if (ca != cb) break;
            a++;
            b++;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool glob_match_ci(const char *text, const char *pattern)
{
    /* Iterative glob with '*' and '?' (case-insensitive). */
    const char *star = NULL;
    const char *t = text;
    const char *p = pattern;
    const char *s = NULL;
    while (*t != '\0') {
        char pt = *p;
        if (pt == '?' || (pt != '\0' && (pt | 0x20) == (*t | 0x20))) {
            t++;
            p++;
        } else if (pt == '*') {
            star = p++;
            s = t;
        } else if (star != NULL) {
            p = star + 1;
            t = ++s;
        } else {
            return false;
        }
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

static bool uuid_matches(const char *rule_text, uint16_t svc_uuid)
{
    if (svc_uuid == 0) {
        return false;
    }
    char hex[40];
    hex_compact(rule_text, hex, sizeof(hex));
    const size_t len = strlen(hex);
    if (len == 4) {
        char want[5];
        snprintf(want, sizeof(want), "%04X", svc_uuid);
        return strcmp(hex, want) == 0;
    }
    if (len == 32 && strncmp(hex, "0000", 4) == 0 &&
        strcmp(hex + 8, "00001000800000805F9B34FB") == 0) {
        char want[5];
        snprintf(want, sizeof(want), "%04X", svc_uuid);
        return strncmp(hex + 4, want, 4) == 0;
    }
    return false;
}

static bool mac_prefix_hits(const uint8_t mac[6], const char *rule_text)
{
    char prefix[40];
    hex_compact(rule_text, prefix, sizeof(prefix));
    const size_t plen = strlen(prefix);
    if (plen == 0 || plen > 12) {
        return false;
    }
    char m[13];
    snprintf(m, sizeof(m), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return strncmp(m, prefix, plen) == 0;
}

static bool rule_hits(const sig_rule_t *r, const ble_signature_input_t *in)
{
    if (r->radio == 2) {
        return false;
    }
    const char *text = (r->text_off != 0) ? s_arena + r->text_off : "";
    const char *prefix = (r->data_prefix_off != 0) ? s_arena + r->data_prefix_off : "";
    char compact[256];

    switch (r->kind) {
    case RULE_OUI:
    case RULE_MAC_PREFIX:
        return in->mac != NULL && mac_prefix_hits(in->mac, text);
    case RULE_NAME_CONTAINS:
        return in->name != NULL && contains_ci(in->name, text);
    case RULE_NAME_GLOB:
        return in->name != NULL && glob_match_ci(in->name, text);
    case RULE_SERVICE_UUID:
        return uuid_matches(text, in->svc_uuid);
    case RULE_MANUFACTURER_ID:
        return in->company_id != 0 && in->company_id == r->company_id;
    case RULE_MANUFACTURER_DATA: {
        if (in->mfg_hex == NULL || in->mfg_hex[0] == '\0') {
            return false;
        }
        if (r->company_id != 0 && in->company_id != r->company_id) {
            return false;
        }
        char pcompact[256];
        hex_compact(prefix, pcompact, sizeof(pcompact));
        if (pcompact[0] == '\0') {
            return true;
        }
        hex_compact(in->mfg_hex, compact, sizeof(compact));
        return strncmp(compact, pcompact, strlen(pcompact)) == 0;
    }
    case RULE_SERVICE_DATA:
        if (in->svc_hex == NULL || in->svc_hex[0] == '\0') {
            return false;
        }
        if (text[0] != '\0' && !uuid_matches(text, in->svc_uuid)) {
            return false;
        }
        if (prefix[0] != '\0') {
            char pcompact[256];
            hex_compact(prefix, pcompact, sizeof(pcompact));
            if (pcompact[0] == '\0') {
                return true;
            }
            hex_compact(in->svc_hex, compact, sizeof(compact));
            return strncmp(compact, pcompact, strlen(pcompact)) == 0;
        }
        return true;
    case RULE_RADIO_KIND:
        return true;
    default:
        return false;
    }
}

const char *vendor_signature_match(const ble_signature_input_t *in,
                                   ble_signature_class_t *class_out)
{
    if (!s_loaded || in == NULL) {
        return NULL;
    }

    /* Two passes: specific (mfg/service) rules first, then generic (OUI/name). */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t fi = 0; fi < s_fleet_count; fi++) {
            const sig_fleet_t *f = &s_fleets[fi];
            const int want_specific = (pass == 0);
            if ((f->specific != 0) != want_specific) {
                continue;
            }
            bool hit;
            if (f->match_any) {
                hit = false;
                for (uint16_t i = 0; i < f->rule_count; i++) {
                    if (rule_hits(&s_rules[f->rule_start + i], in)) {
                        hit = true;
                        break;
                    }
                }
            } else {
                hit = true;
                for (uint16_t i = 0; i < f->rule_count; i++) {
                    if (!rule_hits(&s_rules[f->rule_start + i], in)) {
                        hit = false;
                        break;
                    }
                }
            }
            if (hit) {
                if (class_out != NULL) {
                    *class_out = (ble_signature_class_t)f->cls;
                }
                return s_arena + f->name_off;
            }
        }
    }
    return NULL;
}

const char *vendor_signature_class_label(ble_signature_class_t cls)
{
    static const char *const labels[] = {
        "Finder tags", "Retail beacons", "Signage", "Wearables", "Surveillance",
        "Drones", "Pentest", "Body-worn", "Public safety", "Vehicle", "Glasses",
        "Audio", "Cameras", "Thermostats", "Access control", "Health", "Home IoT",
        "ISP / routers", "Mesh", "Phones / PCs", "Other", NULL,
    };
    if ((int)cls < 0 || cls >= SIG_CLASS_UNKNOWN) {
        return NULL;
    }
    return labels[cls];
}
