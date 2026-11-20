#include <stdlib.h>
typedef void *(*malloc_func_t)(size_t); typedef void (*free_func_t)(void *);
malloc_func_t libdeflate_default_malloc_func = malloc;
free_func_t libdeflate_default_free_func = free;
