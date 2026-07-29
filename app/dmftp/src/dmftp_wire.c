/**
 * @file dmftp_wire.c
 * @brief The FTP control-line codec: commands in, replies out, and RFC
 *        959's `h1,h2,h3,h4,p1,p2` host-port tuple both ways
 *
 * Everything here is pure - no connection, no session, no allocation - so
 * it is also the part of dmftp that tests can exercise directly without a
 * network fixture (see tests/dmftp_test.c).
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

/** @brief Telnet "Interpret As Command" escape (RFC 854) */
#define DMFTP_TELNET_IAC 0xFFu

/**
 * @brief Skip the Telnet control sequences a client may put in front of a
 *        command
 *
 * RFC 959 §4.1.3.5 has a client precede ABOR with `IAC IP` / `IAC DM`, and
 * some do it literally. An IAC is followed by one command byte, and by a
 * second (option) byte for the WILL/WONT/DO/DONT range 251..254 - skipping
 * exactly that much leaves the real verb at the front.
 */
static size_t skip_telnet(const char* line, size_t length)
{
    size_t offset = 0;
    while (offset < length && (uint8_t)line[offset] == DMFTP_TELNET_IAC)
    {
        if (offset + 1u >= length)
            return length;

        uint8_t command = (uint8_t)line[offset + 1u];
        offset += (command >= 251u && command <= 254u) ? 3u : 2u;
    }
    return offset < length ? offset : length;
}

dmod_dmftp_api_declaration(1.0, int, _parse_command, ( const char* line, size_t length, dmftp_command_t* out ))
{
    if (line == NULL || out == NULL)
        return -EINVAL;

    out->verb[0] = '\0';
    out->arg = NULL;
    out->arg_len = 0;

    size_t start = skip_telnet(line, length);
    while (start < length && line[start] == ' ')
    {
        start++;
    }

    size_t verb_end = start;
    while (verb_end < length && line[verb_end] != ' ')
    {
        verb_end++;
    }

    size_t verb_len = verb_end - start;
    if (verb_len == 0 || verb_len > DMFTP_VERB_MAX)
        return -EPROTO;

    for (size_t i = 0; i < verb_len; i++)
    {
        out->verb[i] = dmftp_str_upper(line[start + i]);
    }
    out->verb[verb_len] = '\0';

    /* Exactly one space separates verb from argument (RFC 959 §5.3); any
     * further spaces belong to the argument, since a filename may start
     * with one. Trailing whitespace is stripped, which is what lets a
     * client's stray CR or padding through without inventing a filename. */
    size_t arg_start = verb_end < length ? verb_end + 1u : length;
    size_t arg_end = length;
    while (arg_end > arg_start && (line[arg_end - 1u] == ' ' || line[arg_end - 1u] == '\t' || line[arg_end - 1u] == '\r'))
    {
        arg_end--;
    }

    if (arg_end > arg_start)
    {
        out->arg = line + arg_start;
        out->arg_len = arg_end - arg_start;
    }
    return 0;
}

/**
 * @brief Write `value` as exactly three decimal digits
 *
 * A reply code is always three digits by definition, so this beats a
 * general integer formatter and keeps the caller's bounds check trivial.
 */
static void write_code(char* buffer, int value)
{
    buffer[0] = (char)('0' + (value / 100) % 10);
    buffer[1] = (char)('0' + (value / 10) % 10);
    buffer[2] = (char)('0' + value % 10);
}

dmod_dmftp_api_declaration(1.0, int, _format_reply, ( char* buffer, size_t buffer_len, int code, const char* text, size_t* out_len ))
{
    if (buffer == NULL || text == NULL || code < 100 || code > 599)
        return -EINVAL;

    size_t text_len = strlen(text);
    size_t needed = 3u + 1u + text_len + 2u; /* "ddd" + ' ' + text + CRLF */
    if (buffer_len < needed)
        return -ENOSPC;

    write_code(buffer, code);
    buffer[3] = ' ';
    if (text_len > 0)
    {
        memcpy(buffer + 4, text, text_len);
    }
    buffer[4 + text_len] = '\r';
    buffer[5 + text_len] = '\n';

    if (out_len != NULL)
    {
        *out_len = needed;
    }
    return 0;
}

dmod_dmftp_api_declaration(1.0, int, _parse_reply, ( const char* line, size_t length, int* out_code, bool* out_final, const char** out_text ))
{
    if (line == NULL || out_code == NULL || out_final == NULL || out_text == NULL)
        return -EINVAL;
    if (length < 4u)
        return -EPROTO;

    uint32_t code = 0;
    if (!dmftp_str_to_u32(line, 3u, &code) || code < 100u || code > 599u)
        return -EPROTO;

    /* A space ends the reply, a '-' marks a continuation line that another
     * line with the same code will eventually close (RFC 959 §4.2). */
    if (line[3] != ' ' && line[3] != '-')
        return -EPROTO;

    *out_code = (int)code;
    *out_final = line[3] == ' ';
    *out_text = line + 4;
    return 0;
}

/**
 * @brief Append `value` in decimal, returning how many bytes it took
 *
 * Only ever called with values below 65536, so a five-digit scratch space
 * is a genuine bound rather than a guess (dmod-coding-conventions allows a
 * fixed buffer exactly here).
 */
static size_t append_u16(char* buffer, uint16_t value)
{
    char digits[5];
    size_t count = 0;

    do
    {
        digits[count++] = (char)('0' + (value % 10u));
        value = (uint16_t)(value / 10u);
    } while (value != 0);

    for (size_t i = 0; i < count; i++)
    {
        buffer[i] = digits[count - 1u - i];
    }
    return count;
}

dmod_dmftp_api_declaration(1.0, int, _format_host_port, ( char* buffer, size_t buffer_len, const dmip_addr_t* addr, uint16_t port, size_t* out_len ))
{
    if (buffer == NULL || addr == NULL)
        return -EINVAL;
    if (addr->family != dmip_family_v4)
        return -EINVAL; /* RFC 959's tuple is IPv4-only; EPSV/EPRT are out of scope */

    /* Worst case "255,255,255,255,255,255" plus NUL. */
    if (buffer_len < 24u)
        return -ENOSPC;

    size_t offset = 0;
    for (size_t i = 0; i < DMIP_IPV4_ADDR_LEN; i++)
    {
        offset += append_u16(buffer + offset, addr->addr.v4[i]);
        buffer[offset++] = ',';
    }
    offset += append_u16(buffer + offset, (uint16_t)(port >> 8));
    buffer[offset++] = ',';
    offset += append_u16(buffer + offset, (uint16_t)(port & 0xFFu));
    buffer[offset] = '\0';

    if (out_len != NULL)
    {
        *out_len = offset;
    }
    return 0;
}

/**
 * @brief Read one comma-separated decimal field, skipping leading spaces
 *
 * @return Pointer just past the field (at its ',' or terminator), or NULL
 *         if the field was missing or out of range
 */
static const char* read_field(const char* text, uint32_t* out)
{
    while (*text == ' ')
    {
        text++;
    }

    const char* start = text;
    while (*text >= '0' && *text <= '9')
    {
        text++;
    }

    if (text == start || !dmftp_str_to_u32(start, (size_t)(text - start), out) || *out > 255u)
        return NULL;

    return text;
}

dmod_dmftp_api_declaration(1.0, int, _parse_host_port, ( const char* text, dmip_addr_t* out_addr, uint16_t* out_port ))
{
    if (text == NULL || out_addr == NULL || out_port == NULL)
        return -EINVAL;

    uint32_t fields[6];
    for (size_t i = 0; i < 6u; i++)
    {
        text = read_field(text, &fields[i]);
        if (text == NULL)
            return -EPROTO;

        if (i < 5u)
        {
            if (*text != ',')
                return -EPROTO;
            text++;
        }
    }

    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->family = dmip_family_v4;
    for (size_t i = 0; i < DMIP_IPV4_ADDR_LEN; i++)
    {
        out_addr->addr.v4[i] = (uint8_t)fields[i];
    }
    *out_port = (uint16_t)((fields[4] << 8) | fields[5]);
    return 0;
}
