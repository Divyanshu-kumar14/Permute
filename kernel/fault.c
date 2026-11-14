// SPDX-License-Identifier: Apache-2.0

#include "kernel/fault.h"

#include <errno.h>
#include <string.h>

static const char *const site_names[PERMUTE_FAULT_SITE_COUNT] = {
    "send", "recv", "reply", "pulse",
};

static const char *const action_names[PERMUTE_FAULT_ACTION_COUNT] = {
    "drop", "duplicate", "reorder", "delay", "timeout", "kill",
};

const char *permute_fault_site_name(uint8_t site)
{
    return site < PERMUTE_FAULT_SITE_COUNT ? site_names[site] : "?";
}

const char *permute_fault_action_name(uint8_t action)
{
    return action < PERMUTE_FAULT_ACTION_COUNT ? action_names[action] : "?";
}

void permute_fault_init(permute_fault_plan_t *plan, uint64_t seed)
{
    memset(plan, 0, sizeof *plan);
    permute_stream_init(&plan->stream, seed);
}

int permute_fault_add(permute_fault_plan_t *plan, uint8_t site, uint8_t action,
                      uint32_t numerator, uint32_t denominator,
                      uint64_t delay_ns)
{
    if (plan->count >= PERMUTE_MAX_FAULT_RULES)
        return -ENOSPC;
    if (site >= PERMUTE_FAULT_SITE_COUNT || action >= PERMUTE_FAULT_ACTION_COUNT)
        return -EINVAL;
    if (denominator == 0 || numerator > denominator)
        return -EINVAL;

    permute_fault_rule_t *rule = &plan->rules[plan->count++];
    rule->site = site;
    rule->action = action;
    rule->numerator = numerator;
    rule->denominator = denominator;
    rule->delay_ns = delay_ns;
    return 0;
}

int permute_fault_decide(permute_fault_plan_t *plan, uint8_t site,
                         uint8_t *action, uint64_t *delay_ns)
{
    if (plan == NULL)
        return 0;

    for (uint32_t i = 0; i < plan->count; i++) {
        const permute_fault_rule_t *rule = &plan->rules[i];
        if (rule->site != site)
            continue;

        // One draw per armed rule per site invocation. An unarmed site draws
        // nothing, so adding a call site cannot move any other domain.
        uint64_t value = permute_stream_next(&plan->stream, PERMUTE_D_FAULT);
        plan->decisions++;

        if ((value % rule->denominator) < rule->numerator) {
            plan->fires++;
            *action = rule->action;
            *delay_ns = rule->delay_ns;
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Parsing

static int parse_site(const char *text, uint8_t *out)
{
    for (uint8_t i = 0; i < PERMUTE_FAULT_SITE_COUNT; i++) {
        size_t n = strlen(site_names[i]);
        if (strncmp(text, site_names[i], n) == 0 && text[n] == ':') {
            *out = i;
            return (int)n + 1;
        }
    }
    return -1;
}

static int parse_action(const char *text, uint8_t *out)
{
    for (uint8_t i = 0; i < PERMUTE_FAULT_ACTION_COUNT; i++) {
        size_t n = strlen(action_names[i]);
        if (strncmp(text, action_names[i], n) == 0 &&
            (text[n] == '@' || text[n] == ':' || text[n] == '\0')) {
            *out = i;
            return (int)n;
        }
    }
    return -1;
}

// Decimal probability to parts per million, without floating point.
static int parse_probability(const char *text, uint32_t *numerator)
{
    uint64_t whole = 0;
    uint64_t frac = 0;
    uint64_t scale = 1;
    int digits = 0;

    while (*text >= '0' && *text <= '9') {
        whole = whole * 10 + (uint64_t)(*text - '0');
        if (whole > 1)
            return -EINVAL;
        text++;
    }

    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9') {
            if (digits < 6) {
                frac = frac * 10 + (uint64_t)(*text - '0');
                scale *= 10;
                digits++;
            }
            text++;
        }
    }

    if (*text != '\0' && *text != ':')
        return -EINVAL;

    uint64_t value = whole * PERMUTE_FAULT_SCALE +
                     (frac * PERMUTE_FAULT_SCALE) / scale;
    if (value > PERMUTE_FAULT_SCALE)
        return -EINVAL;

    *numerator = (uint32_t)value;
    return 0;
}

int permute_fault_add_string(permute_fault_plan_t *plan, const char *text)
{
    if (text == NULL || text[0] == '\0')
        return -EINVAL;

    uint8_t site = 0;
    int used = parse_site(text, &site);
    if (used < 0)
        return -EINVAL;

    const char *p = text + used;
    uint8_t action = 0;
    int action_len = parse_action(p, &action);
    if (action_len < 0)
        return -EINVAL;
    p += action_len;

    if (*p != '@')
        return -EINVAL;
    p++;

    uint32_t numerator = 0;
    if (parse_probability(p, &numerator) != 0)
        return -EINVAL;

    // Walk past the probability to an optional :delay_ns.
    while ((*p >= '0' && *p <= '9') || *p == '.')
        p++;

    uint64_t delay_ns = 0;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9')
            return -EINVAL;
        while (*p >= '0' && *p <= '9') {
            delay_ns = delay_ns * 10 + (uint64_t)(*p - '0');
            p++;
        }
    }

    if (*p != '\0')
        return -EINVAL;

    return permute_fault_add(plan, site, action, numerator,
                             PERMUTE_FAULT_SCALE, delay_ns);
}
