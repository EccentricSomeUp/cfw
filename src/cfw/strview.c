/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/strview.h>

#include <string.h>

static int cfw_strview_is_valid(const cfw_strview_t *view)
{
    if (view == NULL) {
        return 0;
    }
    if (view->size != 0 && view->data == NULL) {
        return 0;
    }
    return 1;
}

static unsigned char cfw_ascii_fold(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static int cfw_strview_fold_equal(const char *a, const char *b, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        if (cfw_ascii_fold((unsigned char)a[i]) !=
            cfw_ascii_fold((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

cfw_status_t cfw_strview_init(cfw_strview_t *view, const void *data,
                              size_t size)
{
    if (view == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (data == NULL && size != 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    view->data = (const char *)data;
    view->size = size;
    return CFW_OK;
}

cfw_status_t cfw_strview_from_cstr(cfw_strview_t *view, const char *cstr)
{
    if (view == NULL || cstr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    return cfw_strview_init(view, cstr, strlen(cstr));
}

cfw_status_t cfw_strview_from_span(const cfw_span_t *span, cfw_strview_t *view)
{
    if (span == NULL || view == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (span->size != 0 && span->data == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    return cfw_strview_init(view, span->data, span->size);
}

cfw_status_t cfw_strview_to_span(const cfw_strview_t *view, cfw_span_t *span)
{
    if (!cfw_strview_is_valid(view) || span == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    return cfw_span_init(span, view->data, view->size);
}

int cfw_strview_is_empty(const cfw_strview_t *view)
{
    if (!cfw_strview_is_valid(view)) {
        return 0;
    }
    return view->size == 0;
}

cfw_status_t cfw_strview_equal(const cfw_strview_t *a, const cfw_strview_t *b,
                               int *equal)
{
    if (!cfw_strview_is_valid(a) || !cfw_strview_is_valid(b) ||
        equal == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (a->size != b->size) {
        *equal = 0;
        return CFW_OK;
    }
    if (a->size == 0) {
        *equal = 1;
        return CFW_OK;
    }

    *equal = memcmp(a->data, b->data, a->size) == 0;
    return CFW_OK;
}

cfw_status_t cfw_strview_equal_nocase(const cfw_strview_t *a,
                                      const cfw_strview_t *b, int *equal)
{
    if (!cfw_strview_is_valid(a) || !cfw_strview_is_valid(b) ||
        equal == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (a->size != b->size) {
        *equal = 0;
        return CFW_OK;
    }
    if (a->size == 0) {
        *equal = 1;
        return CFW_OK;
    }

    *equal = cfw_strview_fold_equal(a->data, b->data, a->size);
    return CFW_OK;
}

cfw_status_t cfw_strview_starts_with(const cfw_strview_t *view,
                                     const cfw_strview_t *prefix, int *matches)
{
    if (!cfw_strview_is_valid(view) || !cfw_strview_is_valid(prefix) ||
        matches == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (prefix->size > view->size) {
        *matches = 0;
        return CFW_OK;
    }
    if (prefix->size == 0) {
        *matches = 1;
        return CFW_OK;
    }

    *matches = memcmp(view->data, prefix->data, prefix->size) == 0;
    return CFW_OK;
}

cfw_status_t cfw_strview_ends_with(const cfw_strview_t *view,
                                   const cfw_strview_t *suffix, int *matches)
{
    const char *tail;

    if (!cfw_strview_is_valid(view) || !cfw_strview_is_valid(suffix) ||
        matches == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (suffix->size > view->size) {
        *matches = 0;
        return CFW_OK;
    }
    if (suffix->size == 0) {
        *matches = 1;
        return CFW_OK;
    }

    tail = view->data + (view->size - suffix->size);
    *matches = memcmp(tail, suffix->data, suffix->size) == 0;
    return CFW_OK;
}
