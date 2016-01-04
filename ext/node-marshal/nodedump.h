#ifndef RUBY_API_VERSION_MAJOR
#error Cannot find Ruby version constants
#endif

/* Macros that depend on Ruby version */
// Pre-2.0 Ruby versions don't use this version
#if RUBY_API_VERSION_MAJOR == 2
#define USE_RB_ARGS_INFO 1
#endif

#if RUBY_API_VERSION_MAJOR == 1
#define RESET_GC_FLAGS 1
#endif


/* Some constants */
// Magic value with the version of the format
#define NODEMARSHAL_MAGIC "NODEMARSHAL11"
// Type of the node "Child"
#define NT_NULL 0
#define NT_UNKNOWN 1
#define NT_NODE 2
#define NT_VALUE 3
#define NT_ID 4
#define NT_INTEGER 5
#define NT_LONG 5
#define NT_ARGS 6
#define NT_ENTRY 7
#define NT_IDTABLE 8
#define NT_MEMORY 9

/* Value locations */
#define VL_RAW  0 // Just here
#define VL_NODE 1 // Global table of nodes
#define VL_ID   2 // Global table of identifiers
#define VL_GVAR 3 // Global variables table
#define VL_IDTABLE 4 // Global table of local ID tables
#define VL_ARGS 5 // Global table of arguments info structures
#define VL_LIT  6 // Global table of literals

/* base85r.c */
void base85r_init_tables();
VALUE base85r_encode(VALUE input);
VALUE base85r_decode(VALUE input);

/* nodechk.c */
void check_nodes_child_info(int pos);
void init_nodes_table(int *nodes_ctbl, int num_of_entries);

/* Parts of node.h from Ruby source code */
#if (RUBY_API_VERSION_MAJOR == 2) && (RUBY_API_VERSION_MINOR == 3)
#include "node230.h"
#elif (RUBY_API_VERSION_MAJOR == 2) && (RUBY_API_VERSION_MINOR == 2)
#include "node220.h"
#elif (RUBY_API_VERSION_MAJOR == 1) && (RUBY_API_VERSION_MINOR == 9)
#include "node193.h"
#else
#include "node220.h"
#error Unsupported version of Ruby
#endif
