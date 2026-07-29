/**
 * @file dmftp.c
 * @brief dmod_init()/_deinit(), the module-wide lock, and the buffer and
 *        string primitives the rest of dmftp is built out of
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

/**
 * @brief The one lock serializing everything in this module
 *
 * Recursive on purpose - see dmftp_internal.h's "Threading" note for why
 * dmftp holds it across user callbacks instead of snapshot-and-release.
 */
static dmosi_mutex_t g_lock = NULL;

void dmftp_lock(void)
{
    if (g_lock != NULL)
    {
        dmosi_mutex_lock(g_lock);
    }
}

void dmftp_unlock(void)
{
    if (g_lock != NULL)
    {
        dmosi_mutex_unlock(g_lock);
    }
}

/* ============================================================================
 *                      Growable byte buffer
 * ========================================================================== */

void dmftp_buf_init(dmftp_buf_t* buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void dmftp_buf_free(dmftp_buf_t* buf)
{
    Dmod_Free(buf->data);
    dmftp_buf_init(buf);
}

/**
 * @brief Grow to hold at least `needed` bytes, doubling to amortize the
 *        byte-at-a-time appends a line accumulator does
 */
static int buf_reserve(dmftp_buf_t* buf, size_t needed)
{
    if (buf->cap >= needed)
        return 0;

    size_t cap = buf->cap != 0 ? buf->cap : DMFTP_BUF_INITIAL;
    while (cap < needed)
    {
        cap *= 2u;
    }

    uint8_t* grown = Dmod_Realloc(buf->data, cap);
    if (grown == NULL)
        return -ENOMEM;

    buf->data = grown;
    buf->cap = cap;
    return 0;
}

int dmftp_buf_append(dmftp_buf_t* buf, const void* data, size_t len)
{
    if (len == 0)
        return 0;

    int result = buf_reserve(buf, buf->len + len);
    if (result != 0)
        return result;

    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

char* dmftp_buf_take_line(dmftp_buf_t* buf, size_t* out_len)
{
    size_t newline = 0;
    bool   found = false;
    for (size_t i = 0; i < buf->len; i++)
    {
        if (buf->data[i] == '\n')
        {
            newline = i;
            found = true;
            break;
        }
    }

    if (!found)
        return NULL;

    /* A peer may terminate with a bare LF; strip the CR only if it is
     * actually there rather than assuming a well-formed CRLF. */
    size_t text_len = newline;
    if (text_len > 0 && buf->data[text_len - 1u] == '\r')
    {
        text_len--;
    }

    char* line = dmftp_str_ndup((const char*)buf->data, text_len);
    dmftp_buf_consume(buf, newline + 1u);

    if (line != NULL && out_len != NULL)
    {
        *out_len = text_len;
    }
    return line;
}

void dmftp_buf_consume(dmftp_buf_t* buf, size_t len)
{
    if (len >= buf->len)
    {
        buf->len = 0;
        return;
    }

    memmove(buf->data, buf->data + len, buf->len - len);
    buf->len -= len;
}

/* ============================================================================
 *                      String helpers
 * ========================================================================== */

char* dmftp_str_ndup(const char* str, size_t len)
{
    char* copy = Dmod_Malloc(len + 1u);
    if (copy == NULL)
        return NULL;

    if (len > 0)
    {
        memcpy(copy, str, len);
    }
    copy[len] = '\0';
    return copy;
}

char* dmftp_str_join(const char* a, const char* b, const char* c)
{
    size_t len_a = a != NULL ? strlen(a) : 0;
    size_t len_b = b != NULL ? strlen(b) : 0;
    size_t len_c = c != NULL ? strlen(c) : 0;

    char* joined = Dmod_Malloc(len_a + len_b + len_c + 1u);
    if (joined == NULL)
        return NULL;

    if (len_a > 0) memcpy(joined, a, len_a);
    if (len_b > 0) memcpy(joined + len_a, b, len_b);
    if (len_c > 0) memcpy(joined + len_a + len_b, c, len_c);
    joined[len_a + len_b + len_c] = '\0';
    return joined;
}

bool dmftp_str_to_u32(const char* text, size_t len, uint32_t* out)
{
    if (text == NULL || out == NULL || len == 0)
        return false;

    uint32_t value = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return false;

        uint32_t digit = (uint32_t)(text[i] - '0');
        if (value > (0xFFFFFFFFu - digit) / 10u)
            return false; /* would overflow */

        value = value * 10u + digit;
    }

    *out = value;
    return true;
}

char dmftp_str_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool dmftp_str_iequal(const char* a, const char* b)
{
    if (a == NULL || b == NULL)
        return a == b;

    while (*a != '\0' && *b != '\0')
    {
        if (dmftp_str_upper(*a) != dmftp_str_upper(*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* ============================================================================
 *                      Module lifecycle
 * ========================================================================== */

/**
 * @brief Bring up the module lock and the listener registry
 *
 * No port is opened here - dmftp only listens once somebody calls
 * dmftp_server_start(), so merely loading this module has no visible
 * effect on the network.
 */
int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;

    g_lock = dmosi_mutex_create(true);
    if (g_lock == NULL)
    {
        DMOD_LOG_ERROR("dmftp: could not create the module lock\n");
        return -ENOMEM;
    }

    int result = dmftp_net_init();
    if (result != 0)
    {
        dmosi_mutex_destroy(g_lock);
        g_lock = NULL;
        return result;
    }

    return 0;
}

int dmod_deinit(void)
{
    dmftp_net_deinit();

    if (g_lock != NULL)
    {
        dmosi_mutex_destroy(g_lock);
        g_lock = NULL;
    }
    return 0;
}
