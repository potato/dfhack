/*
www.sourceforge.net/projects/dfhack
Copyright (c) 2009 Petr Mrázek (peterix), Kenneth Ferland (Impaler[WrG]), dorf

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include "Internal.h"
#include "dfhack/DFProcess.h"
#include "dfhack/VersionInfo.h"
#include "dfhack/DFError.h"
#include <string.h>
using namespace DFHack;

class NormalProcess::Private
{
    public:
        Private()
        {
            my_descriptor = NULL;
            my_handle = NULL;
            my_main_thread = NULL;
            my_pid = 0;
            attached = false;
            suspended = false;
            base = 0;
            sections = 0;
        };
        ~Private(){};
        VersionInfo * my_descriptor;
        HANDLE my_handle;
        HANDLE my_main_thread;
        uint32_t my_pid;
        string memFile;
        bool attached;
        bool suspended;
        bool identified;
        uint32_t STLSTR_buf_off;
        uint32_t STLSTR_size_off;
        uint32_t STLSTR_cap_off;
        IMAGE_NT_HEADERS32 pe_header;
        IMAGE_SECTION_HEADER * sections;
        uint32_t base;
};

NormalProcess::NormalProcess(uint32_t pid, vector <VersionInfo *> & known_versions)
: d(new Private())
{
    HMODULE hmod = NULL;
    DWORD needed;
    HANDLE hProcess;
    bool found = false;

    d->identified = false;
    // open process
    hProcess = OpenProcess( PROCESS_ALL_ACCESS, FALSE, pid );
    if (NULL == hProcess)
        return;

    // try getting the first module of the process
    if(EnumProcessModules(hProcess, &hmod, sizeof(hmod), &needed) == 0)
    {
        CloseHandle(hProcess);
        // cout << "EnumProcessModules fail'd" << endl;
        return; //if enumprocessModules fails, give up
    }

    // got base ;)
    d->base = (uint32_t)hmod;

    // temporarily assign this to allow some checks
    d->my_handle = hProcess;
    d->my_main_thread = 0;
    // read from this process
    try
    {
        uint32_t pe_offset = readDWord(d->base+0x3C);
        read(d->base + pe_offset                       , sizeof(d->pe_header), (uint8_t *)&d->pe_header);
        const size_t sectionsSize = sizeof(IMAGE_SECTION_HEADER) * d->pe_header.FileHeader.NumberOfSections;
        d->sections = (IMAGE_SECTION_HEADER *) malloc(sectionsSize); 
        read(d->base + pe_offset + sizeof(d->pe_header), sectionsSize, (uint8_t *)d->sections);
        d->my_handle = 0;
    }
    catch (exception &)
    {
        CloseHandle(hProcess);
        d->my_handle = 0;
        return;
    }

    // see if there's a version entry that matches this process
    vector<VersionInfo*>::iterator it;
    for ( it=known_versions.begin() ; it < known_versions.end(); it++ )
    {
        // filter by OS
        if(OS_WINDOWS != (*it)->getOS())
            continue;
        uint32_t pe_timestamp;
        // filter by timestamp, skip entries without a timestamp
        try
        {
            pe_timestamp = (*it)->getPE();
        }
        catch(Error::AllMemdef&)
        {
            continue;
        }
        if (pe_timestamp != d->pe_header.FileHeader.TimeDateStamp)
            continue;

        // all went well
        {
            printf("Match found! Using version %s.\n", (*it)->getVersion().c_str());
            d->identified = true;
            // give the process a data model and memory layout fixed for the base of first module
            VersionInfo *m = new VersionInfo(**it);
            m->RebaseAll(d->base);
            // keep track of created memory_info object so we can destroy it later
            d->my_descriptor = m;
            m->setParentProcess(this);
            // process is responsible for destroying its data model
            d->my_pid = pid;
            d->my_handle = hProcess;
            d->identified = true;

            // TODO: detect errors in thread enumeration
            vector<uint32_t> threads;
            getThreadIDs( threads );
            d->my_main_thread = OpenThread(THREAD_ALL_ACCESS, FALSE, (DWORD) threads[0]);
            OffsetGroup * strGrp = m->getGroup("string")->getGroup("MSVC");
            d->STLSTR_buf_off = strGrp->getOffset("buffer");
            d->STLSTR_size_off = strGrp->getOffset("size");
            d->STLSTR_cap_off = strGrp->getOffset("capacity");
            found = true;
            break; // break the iterator loop
        }
    }
    // close handle of processes that aren't DF
    if(!found)
    {
        CloseHandle(hProcess);
    }
}
/*
*/

NormalProcess::~NormalProcess()
{
    if(d->attached)
    {
        detach();
    }
    // destroy our rebased copy of the memory descriptor
    delete d->my_descriptor;
    if(d->my_handle != NULL)
    {
        CloseHandle(d->my_handle);
    }
    if(d->my_main_thread != NULL)
    {
        CloseHandle(d->my_main_thread);
    }
    if(d->sections != NULL)
        free(d->sections);
    delete d;
}

VersionInfo * NormalProcess::getDescriptor()
{
    return d->my_descriptor;
}

int NormalProcess::getPID()
{
    return d->my_pid;
}

bool NormalProcess::isSuspended()
{
    return d->suspended;
}
bool NormalProcess::isAttached()
{
    return d->attached;
}

bool NormalProcess::isIdentified()
{
    return d->identified;
}

bool NormalProcess::asyncSuspend()
{
    return suspend();
}

bool NormalProcess::suspend()
{
    if(!d->attached)
        return false;
    if(d->suspended)
    {
        return true;
    }
    SuspendThread(d->my_main_thread);
    d->suspended = true;
    return true;
}

bool NormalProcess::forceresume()
{
    if(!d->attached)
        return false;
    while (ResumeThread(d->my_main_thread) > 1);
    d->suspended = false;
    return true;
}


bool NormalProcess::resume()
{
    if(!d->attached)
        return false;
    if(!d->suspended)
    {
        return true;
    }
    ResumeThread(d->my_main_thread);
    d->suspended = false;
    return true;
}

bool NormalProcess::attach()
{
    if(d->attached)
    {
        if(!d->suspended)
            return suspend();
        return true;
    }
    d->attached = true;
    suspend();

    return true;
}


bool NormalProcess::detach()
{
    if(!d->attached) return true;
    resume();
    d->attached = false;
    return true;
}

bool NormalProcess::getThreadIDs(vector<uint32_t> & threads )
{
    HANDLE AllThreads = INVALID_HANDLE_VALUE;
    THREADENTRY32 te32;

    AllThreads = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if( AllThreads == INVALID_HANDLE_VALUE )
    {
        return false;
    }
    te32.dwSize = sizeof(THREADENTRY32 );

    if( !Thread32First( AllThreads, &te32 ) )
    {
        CloseHandle( AllThreads );
        return false;
    }

    do
    {
        if( te32.th32OwnerProcessID == d->my_pid )
        {
            threads.push_back(te32.th32ThreadID);
        }
    } while( Thread32Next(AllThreads, &te32 ) );

    CloseHandle( AllThreads );
    return true;
}
/*
typedef struct _MEMORY_BASIC_INFORMATION
{
  void *  BaseAddress;
  void *  AllocationBase;
  uint32_t  AllocationProtect;
  size_t RegionSize;
  uint32_t  State;
  uint32_t  Protect;
  uint32_t  Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
*/
/*
//Internal structure used to store heap block information.
struct HeapBlock
{
      PVOID dwAddress;
      DWORD dwSize;
      DWORD dwFlags;
      ULONG reserved;
};
*/
void HeapNodes(DWORD pid, map<uint64_t, unsigned int> & heaps)
{
    // Create debug buffer
    PDEBUG_BUFFER db = RtlCreateQueryDebugBuffer(0, FALSE); 
    // Get process heap data
    RtlQueryProcessDebugInformation( pid, PDI_HEAPS/* | PDI_HEAP_BLOCKS*/, db);
    ULONG heapNodeCount = db->HeapInformation ? *PULONG(db->HeapInformation):0;
    PDEBUG_HEAP_INFORMATION heapInfo = PDEBUG_HEAP_INFORMATION(PULONG(db-> HeapInformation) + 1);
    // Go through each of the heap nodes and dispaly the information
    for (unsigned int i = 0; i < heapNodeCount; i++) 
    {
        heaps[heapInfo[i].Base] = i;
    }
    // Clean up the buffer
    RtlDestroyQueryDebugBuffer( db );
}

// FIXME: NEEDS TESTING!
void NormalProcess::getMemRanges( vector<t_memrange> & ranges )
{
    MEMORY_BASIC_INFORMATION MBI;
    map<uint64_t, unsigned int> heaps;
    uint64_t movingStart = 0;
    map <uint64_t, string> nameMap;

    // get page size
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t PageSize = si.dwPageSize;
    // enumerate heaps
    HeapNodes(d->my_pid, heaps);
    // go through all the VM regions, convert them to our internal format
    while (VirtualQueryEx(this->d->my_handle, (const void*) (movingStart), &MBI, sizeof(MBI)) == sizeof(MBI))
    {
        movingStart = ((uint64_t)MBI.BaseAddress + MBI.RegionSize);
        if(movingStart % PageSize != 0)
            movingStart = (movingStart / PageSize + 1) * PageSize;
        // skip empty regions and regions we share with other processes (DLLs)
        if( !(MBI.State & MEM_COMMIT) /*|| !(MBI.Type & MEM_PRIVATE)*/ )
            continue;
        t_memrange temp;
        temp.start   = (uint64_t) MBI.BaseAddress;
        temp.end     =  ((uint64_t)MBI.BaseAddress + (uint64_t)MBI.RegionSize);
        temp.read    = MBI.Protect & PAGE_EXECUTE_READ || MBI.Protect & PAGE_EXECUTE_READWRITE || MBI.Protect & PAGE_READONLY || MBI.Protect & PAGE_READWRITE;
        temp.write   = MBI.Protect & PAGE_EXECUTE_READWRITE || MBI.Protect & PAGE_READWRITE;
        temp.execute = MBI.Protect & PAGE_EXECUTE_READ || MBI.Protect & PAGE_EXECUTE_READWRITE || MBI.Protect & PAGE_EXECUTE;
        temp.valid = true;
        if(!GetModuleBaseName(this->d->my_handle, (HMODULE) temp.start, temp.name, 1024))
        {
            if(nameMap.count(temp.start))
            {
                // potential buffer overflow...
                strcpy(temp.name, nameMap[temp.start].c_str());
            }
            else
            {
                // filter away shared segments without a name.
                if( !(MBI.Type & MEM_PRIVATE) )
                    continue;
                else
                {
                    // could be a heap?
                    if(heaps.count(temp.start))
                    {
                        sprintf(temp.name,"HEAP %d",heaps[temp.start]);
                    }
                    else temp.name[0]=0;
                }
                

                
            }
        }
        else
        {
            // this is our executable! (could be generalized to pull segments from libs, but whatever)
            if(d->base == temp.start)
            {
                for(int i = 0; i < d->pe_header.FileHeader.NumberOfSections; i++)
                {
                    char sectionName[9];
                    memcpy(sectionName,d->sections[i].Name,8);
                    sectionName[8] = 0;
                    string nm;
                    nm.append(temp.name);
                    nm.append(" : ");
                    nm.append(sectionName);
                    nameMap[temp.start + d->sections[i].VirtualAddress] = nm;
                }
            }
            else
                continue;
        }
        ranges.push_back(temp);
    }
}

uint8_t NormalProcess::readByte (const uint32_t offset)
{
    uint8_t result;
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint8_t), NULL))
        throw Error::MemoryAccessDenied();
    return result;
}

void NormalProcess::readByte (const uint32_t offset,uint8_t &result)
{
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint8_t), NULL))
        throw Error::MemoryAccessDenied();
}

uint16_t NormalProcess::readWord (const uint32_t offset)
{
    uint16_t result;
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint16_t), NULL))
        throw Error::MemoryAccessDenied();
    return result;
}

void NormalProcess::readWord (const uint32_t offset, uint16_t &result)
{
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint16_t), NULL))
        throw Error::MemoryAccessDenied();
}

uint32_t NormalProcess::readDWord (const uint32_t offset)
{
    uint32_t result;
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint32_t), NULL))
        throw Error::MemoryAccessDenied();
    return result;
}

void NormalProcess::readDWord (const uint32_t offset, uint32_t &result)
{
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint32_t), NULL))
        throw Error::MemoryAccessDenied();
}

uint64_t NormalProcess::readQuad (const uint32_t offset)
{
    uint64_t result;
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint64_t), NULL))
        throw Error::MemoryAccessDenied();
    return result;
}

void NormalProcess::readQuad (const uint32_t offset, uint64_t &result)
{
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(uint64_t), NULL))
        throw Error::MemoryAccessDenied();
}

float NormalProcess::readFloat (const uint32_t offset)
{
    float result;
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(float), NULL))
        throw Error::MemoryAccessDenied();
    return result;
}

void NormalProcess::readFloat (const uint32_t offset, float &result)
{
    if(!ReadProcessMemory(d->my_handle, (int*) offset, &result, sizeof(float), NULL))
        throw Error::MemoryAccessDenied();
}

void NormalProcess::read (const uint32_t offset, uint32_t size, uint8_t *target)
{
    if(!ReadProcessMemory(d->my_handle, (int*) offset, target, size, NULL))
        throw Error::MemoryAccessDenied();
}

// WRITING
void NormalProcess::writeQuad (const uint32_t offset, uint64_t data)
{
    if(!WriteProcessMemory(d->my_handle, (int*) offset, &data, sizeof(data), NULL))
        throw Error::MemoryAccessDenied();
}

void NormalProcess::writeDWord (const uint32_t offset, uint32_t data)
{
    if(!WriteProcessMemory(d->my_handle, (int*) offset, &data, sizeof(data), NULL))
        throw Error::MemoryAccessDenied();
}

// using these is expensive.
void NormalProcess::writeWord (uint32_t offset, uint16_t data)
{
    if(!WriteProcessMemory(d->my_handle, (int*) offset, &data, sizeof(data), NULL))
        throw Error::MemoryAccessDenied();
}

void NormalProcess::writeByte (uint32_t offset, uint8_t data)
{
    if(!WriteProcessMemory(d->my_handle, (int*) offset, &data, sizeof(data), NULL))
        throw Error::MemoryAccessDenied();
}

void NormalProcess::write (uint32_t offset, uint32_t size, uint8_t *source)
{
    if(!WriteProcessMemory(d->my_handle, (int*) offset, source, size, NULL))
        throw Error::MemoryAccessDenied();
}



///FIXME: reduce use of temporary objects
const string NormalProcess::readCString (const uint32_t offset)
{
    string temp;
    char temp_c[256];
    SIZE_T read;
    if(!ReadProcessMemory(d->my_handle, (int *) offset, temp_c, 254, &read))
        throw Error::MemoryAccessDenied();
    // needs to be 254+1 byte for the null term
    temp_c[read+1] = 0;
    temp.assign(temp_c);
    return temp;
}

size_t NormalProcess::readSTLString (uint32_t offset, char * buffer, size_t bufcapacity)
{
    uint32_t start_offset = offset + d->STLSTR_buf_off;
    size_t length = readDWord(offset + d->STLSTR_size_off);
    size_t capacity = readDWord(offset + d->STLSTR_cap_off);
    size_t read_real = min(length, bufcapacity-1);// keep space for null termination

    // read data from inside the string structure
    if(capacity < 16)
    {
        read(start_offset, read_real , (uint8_t *)buffer);
    }
    else // read data from what the offset + 4 dword points to
    {
        start_offset = readDWord(start_offset);// dereference the start offset
        read(start_offset, read_real, (uint8_t *)buffer);
    }

    buffer[read_real] = 0;
    return read_real;
}

const string NormalProcess::readSTLString (uint32_t offset)
{
    uint32_t start_offset = offset + d->STLSTR_buf_off;
    size_t length = readDWord(offset + d->STLSTR_size_off);
    size_t capacity = readDWord(offset + d->STLSTR_cap_off);
    char * temp = new char[capacity+1];

    // read data from inside the string structure
    if(capacity < 16)
    {
        read(start_offset, capacity, (uint8_t *)temp);
    }
    else // read data from what the offset + 4 dword points to
    {
        start_offset = readDWord(start_offset);// dereference the start offset
        read(start_offset, capacity, (uint8_t *)temp);
    }

    temp[length] = 0;
    string ret = temp;
    delete temp;
    return ret;
}

string NormalProcess::readClassName (uint32_t vptr)
{
    int rtti = readDWord(vptr - 0x4);
    int typeinfo = readDWord(rtti + 0xC);
    string raw = readCString(typeinfo + 0xC); // skips the .?AV
    raw.resize(raw.length() - 2);// trim @@ from end
    return raw;
}
string NormalProcess::getPath()
{
    HMODULE hmod;
    DWORD junk;
    char String[255];
    EnumProcessModules(d->my_handle, &hmod, 1 * sizeof(HMODULE), &junk); //get the module from the handle
    GetModuleFileNameEx(d->my_handle,hmod,String,sizeof(String)); //get the filename from the module
    string out(String);
    return(out.substr(0,out.find_last_of("\\")));
}