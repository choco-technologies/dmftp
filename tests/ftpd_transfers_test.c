/* Exercise the production callbacks with scripted TCP/file imports. */
#include "../tools/ftpd/src/ftpd_commands.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); return 1; } } while (0)
static struct ftpd_context context;
struct ftpd_context* g_ftpd_context = &context;
static ftpd_connection_t session;
static const size_t* script;
static size_t script_len, write_index, accepted, read_index;
static int codes[8], code_count, closes, file_closes;
static bool bad_data, close_completes, retr_mode;
static size_t retr_read, retr_sent, retr_reads, retr_sends, seeks;
#define RETR_SIZE (2u * FTPD_DATA_CHUNK_SIZE + 176u)

static size_t write_file(const void* data, size_t size, size_t count, void* file)
{
    (void)file;
    if (size != 1 || write_index >= script_len) { bad_data = true; return 0; }
    size_t n = script[write_index++];
    for (size_t i=0; i<n && i<count; i++)
        if (((const uint8_t*)data)[i] != (uint8_t)(accepted+i)) bad_data = true;
    accepted += n;
    return n;
}
static void* allocate(size_t n, const char* owner) { (void)owner; return malloc(n); }
static void release(void* p, bool merge) { (void)merge; free(p); }
static void yield_thread(uint32_t ms) { (void)ms; }
static bool logging(Dmod_LogLevel_t level) { (void)level; return false; }
static void close_file(void* file) { (void)file; file_closes++; }
static void* open_file(const char* path, const char* mode) { (void)path; (void)mode; return &session; }
static size_t read_file(void* data, size_t size, size_t count, void* file)
{
    (void)file; (void)size;
    if (retr_mode)
    {
        retr_reads++;
        size_t n = RETR_SIZE - retr_read;
        if (n > count) n = count;
        for (size_t i = 0; i < n; i++) ((uint8_t*)data)[i] = (uint8_t)(retr_read + i);
        retr_read += n;
        return n;
    }
    if (read_index++) return 0;
    size_t n = count < 8 ? count : 8;
    memset(data, 0, n);
    return n;
}
static int capture_reply(libftp_t engine, int code, const char* text)
{
    (void)engine; (void)text;
    if (code_count < 8) codes[code_count++] = code;
    return 0;
}
static int close_tcp(dmtcp_conn_t conn)
{
    closes++;
    if (close_completes) ftpd_data_on_closed(conn, &session);
    return 0;
}
static int send_tcp(dmtcp_conn_t conn, const void* data, size_t n)
{
    (void)conn;
    if (!retr_mode) return (int)n;
    const size_t limits[] = {0, 3, 0, 73, 4096, 1, 0, 4093};
    size_t accepted = limits[retr_sends++ % (sizeof(limits) / sizeof(limits[0]))];
    if (accepted > n) accepted = n;
    for (size_t i = 0; i < accepted; i++)
        if (((const uint8_t*)data)[i] != (uint8_t)(retr_sent + i)) bad_data = true;
    retr_sent += accepted;
    return (int)accepted;
}
static int seek_file(void* file, long offset, int origin)
{
    (void)file; (void)offset; (void)origin; seeks++; return -1;
}
static dmtcp_state_t state_tcp(dmtcp_conn_t conn) { (void)conn; return dmtcp_state_established; }

/* The TCP/listener glue is not involved in these transfer-state tests. */
bool ftpd_is_anonymous_user(const char* user) { return strcmp(user,"anonymous")==0; }
void ftpd_server_stop_pasv(ftpd_connection_t* c) { c->pasv_pending=false; }
int ftpd_server_start_pasv(ftpd_connection_t* c, uint16_t* port) { (void)c; *port=49152; return 0; }
int ftpd_server_connect_port(ftpd_connection_t* c) { (void)c; return 0; }

static void reset(const size_t* writes, size_t n)
{
    memset(&session, 0, sizeof(session));
    memset(codes, 0, sizeof(codes));
    session.data_conn = (dmtcp_conn_t)&session;
    session.transfer_file = &session;
    session.data_op = ftpd_data_op_stor;
    session.transfer_buffer = malloc(FTPD_STOR_CHUNK_SIZE);
    context.root = "/";
    script=writes; script_len=n;
    write_index=accepted=read_index=0;
    code_count=closes=file_closes=0;
    bad_data=close_completes=retr_mode=false;
    retr_read=retr_sent=retr_reads=retr_sends=seeks=0;
    Dmod_MallocEx=allocate; Dmod_FreeEx=release; Dmod_CheckLogLevel=logging;
    Dmod_StrDup=strdup; Dmod_SnPrintf=snprintf; Dmod_VSnPrintf=vsnprintf; Dmod_Sscanf=sscanf;
    Dmod_FileWrite=write_file; Dmod_FileClose=close_file;
    Dmod_FileOpen=open_file; Dmod_FileRead=read_file; Dmod_FileSeek=seek_file;
    libftp_reply=capture_reply;
    dmosi_thread_sleep=yield_thread;
    dmtcp_close=close_tcp; dmtcp_send=send_tcp; dmtcp_conn_get_state=state_tcp;
}
static void finish(void)
{
    dmtcp_conn_t conn=session.data_conn;
    ftpd_data_on_data(conn,NULL,0,&session);
    ftpd_data_on_closed(conn,&session);
}
static int upload_case(const size_t* writes, size_t n, int expected, size_t written)
{
    const uint8_t data[]={0,1,2,3,4,5,6,7};
    reset(writes,n);
    ftpd_data_on_data(session.data_conn,data,sizeof(data),&session);
    finish();
    CHECK(accepted==written && !bad_data);
    CHECK(code_count==1 && codes[0]==expected && file_closes==1);
    CHECK(session.data_op==ftpd_data_op_none && !session.write_failed);
    return 0;
}
static int reject_further_data_after_failure(void)
{
    const size_t writes[]={0}; uint8_t data[FTPD_STOR_CHUNK_SIZE]={0};
    reset(writes,1);
    ftpd_data_on_data(session.data_conn,data,sizeof(data),&session);
    ftpd_data_on_data(session.data_conn,data,sizeof(data),&session);
    CHECK(write_index==1 && session.write_failed);
    finish(); CHECK(code_count==1 && codes[0]==451);
    return 0;
}
static int reply_before_data(void)
{
    reset(NULL,0);
    session.logged_in=true; session.cwd="/"; session.data_op=ftpd_data_op_none;
    session.transfer_file=NULL; free(session.transfer_buffer); session.transfer_buffer=NULL; close_completes=true;
    ftpd_handle_command(NULL,"RETR","file",&session);
    CHECK(code_count==2 && codes[0]==150 && codes[1]==226);
    return 0;
}
static int upload_batches_packets_and_flushes_tail(void)
{
    const size_t writes[]={4096,2000,2096,101};
    uint8_t data[8293];
    for (size_t i=0; i<sizeof(data); i++) data[i]=(uint8_t)i;
    reset(writes,4);
    ftpd_data_on_data(session.data_conn,data,535,&session);
    CHECK(write_index==0 && session.buffer_len==535);
    ftpd_data_on_data(session.data_conn,data+535,sizeof(data)-535,&session);
    CHECK(write_index==3 && accepted==8192 && session.buffer_len==101 && !bad_data);
    finish();
    CHECK(write_index==4 && accepted==sizeof(data) && !bad_data && codes[0]==226);
    return 0;
}
static int upload_failure_after_full_buffer_stays_failed(void)
{
    const size_t writes[]={4096,0};
    uint8_t data[8192];
    for (size_t i=0; i<sizeof(data); i++) data[i]=(uint8_t)i;
    reset(writes,2);
    ftpd_data_on_data(session.data_conn,data,sizeof(data),&session);
    CHECK(write_index==2 && accepted==4096 && session.write_failed);
    ftpd_data_on_data(session.data_conn,data,535,&session);
    finish();
    CHECK(write_index==2 && code_count==1 && codes[0]==451 && !bad_data);
    return 0;
}
static int download_keeps_unsent_tail(void)
{
    reset(NULL, 0);
    session.logged_in=true; session.cwd="/"; session.data_op=ftpd_data_op_none;
    session.transfer_file=NULL; free(session.transfer_buffer); session.transfer_buffer=NULL; retr_mode=true;
    ftpd_handle_command(NULL, "RETR", "file", &session);
    CHECK(code_count==1 && codes[0]==150 && retr_read==FTPD_DATA_CHUNK_SIZE && retr_sent==0);
    for (unsigned i=0; i<100 && !closes; i++)
        ftpd_data_on_writable(session.data_conn, 512, &session);
    CHECK(closes==1 && !bad_data && retr_sent==RETR_SIZE && retr_read==RETR_SIZE);
    CHECK(retr_reads==4 && seeks==0);
    ftpd_data_on_closed(session.data_conn, &session);
    CHECK(code_count==2 && codes[1]==226 && session.transfer_buffer==NULL);
    return 0;
}
int main(void)
{
    const size_t full[]={8}, partial[]={3,5}, zero[]={0}, failed[]={3,0}, invalid[]={9};
    int errors=0;
    errors+=upload_case(full,1,226,8);
    errors+=upload_case(partial,2,226,8);
    errors+=upload_case(zero,1,451,0);
    errors+=upload_case(failed,2,451,3);
    errors+=upload_case(invalid,1,451,9);
    errors+=reject_further_data_after_failure();
    errors+=reply_before_data();
    errors+=download_keeps_unsent_tail();
    errors+=upload_batches_packets_and_flushes_tail();
    errors+=upload_failure_after_full_buffer_stays_failed();
    reset(NULL,0);finish();if(code_count!=1 || codes[0]!=226)errors++;
    printf("ftpd transfer regression tests: %d failure(s)\n",errors);
    return errors;
}
