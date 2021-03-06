#include <assert.h>
#include <errno.h>

#include "idris_rts.h"
#include "idris_gc.h"
#include "idris_utf8.h"
#include "idris_bitstring.h"
#include "getline.h"

#ifdef HAS_PTHREAD
static pthread_key_t vm_key;
#else
static VM* global_vm;
#endif

void free_key(VM *vm) {
    // nothing to free, we just used the VM pointer which is freed elsewhere
}

VM* init_vm(int stack_size, size_t heap_size,
            int max_threads // not implemented yet
            ) {

    VM* vm = malloc(sizeof(VM));
    STATS_INIT_STATS(vm->stats)
    STATS_ENTER_INIT(vm->stats)

    VAL* valstack = malloc(stack_size * sizeof(VAL));

    vm->active = 1;
    vm->valstack = valstack;
    vm->valstack_top = valstack;
    vm->valstack_base = valstack;
    vm->stack_max = valstack + stack_size;

    alloc_heap(&(vm->heap), heap_size, heap_size, NULL);

    c_heap_init(&vm->c_heap);

    vm->ret = NULL;
    vm->reg1 = NULL;
#ifdef HAS_PTHREAD
    vm->inbox = malloc(1024*sizeof(VAL));
    memset(vm->inbox, 0, 1024*sizeof(VAL));
    vm->inbox_end = vm->inbox + 1024;
    vm->inbox_write = vm->inbox;

    // The allocation lock must be reentrant. The lock exists to ensure that
    // no memory is allocated during the message sending process, but we also
    // check the lock in calls to allocate.
    // The problem comes when we use requireAlloc to guarantee a chunk of memory
    // first: this sets the lock, and since it is not reentrant, we get a deadlock.
    pthread_mutexattr_t rec_attr;
    pthread_mutexattr_init(&rec_attr);
    pthread_mutexattr_settype(&rec_attr, PTHREAD_MUTEX_RECURSIVE);

    pthread_mutex_init(&(vm->inbox_lock), NULL);
    pthread_mutex_init(&(vm->inbox_block), NULL);
    pthread_mutex_init(&(vm->alloc_lock), &rec_attr);
    pthread_cond_init(&(vm->inbox_waiting), NULL);

    vm->max_threads = max_threads;
    vm->processes = 0;

#else
    global_vm = vm;
#endif
    STATS_LEAVE_INIT(vm->stats)
    return vm;
}

VM* idris_vm() {
    VM* vm = init_vm(4096000, 4096000, 1);
    init_threadkeys();
    init_threaddata(vm);
    init_gmpalloc();
    init_nullaries();
    init_signals();

    return vm;
}

VM* get_vm(void) {
#ifdef HAS_PTHREAD
    init_threadkeys();
    return pthread_getspecific(vm_key);
#else
    return global_vm;
#endif
}

void close_vm(VM* vm) {
    terminate(vm);
}

#ifdef HAS_PTHREAD
void create_key() {
    pthread_key_create(&vm_key, (void*)free_key);
}
#endif

void init_threadkeys() {
#ifdef HAS_PTHREAD
    static pthread_once_t key_once = PTHREAD_ONCE_INIT;
    pthread_once(&key_once, create_key);
#endif
}

void init_threaddata(VM *vm) {
#ifdef HAS_PTHREAD
    pthread_setspecific(vm_key, vm);
#endif
}

void init_signals() {
#if (__linux__ || __APPLE__ || __FreeBSD__ || __DragonFly__)
    signal(SIGPIPE, SIG_IGN);
#endif
}

Stats terminate(VM* vm) {
    Stats stats = vm->stats;
    STATS_ENTER_EXIT(stats)
#ifdef HAS_PTHREAD
    free(vm->inbox);
#endif
    free(vm->valstack);
    free_heap(&(vm->heap));
    c_heap_destroy(&(vm->c_heap));
#ifdef HAS_PTHREAD
    pthread_mutex_destroy(&(vm -> inbox_lock));
    pthread_mutex_destroy(&(vm -> inbox_block));
    pthread_cond_destroy(&(vm -> inbox_waiting));
#endif
    // free(vm);
    // Set the VM as inactive, so that if any message gets sent to it
    // it will not get there, rather than crash the entire system.
    // (TODO: We really need to be cleverer than this if we're going to
    // write programs than create lots of threads...)
    vm->active = 0;

    STATS_LEAVE_EXIT(stats)
    return stats;
}

CData cdata_allocate(size_t size, CDataFinalizer finalizer)
{
    void * data = (void *) malloc(size);
    return cdata_manage(data, size, finalizer);
}

CData cdata_manage(void * data, size_t size, CDataFinalizer finalizer)
{
    return c_heap_create_item(data, size, finalizer);
}

void idris_requireAlloc(size_t size) {
#ifdef HAS_PTHREAD
    VM* vm = pthread_getspecific(vm_key);
#else
    VM* vm = global_vm;
#endif

    if (!(vm->heap.next + size < vm->heap.end)) {
        idris_gc(vm);
    }
#ifdef HAS_PTHREAD
    int lock = vm->processes > 0;
    if (lock) { // We only need to lock if we're in concurrent mode
       pthread_mutex_lock(&vm->alloc_lock);
    }
#endif
}

void idris_doneAlloc() {
#ifdef HAS_PTHREAD
    VM* vm = pthread_getspecific(vm_key);
    int lock = vm->processes > 0;
    if (lock) { // We only need to lock if we're in concurrent mode
       pthread_mutex_unlock(&vm->alloc_lock);
    }
#endif
}

int space(VM* vm, size_t size) {
    return (vm->heap.next + size + sizeof(size_t) < vm->heap.end);
}

void* idris_alloc(size_t size) {
    Closure* cl = (Closure*) allocate(sizeof(Closure)+size, 0);
    SETTY(cl, CT_RAWDATA);
    cl->info.size = size;
    return (void*)cl+sizeof(Closure);
}

void* idris_realloc(void* old, size_t old_size, size_t size) {
    void* ptr = idris_alloc(size);
    memcpy(ptr, old, old_size);
    return ptr;
}

void idris_free(void* ptr, size_t size) {
}

void* allocate(size_t size, int outerlock) {
//    return malloc(size);

#ifdef HAS_PTHREAD
    VM* vm = pthread_getspecific(vm_key);
    int lock = vm->processes > 0 && !outerlock;

    if (lock) { // not message passing
       pthread_mutex_lock(&vm->alloc_lock);
    }
#else
    VM* vm = global_vm;
#endif

    if ((size & 7)!=0) {
	size = 8 + ((size >> 3) << 3);
    }

    size_t chunk_size = size + sizeof(size_t);

    if (vm->heap.next + chunk_size < vm->heap.end) {
        STATS_ALLOC(vm->stats, chunk_size)
        void* ptr = (void*)(vm->heap.next + sizeof(size_t));
        *((size_t*)(vm->heap.next)) = chunk_size;
        vm->heap.next += chunk_size;

        assert(vm->heap.next <= vm->heap.end);

        memset(ptr, 0, size);
#ifdef HAS_PTHREAD
        if (lock) { // not message passing
           pthread_mutex_unlock(&vm->alloc_lock);
        }
#endif
        return ptr;
    } else {
        idris_gc(vm);
#ifdef HAS_PTHREAD
        if (lock) { // not message passing
           pthread_mutex_unlock(&vm->alloc_lock);
        }
#endif
        return allocate(size, 0);
    }

}

/* Now a macro
void* allocCon(VM* vm, int arity, int outer) {
    Closure* cl = allocate(vm, sizeof(Closure) + sizeof(VAL)*arity,
                               outer);
    SETTY(cl, CT_CON);

    cl -> info.c.arity = arity;
//    cl -> info.c.tag = 42424242;
//    printf("%p\n", cl);
    return (void*)cl;
}
*/

VAL MKFLOAT(VM* vm, double val) {
    Closure* cl = allocate(sizeof(Closure), 0);
    SETTY(cl, CT_FLOAT);
    cl -> info.f = val;
    return cl;
}

VAL MKSTR(VM* vm, const char* str) {
    int len;
    if (str == NULL) {
        len = 0;
    } else {
        len = strlen(str)+1;
    }
    Closure* cl = allocate(sizeof(Closure) + // Type) + sizeof(char*) +
                           sizeof(char)*len, 0);
    SETTY(cl, CT_STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    if (str == NULL) {
        cl->info.str = NULL;
    } else {
        strcpy(cl -> info.str, str);
    }
    return cl;
}

char* GETSTROFF(VAL stroff) {
    // Assume STROFF
    StrOffset* root = stroff->info.str_offset;
    return (root->str->info.str + root->offset);
}

VAL MKCDATA(VM* vm, CHeapItem * item) {
    c_heap_insert_if_needed(vm, &vm->c_heap, item);
    Closure* cl = allocate(sizeof(Closure), 0);
    SETTY(cl, CT_CDATA);
    cl->info.c_heap_item = item;
    return cl;
}

VAL MKCDATAc(VM* vm, CHeapItem * item) {
    c_heap_insert_if_needed(vm, &vm->c_heap, item);
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, CT_CDATA);
    cl->info.c_heap_item = item;
    return cl;
}

VAL MKPTR(VM* vm, void* ptr) {
    Closure* cl = allocate(sizeof(Closure), 0);
    SETTY(cl, CT_PTR);
    cl -> info.ptr = ptr;
    return cl;
}

VAL MKMPTR(VM* vm, void* ptr, size_t size) {
    Closure* cl = allocate(sizeof(Closure) +
                           sizeof(ManagedPtr) + size, 0);
    SETTY(cl, CT_MANAGEDPTR);
    cl->info.mptr = (ManagedPtr*)((char*)cl + sizeof(Closure));
    cl->info.mptr->data = (char*)cl + sizeof(Closure) + sizeof(ManagedPtr);
    memcpy(cl->info.mptr->data, ptr, size);
    cl->info.mptr->size = size;
    return cl;
}

VAL MKFLOATc(VM* vm, double val) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, CT_FLOAT);
    cl -> info.f = val;
    return cl;
}

VAL MKSTRc(VM* vm, char* str) {
    Closure* cl = allocate(sizeof(Closure) + // Type) + sizeof(char*) +
                           sizeof(char)*strlen(str)+1, 1);
    SETTY(cl, CT_STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);

    strcpy(cl -> info.str, str);
    return cl;
}

VAL MKPTRc(VM* vm, void* ptr) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, CT_PTR);
    cl -> info.ptr = ptr;
    return cl;
}

VAL MKMPTRc(VM* vm, void* ptr, size_t size) {
    Closure* cl = allocate(sizeof(Closure) +
                           sizeof(ManagedPtr) + size, 1);
    SETTY(cl, CT_MANAGEDPTR);
    cl->info.mptr = (ManagedPtr*)((char*)cl + sizeof(Closure));
    cl->info.mptr->data = (char*)cl + sizeof(Closure) + sizeof(ManagedPtr);
    memcpy(cl->info.mptr->data, ptr, size);
    cl->info.mptr->size = size;
    return cl;
}

VAL MKB8(VM* vm, uint8_t bits8) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, CT_BITS8);
    cl -> info.bits8 = bits8;
    return cl;
}

VAL MKB16(VM* vm, uint16_t bits16) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, CT_BITS16);
    cl -> info.bits16 = bits16;
    return cl;
}

VAL MKB32(VM* vm, uint32_t bits32) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, CT_BITS32);
    cl -> info.bits32 = bits32;
    return cl;
}

VAL MKB64(VM* vm, uint64_t bits64) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, CT_BITS64);
    cl -> info.bits64 = bits64;
    return cl;
}

void dumpStack(VM* vm) {
    int i = 0;
    VAL* root;

    for (root = vm->valstack; root < vm->valstack_top; ++root, ++i) {
        printf("%d: ", i);
        dumpVal(*root);
        if (*root >= (VAL)(vm->heap.heap) && *root < (VAL)(vm->heap.end)) { printf("OK"); }
        printf("\n");
    }
    printf("RET: ");
    dumpVal(vm->ret);
    printf("\n");
}

void dumpVal(VAL v) {
    if (v==NULL) return;
    int i;
    if (ISINT(v)) {
        printf("%d ", (int)(GETINT(v)));
        return;
    }
    switch(GETTY(v)) {
    case CT_CON:
        printf("%d[", TAG(v));
        for(i = 0; i < ARITY(v); ++i) {
            dumpVal(v->info.c.args[i]);
        }
        printf("] ");
        break;
    case CT_STRING:
        printf("STR[%s]", v->info.str);
        break;
    case CT_FWD:
        printf("CT_FWD ");
        dumpVal((VAL)(v->info.ptr));
        break;
    default:
        printf("val");
    }

}

void idris_memset(void* ptr, i_int offset, uint8_t c, i_int size) {
    memset(((uint8_t*)ptr) + offset, c, size);
}

uint8_t idris_peek(void* ptr, i_int offset) {
    return *(((uint8_t*)ptr) + offset);
}

void idris_poke(void* ptr, i_int offset, uint8_t data) {
    *(((uint8_t*)ptr) + offset) = data;
}


VAL idris_peekPtr(VM* vm, VAL ptr, VAL offset) {
    void** addr = GETPTR(ptr) + GETINT(offset);
    return MKPTR(vm, *addr);
}

VAL idris_pokePtr(VAL ptr, VAL offset, VAL data) {
    void** addr = GETPTR(ptr) + GETINT(offset);
    *addr = GETPTR(data);
    return MKINT(0);
}

VAL idris_peekDouble(VM* vm, VAL ptr, VAL offset) {
    return MKFLOAT(vm, *(double*)(GETPTR(ptr) + GETINT(offset)));
}

VAL idris_pokeDouble(VAL ptr, VAL offset, VAL data) {
    *(double*)(GETPTR(ptr) + GETINT(offset)) = GETFLOAT(data);
    return MKINT(0);
}

VAL idris_peekSingle(VM* vm, VAL ptr, VAL offset) {
    return MKFLOAT(vm, *(float*)(GETPTR(ptr) + GETINT(offset)));
}

VAL idris_pokeSingle(VAL ptr, VAL offset, VAL data) {
    *(float*)(GETPTR(ptr) + GETINT(offset)) = GETFLOAT(data);
    return MKINT(0);
}

void idris_memmove(void* dest, void* src, i_int dest_offset, i_int src_offset, i_int size) {
    memmove(dest + dest_offset, src + src_offset, size);
}

VAL idris_castIntStr(VM* vm, VAL i) {
    int x = (int) GETINT(i);
    Closure* cl = allocate(sizeof(Closure) + sizeof(char)*16, 0);
    SETTY(cl, CT_STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    sprintf(cl -> info.str, "%d", x);
    return cl;
}

VAL idris_castBitsStr(VM* vm, VAL i) {
    Closure* cl;
    ClosureType ty = i->ty;

    switch (ty) {
    case CT_BITS8:
        // max length 8 bit unsigned int str 3 chars (256)
        cl = allocate(sizeof(Closure) + sizeof(char)*4, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu8, (uint8_t)i->info.bits8);
        break;
    case CT_BITS16:
        // max length 16 bit unsigned int str 5 chars (65,535)
        cl = allocate(sizeof(Closure) + sizeof(char)*6, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu16, (uint16_t)i->info.bits16);
        break;
    case CT_BITS32:
        // max length 32 bit unsigned int str 10 chars (4,294,967,295)
        cl = allocate(sizeof(Closure) + sizeof(char)*11, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu32, (uint32_t)i->info.bits32);
        break;
    case CT_BITS64:
        // max length 64 bit unsigned int str 20 chars (18,446,744,073,709,551,615)
        cl = allocate(sizeof(Closure) + sizeof(char)*21, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu64, (uint64_t)i->info.bits64);
        break;
    default:
        fprintf(stderr, "Fatal Error: ClosureType %d, not an integer type", ty);
        exit(EXIT_FAILURE);
    }

    SETTY(cl, CT_STRING);
    return cl;
}

VAL idris_castStrInt(VM* vm, VAL i) {
    char *end;
    i_int v = strtol(GETSTR(i), &end, 10);
    if (*end == '\0' || *end == '\n' || *end == '\r')
        return MKINT(v);
    else
        return MKINT(0);
}

VAL idris_castFloatStr(VM* vm, VAL i) {
    Closure* cl = allocate(sizeof(Closure) + sizeof(char)*32, 0);
    SETTY(cl, CT_STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    snprintf(cl -> info.str, 32, "%.16g", GETFLOAT(i));
    return cl;
}

VAL idris_castStrFloat(VM* vm, VAL i) {
    return MKFLOAT(vm, strtod(GETSTR(i), NULL));
}

VAL idris_concat(VM* vm, VAL l, VAL r) {
    char *rs = GETSTR(r);
    char *ls = GETSTR(l);
    // dumpVal(l);
    // printf("\n");
    Closure* cl = allocate(sizeof(Closure) + strlen(ls) + strlen(rs) + 1, 0);
    SETTY(cl, CT_STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    strcpy(cl -> info.str, ls);
    strcat(cl -> info.str, rs);
    return cl;
}

VAL idris_strlt(VM* vm, VAL l, VAL r) {
    char *ls = GETSTR(l);
    char *rs = GETSTR(r);

    return MKINT((i_int)(strcmp(ls, rs) < 0));
}

VAL idris_streq(VM* vm, VAL l, VAL r) {
    char *ls = GETSTR(l);
    char *rs = GETSTR(r);

    return MKINT((i_int)(strcmp(ls, rs) == 0));
}

VAL idris_strlen(VM* vm, VAL l) {
    return MKINT((i_int)(idris_utf8_strlen(GETSTR(l))));
}

VAL idris_readStr(VM* vm, FILE* h) {
    VAL ret;
    char *buffer = NULL;
    size_t n = 0;
    ssize_t len;
    len = getline(&buffer, &n, h);
    if (len <= 0) {
        ret = MKSTR(vm, "");
    } else {
        ret = MKSTR(vm, buffer);
    }
    free(buffer);
    return ret;
}

VAL idris_strHead(VM* vm, VAL str) {
    return idris_strIndex(vm, str, 0);
}

VAL MKSTROFFc(VM* vm, StrOffset* off) {
    Closure* cl = allocate(sizeof(Closure) + sizeof(StrOffset), 1);
    SETTY(cl, CT_STROFFSET);
    cl->info.str_offset = (StrOffset*)((char*)cl + sizeof(Closure));

    cl->info.str_offset->str = off->str;
    cl->info.str_offset->offset = off->offset;

    return cl;
}

VAL idris_strTail(VM* vm, VAL str) {
    // If there's no room, just copy the string, or we'll have a problem after
    // gc moves str
    if (space(vm, sizeof(Closure) + sizeof(StrOffset))) {
        Closure* cl = allocate(sizeof(Closure) + sizeof(StrOffset), 0);
        SETTY(cl, CT_STROFFSET);
        cl->info.str_offset = (StrOffset*)((char*)cl + sizeof(Closure));

        int offset = 0;
        VAL root = str;

        while(root!=NULL && !ISSTR(root)) { // find the root, carry on.
                              // In theory, at most one step here!
            offset += root->info.str_offset->offset;
            root = root->info.str_offset->str;
        }

        cl->info.str_offset->str = root;
        cl->info.str_offset->offset = offset+idris_utf8_charlen(GETSTR(str));

        return cl;
    } else {
        char* nstr = GETSTR(str);
        return MKSTR(vm, nstr+idris_utf8_charlen(nstr));
    }
}

VAL idris_strCons(VM* vm, VAL x, VAL xs) {
    char *xstr = GETSTR(xs);
    int xval = GETINT(x);
    if ((xval & 0x80) == 0) { // ASCII char
        Closure* cl = allocate(sizeof(Closure) +
                               strlen(xstr) + 2, 0);
        SETTY(cl, CT_STRING);
        cl -> info.str = (char*)cl + sizeof(Closure);
        cl -> info.str[0] = (char)(GETINT(x));
        strcpy(cl -> info.str+1, xstr);
        return cl;
    } else {
        char *init = idris_utf8_fromChar(xval);
        Closure* cl = allocate(sizeof(Closure) + strlen(init) + strlen(xstr) + 1, 0);
        SETTY(cl, CT_STRING);
        cl -> info.str = (char*)cl + sizeof(Closure);
        strcpy(cl -> info.str, init);
        strcat(cl -> info.str, xstr);
        free(init);
        return cl;
    }
}

VAL idris_strIndex(VM* vm, VAL str, VAL i) {
    int idx = idris_utf8_index(GETSTR(str), GETINT(i));
    return MKINT((i_int)idx);
}

VAL idris_substr(VM* vm, VAL offset, VAL length, VAL str) {
    char *start = idris_utf8_advance(GETSTR(str), GETINT(offset));
    char *end = idris_utf8_advance(start, GETINT(length));
    Closure* newstr = allocate(sizeof(Closure) + (end - start) +1, 0);
    SETTY(newstr, CT_STRING);
    newstr -> info.str = (char*)newstr + sizeof(Closure);
    memcpy(newstr -> info.str, start, end - start);
    *(newstr -> info.str + (end - start) + 1) = '\0';
    return newstr;
}

VAL idris_strRev(VM* vm, VAL str) {
    char *xstr = GETSTR(str);
    Closure* cl = allocate(sizeof(Closure) +
                           strlen(xstr) + 1, 0);
    SETTY(cl, CT_STRING);
    cl->info.str = (char*)cl + sizeof(Closure);
    idris_utf8_rev(xstr, cl->info.str);
    return cl;
}

VAL idris_systemInfo(VM* vm, VAL index) {
    int i = GETINT(index);
    switch(i) {
        case 0: // backend
            return MKSTR(vm, "c");
        case 1:
            return MKSTR(vm, IDRIS_TARGET_OS);
        case 2:
            return MKSTR(vm, IDRIS_TARGET_TRIPLE);
    }
    return MKSTR(vm, "");
}

typedef struct {
    VM* vm; // thread's VM
    VM* callvm; // calling thread's VM
    func fn;
    VAL arg;
} ThreadData;

#ifdef HAS_PTHREAD
void* runThread(void* arg) {
    ThreadData* td = (ThreadData*)arg;
    VM* vm = td->vm;
    VM* callvm = td->callvm;

    init_threaddata(vm);

    TOP(0) = td->arg;
    BASETOP(0);
    ADDTOP(1);
    td->fn(vm, NULL);
    callvm->processes--;

    free(td);

    //    Stats stats =
    terminate(vm);
    //    aggregate_stats(&(td->vm->stats), &stats);
    return NULL;
}

void* vmThread(VM* callvm, func f, VAL arg) {
    VM* vm = init_vm(callvm->stack_max - callvm->valstack, callvm->heap.size,
                     callvm->max_threads);
    vm->processes=1; // since it can send and receive messages
    pthread_t t;
    pthread_attr_t attr;
//    size_t stacksize;

    pthread_attr_init(&attr);
//    pthread_attr_getstacksize (&attr, &stacksize);
//    pthread_attr_setstacksize (&attr, stacksize*64);

    ThreadData *td = malloc(sizeof(ThreadData));
    td->vm = vm;
    td->callvm = callvm;
    td->fn = f;
    td->arg = copyTo(vm, arg);

    callvm->processes++;

    pthread_create(&t, &attr, runThread, td);
//    usleep(100);
    return vm;
}

// VM is assumed to be a different vm from the one x lives on

VAL doCopyTo(VM* vm, VAL x) {
    int i, ar;
    VAL* argptr;
    Closure* cl;
    if (x==NULL || ISINT(x)) {
        return x;
    }
    switch(GETTY(x)) {
    case CT_CON:
        ar = CARITY(x);
        if (ar == 0 && CTAG(x) < 256) { // globally allocated
            cl = x;
        } else {
            allocCon(cl, vm, CTAG(x), ar, 1);

            argptr = (VAL*)(cl->info.c.args);
            for(i = 0; i < ar; ++i) {
                *argptr = doCopyTo(vm, *((VAL*)(x->info.c.args)+i)); // recursive version
                argptr++;
            }
        }
        break;
    case CT_FLOAT:
        cl = MKFLOATc(vm, x->info.f);
        break;
    case CT_STRING:
        cl = MKSTRc(vm, x->info.str);
        break;
    case CT_BIGINT:
        cl = MKBIGMc(vm, x->info.ptr);
        break;
    case CT_PTR:
        cl = MKPTRc(vm, x->info.ptr);
        break;
    case CT_MANAGEDPTR:
        cl = MKMPTRc(vm, x->info.mptr->data, x->info.mptr->size);
        break;
    case CT_BITS8:
        cl = idris_b8CopyForGC(vm, x);
        break;
    case CT_BITS16:
        cl = idris_b16CopyForGC(vm, x);
        break;
    case CT_BITS32:
        cl = idris_b32CopyForGC(vm, x);
        break;
    case CT_BITS64:
        cl = idris_b64CopyForGC(vm, x);
        break;
    case CT_RAWDATA:
        {
            size_t size = x->info.size + sizeof(Closure);
            cl = allocate(size, 0);
            memcpy(cl, x, size);
        }
        break;
    default:
        assert(0); // We're in trouble if this happens...
    }
    return cl;
}

VAL copyTo(VM* vm, VAL x) {
    VM* current = pthread_getspecific(vm_key);
    pthread_setspecific(vm_key, vm);
    VAL ret = doCopyTo(vm, x);
    pthread_setspecific(vm_key, current);
    return ret;
}

// Add a message to another VM's message queue
int idris_sendMessage(VM* sender, VM* dest, VAL msg) {
    // FIXME: If GC kicks in in the middle of the copy, we're in trouble.
    // Probably best check there is enough room in advance. (How?)

    // Also a problem if we're allocating at the same time as the
    // destination thread (which is very likely)
    // Should the inbox be a different memory space?

    // So: we try to copy, if a collection happens, we do the copy again
    // under the assumption there's enough space this time.

    if (dest->active == 0) { return 0; } // No VM to send to

    int gcs = dest->stats.collections;
    pthread_mutex_lock(&dest->alloc_lock);
    VAL dmsg = copyTo(dest, msg);
    pthread_mutex_unlock(&dest->alloc_lock);

    if (dest->stats.collections > gcs) {
        // a collection will have invalidated the copy
        pthread_mutex_lock(&dest->alloc_lock);
        dmsg = copyTo(dest, msg); // try again now there's room...
        pthread_mutex_unlock(&dest->alloc_lock);
    }

    pthread_mutex_lock(&(dest->inbox_lock));

    if (dest->inbox_write >= dest->inbox_end) {
        // FIXME: This is obviously bad in the long run. This should
        // either block, make the inbox bigger, or return an error code,
        // or possibly make it user configurable
        fprintf(stderr, "Inbox full");
        exit(-1);
    }

    dest->inbox_write->msg = dmsg;
    dest->inbox_write->sender = sender;
    dest->inbox_write++;

    // Wake up the other thread
    pthread_mutex_lock(&dest->inbox_block);
    pthread_cond_signal(&dest->inbox_waiting);
    pthread_mutex_unlock(&dest->inbox_block);

//    printf("Sending [signalled]...\n");

    pthread_mutex_unlock(&(dest->inbox_lock));
//    printf("Sending [unlock]...\n");
    return 1;
}

VM* idris_checkMessages(VM* vm) {
    return idris_checkMessagesFrom(vm, NULL);
}

VM* idris_checkMessagesFrom(VM* vm, VM* sender) {
    Msg* msg;

    for (msg = vm->inbox; msg < vm->inbox_end && msg->msg != NULL; ++msg) {
        if (sender == NULL || msg->sender == sender) {
            return msg->sender;
        }
    }
    return 0;
}

VM* idris_checkMessagesTimeout(VM* vm, int delay) {
    VM* sender = idris_checkMessagesFrom(vm, NULL);
    if (sender != NULL) {
        return sender;
    }

    struct timespec timeout;
    int status;

    // Wait either for a timeout or until we get a signal that a message
    // has arrived.
    pthread_mutex_lock(&vm->inbox_block);
    timeout.tv_sec = time (NULL) + delay;
    timeout.tv_nsec = 0;
    status = pthread_cond_timedwait(&vm->inbox_waiting, &vm->inbox_block,
                               &timeout);
    (void)(status); //don't emit 'unused' warning
    pthread_mutex_unlock(&vm->inbox_block);

    return idris_checkMessagesFrom(vm, NULL);
}


Msg* idris_getMessageFrom(VM* vm, VM* sender) {
    Msg* msg;

    for (msg = vm->inbox; msg < vm->inbox_write; ++msg) {
        if (sender == NULL || msg->sender == sender) {
            return msg;
        }
    }
    return NULL;
}

// block until there is a message in the queue
Msg* idris_recvMessage(VM* vm) {
    return idris_recvMessageFrom(vm, NULL);
}

Msg* idris_recvMessageFrom(VM* vm, VM* sender) {
    Msg* msg;
    Msg* ret = malloc(sizeof(Msg));

    struct timespec timeout;
    int status;

    pthread_mutex_lock(&vm->inbox_block);
    msg = idris_getMessageFrom(vm, sender);

    while (msg == NULL) {
//        printf("No message yet\n");
//        printf("Waiting [lock]...\n");
        timeout.tv_sec = time (NULL) + 3;
        timeout.tv_nsec = 0;
        status = pthread_cond_timedwait(&vm->inbox_waiting, &vm->inbox_block,
                               &timeout);
        (void)(status); //don't emit 'unused' warning
//        printf("Waiting [unlock]... %d\n", status);
        msg = idris_getMessageFrom(vm, sender);
    }
    pthread_mutex_unlock(&vm->inbox_block);

    if (msg != NULL) {
        ret->msg = msg->msg;
        ret->sender = msg->sender;

        pthread_mutex_lock(&(vm->inbox_lock));

        // Slide everything down after the message in the inbox,
        // Move the inbox_write pointer down, and clear the value at the
        // end - O(n) but it's easier since the message from a specific
        // sender could be anywhere in the inbox

        for(;msg < vm->inbox_write; ++msg) {
            if (msg+1 != vm->inbox_end) {
                msg->sender = (msg + 1)->sender;
                msg->msg = (msg + 1)->msg;
            }
        }

        vm->inbox_write->msg = NULL;
        vm->inbox_write->sender = NULL;
        vm->inbox_write--;

        pthread_mutex_unlock(&(vm->inbox_lock));
    } else {
        fprintf(stderr, "No messages waiting");
        exit(-1);
    }

    return ret;
}
#endif

VAL idris_getMsg(Msg* msg) {
    return msg->msg;
}

VM* idris_getSender(Msg* msg) {
    return msg->sender;
}

void idris_freeMsg(Msg* msg) {
    free(msg);
}

int idris_errno() {
    return errno;
}

char* idris_showerror(int err) {
    return strerror(err);
}

VAL* nullary_cons;

void init_nullaries() {
    int i;
    VAL cl;
    nullary_cons = malloc(256 * sizeof(VAL));
    for(i = 0; i < 256; ++i) {
        cl = malloc(sizeof(Closure));
        SETTY(cl, CT_CON);
        cl->info.c.tag_arity = i << 8;
        nullary_cons[i] = cl;
    }
}

void free_nullaries() {
    int i;
    for(i = 0; i < 256; ++i) {
        free(nullary_cons[i]);
    }
    free(nullary_cons);
}

int __idris_argc;
char **__idris_argv;

int idris_numArgs() {
    return __idris_argc;
}

const char* idris_getArg(int i) {
    return __idris_argv[i];
}

void stackOverflow() {
  fprintf(stderr, "Stack overflow");
  exit(-1);
}
