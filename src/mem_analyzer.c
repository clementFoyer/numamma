#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <string.h>
#include <execinfo.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <gelf.h>
#include <stddef.h>

#include "mem_intercept.h"
#include "mem_analyzer.h"
#include "mem_tools.h"
#include "mem_sampling.h"

#define USE_HASHTABLE
#define WARN_NON_FREED 0

#ifdef USE_HASHTABLE
#include "hash.h"
typedef struct ht_node* mem_info_node_t;
#else
struct memory_info_list {
  struct memory_info_list* next;
  struct memory_info_list* prev;
  struct memory_info mem_info;
};
typedef struct memory_info_list* mem_info_node_t;
#endif

static mem_info_node_t mem_list = NULL; // malloc'd buffers currently in use
static mem_info_node_t past_mem_list = NULL; // malloc'd buffers that were freed
static pthread_mutex_t mem_list_lock;

extern struct mem_counters global_counters[2];

__thread unsigned thread_rank;
unsigned next_thread_rank = 0;
#define PROGRAM_FILE_LEN 4096 // used for readlink cmd
static char program_file[PROGRAM_FILE_LEN];

static __thread int is_record_safe = 1;
#define IS_RECORD_SAFE (is_record_safe)

#define PROTECT_RECORD do {			\
    assert(is_record_safe !=0);			\
    is_record_safe = 0;				\
  } while(0)

#define UNPROTECT_RECORD do {			\
    assert(is_record_safe == 0);		\
    is_record_safe = 1;				\
  } while(0)

__thread struct mem_allocator* mem_info_allocator = NULL;
struct mem_allocator* string_allocator = NULL;

__thread struct tick tick_array[NTICKS];

date_t origin_date;
#define DATE(d) ((d)-origin_date)

/* todo:
 * - set an alarm every 1ms to collect the sampling info
 * - choose the buffer size
 * - collect read/write accesses
 */
void ma_init() {
  PROTECT_RECORD;
  pthread_mutex_init(&mem_list_lock, NULL);
  origin_date = new_date();

  mem_allocator_init(&string_allocator,
		     sizeof(char)*1024,
		     16*1024);

  mem_sampling_init();
  ma_thread_init();
  UNPROTECT_RECORD;
}

void ma_thread_init() {
  thread_rank = __sync_fetch_and_add( &next_thread_rank, 1 );

#ifdef USE_HASHTABLE
  mem_allocator_init(&mem_info_allocator,
		     sizeof(struct memory_info),
		     16*1024);
#else
  mem_allocator_init(&mem_info_allocator,
		     sizeof(struct memory_info_list),
		     16*1024);
#endif

  for(int i=0; i<NTICKS; i++) {
    init_tick(i);
  }

  mem_sampling_thread_init();
}

void ma_thread_finalize() {
  PROTECT_RECORD;

  mem_sampling_thread_finalize();

  pid_t tid = syscall(SYS_gettid);
#if  ENABLE_TICKS
  if(settings.verbose) {
    printf("End of thread %s %d\n", __FUNCTION__, tid);
    for(int i=0; i<NTICKS; i++) {
      if(tick_array[i].nb_calls>0) {
	double total_duration = tick_array[i].total_duration;
	double avg_duration = total_duration / tick_array[i].nb_calls;
	printf("tick[%d] : %s -- %d calls. %lf us per call (total: %lf ms)\n",
	       i, tick_array[i].tick_name, tick_array[i].nb_calls,
	       avg_duration/1e3, total_duration/1e6);
      }
    }
  }
#endif
  UNPROTECT_RECORD;
}

static
int is_address_in_buffer(uint64_t addr, struct memory_info *buffer){
  void* addr_ptr = (void*)addr;
  if(buffer->buffer_addr <= addr_ptr &&
     addr_ptr < buffer->buffer_addr + buffer->buffer_size)
    return 1;
  return 0;
}

static
int is_sample_in_buffer(struct mem_sample *sample, struct memory_info *buffer){
  void* addr_ptr = (void*)sample->addr;
  if(buffer->buffer_addr <= addr_ptr &&
     addr_ptr < buffer->buffer_addr + buffer->buffer_size) {
    /* address matches */
    return 1;
    if(buffer->alloc_date <=sample->timestamp &&
       sample->timestamp <= buffer->free_date) {
      /* timestamp matches */
      return 1;
    }
  }
  return 0;
}

void ma_print_mem_info(FILE*f, struct memory_info *mem) {
  if(mem) {
    if(!mem->caller) {
      mem->caller = get_caller_function_from_rip(mem->caller_rip);
    }

    char callstack_rip_str[1024];
    callstack_rip_str[0] = '\0';
    if(mem->callstack_rip) {
        for(int i = 3; i < mem->callstack_size; i++) {
            char cur_str[16];
            if(i == 3) {
                sprintf(cur_str, "0x%"PRIx64"", mem->callstack_rip[i]);
            } else {
                sprintf(cur_str, ",0x%"PRIx64"", mem->callstack_rip[i]);
            }        
            strcat(callstack_rip_str, cur_str);
        }
    } else {
        strcat(callstack_rip_str, "NULL");
    }

    fprintf(f, "mem %p = {.addr=0x%"PRIx64", .alloc_date=%" PRIu64 ", .free_date=%" PRIu64 ", size=%ld, callstack_rip=%s, alloc_site=%p / %s}\n", mem,
	   mem->buffer_addr, mem->alloc_date?DATE(mem->alloc_date):0, mem->free_date?DATE(mem->free_date):0,
	   mem->buffer_size, callstack_rip_str, mem->caller_rip, mem->caller?mem->caller:"");
  }
}

static void __ma_print_buffers_generic(FILE*f, mem_info_node_t list) {
#ifdef USE_HASHTABLE
  /* todo */
  struct ht_node*p_node = NULL;
  FOREACH_HASH(mem_list, p_node) {
    struct ht_entry*e = p_node->entries;
    while(e) {
      struct memory_info* mem_info = e->value;
      ma_print_mem_info(f, mem_info);
      e = e->next;
    }

  }
#else
  struct memory_info_list * p_node = list;
  while(p_node) {
    ma_print_mem_info(f, &p_node->mem_info);
    p_node = p_node->next;
  }
#endif
}

void ma_print_current_buffers() {
  __ma_print_buffers_generic(stdout, mem_list);
}

void ma_print_past_buffers() {
  __ma_print_buffers_generic(stdout, past_mem_list);
}

static mem_info_node_t
__ma_find_mem_info_from_addr_generic(mem_info_node_t list,
				     uint64_t ptr) {
  mem_info_node_t retval = NULL;
  int n=0;
  pthread_mutex_lock(&mem_list_lock);
#ifdef USE_HASHTABLE
  mem_info_node_t p_node =  ht_lower_key(list, ptr);
  if(p_node) {
    struct ht_entry*e = p_node->entries;
    while(e) {
      if(is_address_in_buffer(ptr, e->value)) {
	retval = p_node;
      }
      e = e->next;
    }
  }
#else
  struct memory_info_list * p_node = list;
  while(p_node) {
    if(is_address_in_buffer(ptr, &p_node->mem_info)) {
      retval = p_node;
      goto out;
    }
    n++;
    p_node = p_node->next;
  }
#endif

 out:
  if(n > 100) {
    printf("%s: %d buffers\n", __FUNCTION__, n);
  }
  pthread_mutex_unlock(&mem_list_lock);
  return retval;
}

static struct memory_info*
__ma_find_mem_info_from_sample_generic(mem_info_node_t list,
				       struct mem_sample *sample) {
  struct memory_info* retval = NULL;
  int n=0;
  pthread_mutex_lock(&mem_list_lock);
#ifdef USE_HASHTABLE
  mem_info_node_t p_node =  ht_lower_key(list, sample->addr);
  if(p_node) {
    struct ht_entry*e = p_node->entries;
    while(e) {
      struct memory_info*val = e->value;   
      if(is_sample_in_buffer(sample, e->value)) {
	retval = e->value;
	goto out;
      }
      e = e->next;
    }
  }
#else
  struct memory_info_list * p_node = list;
  while(p_node) {
    if(is_sample_in_buffer(sample, &p_node->mem_info)) {
      retval = &p_node->mem_info;
      goto out;
    }
    n++;
    p_node = p_node->next;
  }
#endif

 out:
  if(n > 100) {
    printf("%s: %d buffers\n", __FUNCTION__, n);
  }
  pthread_mutex_unlock(&mem_list_lock);
  return retval;
}


struct memory_info*
ma_find_mem_info_from_addr(uint64_t ptr) {
  /* todo: a virer */
  mem_info_node_t ret = __ma_find_mem_info_from_addr_generic(mem_list, ptr);
  if(ret) {
#ifdef USE_HASHTABLE
    return ret->entries->value;
#else
    return &ret->mem_info;
#endif
  }
  return NULL;
}

struct memory_info*
ma_find_mem_info_from_sample(struct mem_sample* sample) {
  return __ma_find_mem_info_from_sample_generic(mem_list, sample);
}

uint64_t avg_pos = 0;

static mem_info_node_t
__ma_find_mem_info_in_list(mem_info_node_t *list,
			   uint64_t ptr,
			   date_t start_date,
			   date_t stop_date) {
#ifdef USE_HASHTABLE
  fprintf(stderr, "%s not implemented\n", __FUNCTION__);
  return NULL;
#else
  mem_info_node_t retval = NULL;
  int n=0;
  pthread_mutex_lock(&mem_list_lock);
  struct memory_info_list * p_node = *list;
  uint64_t pos = 0;
  while(p_node) {
    if(is_address_in_buffer(ptr, &p_node->mem_info)) {
      if((! p_node->mem_info.alloc_date) /* the buffer is a global variable */
	 ||
	 (p_node->mem_info.alloc_date >= start_date &&
	  p_node->mem_info.free_date <= stop_date)  /* the access happened during the lifespan
						       of the buffer */
	 ||
	 (start_date >= p_node->mem_info.alloc_date &&
	  start_date <= p_node->mem_info.free_date) /* the variable was allocated during the
						       range of the memory access */
	 ||
	 (stop_date >= p_node->mem_info.alloc_date &&
	  stop_date <= p_node->mem_info.free_date)  /* the variable was freed during the
						       range of the memory access */
	 ) {

	/* the buffer existed during the timeframe. It may have been allocated or
	 * freed during the timeframe, but let's say we found a hit
	 */

	retval = p_node;
	goto out;
      } else {
	printf("When searching for %p (%" PRIu64 "-%" PRIu64 "), found %p (%" PRIu64 "-%" PRIu64 ")\n",
	       (void*)ptr, DATE(start_date), DATE(stop_date),
	       p_node->mem_info.buffer_addr, DATE(p_node->mem_info.alloc_date),
	       DATE(p_node->mem_info.free_date));
      }
    }
    n++;
    p_node = p_node->next;
    pos++;
  }

 out:
  pthread_mutex_unlock(&mem_list_lock);
  avg_pos += pos;
  return retval;
#endif
}

/* search for a buffer that contains address ptr
 * the memory access occured between start_date and stop_date
 */
struct memory_info*
ma_find_past_mem_info_from_addr(uint64_t ptr,
				date_t start_date,
				date_t stop_date) {
  /* todo: a virer */
#ifdef USE_HASHTABLE
  mem_info_node_t ret = __ma_find_mem_info_from_addr_generic(past_mem_list, ptr);
#else
  mem_info_node_t ret = __ma_find_mem_info_in_list(&past_mem_list, ptr, start_date, stop_date);
#endif

  if(ret) {
    struct memory_info* retval = NULL;
#ifdef USE_HASHTABLE
    retval = ret->entries->value;
    if((retval->alloc_date >= start_date &&
	retval->alloc_date <= stop_date) ||
       (retval->free_date >= start_date &&
	retval->free_date <= stop_date))    
#else
      retval = &ret->mem_info;
    if(1) 
#endif
      {
	/* the buffer existed during the timeframe. It may have been allocated or
	 * freed during the timeframe, but let's say we found a hit
	 */
	return retval;
      } else {
        printf("When searching for %p (%" PRIu64 "-%" PRIu64 "), found %p (%" PRIu64 "-%" PRIu64 ")\n",
	       (void*)ptr, DATE(start_date), DATE(stop_date),
	     retval->buffer_addr, DATE(retval->alloc_date), DATE(retval->free_date));
    }
  }
  return NULL;
}

static void __allocate_counters(struct memory_info* mem_info) {
  mem_info->blocks = malloc(sizeof(struct block_info*) * MAX_THREADS);
  for(int i=0; i<MAX_THREADS; i++) {
    mem_info->blocks[i] = malloc(sizeof(struct block_info));
    mem_info->blocks[i]->block_id = 0;
    mem_info->blocks[i]->next = 0;
  }
}

#define INIT_COUNTER(c) do {		\
    c.count = 0;				\
    c.min_weight = UINT64_MAX;		\
    c.max_weight = 0;			\
    c.sum_weight = 0;			\
  } while(0)

/* initialize a mem_counters structure */
void init_mem_counter(struct mem_counters* counters) {
  counters->total_count = 0;
  counters->total_weight = 0;
  counters->na_miss_count = 0;

  INIT_COUNTER(counters->cache1_hit);
  INIT_COUNTER(counters->cache2_hit);
  INIT_COUNTER(counters->cache3_hit);
  INIT_COUNTER(counters->lfb_hit);
  INIT_COUNTER(counters->local_ram_hit);
  INIT_COUNTER(counters->remote_ram_hit);
  INIT_COUNTER(counters->remote_cache_hit);
  INIT_COUNTER(counters->io_memory_hit);
  INIT_COUNTER(counters->uncached_memory_hit);
  INIT_COUNTER(counters->cache1_miss);
  INIT_COUNTER(counters->cache2_miss);
  INIT_COUNTER(counters->cache3_miss);
  INIT_COUNTER(counters->lfb_miss);
  INIT_COUNTER(counters->local_ram_miss);
  INIT_COUNTER(counters->remote_ram_miss);
  INIT_COUNTER(counters->remote_cache_miss);
  INIT_COUNTER(counters->io_memory_miss);
  INIT_COUNTER(counters->uncached_memory_miss);
}

/* initialize the counters of a mem_info structure */
static void __init_counters(struct memory_info* mem_info) {
  int i, j;
  for(i=0; i<MAX_THREADS; i++) {
    struct block_info*block = mem_info->blocks[i];
    while(block) {
      for(j=0; j<ACCESS_MAX; j++) {
	init_mem_counter(&block->counters[j]);
      }
      block = block->next;
    }
  }
}

void ma_allocate_counters(struct memory_info* mem_info) {
  __allocate_counters(mem_info);
}

void ma_init_counters(struct memory_info* mem_info) {
  __init_counters(mem_info);
}


#define PAGE_SIZE 4096

/* return the block_info corresponding to page_no in a list of blocks */
struct block_info* __ma_search_block(struct block_info* block,
				     int page_no) {

  /* browse the list of block and search for page_no */
  while(block) {
    if(block->block_id == page_no) {
      return block;
    }
    if((! block->next) ||	/* we are on the last block */
       (block->next->block_id > page_no)) { /* the next block is too high  */
      return NULL;
    }
    block = block->next;
  }
  return NULL;
}

/* return the block_info corresponding to page_no in a list of blocks
 * if not found, this function allocates a new block and returns it
 */
struct block_info* __ma_get_block(struct block_info* block,
				  int page_no) {
  /* uncomment this to store all the memory accesses in a single block */
  //  return block;

  /* browse the list of block and search for page_no */
  while(block) {
    if(block->block_id == page_no) {
      return block;
    }
    if((! block->next) ||	/* we are on the last block */
       (block->next->block_id > page_no)) { /* the next block is too high  */
      /* insert a new block after the current block */
      struct block_info *new_block = malloc(sizeof(struct block_info));

      /* initialize the block */
      new_block->block_id = page_no;
      for(int j=0; j<ACCESS_MAX; j++) {
	init_mem_counter(&new_block->counters[j]);
      }

      /* enqueue it after block */
      new_block->next = block->next;
      block->next = new_block;
    }

    block = block->next;
  }
  return NULL;
}
/* return the block that contains ptr in a mem_info */
struct block_info* ma_get_block(struct memory_info* mem_info,
				int thread_rank,
				uintptr_t ptr) {
  assert(ptr <= ((uintptr_t)mem_info->buffer_addr) + mem_info->buffer_size);

  size_t offset = ptr - (uintptr_t)mem_info->buffer_addr;
  int page_no = offset / PAGE_SIZE;
  struct block_info* block = mem_info->blocks[thread_rank];
  return __ma_get_block(block, page_no);
}


static void _init_mem_info(struct memory_info* mem_info,
			   enum mem_type mem_type,
			   date_t alloc_date,
			   size_t initial_buffer_size,
			   void* buffer_addr,
               void** callstack_rip,
               int callstack_size,
			   void* caller_rip,
			   const char* caller) {

  mem_info->mem_type = mem_type;
  mem_info->alloc_date = alloc_date;
  mem_info->free_date = 0;
  mem_info->initial_buffer_size = initial_buffer_size;
  mem_info->buffer_size = initial_buffer_size;
  mem_info->buffer_addr = buffer_addr;
  mem_info->callstack_rip = callstack_rip;
  mem_info->callstack_size = callstack_size;
  mem_info->caller_rip = caller_rip;
  if(caller) {
    mem_info->caller = mem_allocator_alloc(string_allocator);
    snprintf(mem_info->caller, 1024, "%s", caller);
  } else {
    mem_info->caller = NULL;
  }

  mem_info->call_site = NULL;
  mem_info->blocks = NULL;

  static _Atomic int next_mem_info_id = 1;
  mem_info->id = next_mem_info_id++;

  if(settings.online_analysis) {
    __allocate_counters(mem_info);
    __init_counters(mem_info);
  }
}


char null_str[]="";

/* unset LD_PRELOAD
 * this makes sure that forked processes will not be analyzed
 */
extern void unset_ld_preload();

/* set LD_PRELOAD so that future forked processes are analyzed
 *  you need to call unset_ld_preload before calling this function
 */
extern void reset_ld_preload();

/* find the address range of the stack and add a mem_info record */
static void __ma_register_stack_range(uintptr_t stack_base_addr,
				      uintptr_t stack_end_addr) {
  size_t stack_size = stack_end_addr - stack_base_addr;

  debug_printf("Stack address range: %p-%p (stack size: %lu bytes)\n",
	       stack_base_addr, stack_end_addr, stack_size);

  /* create a mem_info record */
  struct memory_info * mem_info = NULL;
#ifdef USE_HASHTABLE
  mem_info = mem_allocator_alloc(mem_info_allocator);
#else
  struct memory_info_list * p_node = mem_allocator_alloc(mem_info_allocator);
  mem_info = &p_node->mem_info;
#endif

#if 0
  mem_info->mem_type=stack;
  mem_info->alloc_date = 0;
  mem_info->free_date = 0;
  mem_info->initial_buffer_size = stack_size;
  mem_info->buffer_size = stack_size;
  mem_info->buffer_addr = (void*)stack_base_addr;
  mem_info->caller = mem_allocator_alloc(string_allocator);
  snprintf(mem_info->caller, 1024, "[stack]");
  if(settings.online_analysis) {
    __allocate_counters(mem_info);
    __init_counters(mem_info);
  }
#else
  _init_mem_info(mem_info, stack, 0, stack_size, (void*)stack_base_addr, NULL, 0, NULL, "[stack]");
#endif

  pthread_mutex_lock(&mem_list_lock);
#ifdef USE_HASHTABLE
  mem_list = ht_insert(mem_list, (uint64_t) mem_info->buffer_addr, mem_info);
#else
  p_node->next = mem_list;
  p_node->prev = NULL;
  if(p_node->next)
    p_node->next->prev = p_node;
  mem_list = p_node;
#endif
  pthread_mutex_unlock(&mem_list_lock);
}

void ma_register_stack() {
  char cmd[4096];
  char line[4096];

  uintptr_t stack_base_addr= (uintptr_t)0x7fa000000000;
  uintptr_t stack_end_addr= (uintptr_t)0x7fffffffffff;
  __ma_register_stack_range(stack_base_addr, stack_end_addr);
  return;
  
  FILE* f=fopen("/proc/self/maps", "r");
  if(!f) {
    perror("fopen failed");
    abort();
  }
  while(fgets(line, 4096, f) != NULL) {
    /* extract start/end addresses */
    // each line is in the form:
    // <start_addr>-<end_addr> <permission> <offset> <device> <inode> <file>

    void *stack_base_addr = NULL;
    void *stack_end_addr = NULL;
    char permission[10];
    size_t offset=0;
    int device1;
    int device2;
    int inode;
    char file[4096];
      
    int nfields = sscanf(line, "%p-%p %s %x %x:%x %d %s",
		     &stack_base_addr, &stack_end_addr, permission, &offset,
		     &device1, &device2, &inode, file);
    if(nfields == 7 || (inode == 0 && strcmp(file, "[stack]")==0)) {
      if((uintptr_t)stack_base_addr > (uintptr_t)0x7f0000000000) {
	/* let's assume this is a stack region */
	printf("While reading '%s', found %d fields. inode=%d, file='%s'\n", line, nfields, inode, file);

	__ma_register_stack_range((uintptr_t)stack_base_addr, (uintptr_t)stack_end_addr);
      }
    }
  }
  fclose(f);
}


/* writes given information into a new memory_info struct and adds it to list or hashtable, and returns the inserted element */
struct memory_info* insert_memory_info(enum mem_type mem_type,
				       size_t initial_buffer_size,
				       void* buffer_addr,
				       const char* caller)
{
	struct memory_info* mem_info = NULL;
#ifdef USE_HASHTABLE
	mem_info = mem_allocator_alloc(mem_info_allocator);
#else
	struct memory_info_list * p_node = mem_allocator_alloc(mem_info_allocator);
	mem_info = &p_node->mem_info;
#endif

#if 0
	static _Atomic int next_mem_info_id = 1;
	mem_info->id = next_mem_info_id++;

	mem_info->mem_type = mem_type;
	mem_info->alloc_date = 0;
	mem_info->free_date = 0;
	mem_info->initial_buffer_size = initial_buffer_size;
	mem_info->buffer_size = mem_info->initial_buffer_size;

	mem_info->buffer_addr = buffer_addr;
	mem_info->caller = mem_allocator_alloc(string_allocator);
	snprintf(mem_info->caller, 1024, "%s", caller);

	if(settings.online_analysis) {
	  __allocate_counters(mem_info);
	  __init_counters(mem_info);
	}
#else
	_init_mem_info(mem_info, mem_type, 0, initial_buffer_size, buffer_addr, NULL, 0, NULL, caller);
#endif

	pthread_mutex_lock(&mem_list_lock);
#ifdef USE_HASHTABLE
	mem_list = ht_insert(mem_list, (uint64_t) mem_info->buffer_addr, mem_info);
#else
	/* todo: insert large buffers at the beginning of the list since
	 * their are more likely to be accessed often (this will speed
	 * up searching at runtime)
	 */
	p_node->next = mem_list;
	p_node->prev = NULL;
	if(p_node->next)
	  p_node->next->prev = p_node;
	mem_list = p_node;
#endif
	pthread_mutex_unlock(&mem_list_lock);
	return mem_info;
}

// a file can appear several times in maps, and thus we need to track the several ranges it has
struct maps_addr_ranges {
  uintptr_t addr_begin;
  uintptr_t addr_end;
  char permissions[16];
  uintptr_t offset;
  struct maps_addr_ranges* next;
};

struct maps_file_list {
  struct maps_addr_ranges *addr_ranges;
  char device[1024];
  uint64_t inode;
  char pathname[1024];
  struct maps_file_list *next;
};

// insert the maps file described in line into current_list, or create a new one if is NULL, and returns the new list
// the file appened may just add a new addr_range into current_list->maps_addr_ranges, or even just update an existing range
struct maps_file_list* insert_new_maps_file_from_line(char *line, struct maps_file_list* current_list)
{
  uintptr_t addr_begin;
  uintptr_t addr_end;
  char permissions[16];
  uintptr_t offset;
  char device[1024];
  uint64_t inode;
  char pathname[1024];

  sscanf(line, "%p-%p %s %p %s %ld %s\n", &addr_begin, &addr_end, permissions, &offset, device, &inode, pathname);
  if (pathname == NULL) return current_list;
  struct maps_file_list* current_file = current_list;
  while (current_file != NULL) {
    if (strcmp(current_file->pathname, pathname) == 0) {
      struct maps_addr_ranges* current_range = current_file->addr_ranges;
      do {
        if (strcmp(current_range->permissions, permissions) == 0 &&
            current_range->addr_begin == addr_begin &&
            current_range->addr_end == addr_end &&
            current_range->offset == offset) {
          return current_list;
        }
      } while ((current_range = current_range->next) != NULL);
      // this range has to be added on its own but is in a known file
      current_range = malloc(sizeof(struct maps_addr_ranges));
      current_range->addr_begin = addr_begin;
      current_range->addr_end = addr_end;
      current_range->offset = offset;
      strncpy(current_range->permissions, permissions, 16);
      current_range->next = current_file->addr_ranges;
      current_file->addr_ranges = current_range;
      return current_list;
    }
    current_file = current_file->next;
  }
  // the file is not in the list, add it
  struct maps_file_list* new_file;
  new_file = malloc(sizeof(struct maps_file_list));
  new_file->addr_ranges = malloc(sizeof(struct maps_addr_ranges));
  new_file->addr_ranges->addr_begin = addr_begin;
  new_file->addr_ranges->addr_end = addr_end;
  new_file->addr_ranges->offset = offset;
  new_file->addr_ranges->next = NULL;
  strncpy(new_file->addr_ranges->permissions, permissions, 16);
  strncpy(new_file->device, device, 1024);
  new_file->inode = inode;
  strncpy(new_file->pathname, pathname, 1024);
  new_file->next = current_list;
  return new_file;
}

void fprint_maps_file(FILE *f, struct maps_file_list* list) {
  fprintf(f, "%s :\n", list->pathname);
  fprintf(f, "\tdevice : %s\n", list->device);
  fprintf(f, "\tinode : %ld\n", list->inode);
  struct maps_addr_ranges* current_range = list->addr_ranges;
  do {
    fprintf(f, "\t%p-%p (%p) : %p : %s\n",
	    current_range->addr_begin,
	    current_range->addr_end,
	    (void*) ((size_t) current_range->addr_end - (size_t) current_range->addr_begin),
	    current_range->offset,
	    current_range->permissions);
  } while((current_range = current_range->next) != NULL);
}

void fprint_maps_file_list(FILE *f, struct maps_file_list* list) {
  while (list != NULL) {
    fprint_maps_file(f, list);
    list = list->next;
  }
}

void print_elf_header(GElf_Ehdr header) {
  printf("ehdr :\n");
  printf("\te_type : %d\n", header.e_type);
  printf("\te_machine : %d\n", header.e_machine);
  printf("\te_version : %d\n", header.e_version);
  printf("\te_entry : %d\n", header.e_entry);
  printf("\te_phoff : %d\n", header.e_phoff);
  printf("\te_shoff : %p\n", (void*)header.e_shoff);
  printf("\te_flags : %d\n", header.e_flags);
  printf("\te_ehsize : %d\n", header.e_ehsize);
  printf("\te_phentsize : %d\n", header.e_phentsize);
  printf("\te_phnum : %d\n", header.e_phnum);
  printf("\te_shentsize : %d\n", header.e_shentsize);
  printf("\te_shnum : %d\n", header.e_shnum);
  printf("\te_shstrndx : %d\n", header.e_shstrndx);
}

void print_section_header(GElf_Shdr shdr, const char* spacing)
{
  printf("%ssh_name : %d\n", spacing, shdr.sh_name);
  printf("%ssh_type : %d (", spacing, shdr.sh_type);
  switch(shdr.sh_type) {
    case SHT_NULL:
      printf("SHT_NULL");
      break;
    case SHT_PROGBITS:
      printf("SHT_PROGBITS");
      break;
    case SHT_SYMTAB:
      printf("SHT_SYMTAB");
      break;
    case SHT_STRTAB:
      printf("SHT_STRTAB");
      break;
    case SHT_RELA:
      printf("SHT_RELA");
      break;
    case SHT_HASH:
      printf("SHT_HASH");
      break;
    case SHT_DYNAMIC:
      printf("SHT_DYNAMIC");
      break;
    case SHT_NOTE:
      printf("SHT_NOTE");
      break;
    case SHT_NOBITS:
      printf("SHT_NOBITS");
      break;
    case SHT_REL:
      printf("SHT_REL");
      break;
    case SHT_SHLIB:
      printf("SHT_SHLIB");
      break;
    case SHT_DYNSYM:
      printf("SHT_DYNSYM");
      break;
    case SHT_INIT_ARRAY:
      printf("SHT_INIT_ARRAY");
      break;
    case SHT_FINI_ARRAY:
      printf("SHT_FINI_ARRAY");
      break;
    case SHT_PREINIT_ARRAY:
      printf("SHT_PREINIT_ARRAY");
      break;
    case SHT_GROUP:
      printf("SHT_GROUP");
      break;
    case SHT_SYMTAB_SHNDX:
      printf("SHT_SYMTAB_SHNDX");
      break;
    case  SHT_NUM:
      printf("SHT_NUM");
      break;
    case SHT_LOOS:
      printf("SHT_LOOS");
      break;
    case SHT_GNU_ATTRIBUTES:
      printf("SHT_GNU_ATTRIBUTES");
      break;
    case SHT_GNU_HASH:
      printf("SHT_GNU_HASH");
      break;
    case SHT_GNU_LIBLIST:
      printf("SHT_GNU_LIBLIST");
      break;
    case SHT_CHECKSUM:
      printf("SHT_CHECKSUM");
      break;
    case SHT_LOSUNW:
    //case SHT_SUNW_move:
      printf("SHT_LOSUNW or SHT_SUNW_move");
      break;
    case SHT_SUNW_COMDAT:
      printf("SHT_SUNW_COMDAT");
      break;
    case SHT_SUNW_syminfo:
      printf("SHT_SUNW_syminfo");
      break;
    case SHT_GNU_verdef:
      printf("SHT_GNU_verdef");
      break;
    case SHT_GNU_verneed:
      printf("SHT_GNU_verneed");
      break;
    case SHT_GNU_versym:
    //case SHT_HISUNW:
    //case SHT_HIOS:
      printf("SHT_GNU_versym or SHT_HIOS or SHT_HISUNW");
      break;
    case SHT_LOPROC:
      printf("SHT_LOPROC");
      break;
    case SHT_HIPROC:
      printf("SHT_HIPROC");
      break;
    case SHT_LOUSER:
      printf("SHT_LOUSER");
      break;
    case SHT_HIUSER:
      printf("SHT_HIUSER");
      break;
    default:
      printf("unknown");
      break;
  }
  printf(")\n");
  printf("%ssh_flags : %d\n", spacing, shdr.sh_flags);
  printf("%ssh_addr : %p\n", spacing, shdr.sh_addr);
  printf("%ssh_offset : %p\n", spacing, (void*)shdr.sh_offset);
  printf("%ssh_size : %d\n", spacing, shdr.sh_size);
  printf("%ssh_link : %d\n", spacing, shdr.sh_link);
  printf("%ssh_info : %d\n", spacing, shdr.sh_info);
  printf("%ssh_addralign : %d\n", spacing, shdr.sh_addralign);
  printf("%ssh_entsize : %d\n", spacing, shdr.sh_entsize);
}

static void __ma_parse_elf(struct maps_file_list maps_file) {
  elf_version(EV_CURRENT); // has to be called before elf_begin

  
  Elf *elf = NULL;
  GElf_Ehdr header;
  int fd = open(maps_file.pathname, O_RDONLY);
  if (fd == -1 && errno == ENOENT) {
    /* file does not exist. It's probably a pseudo-files like [stack] or [heap] */
    return;
  }

  if (fd == -1) {
    fprintf(stderr, "open %s failed : (%d) %s\n", maps_file.pathname, errno, strerror(errno));
    return;
  }

  if(settings.verbose)
    printf("Exploring %s\n", maps_file.pathname);
  elf = elf_begin(fd, ELF_C_READ, NULL); // obtain ELF descriptor
  if (elf == NULL) {
    fprintf(stderr, "elf_begin failed on %s : (%d) %s\n", maps_file.pathname, errno, strerror(errno));
    return;
  }

  if (gelf_getehdr(elf, &header) == NULL) {
    fprintf(stderr, "elf_getehdr failed on %s : (%d) %s\n", maps_file.pathname, errno, strerror(errno));
    return;
  }

  Elf_Scn *scn = NULL; // section
  GElf_Shdr shdr; // section header
  Elf_Data *data; // section data
  while ((scn=elf_nextscn(elf, scn)) != NULL) { // iterate through sections
    if (gelf_getshdr(scn, &shdr) == NULL) continue;
    if (shdr.sh_entsize == 0) continue;
    data = elf_getdata(scn, NULL);
    if (shdr.sh_entsize == 0) continue; // can't explore this one
    int count = (shdr.sh_size / shdr.sh_entsize);

    uintptr_t section_alignment = shdr.sh_addralign;

    /* iterate over the section's symbols */
    for (int index = 0; index < count ; index++) {
      GElf_Sym sym;
      if (gelf_getsym(data, index, &sym) == NULL) continue;
      char *symbol = elf_strptr(elf, shdr.sh_link, sym.st_name);
      if (symbol == NULL) continue;      
      uintptr_t value = sym.st_value;
      size_t size = sym.st_size;


      // we want objects with a non zero size, and that are global objects 
      // see elf.h for type and bind values
      if (size != 0 && GELF_ST_BIND(sym.st_info) == STB_GLOBAL
		      && (GELF_ST_TYPE(sym.st_info) == STT_OBJECT
			      || GELF_ST_TYPE(sym.st_info) == STT_TLS)
			      )
      {

        struct maps_addr_ranges* current_range = maps_file.addr_ranges;

#if 0
	// compute the address of the symbol
        // what is done here is a replica of what was done in ma_get_lib_variables :
        //   take the address of the first entry in maps and suppose you just have to add the symbol value
        //   this seemed to work with most symbols
	//        struct maps_addr_ranges* first_maps_entry = maps_file.addr_ranges;
	//        while (first_maps_entry->next != NULL) first_maps_entry = first_maps_entry->next;
	
        // check if the address is within a range of maps
        // note : when the address computation is done, maybe consider doing this check based on section address or something like that

	// while this *should* work, the computed address does not correspond to the actual address. This should be investigated !
#warning TODO: fix the symbol address computation
	do {
	  uintptr_t addr = value + current_range->addr_begin - current_range->offset;

	  size_t range_size = current_range->addr_end - current_range->addr_begin;
	  if (addr >= current_range->addr_begin &&
	      addr + size <= current_range->addr_end) {

            // the symbol fits in the range, add it
            struct memory_info *mem_info = insert_memory_info(lib, size, addr, symbol);
	    if(settings.verbose) {
	      printf("Found a variable (defined at %s). addr=%p, size=%zu, symbol=%s, value=%p\n",
		     maps_file.pathname, mem_info->buffer_addr, mem_info->buffer_size, mem_info->caller, value);
	    }
	    break;
          }
        } while ((current_range = current_range->next) != NULL);
#else
	/* since the proper */

	/* find the lowest address where the binary is mmapped and assume the whole binary is mapped here.
	 */
	uintptr_t addr_begin = (uintptr_t)(current_range ? current_range->addr_begin : 0);
	while(current_range) {
	  if(current_range->addr_begin < addr_begin)
	    addr_begin=current_range->addr_begin;
	  current_range = current_range->next;
	}
	
	uintptr_t addr = value + addr_begin;
	struct memory_info *mem_info = insert_memory_info(lib, size, (void*)addr, symbol);
	if(settings.verbose)
	  printf("Found a lib variable (defined at %s). addr=%p, size=%zu, symbol=%s, value=%p\n",
		 maps_file.pathname, mem_info->buffer_addr, mem_info->buffer_size, mem_info->caller, value);
#endif
      }
    }
  }
}

void free_maps_file_list(struct maps_file_list* list) {
  while (list != NULL)
  {
    struct maps_addr_ranges* current_range = list->addr_ranges;
    while (current_range != NULL)
    {
      struct maps_addr_ranges* old = current_range;
      current_range = current_range->next;
      free(old);
    }
    struct maps_file_list* old_file = list;
    list = list->next;
    free(old_file);
  }
}


/* get the list of global/static variables with their address and size */
void ma_get_variables () {
  char line[4096];
  char maps_path[1024];
  FILE *f = NULL;
  sprintf(maps_path, "/proc/%d/maps", getpid());
  f = fopen(maps_path, "r");
  if (f == NULL) {
    fprintf(stderr, "Could not open %s (reading mode)\n", maps_path);
    abort();
  }
  struct maps_file_list* list = NULL;
  while (!feof(f)) {
    fgets(line, sizeof(line), f);
    list = insert_new_maps_file_from_line(line, list);
  }
  fclose(f);
  struct maps_file_list *current_file = list;

  while (current_file != NULL) {
    __ma_parse_elf(*current_file);
    current_file = current_file->next;
  }
  free_maps_file_list(list);
}

void ma_record_malloc(struct mem_block_info* info) {
  if(!IS_RECORD_SAFE)
    return;
  PROTECT_RECORD;

  start_tick(record_malloc);

  mem_sampling_collect_samples();

  start_tick(fast_alloc);

  struct memory_info * mem_info = NULL;
#ifdef USE_HASHTABLE
  mem_info = mem_allocator_alloc(mem_info_allocator);
#else
  struct memory_info_list * p_node = mem_allocator_alloc(mem_info_allocator);
  mem_info = &p_node->mem_info;
#endif
  stop_tick(fast_alloc);
  start_tick(init_block);

#if 0
  mem_info->mem_type = dynamic_allocation;
  mem_info->alloc_date = new_date();
  mem_info->free_date = 0;
  mem_info->initial_buffer_size = info->size;
  mem_info->buffer_size = info->size;
  mem_info->buffer_addr = info->u_ptr;
  mem_info->blocks = NULL;
  info->record_info = mem_info;

  /* the current backtrace looks like this:
   * 0 - get_caller_function()
   * 1 - ma_record_malloc()
   * 2 - malloc()
   * 3 - caller_function()
   *
   * So, we need to get the name of the function in frame 3.
   */
  //  mem_info->caller = get_caller_function(3);
  mem_info->call_site = NULL;
  mem_info->caller = NULL;
  mem_info->caller_rip = get_caller_rip(3);
  if(settings.online_analysis) {
    /* todo: when implementing offline analysis, make sure counters are initialized */
    __allocate_counters(mem_info);
    __init_counters(mem_info);
  }
#else
  void* caller_rip;
  int callstack_size;
  void** callstack_rip = get_caller_rip(3, &callstack_size, &caller_rip);
  _init_mem_info(mem_info, dynamic_allocation, new_date(), info->size, info->u_ptr, callstack_rip, callstack_size, caller_rip, NULL);
#endif
  info->record_info = mem_info;
  
  stop_tick(init_block);

  start_tick(insert_in_tree);
  pthread_mutex_lock(&mem_list_lock);
#ifdef USE_HASHTABLE
  mem_list = ht_insert(mem_list, (uint64_t) mem_info->buffer_addr, mem_info);
#else
  p_node->next = mem_list;
  p_node->prev = NULL;
  if(p_node->next)
    p_node->next->prev = NULL;
  mem_list = p_node;
#endif
  pthread_mutex_unlock(&mem_list_lock);

  stop_tick(insert_in_tree);

  start_tick(sampling_resume);
  mem_sampling_resume();
  stop_tick(sampling_resume);

  stop_tick(record_malloc);

  UNPROTECT_RECORD;
}

void ma_update_buffer_address(struct mem_block_info* info, void *old_addr, void *new_addr) {
  if(!IS_RECORD_SAFE)
    return;
  if(!info->record_info)
    return;

  PROTECT_RECORD;

  mem_sampling_collect_samples();

  struct memory_info* mem_info = info->record_info;
  assert(mem_info);
  mem_info->buffer_addr = new_addr;

  start_tick(sampling_resume);
  mem_sampling_resume();
  stop_tick(sampling_resume);

  UNPROTECT_RECORD;
}

/*
 * remove mem_info from the list of active buffers and add it to the list of inactive buffers
 */
void set_buffer_free(struct mem_block_info* p_block) {
  pthread_mutex_lock(&mem_list_lock);
#ifdef USE_HASHTABLE
  /* nothing to do here: we keep all buffers in the same hashmap. We'll use the timestamps to differenciate them  */
  struct memory_info* mem_info = p_block->record_info;
#else
  struct memory_info_list * p_node = mem_list;
  if(p_block->record_info == &p_node->mem_info) {
    /* the first record is the one we're looking for */
    mem_list = p_node->next;
    if(p_node->next)
      p_node->next->prev = p_node->prev;
    p_node->next = past_mem_list;
    p_node->prev = NULL;
    if(p_node->next)
      p_node->next->prev = p_node;
    past_mem_list = p_node;
    goto out;
  }

  /* browse the list of malloc'd buffers */
  while(p_node->next) {
    if(&p_node->next->mem_info == p_block->record_info) {
      struct memory_info_list *to_move = p_node->next;
      /* remove to_move from the list of malloc'd buffers */
      p_node->next = to_move->next;
      if(p_node->next)
	p_node->next->prev = p_node;
      /* add it to the list of freed buffers */
      to_move->next = past_mem_list;
      to_move->prev = NULL;
      if(to_move->next)
	to_move->next->prev = to_move;
      past_mem_list = to_move;
      goto out;
    }
    p_node = p_node->next;
  }
  /* couldn't find p_block in the list of malloc'd buffers */
  fprintf(stderr, "Error: I tried to free block %p, but I could'nt find it in the list of malloc'd buffers\n", p_block);
  abort();
#endif
 out:
  pthread_mutex_unlock(&mem_list_lock);
}

void ma_record_free(struct mem_block_info* info) {
  if(!IS_RECORD_SAFE)
    return;
  if(!info->record_info)
    return;

  PROTECT_RECORD;
  start_tick(record_free);
  mem_sampling_collect_samples();


  struct memory_info* mem_info = info->record_info;
  assert(mem_info);
  mem_info->buffer_size = info->size;
  mem_info->free_date = new_date();

  set_buffer_free(info);


  start_tick(sampling_resume);
  mem_sampling_resume();
  stop_tick(sampling_resume);
  stop_tick(record_free);
  UNPROTECT_RECORD;
}

struct call_site* call_sites = NULL;

struct call_site *find_call_site(struct memory_info* mem_info) {
//  if(mem_info->mem_type != dynamic_allocation)
//    return NULL;

  struct call_site * cur_site = call_sites;
  while(cur_site) {
    if(cur_site->buffer_size == mem_info->initial_buffer_size) {
        if(cur_site->callstack_rip) {
            if(cur_site->callstack_size == mem_info->callstack_size) {
                int match = 1;
                for(int i = 3; i < cur_site->callstack_size; i++) {
                    if(cur_site->callstack_rip[i] != mem_info->callstack_rip[i]) {
                        match = 0;
                        break;
                    }
                }
                if(match) {
                    return cur_site;
                }
            }
        } else {
            if(cur_site->caller_rip == mem_info->caller_rip) {
                return cur_site;
            }
        }
    }
    cur_site = cur_site->next;
  }
  return NULL;
}

struct call_site * new_call_site(struct memory_info* mem_info) {
  struct call_site * site = libmalloc(sizeof(struct call_site));
  if(!mem_info->caller) {
    mem_info->caller = get_caller_function_from_rip(mem_info->caller_rip);
  }

  static _Atomic uint32_t next_call_site_id = 1;
  site->id = next_call_site_id++;

  site->callstack_rip = mem_info->callstack_rip;
  site->callstack_size = mem_info->callstack_size;
  site->caller_rip = mem_info->caller_rip;
  site->caller = mem_allocator_alloc(string_allocator);
  strcpy(site->caller, mem_info->caller);
  site->buffer_size =  mem_info->initial_buffer_size;
  site->nb_mallocs = 0;
  site->dump_file = NULL;

#if 0
  site->mem_info.mem_type = mem_info->mem_type;
  site->mem_info.alloc_date = 0;
  site->mem_info.free_date = 0;
  site->mem_info.initial_buffer_size = mem_info->initial_buffer_size;
  site->mem_info.buffer_size = mem_info->buffer_size;
  site->mem_info.buffer_addr = mem_info->buffer_addr;
  site->mem_info.caller = site->caller;
  site->mem_info.caller_rip = site->caller_rip;
#else
  _init_mem_info(&site->mem_info, mem_info->mem_type, 0, mem_info->initial_buffer_size,
		 mem_info->buffer_addr, site->callstack_rip, site->callstack_size, site->caller_rip, site->caller);
  site->mem_info.buffer_size = mem_info->buffer_size;
#endif
  ma_allocate_counters(&site->mem_info);
  ma_init_counters(&site->mem_info);
  site->cumulated_counters.block_id = 0;
  site->cumulated_counters.next = NULL;
  int i, j;
  for(j = 0; j<ACCESS_MAX; j++) {
    memset(&site->cumulated_counters.counters[j], 0, sizeof(struct mem_counters));
  }
  __init_counters(&site->mem_info);

  site->next = call_sites;
  call_sites = site;
  return site;
}

struct call_site*  update_call_sites(struct memory_info* mem_info) {
  struct call_site* site = find_call_site(mem_info);
  if(!site) {
    site = new_call_site(mem_info);
  }

  site->nb_mallocs++;
  int i, j;
  for(i = 0; i<MAX_THREADS; i++) {
    struct block_info *block = mem_info->blocks[i];
    while(block) {
      struct block_info* mem_block = __ma_get_block(site->mem_info.blocks[i], block->block_id);
      struct block_info* site_block = __ma_get_block(&site->cumulated_counters, 0);

      for(j = 0; j<ACCESS_MAX; j++) {

#define ACC_COUNTER(to, from, _c) do {			\
	  to._c.count += from._c.count;			\
	  to._c.sum_weight += from._c.sum_weight;	\
	  if(to._c.min_weight > from._c.min_weight)	\
	    to._c.min_weight = from._c.min_weight;	\
	  if(to._c.max_weight > from._c.max_weight)	\
	    to._c.max_weight = from._c.max_weight;	\
	} while(0)

#define ACC_COUNTERS(to, from) do {			\
	  to.total_count += from.total_count;		\
	  to.total_weight += from.total_weight;		\
	  to.na_miss_count += from.na_miss_count;	\
	  ACC_COUNTER(to, from, cache1_hit);		\
	  ACC_COUNTER(to, from, cache2_hit);		\
	  ACC_COUNTER(to, from, cache3_hit);		\
	  ACC_COUNTER(to, from, lfb_hit);		\
	  ACC_COUNTER(to, from, local_ram_hit);		\
	  ACC_COUNTER(to, from, remote_ram_hit);	\
	  ACC_COUNTER(to, from, remote_cache_hit);	\
	  ACC_COUNTER(to, from, io_memory_hit);		\
	  ACC_COUNTER(to, from, uncached_memory_hit);	\
	  ACC_COUNTER(to, from, cache1_miss);		\
	  ACC_COUNTER(to, from, cache2_miss);		\
	  ACC_COUNTER(to, from, cache3_miss);		\
	  ACC_COUNTER(to, from, lfb_miss);		\
	  ACC_COUNTER(to, from, local_ram_miss);	\
	  ACC_COUNTER(to, from, remote_ram_miss);	\
	  ACC_COUNTER(to, from, remote_cache_miss);	\
	  ACC_COUNTER(to, from, io_memory_miss);	\
	  ACC_COUNTER(to, from, uncached_memory_miss);	\
	}while(0)

	ACC_COUNTERS(mem_block->counters[j], block->counters[j]);
	ACC_COUNTERS(site_block->counters[j], block->counters[j]);
      }
      block = block->next;
    }
  }
  return site;
}

static void __print_counters(FILE*f, struct mem_counters* counters) {
  for(int i=0; i< ACCESS_MAX; i++){
    if(i==ACCESS_READ) {
      fprintf(f, "\n");
      fprintf(f, "# --------------------------------------\n");
      fprintf(f, "# Summary of all the read memory access:\n");
    } else {
      fprintf(f, "# --------------------------------------\n");
      fprintf(f, "# Summary of all the write memory access:\n");
    }

#define _PERCENT(c) (100.*c / counters[i].total_count)
#define PERCENT(__c) (_PERCENT(counters[i].__c.count))
#define MIN_COUNT(__c) (counters[i].__c.min_weight)
#define MAX_COUNT(__c) (counters[i].__c.max_weight)
#define AVG_COUNT(__c) (counters[i].__c.count? counters[i].__c.sum_weight / counters[i].__c.count : 0)
#define WEIGHT(__c) (counters[i].__c.sum_weight)
#define PERCENT_WEIGHT(__c) (counters[i].total_weight?100.*counters[i].__c.sum_weight/counters[i].total_weight:0)
    
#define PRINT_COUNTER(__c, str) \
    if(counters[i].__c.count) fprintf(f, "# %s\t: %ld (%f %%) \tmin: %llu cycles\tmax: %llu cycles\t avg: %llu cycles\ttotal weight: % "PRIu64" (%f %%)\n", \
				     str, counters[i].__c.count, PERCENT(__c), MIN_COUNT(__c), MAX_COUNT(__c), AVG_COUNT(__c), \
				     WEIGHT(__c), PERCENT_WEIGHT(__c))
    
    fprintf(f, "# Total count          : \t %"PRIu64"\n", counters[i].total_count);
    fprintf(f, "# Total weigh          : \t %"PRIu64"\n", counters[i].total_weight);
    if(counters[i].na_miss_count)
      fprintf(f, "# N/A                  : \t %"PRIu64" (%f %%)\n", counters[i].na_miss_count, _PERCENT(counters[i].na_miss_count));

    PRINT_COUNTER(cache1_hit, "L1 Hit");
    PRINT_COUNTER(cache2_hit, "L2 Hit");
    PRINT_COUNTER(cache3_hit, "L3 Hit");

    PRINT_COUNTER(lfb_hit, "LFB Hit");
    PRINT_COUNTER(local_ram_hit, "Local RAM Hit");
    PRINT_COUNTER(remote_ram_hit, "Remote RAM Hit");
    PRINT_COUNTER(remote_cache_hit, "Remote cache Hit");
    PRINT_COUNTER(io_memory_hit, "IO memory Hit");
    PRINT_COUNTER(uncached_memory_hit, "Uncached memory Hit");

    fprintf(f, "\n");

    PRINT_COUNTER(lfb_miss, "LFB Miss");
    PRINT_COUNTER(local_ram_miss, "Local RAM Miss");
    PRINT_COUNTER(remote_ram_miss, "Remote RAM Miss");
    PRINT_COUNTER(remote_cache_miss, "Remote cache Miss");
    PRINT_COUNTER(io_memory_miss, "IO memory Miss");
    PRINT_COUNTER(uncached_memory_miss, "Uncached memory Miss");
  }
}

static void __print_call_site_stats(struct call_site*site) {

  char filename[4096];
  char file_basename[STRING_LEN];
  snprintf(file_basename, STRING_LEN, "callsite_summary_%d.dat", site->id);
  create_log_filename(file_basename, filename, 4096);
  FILE* f = fopen(filename, "w");
  if(! f) {
    fprintf(stderr, "failed to open %s for writing: %s\n", filename, strerror(errno));
    abort();
  }

  __print_counters(f, site->cumulated_counters.counters);
  fclose(f);
}

/* remove site from the list of callsites */
static void __remove_site(struct call_site*site) {
  struct call_site*cur_site = call_sites;
  if(cur_site == site) {
    /* remove the first site */
    call_sites = cur_site->next;
    goto out;
  }

  while(cur_site->next) {
    if(cur_site->next == site) {
      /* remove cur_site->next */
      cur_site->next = site->next;
      goto out;
    }
    cur_site = cur_site->next;
  }
 out:
  if(cur_site &&cur_site->dump_file) {
    __print_call_site_stats(cur_site);
    fclose(cur_site->dump_file);
    cur_site->dump_file = NULL;
  }
}

/* sort sites depending on their total weight */
static void __sort_sites() {
  struct call_site* head = NULL;
  printf("Sorting call sites\n");

  while(call_sites) {
    struct call_site* cur_site = call_sites;
    struct call_site*min_weight_site  = cur_site;
    /* todo: for now, the sites are sorted according to the number of
     * access to the first block.
     * This should be changed so that they
     * are sorted based on the total number of access (to any block)
     */

    int min_weight = cur_site->cumulated_counters.counters[ACCESS_READ].total_weight;
    while (cur_site) {
      if(cur_site->cumulated_counters.counters[ACCESS_READ].total_weight < min_weight) {
	min_weight = cur_site->cumulated_counters.counters[ACCESS_READ].total_weight;
	min_weight_site = cur_site;
      }
      cur_site = cur_site->next;
    }
    __remove_site(min_weight_site);
    min_weight_site->next = head;
    head = min_weight_site;
  }
  call_sites = head;
}

static void __plot_counters(struct memory_info *mem_info,
			    int nb_threads,
			    const char*filename) {
  FILE* file = fopen(filename, "w");
  assert(file);

  int nb_pages = (mem_info->buffer_size / PAGE_SIZE)+1;
  for(int i=0; i<nb_pages; i++) {
    /* the block was accessed by at least one thread */
      size_t start_offset = i*PAGE_SIZE;
      size_t stop_offset = (i+1)*PAGE_SIZE;
      for(int th=0; th< nb_threads; th++) {
	struct block_info* block =  __ma_search_block(mem_info->blocks[th], i);
	int total_access = 0;
	if(block) {
	  total_access += block->counters[ACCESS_READ].total_count;
	  total_access += block->counters[ACCESS_WRITE].total_count;
	}
	fprintf(file, "\t%d", total_access);
      }
      fprintf(file, "\n");

  }
  fclose(file);
}

void print_buffer_list() {
  char filename[4096];
  create_log_filename("buffers.log", filename, 4096);
  FILE* f=fopen(filename, "w");
  if(!f) {
    perror("failed to open buffer.log for writing");
    return;
  }
  __ma_print_buffers_generic(f, mem_list);
  fclose(f);
}

void print_call_site_summary() {
  printf("Summary of the call sites:\n");
  printf("--------------------------\n");
  __sort_sites();
  struct call_site* site = call_sites;
  int nb_threads = next_thread_rank;

  char callsite_filename[1024];
  create_log_filename("call_sites.log", callsite_filename, 1024);
  FILE* callsite_file=fopen(callsite_filename, "w");
  assert(callsite_file!=NULL);
  while(site) {
    if(site->cumulated_counters.counters[ACCESS_READ].total_count ||
       site->cumulated_counters.counters[ACCESS_WRITE].total_count) {

      double avg_read_weight = 0;
      if(site->cumulated_counters.counters[ACCESS_READ].total_count) {
	avg_read_weight = (double)site->cumulated_counters.counters[ACCESS_READ].total_weight / site->cumulated_counters.counters[ACCESS_READ].total_count;
      }

      fprintf(callsite_file, "%d\t%s (size=%zu) - %d buffers. %d read access (total weight: %u, avg weight: %f). %d wr_access\n",
	      site->id, site->caller, site->buffer_size, site->nb_mallocs,
	      site->cumulated_counters.counters[ACCESS_READ].total_count,
	      site->cumulated_counters.counters[ACCESS_READ].total_weight,
	      avg_read_weight,
	      site->cumulated_counters.counters[ACCESS_WRITE].total_count);
      printf("%d\t%s (size=%zu) - %d buffers. %d read access (total weight: %u, avg weight: %f). %d wr_access\n",
	     site->id, site->caller, site->buffer_size, site->nb_mallocs,
	     site->cumulated_counters.counters[ACCESS_READ].total_count,
	     site->cumulated_counters.counters[ACCESS_READ].total_weight,
	     avg_read_weight,
	     site->cumulated_counters.counters[ACCESS_WRITE].total_count);

      if(site->mem_info.mem_type != stack) {
	char filename[1024];
	sprintf(filename, "%s/callsite_counters_%d.dat", get_log_dir(), site->id);
	__plot_counters(&site->mem_info, nb_threads, filename);
      }
    }
    site = site->next;
  }
  fclose(callsite_file);
  //  print_buffer_list();
}

static void _print_object_summary(FILE* f, struct memory_info* mem_info) {
  long long rd_count = 0;
  long long wr_count = 0;

  if(!mem_info->caller) {
    mem_info->caller = get_caller_function_from_rip(mem_info->caller_rip);
  }

  char* caller = NULL;
  if(mem_info->caller) {
    caller = mem_info->caller;
  }

  char callstack_rip_str[1024];
  char callstack_offset_str[16384];
  callstack_rip_str[0] = '\0';
  callstack_offset_str[0] = '\0';

  if(mem_info->callstack_rip) {
    for(int i = 3; i < mem_info->callstack_size; i++) {
      char cur_str_rip[16];
      char cur_str_offset[256];

      // get information about base address of executable or shared library for code location
      int rc;
      Dl_info info;
      rc = dladdr(mem_info->callstack_rip[i], &info);
      
      // === DEBUG ===
      //   printf("Output for callstack item 0x%"PRIx64":\n", mem_info->callstack_rip[i]);
      //   // provides more information about loaded libraries
      //   rc = dladdr1(mem_info->callstack_rip[i], &info, (void**)&map, RTLD_DL_LINKMAP);
      //   struct link_map *map = (struct link_map *)malloc(1000*sizeof(struct link_map));
      //   void *start_ptr = (void*)map;
      //   struct link_map *cur_entry = &map[0];
      //   while(cur_entry) {
      //     printf("-- l_name = %s; l_addr=%ld; l_ld=%p\n", cur_entry->l_name, cur_entry->l_addr, (void*)cur_entry->l_ld);
      //     cur_entry = cur_entry->l_next;
      //   }
      //   free(start_ptr)
      //   printf("Found in: 0x%"PRIx64"\t%s\n", info.dli_fbase, info.dli_fname);
      // === DEBUG ===

      // calculate offset to where shared library / executable has been loaded into memory
      ptrdiff_t offset = (uintptr_t)mem_info->callstack_rip[i] - (uintptr_t)info.dli_fbase;

      if(i == 3) {
        sprintf(cur_str_rip, "0x%"PRIx64"", mem_info->callstack_rip[i]);
        sprintf(cur_str_offset, "%s:%td", info.dli_fname, offset);
      } else {
        sprintf(cur_str_rip, ",0x%"PRIx64"", mem_info->callstack_rip[i]);
        sprintf(cur_str_offset, ",%s:%td", info.dli_fname, offset);
      }        
      strcat(callstack_rip_str, cur_str_rip);
      strcat(callstack_offset_str, cur_str_offset);
    }
  } else {
    strcat(callstack_rip_str, "NULL");
    strcat(callstack_offset_str, "NULL");
  }

  fprintf(f, "%d\t0x%"PRIx64"\t%ld\t%"PRIu64"\t%"PRIu64"\t%s\t%s\t0x%"PRIx64"\t%s\n",
	  mem_info->id, mem_info->buffer_addr, mem_info->buffer_size,
	  mem_info->alloc_date, mem_info->free_date, callstack_rip_str, callstack_offset_str, mem_info->caller_rip, caller);
}

static void print_object_summary_from_list(FILE* f, mem_info_node_t list) {
#ifdef USE_HASHTABLE
  /* todo */
  struct ht_node*p_node = NULL;
  FOREACH_HASH(mem_list, p_node) {
    struct ht_entry*e = p_node->entries;
    while(e) {
      struct memory_info* mem_info = e->value;
      _print_object_summary(f, mem_info);
      e = e->next;
    }

  }
#else
  struct memory_info_list * p_node = list;
  while(p_node) {
    _print_object_summary(f, &p_node->mem_info);
    p_node = p_node->next;
  }
#endif
}

void print_object_summary() {
  if(settings.dump_all) {

    char filename[4096];
    char file_basename[STRING_LEN];
    snprintf(file_basename, STRING_LEN, "all_memory_objects.dat");
    create_log_filename(file_basename, filename, 4096);
    FILE * all_objects_file=fopen(filename, "w");
    if(!all_objects_file) {
      fprintf(stderr, "failed to open %s for writing: %s\n", filename, strerror(errno));
      abort();
    }

    /* write the content of the sample to a file */
    fprintf(all_objects_file,
	    "#object_id\taddress\tsize\tallocation_date\tdeallocation_date\tcallstack_rip\tcallstack_offsets\tcallsite_rip\tcallsite\n");

    print_object_summary_from_list(all_objects_file, mem_list);
    print_object_summary_from_list(all_objects_file, past_mem_list);
  }
}

/* browse the list of malloc'd buffers that were not freed */
void warn_non_freed_buffers() {

  pthread_mutex_lock(&mem_list_lock);

  struct memory_info* mem_info = NULL;
#ifdef USE_HASHTABLE
  struct ht_node*p_node = NULL;
  FOREACH_HASH(mem_list, p_node) {
    struct ht_entry*e = p_node->entries;
    while(e) {
      mem_info = e->value;
      if(! mem_info->free_date) {
#if WARN_NON_FREED
	printf("Warning: buffer %p (size=%lu bytes) was not freed\n",
	       mem_info->buffer_addr, mem_info->buffer_size);
#endif
	mem_info->free_date = new_date();
      }

      e=e->next;
    }
  }

#else
  while(mem_list) {
    mem_info = &mem_list->mem_info;

#if WARN_NON_FREED
    printf("Warning: buffer %p (size=%lu bytes) was not freed\n",
	   mem_info->buffer_addr, mem_info->buffer_size);
#endif

    mem_info->free_date = new_date();

    /* remove the record from the list of malloc'd buffers */
    struct memory_info_list* p_node = mem_list;
    mem_list = p_node->next;
    if(mem_list)
      mem_list->prev = NULL;
    /* add to the list of freed buffers */
    p_node->next = past_mem_list;
    if(p_node->next)
      p_node->next->prev = p_node;
    past_mem_list = p_node;
  }
#endif	/* USE_HASHTABLE */

  pthread_mutex_unlock(&mem_list_lock);
}


void ma_finalize() {

  ma_thread_finalize();
  PROTECT_RECORD;
  warn_non_freed_buffers();
  mem_sampling_finalize();

  printf("---------------------------------\n");
  printf("         MEM ANALYZER\n");
  printf("---------------------------------\n");

  pthread_mutex_lock(&mem_list_lock);

  mem_info_node_t p_node = NULL;
  struct memory_info* mem_info = NULL;

  /* browse the list of memory buffers  */
#ifdef USE_HASHTABLE
  FOREACH_HASH(mem_list, p_node) {
    struct ht_entry*e = p_node->entries;
    while(e) {
      mem_info = e->value;
#else
    for(p_node = past_mem_list;
	p_node;
	p_node = p_node->next) {
      mem_info = &p_node->mem_info;
#endif

      if(mem_info->blocks) {
	/* at least one memory access was detected */
	update_call_sites(mem_info);

	uint64_t duration = mem_info->free_date?
	  mem_info->free_date-mem_info->alloc_date:
	  0;

	int nb_threads = next_thread_rank;
	uint64_t total_read_count = 0;
	uint64_t total_write_count = 0;
	size_t nb_blocks_with_samples = 0;
	for(int i=0; i<nb_threads; i++) {
	  struct block_info* block = mem_info->blocks[i];
	  while (block) {
	    total_read_count += block->counters[ACCESS_READ].total_count;
	    total_write_count += block->counters[ACCESS_WRITE].total_count;
	    if (block->counters[ACCESS_READ].total_count != 0 ||
		block->counters[ACCESS_WRITE].total_count != 0) {
	      nb_blocks_with_samples++;
	    }
	    block = block->next;
	  }
	}

	if(total_read_count > 0 ||
	   total_write_count > 0) {

	  double r_access_frequency;
	  if(total_read_count)
	    r_access_frequency = (duration/settings.sampling_rate)/total_read_count;
	  else
	    r_access_frequency = 0;
	  double w_access_frequency;
	  if(total_write_count)
	    w_access_frequency = (duration/settings.sampling_rate)/total_write_count;
	  else
	    w_access_frequency = 0;
	}
      }
#ifdef USE_HASHTABLE
      e = e->next;
    }
#endif
    }

    __print_counters(stdout, global_counters);
    print_call_site_summary();
    print_object_summary();

    mem_sampling_statistics();
    pthread_mutex_unlock(&mem_list_lock);
    UNPROTECT_RECORD;
  }

