#define main ftpd_application_main
#include "../tools/ftpd/src/ftpd.c"
#undef main
#include <stdio.h>
#include <stdarg.h>

static unsigned starts;
static uint32_t observed_yield;
int ftpd_server_start(uint16_t port, const char* root, const char* user, const char* pass, uint32_t data_yield_ms)
{
    (void)port; (void)root; (void)user; (void)pass;
    starts++; observed_yield=data_yield_ms;
    return -ECANCELED; /* Stop before the production application's lifetime loop. */
}
static bool logging(Dmod_LogLevel_t level) { (void)level; return false; }
static int print_usage_text(const char* format, ...) { (void)format; return 0; }
static int check(const char* value, bool valid, uint32_t expected)
{
    char* argv[]={"ftpd","--data-yield",(char*)value};starts=0;observed_yield=0;
    int ret=ftpd_application_main(value?3:2,argv);
    if(valid)return ret!=-ECANCELED || starts!=1 || observed_yield!=expected;
    return ret!=-EINVAL || starts!=0;
}
int main(void)
{
    Dmod_CheckLogLevel=logging;Dmod_Printf=print_usage_text;Dmod_Sscanf=sscanf;
    int errors=check("50",true,50)+check("0",true,0)+check("1000",true,1000)
              +check("1001",false,0)+check("-1",false,0)+check("bad",false,0)+check(NULL,false,0);
    char* defaults[]={"ftpd"};starts=0;observed_yield=99;
    errors+=ftpd_application_main(1,defaults)!=-ECANCELED || starts!=1 || observed_yield!=0;
    printf("ftpd argument regression tests: %d failure(s)\n",errors);return errors;
}
