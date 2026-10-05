/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Binary search over the generated sorted tables. The tables are large (the IEEE OUI
 * registry alone is ~40k entries), so they live in a separate generated .inc included
 * here as static const arrays in flash/rodata, never copied to RAM.
 */
#include "vendor_lookup.h"

#include <stdlib.h>

#include "vendor_table.inc"
#include "fastpair_models.inc"

static int oui_cmp(const void *key, const void *elem)
{
    const uint32_t k = *(const uint32_t *)key;
    const vendor_oui_t *e = (const vendor_oui_t *)elem;
    return (k > e->oui) - (k < e->oui);
}

static int co_cmp(const void *key, const void *elem)
{
    const uint16_t k = *(const uint16_t *)key;
    const vendor_co_t *e = (const vendor_co_t *)elem;
    return (k > e->company) - (k < e->company);
}

const char *vendor_lookup_oui(const uint8_t mac[6])
{
    if (mac == NULL) {
        return NULL;
    }

    const uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | (uint32_t)mac[2];
    const vendor_oui_t *e = (const vendor_oui_t *)bsearch(
        &oui, vendor_oui_table, vendor_oui_count, sizeof(vendor_oui_table[0]), oui_cmp);
    return e ? e->name : NULL;
}

const char *vendor_lookup_company(uint16_t company_id)
{
    const vendor_co_t *e = (const vendor_co_t *)bsearch(
        &company_id, vendor_co_table, vendor_co_count, sizeof(vendor_co_table[0]), co_cmp);
    return e ? e->name : NULL;
}

static int svc_cmp(const void *key, const void *elem)
{
    const uint16_t k = *(const uint16_t *)key;
    const vendor_svc_t *e = (const vendor_svc_t *)elem;
    return (k > e->uuid) - (k < e->uuid);
}

static int appearance_cmp(const void *key, const void *elem)
{
    const uint16_t k = *(const uint16_t *)key;
    const vendor_appearance_t *e = (const vendor_appearance_t *)elem;
    return (k > e->appearance) - (k < e->appearance);
}

const char *vendor_lookup_service(uint16_t uuid)
{
    const vendor_svc_t *e = (const vendor_svc_t *)bsearch(
        &uuid, vendor_svc_table, vendor_svc_count, sizeof(vendor_svc_table[0]), svc_cmp);
    return e ? e->name : NULL;
}

const char *vendor_lookup_appearance(uint16_t appearance)
{
    const vendor_appearance_t *e = (const vendor_appearance_t *)bsearch(
        &appearance, vendor_appearance_table, vendor_appearance_count,
        sizeof(vendor_appearance_table[0]), appearance_cmp);
    return e ? e->name : NULL;
}

const char *vendor_lookup_fastpair(uint32_t model_id)
{
    for (uint32_t i = 0; i < fastpair_model_count; i++) {
        if (fastpair_models[i].id == model_id) {
            return fastpair_models[i].name;
        }
    }
    return NULL;
}
