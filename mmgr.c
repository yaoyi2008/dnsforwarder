#include <string.h>
#include "mmgr.h"
#include "stringchunk.h"
#include "utils.h"
#include "filter.h"
#include "hosts.h"
#include "dnscache.h"
#include "logs.h"
#include "ipmisc.h"

typedef int (*SendFunc)(void *Module,
                        IHeader *h, /* Entity followed */
                        int BufferLength
                        );

typedef struct _ModuleInterface {
    union {
        UdpM    Udp;
        TcpM    Tcp;
    } ModuleUnion;

    SendFunc    Send;

    const char *ModuleName;

} ModuleInterface;

static StableBuffer Modules; /* Storing ModuleInterfaces */
static Array        ModuleArray; /* ModuleInterfaces' references */
static StringChunk  Distributor; /* Domain-to-ModuleInterface mapping */

static int MappingAModule(ModuleInterface *Stored, const char *Domains)
{
    StringList  DomainList;
    StringListIterator  i;

    const char *OneDomain;

    if( StringList_Init(&DomainList, Domains, ",") != 0 )
    {
        return -38;
    }

    DomainList.TrimAll(&DomainList, "\t .");
    DomainList.LowercaseAll(&DomainList);

    if( StringListIterator_Init(&i, &DomainList) != 0 )
    {
        return -46;
    }

    while( (OneDomain = i.Next(&i)) != NULL )
    {
        StringChunk_Add_Domain(&Distributor,
                               OneDomain,
                               &Stored,
                               sizeof(ModuleInterface *)
                               );
    }

    DomainList.Free(&DomainList);

    return 0;
}

static ModuleInterface *StoreAModule(void)
{
    ModuleInterface *Added;

    Added = Modules.Add(&Modules, NULL, sizeof(ModuleInterface), TRUE);
    if( Added == NULL )
    {
        return NULL;
    }

    if( Array_PushBack(&ModuleArray, &Added, NULL) < 0 )
    {
        return NULL;
    }

    Added->ModuleName = "Unknown";

    return Added;
}

static int Udp_Init(ConfigFileInfo *ConfigInfo)
{
    ModuleInterface *NewM;

    StringList  *UDPGroups;
    StringListIterator  i;

    UDPGroups = ConfigGetStringList(ConfigInfo, "UDPGroup");
    if( UDPGroups == NULL )
    {
        return 0;
    }

    if( StringListIterator_Init(&i, UDPGroups) != 0 )
    {
        return -33;
    }

    while( TRUE )
    {
        const char *Services;
        const char *Domains;
        const char *Parallel;
        char ParallelOnOff[8];
        BOOL ParallelQuery;

        /* Initializing parameters */
        Services = i.Next(&i);
        Domains = i.Next(&i);
        Parallel = i.Next(&i);

        if( Services == NULL || Domains == NULL || Parallel == NULL )
        {
            break;
        }

        NewM = StoreAModule();
        if( NewM == NULL )
        {
            return -101;
        }

        NewM->ModuleName = "UDP";

        strncpy(ParallelOnOff, Parallel, sizeof(ParallelOnOff));
        ParallelOnOff[sizeof(ParallelOnOff) - 1] = '\0';
        StrToLower(ParallelOnOff);

        if( strcmp(ParallelOnOff, "on") == 0 )
        {
            ParallelQuery = TRUE;
        } else {
            ParallelQuery = FALSE;
        }

        /* Initializing module */
        if( UdpM_Init(&(NewM->ModuleUnion.Udp), Services, ParallelQuery) != 0 )
        {
            continue;
        }

        NewM->Send = (SendFunc)(NewM->ModuleUnion.Udp.Send);

        if( MappingAModule(NewM, Domains) != 0 )
        {
            /** TODO: Show error */
        }
    }

    UDPGroups->Free(UDPGroups);
    return 0;
}

static int Tcp_Init(ConfigFileInfo *ConfigInfo)
{
    ModuleInterface *NewM;

    StringList  *TCPGroups;
    StringListIterator  i;

    TCPGroups = ConfigGetStringList(ConfigInfo, "TCPGroup");
    if( TCPGroups == NULL )
    {
        return 0;
    }

    if( StringListIterator_Init(&i, TCPGroups) != 0 )
    {
        return -33;
    }

    while( TRUE )
    {
        const char *Services;
        const char *Domains;
        const char *Proxies;
        char ProxyString[8];

        /* Initializing parameters */
        Services = i.Next(&i);
        Domains = i.Next(&i);
        Proxies = i.Next(&i);

        if( Services == NULL || Domains == NULL || Proxies == NULL )
        {
            break;
        }

        NewM = StoreAModule();
        if( NewM == NULL )
        {
            return -192;
        }

        NewM->ModuleName = "TCP";

        strncpy(ProxyString, Proxies, sizeof(ProxyString));
        ProxyString[sizeof(ProxyString) - 1] = '\0';
        StrToLower(ProxyString);

        if( strcmp(ProxyString, "no") == 0 )
        {
            Proxies = NULL;
        }

        /* Initializing module */
        if( TcpM_Init(&(NewM->ModuleUnion.Tcp), Services, Proxies) != 0 )
        {
            continue;
        }

        NewM->Send = (SendFunc)(NewM->ModuleUnion.Tcp.Send);

        if( MappingAModule(NewM, Domains) != 0 )
        {
            /** TODO: Show error */
        }
    }

    TCPGroups->Free(TCPGroups);
    return 0;
}

int MMgr_Init(ConfigFileInfo *ConfigInfo)
{
    BOOL ret = FALSE;

    if( Filter_Init(ConfigInfo) != 0 )
    {
        return -159;
    }

    /* Hosts & Cache */
    if( Hosts_Init(ConfigInfo) != 0 )
    {
        return -165;
    }

    if( DNSCache_Init(ConfigInfo) != 0 )
    {
        return -164;
    }

    if( IpMiscSingleton_Init(ConfigInfo) != 0 )
    {
        return -176;
    }

    /* Ordinary modeles */
    if( StringChunk_Init(&Distributor, NULL) != 0 )
    {
        return -10;
    }

    if( StableBuffer_Init(&Modules) != 0 )
    {
        return -27;
    }

    if( Array_Init(&ModuleArray,
                   sizeof(ModuleInterface *),
                   0,
                   FALSE,
                   NULL
                   )
       != 0 )
    {
        return -98;
    }

    ret |= (Udp_Init(ConfigInfo) == 0);
    ret |= (Tcp_Init(ConfigInfo) == 0);

    return !ret;
}

int MMgr_Send(IHeader *h, int BufferLength)
{
    ModuleInterface **i;
    ModuleInterface *TheModule;

    /* Determine whether to discard the query */
    if( Filter_Out(h) )
    {
        return 0;
    }

    /* Hosts & Cache */
    if( Hosts_Get(h, BufferLength) == 0 )
    {
        return 0;
    }

    if( DNSCache_FetchFromCache(h, BufferLength) == 0 )
    {
        return 0;
    }

    /* Ordinary modeles */
    if( StringChunk_Domain_Match(&Distributor,
                                 h->Domain,
                                 &(h->HashValue),
                                 (void **)&i
                                 )
       )
    {
    } else if( Array_GetUsed(&ModuleArray) > 0 ){
        i = Array_GetBySubscript(&ModuleArray,
                                 h->EntityLength % Array_GetUsed(&ModuleArray)
                                 );
    } else {
        i = NULL;
    }

    if( i == NULL || *i == NULL )
    {
        return -190;
    }

    TheModule = *i;

    return TheModule->Send(&(TheModule->ModuleUnion), h, BufferLength);
}
