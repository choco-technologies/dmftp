/**
 * @file dmftp_path.c
 * @brief The one place a remote-supplied path becomes a real filesystem
 *        path
 *
 * Every server command that takes a filename funnels through
 * dmftp_path_resolve(), so the jail is enforced once rather than once per
 * command. The rule is RFC-agnostic and deliberately boring: normalize the
 * argument into an absolute *virtual* path where ".." can never climb past
 * "/", then prefix the session's root. A path that tries to escape is not
 * rejected - it is clamped, which is what a real chrooted FTP server does
 * and what clients expect when they blindly send "CWD ../..".
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>

/**
 * @brief Append one path component to `out`, prefixed by '/'
 *
 * @return 0 on success, -ENOMEM
 */
static int append_component(dmftp_buf_t* out, const char* name, size_t len)
{
    int result = dmftp_buf_append(out, "/", 1u);
    if (result != 0)
        return result;

    return dmftp_buf_append(out, name, len);
}

/**
 * @brief Drop the last component of the path accumulated in `out`
 *
 * The ".." case. At the top there is nothing to drop, and the attempt is
 * silently ignored - that single line is the whole escape-prevention
 * mechanism.
 */
static void drop_component(dmftp_buf_t* out)
{
    while (out->len > 0 && out->data[out->len - 1u] != '/')
    {
        out->len--;
    }
    if (out->len > 0)
    {
        out->len--; /* the '/' itself */
    }
}

/**
 * @brief Walk `path`'s components into `out`, resolving "." and ".."
 *
 * @return 0 on success, -ENOMEM
 */
static int normalize_into(dmftp_buf_t* out, const char* path)
{
    size_t i = 0;
    while (path[i] != '\0')
    {
        if (path[i] == '/')
        {
            i++;
            continue;
        }

        size_t start = i;
        while (path[i] != '\0' && path[i] != '/')
        {
            i++;
        }

        size_t len = i - start;
        if (len == 1u && path[start] == '.')
            continue;

        if (len == 2u && path[start] == '.' && path[start + 1u] == '.')
        {
            drop_component(out);
            continue;
        }

        int result = append_component(out, path + start, len);
        if (result != 0)
            return result;
    }
    return 0;
}

char* dmftp_path_virtual(const char* cwd, const char* arg)
{
    bool absolute = arg != NULL && arg[0] == '/';
    const char* base = absolute ? NULL : cwd;

    dmftp_buf_t out;
    dmftp_buf_init(&out);

    if (base != NULL && normalize_into(&out, base) != 0)
    {
        dmftp_buf_free(&out);
        return NULL;
    }
    if (arg != NULL && arg[0] != '\0' && normalize_into(&out, arg) != 0)
    {
        dmftp_buf_free(&out);
        return NULL;
    }

    /* An empty accumulator means every component cancelled out - that is
     * the root, which is spelled "/" rather than "". */
    char* result = (out.len == 0) ? dmftp_str_ndup("/", 1u)
                                  : dmftp_str_ndup((const char*)out.data, out.len);
    dmftp_buf_free(&out);
    return result;
}

char* dmftp_path_to_os(const char* root, const char* virtual_path)
{
    if (virtual_path == NULL)
        return NULL;

    bool root_is_slash = root == NULL || root[0] == '\0' || (root[0] == '/' && root[1] == '\0');
    if (root_is_slash)
        return dmftp_str_ndup(virtual_path, strlen(virtual_path));

    /* The virtual path already starts with '/', so joining needs no
     * separator - and a virtual path of exactly "/" must not leave a
     * trailing slash on the root. */
    if (virtual_path[1] == '\0')
        return dmftp_str_ndup(root, strlen(root));

    return dmftp_str_join(root, virtual_path, NULL);
}

char* dmftp_path_resolve(const char* root, const char* cwd, const char* arg, char** out_virtual)
{
    char* virtual_path = dmftp_path_virtual(cwd, arg);
    if (virtual_path == NULL)
        return NULL;

    char* os_path = dmftp_path_to_os(root, virtual_path);
    if (os_path == NULL || out_virtual == NULL)
    {
        Dmod_Free(virtual_path);
        return os_path;
    }

    *out_virtual = virtual_path;
    return os_path;
}

bool dmftp_path_is_dir(const char* os_path)
{
    if (os_path == NULL)
        return false;

    void* dir = Dmod_OpenDir(os_path);
    if (dir == NULL)
        return false;

    Dmod_CloseDir(dir);
    return true;
}

bool dmftp_path_file_size(const char* os_path, uint32_t* out_size)
{
    if (os_path == NULL || out_size == NULL)
        return false;

    /* The SAL exposes no stat(), so "how big is this file" costs an open
     * and a close - see dmftp.h's note on what the SAL does not give us. */
    void* file = Dmod_FileOpen(os_path, "rb");
    if (file == NULL)
        return false;

    *out_size = (uint32_t)Dmod_FileSize(file);
    Dmod_FileClose(file);
    return true;
}
