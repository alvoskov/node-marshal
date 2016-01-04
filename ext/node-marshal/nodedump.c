/*
 * This file contains implementation of classes for Ruby nodes
 * marshalization (i.e. loading and saving them from disk)
 * 
 * (C) 2015 Alexey Voskov
 * License: 2-clause BSD
 */
#define __STDC_FORMAT_MACROS
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ruby.h>
#include <ruby/version.h>

/*
 * Some global variables
 */
static VALUE cNodeObjAddresses, cNodeInfo;

/*
 * Part 1. .H files: nodedump functions + parts of Ruby internals
 */
#include "nodedump.h"

#ifdef WITH_CUSTOM_RB_GLOBAL_ENTRY
/* Custom (and slow) implementation of rb_global_entry internal API for Ruby 2.3
   (original rb_global_entry API was opened before Ruby 2.3)
   It uses a hack with the node creation. The main idea of the hack is 
   to create a node from the expression containing only a name of the global variable
   and extract global entry address from NODE_GVAR u3 "leaf" */
static struct rb_global_entry *rb_global_entry(ID id)
{
	NODE *node, *gvar_node;
	struct rb_global_entry *gentry;
	/* a) Step 1: create node from the expression consisting only from
	   our global variable */
	node = rb_compile_string("<compiled>", rb_id2str(id), NUM2INT(1));
	if (nd_type(node) != NODE_SCOPE)
	{
		return NULL;
	}
	/* b) Trace the node to the NODE_GVAR */
	gvar_node = node->u2.node;
	if (nd_type(gvar_node) == NODE_PRELUDE) /* Present only in 2.3 */
	{
		gvar_node = gvar_node->u2.node;
	}
	if (nd_type(gvar_node) != NODE_GVAR) /* Error: no GVAR found */
	{
		return NULL;
	}
	/* c) Get the global entry address and return its address */
	gentry = gvar_node->u3.entry;
	return gentry;
}
#endif


/*
 * Part 2. Information about the nodes
 * 
 */

// Pre-2.0 Ruby versions don't use this version
#if RUBY_API_VERSION_MAJOR == 2
#define USE_RB_ARGS_INFO 1
#endif

#if RUBY_API_VERSION_MAJOR == 1
#define RESET_GC_FLAGS 1
#endif


// Some generic utilities
int is_value_in_heap(VALUE val)
{
	if (val == Qfalse || val == Qtrue ||
	    val == Qnil || val == Qundef ||
	    (val & FIXNUM_FLAG)
#ifdef FLONUM_MASK
	    || ((val & FLONUM_MASK) == FLONUM_FLAG) // This memory trick with floats is present only in 2.x
#endif
	)
	{
		return 0;
	}
	else
		return 1;
}


/*
 * Converts Ruby string with hexadecimal number 
 * to the Ruby VALUE
 */
VALUE str_to_value(VALUE str)
{
	intptr_t ans = (intptr_t) Qnil;
	sscanf(RSTRING_PTR(str), "%"PRIxPTR, &ans);
	return (VALUE) ans;
}


/*
 * Converts Ruby VALUE (i.e. machine address) to the
 * hexadecimal Ruby string
 */
VALUE value_to_str(VALUE val)
{
	char str[16];
	sprintf(str, "%" PRIxPTR, (intptr_t) val);
	return rb_str_new2(str);
}

/*
 * Converts VALUE to the sequence of bytes using big-endian
 * standard. Returns number of non-zero bytes
 *
 * Inputs
 *   val -- input value
 *   buf -- pointer to the output buffer
 * Returns
 *   number of written bytes
 */
int value_to_bin(VALUE val, unsigned char *buf)
{
	int i, len = 0;
	unsigned char byte;
	for (i = sizeof(VALUE) - 1; i >= 0; i--)
	{
		byte = (unsigned char) ((val >> (i * 8)) & 0xFF);
		if (len > 0 || byte != 0)
		{
			*buf++ = byte;
			len++;
		}
	}
	return len;
}

/*
 * Converts sequence of bytes (big-endian standard) to the VALUE.
 *
 * Inputs
 *   buf -- poiner to the input buffer
 *   len -- number of bytes
 * Returns
 *   VALUE
 */
VALUE bin_to_value(unsigned char *buf, int len)
{
	VALUE val = (VALUE) 0;
	int i;
	for (i = len - 1; i >= 0; i--)
		val |= ((VALUE) *buf++) << (i * 8);
	return val;
}

#define NODES_CTBL_SIZE 256
static int nodes_ctbl[NODES_CTBL_SIZE * 3];


/*
 * Part 3. Functions for node marshalization
 */

/*
 * Keeps the information about node elements position 
 * in the memory and its IDs/ordinals for export to the file
 */
typedef struct {
	VALUE vals; // values: key=>val Hash
	VALUE ids; // identifiers: key=>id Hash
	VALUE pos; // free identifier
} LeafTableInfo;

void LeafTableInfo_init(LeafTableInfo *lti)
{
	lti->vals = rb_hash_new();
	lti->ids = rb_hash_new();
	lti->pos = 0;
}

void LeafTableInfo_mark(LeafTableInfo *lti)
{
	rb_gc_mark(lti->vals);
	rb_gc_mark(lti->ids);
}


int LeafTableInfo_addEntry(LeafTableInfo *lti, VALUE key, VALUE value)
{
	VALUE v_id = rb_hash_aref(lti->ids, key);
	if (v_id == Qnil)
	{
		int id = lti->pos++;
		rb_hash_aset(lti->vals, key, value);
		rb_hash_aset(lti->ids, key, INT2FIX(id));
		return id;
	}
	else
	{
		return FIX2INT(v_id);
	}
}

/*
 * Adds Ruby ID data type as the entry to the LeafTableInfo struct.
 * Main features:
 * 1) ID will be converted to Fixnum
 * 2) If ID can be converted to string by rb_id2str it will be saved as
      String object. Otherwise it will be converted to Fixnum.
 */
int LeafTableInfo_addIDEntry(LeafTableInfo *lti, ID id)
{
	VALUE r_idval = rb_id2str(id);
	if (TYPE(r_idval) != T_STRING)
	{
		r_idval = INT2FIX(id);
	}
	return LeafTableInfo_addEntry(lti, INT2FIX(id), r_idval);
}

VALUE LeafTableInfo_getLeavesTable(LeafTableInfo *lti)
{
	VALUE key, keys = rb_funcall(lti->vals, rb_intern("keys"), 0);
	unsigned int i;
	VALUE val;
	for (i = 0; i < lti->pos; i++)
	{
		key = RARRAY_PTR(keys)[i];
		val = rb_hash_aref(lti->vals, key);
		rb_ary_store(keys, i, val);
	}
	return keys;
}

int LeafTableInfo_keyToID(LeafTableInfo *lti, VALUE key)
{
	VALUE id = rb_hash_aref(lti->ids, key);
	return (id == Qnil) ? -1 : FIX2INT(id);
}

/* The structure keeps information about the node
   that is required for its dumping to the file
   (mainly hashes with relocatable identifiers) */
typedef struct {
	LeafTableInfo syms; // Node symbols
	LeafTableInfo lits; // Node literals
	LeafTableInfo idtabs; // Table of identifiers
#ifdef USE_RB_ARGS_INFO
	LeafTableInfo args; // Table of arguments
#endif
	LeafTableInfo gentries; // Global variables table
	LeafTableInfo nodes; // Table of nodes
} NODEInfo;

void NODEInfo_init(NODEInfo *info)
{
	LeafTableInfo_init(&(info->syms));
	LeafTableInfo_init(&(info->lits));
	LeafTableInfo_init(&(info->idtabs));
#ifdef USE_RB_ARGS_INFO
	LeafTableInfo_init(&(info->args));
#endif
	LeafTableInfo_init(&(info->gentries));
	LeafTableInfo_init(&(info->nodes));
}

void NODEInfo_mark(NODEInfo *info)
{
	LeafTableInfo_mark(&(info->syms));
	LeafTableInfo_mark(&(info->lits));
	LeafTableInfo_mark(&(info->idtabs));
#ifdef USE_RB_ARGS_INFO
	LeafTableInfo_mark(&(info->args));
#endif
	LeafTableInfo_mark(&(info->gentries));
	LeafTableInfo_mark(&(info->nodes));
}

void NODEInfo_free(NODEInfo *info)
{
	xfree(info);
}

LeafTableInfo *NODEInfo_getTableByID(NODEInfo *info, int id)
{
	switch (id)
	{
	case NT_ID:
		return &info->syms;
	case NT_VALUE:
		return &info->lits;
	case NT_IDTABLE:
		return &info->idtabs;
#ifdef USE_RB_ARGS_INFO
	case NT_ARGS:
		return &info->args;
#endif
	case NT_ENTRY:
		return &info->gentries;
	case NT_NODE:
		return &info->nodes;
	default:
		return NULL;
	}
}

/*
 * Converts node value to the binary data
 * Input parameters:
 *   info -- current NODEInfo structure
 *   node -- parent node (that contains the value)
 *   ptr  -- pointer to the output memory buffer
 *   type -- type of the entry (NT_...)
 *   value -- node->u?.value VALUE
 *   child_id -- child node number (1,2,3)
 * Returns:
 *   Byte that contains the next information
 *    a) upper half-byte: VL_... data type (for node loader)
 *    b) lower half-byte: number of bytes written to the buffer
 */
#define DUMP_RAW_VALUE(vl_ans, vl) (vl_ans | (value_to_bin(vl, (unsigned char *) ptr) << 4))
static int dump_node_value(NODEInfo *info, char *ptr, NODE *node, int type, VALUE value, int child_id)
{
	if (type == NT_NULL || type == NT_LONG)
	{
		return DUMP_RAW_VALUE(VL_RAW, value);
	}
	else if (type == NT_NODE)
	{
		if (value == 0)
		{	// Variant a: empty node
			return DUMP_RAW_VALUE(VL_RAW, value);
		}
		else if (nd_type(node) == NODE_ATTRASGN && value == 1 && child_id == 1)
		{	// Special case: "self"
			return DUMP_RAW_VALUE(VL_RAW, value);
		}
		else if (TYPE(value) != T_NODE)
		{
			rb_raise(rb_eArgError, "dump_node_value, parent node %s: child node %d (ADR 0x%s): is not a node\n"
				"  Type: %s (%d), Value: %s",
				ruby_node_name(nd_type(node)), child_id, RSTRING_PTR(value_to_str(value)),
				RSTRING_PTR(rb_funcall(rb_funcall(value, rb_intern("class"), 0), rb_intern("to_s"), 0)),
				TYPE(value),
				RSTRING_PTR(rb_funcall(value, rb_intern("to_s"), 0)) );
		}
		else
		{	// Variant b: not empty node
			VALUE id = LeafTableInfo_keyToID(&info->nodes, value_to_str(value));
			if (id == (VALUE) -1)
			{
				rb_raise(rb_eArgError, "dump_node_value, parent node %s: child node %d (ADR 0x%s) not found",
					ruby_node_name(nd_type(node)), child_id, RSTRING_PTR(value_to_str(value)));
				return VL_RAW;
			}
			else
			{
				return DUMP_RAW_VALUE(VL_NODE, id);
			}
			return VL_NODE;
		}
	}
	else if (type == NT_VALUE)
	{
		if (!is_value_in_heap(value))
		{	// a) value that is inside VALUE
			return DUMP_RAW_VALUE(VL_RAW, value);
		}
		else
		{	// b) value that requires reference to literals table
			VALUE id = LeafTableInfo_keyToID(&info->lits, value_to_str(value));
			if (id == (VALUE) -1)
				rb_raise(rb_eArgError, "Cannot find literal");
			else
				return DUMP_RAW_VALUE(VL_LIT, id);
		}
	}
	else if (type == NT_ID)
	{
		ID sym = (VALUE) value; // We are working with RAW data from RAM!
		VALUE id = LeafTableInfo_keyToID(&info->syms, INT2FIX(sym));
		if (id == (VALUE) -1)
		{
			rb_raise(rb_eArgError, "Cannot find symbol ID %d (%s) (parent node %s, line %d)",
				(int) sym, RSTRING_PTR(rb_id2str(ID2SYM(sym))),
				ruby_node_name(nd_type(node)), nd_line(node));
			return VL_RAW;
		}
		else
		{
			return DUMP_RAW_VALUE(VL_ID, id);
		}
	}
	else if (type == NT_ENTRY || type == NT_ARGS || type == NT_IDTABLE)
	{
		VALUE key = value_to_str(value);
		LeafTableInfo *lti = NODEInfo_getTableByID(info, type);
		VALUE id = LeafTableInfo_keyToID(lti, key);
		if (id == (VALUE) -1)
		{
			rb_raise(rb_eArgError, "Cannot find some entry");
			return VL_RAW;
		}
		else
		{
			//memcpy(ptr, &id, sizeof(VALUE));
			switch(type)
			{
				case NT_ENTRY: return DUMP_RAW_VALUE(VL_GVAR, id);
				case NT_IDTABLE: return DUMP_RAW_VALUE(VL_IDTABLE, id);
				case NT_ARGS: return DUMP_RAW_VALUE(VL_ARGS, id);
				default: rb_raise(rb_eArgError, "Internal error");
			}
		}
	}
	else
	{
		rb_raise(rb_eArgError, "Unknown child node type %d", type);
	}
}

static VALUE dump_nodes(NODEInfo *info)
{
	int node_size = sizeof(int) + sizeof(VALUE) * 4;
	int i, nt, flags_len;
	NODE *node;
	char *bin, *ptr, *rtypes;
	VALUE nodes_ary = rb_funcall(info->nodes.vals, rb_intern("keys"), 0);
	VALUE nodes_bin = rb_str_new(NULL, RARRAY_LEN(nodes_ary) * node_size);
	VALUE ut[3];
	bin = RSTRING_PTR(nodes_bin);

	for (i = 0, ptr = bin; i < RARRAY_LEN(nodes_ary); i++)
	{
		node = RNODE(str_to_value(RARRAY_PTR(nodes_ary)[i]));
		nt = nd_type(node);
		rtypes = (char *) ptr; ptr += sizeof(int);
		flags_len = value_to_bin(node->flags >> 5, (unsigned char *) ptr); ptr += flags_len;
		//memcpy(ptr, &(node->flags), sizeof(VALUE)); ptr += sizeof(VALUE);
		ut[0] = nodes_ctbl[nt * 3];
		ut[1] = nodes_ctbl[nt * 3 + 1];
		ut[2] = nodes_ctbl[nt * 3 + 2];
		if (nt == NODE_OP_ASGN2 && LeafTableInfo_keyToID(&info->syms, INT2FIX(node->u1.value)) != -1)
		{
			ut[0] = NT_ID; ut[1] = NT_ID; ut[2] = NT_ID;
		}

		if (nt == NODE_ARGS_AUX)
		{
			ut[0] = NT_ID; ut[1] = NT_LONG; ut[2] = NT_NODE;
			if (LeafTableInfo_keyToID(&info->syms, INT2FIX(node->u2.value)) != -1)
			{
				ut[1] = NT_ID;
			}
			else
			{
				ut[1] = NT_LONG;
			}
			if (node->u1.value == 0) ut[0] = NT_NULL;
			if (node->u2.value == 0) ut[1] = NT_NULL;
			if (node->u3.value == 0) ut[2] = NT_NULL;
		}

		rtypes[0] = dump_node_value(info, ptr, node, ut[0], node->u1.value, 1);
		ptr += (rtypes[0] & 0xF0) >> 4;
		rtypes[1] = dump_node_value(info, ptr, node, ut[1], node->u2.value, 2);
		ptr += (rtypes[1] & 0xF0) >> 4;
		rtypes[2] = dump_node_value(info, ptr, node, ut[2], node->u3.value, 3);
		ptr += (rtypes[2] & 0xF0) >> 4;
		rtypes[3] = flags_len;
	}
	rb_str_resize(nodes_bin, (int) (ptr - bin) + 1);
	//printf("%d", ptr - bin);
	return nodes_bin;
}


/*
 * Transforms preprocessed node to Ruby hash that can be used
 * to load the node from disk.
 *
 * See m_nodedump_to_hash function for output hash format details
 */
VALUE NODEInfo_toHash(NODEInfo *info)
{
	VALUE ans = rb_hash_new();
	VALUE idtbl, idtabs = LeafTableInfo_getLeavesTable(&info->idtabs);
	VALUE syms = LeafTableInfo_getLeavesTable(&info->syms);
	VALUE args;
	int i, j, id;
	// Add some signatures
	rb_hash_aset(ans, ID2SYM(rb_intern("MAGIC")), rb_str_new2(NODEMARSHAL_MAGIC));
	rb_hash_aset(ans, ID2SYM(rb_intern("RUBY_PLATFORM")),
		rb_const_get(rb_cObject, rb_intern("RUBY_PLATFORM")));
	rb_hash_aset(ans, ID2SYM(rb_intern("RUBY_VERSION")),
		rb_const_get(rb_cObject, rb_intern("RUBY_VERSION")));
	// Write literals, symbols and global_entries arrays: they don't need to be corrected
	rb_hash_aset(ans, ID2SYM(rb_intern("literals")), LeafTableInfo_getLeavesTable(&info->lits));
	rb_hash_aset(ans, ID2SYM(rb_intern("symbols")), syms);
	rb_hash_aset(ans, ID2SYM(rb_intern("global_entries")), LeafTableInfo_getLeavesTable(&info->gentries));
	// Replace RAM IDs to disk IDs in id_tables
	for (i = 0; i < RARRAY_LEN(idtabs); i++)
	{
		idtbl = RARRAY_PTR(idtabs)[i];
		for (j = 0; j < RARRAY_LEN(idtbl); j++)
		{
			id = LeafTableInfo_keyToID(&info->syms, RARRAY_PTR(idtbl)[j]);

			if (id == -1)
			{
				ID sym = FIX2INT(RARRAY_PTR(idtbl)[j]);
				rb_raise(rb_eArgError, "Cannot find the symbol ID %d", (int) sym);
			}
			else
			{
				rb_ary_store(idtbl, j, INT2FIX(id));
			}

		}
	}
	rb_hash_aset(ans, ID2SYM(rb_intern("id_tables")), idtabs);
	// Replace RAM IDs to disk IDs in args tables
#ifdef USE_RB_ARGS_INFO
	args = LeafTableInfo_getLeavesTable(&info->args);
	for (i = 0; i < RARRAY_LEN(args); i++)
	{
		VALUE args_entry = RARRAY_PTR(args)[i];
		VALUE *eptr = RARRAY_PTR(args_entry);
		int args_vals[5] = {0, 1, 7, 8, 9};
		int args_ids[3] = {4, 5, 6};
		if (RARRAY_LEN(args_entry) != 10)
			rb_raise(rb_eArgError, "Corrupted args entry");
		// Pointer to nodes to be replaced:
		// a) VALUES
		//   (0) pre_init, (1) post_init,
		//   (7) kw_args, (8) kw_rest_arg, (9) opt_args
		for (j = 0; j < 5; j++)
		{
			int ind = args_vals[j];
			VALUE key = eptr[ind];
			if (!strcmp(RSTRING_PTR(key), "0"))
				eptr[ind] = INT2FIX(-1);
			else
			{
				eptr[ind] = INT2FIX(LeafTableInfo_keyToID(&info->nodes, key));
				if (FIX2INT(eptr[ind]) == -1)
					rb_raise(rb_eArgError, "Unknown NODE in args tables");
			}
		}
		// b) IDs (symbols)
		//   (4) first_post_arg (5) rest_arg (6) block_arg
		for (j = 0; j < 3; j++)
		{
			int ind = args_ids[j];
			VALUE key = eptr[ind];
			if (FIX2INT(key) != 0)
			{
				eptr[ind] = INT2FIX(LeafTableInfo_keyToID(&info->syms, key));
				if (FIX2INT(eptr[ind]) == -1)
					rb_raise(rb_eArgError, "Unknown symbolic ID in args tables");
			}
			else
				eptr[ind] = INT2FIX(-1);
		}
	}
#else
	args = rb_ary_new();
#endif

	rb_hash_aset(ans, ID2SYM(rb_intern("args")), args);
	// Special case: NODES. Nodes are kept as binary string
	rb_hash_aset(ans, ID2SYM(rb_intern("nodes")), dump_nodes(info));
	return ans;
}


static void NODEInfo_addValue(NODEInfo *info, VALUE value)
{
	if (is_value_in_heap(value))
	{
		VALUE lkey = value_to_str(value);
		LeafTableInfo_addEntry(&info->lits, lkey, value);
	}
}

/*
 * Function counts number of nodes and fills NODEInfo struct
 * that is neccessary for the node saving to the HDD
 */
static int count_num_of_nodes(NODE *node, NODE *parent, NODEInfo *info)
{
	int ut[3], num, offset;
	if (node == 0)
	{
		return 0;
	}
	else if (TYPE((VALUE) node) != T_NODE)
	{
		rb_raise(rb_eArgError, "count_num_of_nodes: parent node %s: child node (ADR 0x%s) is not a node; Type: %d",
			ruby_node_name(nd_type(parent)), RSTRING_PTR(value_to_str((VALUE) node)), TYPE((VALUE) node));
		return 0;
	}
	else
	{
		offset = nd_type(node) * 3;
		ut[0] = nodes_ctbl[offset++];
		ut[1] = nodes_ctbl[offset++];
		ut[2] = nodes_ctbl[offset];

		if (nd_type(node) == NODE_OP_ASGN2 && nd_type(parent) == NODE_OP_ASGN2)
		{
			ut[0] = NT_ID;
			ut[1] = NT_ID;
			ut[2] = NT_ID;
		}

		/* Some Ruby 1.9.3 style function arguments (without rb_args_info) */
		if (nd_type(node) == NODE_ARGS_AUX)
		{
			ut[0] = NT_ID;
			ut[1] = (nd_type(parent) == NODE_ARGS_AUX) ? NT_LONG : NT_ID;
			ut[2] = NT_NODE;

			if (node->u1.value == 0) ut[0] = NT_NULL;
			if (node->u2.value == 0) ut[1] = NT_NULL;
			if (node->u3.value == 0) ut[2] = NT_NULL;
		}
		/* Some Ruby 1.9.3-specific code for NODE_ATTRASGN */
		if (nd_type(node) == NODE_ATTRASGN)
		{
			if (node->u1.value == 1) ut[0] = NT_LONG;
		}
		/* Check if there is information about child nodes types */
		if (ut[0] == NT_UNKNOWN || ut[1] == NT_UNKNOWN || ut[2] == NT_UNKNOWN)
		{
			rb_raise(rb_eArgError, "Cannot interpret node %d (%s)", nd_type(node), ruby_node_name(nd_type(node)));
		}
		/* Save the ID of the node */
		num = 1;
		LeafTableInfo_addEntry(&info->nodes, value_to_str((VALUE) node), value_to_str((VALUE) node));
		/* Analyze node childs */
		/* a) child 1 */
		if (ut[0] == NT_NODE)
		{
			num += count_num_of_nodes(node->u1.node, node, info);
		}
		else if (ut[0] == NT_ID)
		{
			LeafTableInfo_addIDEntry(&info->syms, node->u1.id);
		}
		else if (ut[0] == NT_VALUE)
		{
			if (TYPE(node->u1.value) == T_NODE)
				rb_raise(rb_eArgError, "NODE instead of VALUE in child 1 of node %s", ruby_node_name(nd_type(node)));
			NODEInfo_addValue(info, node->u1.value);
		}
		else if (ut[0] == NT_IDTABLE)
		{
			VALUE tkey = value_to_str(node->u1.value);
			VALUE idtbl_ary = rb_ary_new();
			ID *idtbl = (ID *) node->u1.value;
			int i, size = (node->u1.value) ? *idtbl++ : 0;
			for (i = 0; i < size; i++)
			{
				ID sym = *idtbl++;
				rb_ary_push(idtbl_ary, INT2FIX(sym));
				LeafTableInfo_addIDEntry(&info->syms, sym);
			}
			LeafTableInfo_addEntry(&info->idtabs, tkey, idtbl_ary);
		}
		else if (ut[0] != NT_LONG && ut[0] != NT_NULL)
		{
			rb_raise(rb_eArgError, "1!");
		}
		/* b) child 2 */
		if (ut[1] == NT_NODE)
		{
			num += count_num_of_nodes(node->u2.node, node, info);
		}
		else if (ut[1] == NT_ID)
		{
			LeafTableInfo_addIDEntry(&info->syms, node->u2.id);
		}
		else if (ut[1] == NT_VALUE)
		{
			if (TYPE(node->u2.value) == T_NODE)
				rb_raise(rb_eArgError, "NODE instead of VALUE in child 2 of node %s", ruby_node_name(nd_type(node)));
			NODEInfo_addValue(info, node->u2.value);
		}
		else if (ut[1] != NT_LONG && ut[1] != NT_NULL)
		{
			rb_raise(rb_eArgError, "2!");
		}

		/* c) child 3 */
		if (ut[2] == NT_NODE)
		{
			num += count_num_of_nodes(node->u3.node, node, info);
		}
		else if (ut[2] == NT_ID)
		{
			LeafTableInfo_addIDEntry(&info->syms, node->u3.id);
		}
		else if (ut[2] == NT_ARGS)
		{
#ifdef USE_RB_ARGS_INFO
			VALUE varg = Qtrue;
			struct rb_args_info *ainfo;
			ID asym;
			ainfo = node->u3.args;
			// Save child nodes
			num += count_num_of_nodes(ainfo->pre_init, node, info);
			num += count_num_of_nodes(ainfo->post_init, node, info);
			num += count_num_of_nodes(ainfo->kw_args, node, info);
			num += count_num_of_nodes(ainfo->kw_rest_arg, node, info);
			num += count_num_of_nodes(ainfo->opt_args, node, info);
			// Save rb_args_info structure content
			varg = rb_ary_new();
			rb_ary_push(varg, value_to_str((VALUE) ainfo->pre_init));
			rb_ary_push(varg, value_to_str((VALUE) ainfo->post_init));
			rb_ary_push(varg, INT2FIX(ainfo->pre_args_num));
			rb_ary_push(varg, INT2FIX(ainfo->post_args_num));

			asym = ainfo->first_post_arg; rb_ary_push(varg, INT2FIX(asym)); // ID
			if (asym != 0)
				LeafTableInfo_addIDEntry(&info->syms, asym);

			asym = ainfo->rest_arg; rb_ary_push(varg, INT2FIX(asym)); // ID
			if (asym != 0)
				LeafTableInfo_addIDEntry(&info->syms, asym);

			asym = ainfo->block_arg; rb_ary_push(varg, INT2FIX(asym)); // ID
			if (asym != 0)
				LeafTableInfo_addIDEntry(&info->syms, asym);
			rb_ary_push(varg, value_to_str((VALUE) ainfo->kw_args));
			rb_ary_push(varg, value_to_str((VALUE) ainfo->kw_rest_arg));
			rb_ary_push(varg, value_to_str((VALUE) ainfo->opt_args));

			LeafTableInfo_addEntry(&info->args, value_to_str((VALUE) ainfo), varg);
#else
			rb_raise(rb_eArgError, "NT_ARGS entry without USE_RB_ARGS_INFO");
#endif
		}
		else if (ut[2] == NT_ENTRY)
		{
			ID gsym = node->u3.entry->id;
			// Save symbol to the symbol table
			int newid = LeafTableInfo_addIDEntry(&info->syms, gsym);
			LeafTableInfo_addEntry(&info->gentries, value_to_str(node->u3.value), INT2FIX(newid));
		}
		else if (ut[2] != NT_LONG && ut[2] != NT_NULL)
		{
			rb_raise(rb_eArgError, "Invalid child node 3 of node %s: TYPE %d, VALUE %"PRIxPTR,
				ruby_node_name(nd_type(node)), ut[2], (uintptr_t) (node->u3.value));
		}

		return num;
	}
}



//-------------------------------------------------------------------------

/*
 * Part 4. Functions for loading marshalled nodes
 */
typedef struct {
	ID *syms_adr; // Table of symbols
	int syms_len;

	VALUE *lits_adr; // Table of literals
	int lits_len;

	ID **idtbls_adr; // Table of symbols tables
	int idtbls_len;

	struct rb_global_entry **gvars_adr; // Table of global variables entries
	int gvars_len;

	NODE **nodes_adr; // Table of nodes
	int nodes_len;
#ifdef USE_RB_ARGS_INFO
	struct rb_args_info **args_adr; // Table of code blocks arguments
	int args_len;
#endif
} NODEObjAddresses;


void NODEObjAddresses_free(NODEObjAddresses *obj)
{
	xfree(obj->syms_adr);
	xfree(obj->idtbls_adr);
	xfree(obj->gvars_adr);
	xfree(obj->nodes_adr);
#ifdef USE_RB_ARGS_INFO
	xfree(obj->args_adr);
#endif
	xfree(obj);
}



void rbstr_printf(VALUE str, const char *fmt, ...)
{
	char buf[1024];
	va_list ptr;

	va_start(ptr, fmt);
	vsprintf(buf, fmt, ptr);
	rb_str_append(str, rb_str_new2(buf));
	va_end(ptr);
}

#define PRINT_NODE_TAB for (j = 0; j < tab; j++) rbstr_printf(str, "  ");
static void print_node(VALUE str, NODE *node, int tab, int show_offsets)
{
	int i, j, type, ut[3];
	VALUE uref[3];

	PRINT_NODE_TAB
	if (node == NULL)
	{
		rbstr_printf(str, "(NULL)\n");
		return;
	}
	type = nd_type(node);

	if (show_offsets)
	{
		rbstr_printf(str, "@ %s | %16"PRIxPTR " %16"PRIxPTR " %16"PRIxPTR "\n", ruby_node_name(type),
			(intptr_t) node->u1.value, (intptr_t) node->u2.value, (intptr_t) node->u3.value);
	}
	else
	{
		rbstr_printf(str, "@ %s\n", ruby_node_name(type));
	}

	ut[0] = nodes_ctbl[type * 3];
	ut[1] = nodes_ctbl[type * 3 + 1];
	ut[2] = nodes_ctbl[type * 3 + 2];

	uref[0] = node->u1.value;
	uref[1] = node->u2.value;
	uref[2] = node->u3.value;

	for (i = 0; i < 3; i++)
	{

		if (ut[i] == NT_NODE)
		{
			if (nd_type(node) != NODE_OP_ASGN2 || i != 2)
				print_node(str, RNODE(uref[i]), tab + 1, show_offsets);
			else
			{
				if (ut[i] != 0 && TYPE(ut[i]) != T_NODE)
					rb_raise(rb_eArgError, "print_node: broken node 0x%s", RSTRING_PTR(value_to_str(ut[i])));
				PRINT_NODE_TAB;	rbstr_printf(str, "  ");
				rbstr_printf(str, "%"PRIxPTR " %"PRIxPTR " %"PRIxPTR"\n",
					(intptr_t) RNODE(uref[i])->u1.value,
					(intptr_t) RNODE(uref[i])->u2.value,
					(intptr_t) RNODE(uref[i])->u3.value);
			}
		}
		else if (ut[i] == NT_VALUE)
		{
			char *class_name = RSTRING_PTR(rb_funcall(rb_funcall(uref[i], rb_intern("class"), 0), rb_intern("to_s"), 0));
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			if (show_offsets)
			{
				rbstr_printf(str, ">| ADR: %"PRIxPTR"; CLASS: %s (TYPE %d); VALUE: %s\n",
					(intptr_t) uref[i],
					class_name, TYPE(uref[i]),
					RSTRING_PTR(rb_funcall(uref[i], rb_intern("to_s"), 0)));
			}
			else
			{
				rbstr_printf(str, ">| CLASS: %s (TYPE %d); VALUE: %s\n",
					class_name, TYPE(uref[i]),
					RSTRING_PTR(rb_funcall(uref[i], rb_intern("to_s"), 0)));
			}
		}
		else if (ut[i] == NT_ID)
		{
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			const char *str_null = "<NULL>", *str_intern = "<NONAME>";
			const char *str_sym;
			if (uref[i] == 0)
				str_sym = str_null;
			else
			{
				VALUE rbstr_sym = rb_id2str(uref[i]);
				if (TYPE(rbstr_sym) == T_STRING)
					str_sym = RSTRING_PTR(rb_id2str(uref[i]));
				else
					str_sym = str_intern;
			}

			if (show_offsets)
				rbstr_printf(str, ">| ID: %d; SYMBOL: :%s\n", (ID) uref[i], str_sym);
			else
				rbstr_printf(str, ">| SYMBOL: :%s\n", str_sym);
		}
		else if (ut[i] == NT_LONG)
		{
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			rbstr_printf(str, ">| %"PRIxPTR "\n", (intptr_t) uref[i]);
		}
		else if (ut[i] == NT_NULL)
		{
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			rbstr_printf(str, ">| (NULL)\n");
		}
		else if (ut[i] == NT_ARGS)
		{
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			rbstr_printf(str, ">| ARGS\n");
		}
		else if (ut[i] == NT_IDTABLE)
		{
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			rbstr_printf(str, ">| IDTABLE\n");
		}
		else if (ut[i] == NT_ENTRY)
		{
			struct rb_global_entry *gentry;
			gentry = (struct rb_global_entry *) uref[i];
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			rbstr_printf(str, ">| [GLOBAL ENTRY PTR=0x%"PRIxPTR" ID=%X]\n", (uintptr_t) gentry->var, gentry->id);
		}
		else
		{
			PRINT_NODE_TAB; rbstr_printf(str, "  ");
			rbstr_printf(str, ">| [UNKNOWN]\n");
		}
	}
}



void resolve_syms_ords(VALUE data, NODEObjAddresses *relocs)
{
	VALUE tbl_val = rb_hash_aref(data, ID2SYM(rb_intern("symbols")));
	int i;
	if (tbl_val == Qnil)
	{
		rb_raise(rb_eArgError, "Cannot find symbols table");
	}
	if (TYPE(tbl_val) != T_ARRAY)
	{
		rb_raise(rb_eArgError, "Symbols table is not an array");
	}
	relocs->syms_len = RARRAY_LEN(tbl_val);
	relocs->syms_adr = ALLOC_N(ID, relocs->syms_len);
	for (i = 0; i < relocs->syms_len; i++)
	{
		VALUE r_sym = RARRAY_PTR(tbl_val)[i];
		if (TYPE(r_sym) == T_STRING)
		{
			relocs->syms_adr[i] = rb_intern(RSTRING_PTR(r_sym));
		}
		else if (TYPE(r_sym) == T_FIXNUM)
		{
			relocs->syms_adr[i] = (ID) FIX2INT(r_sym);
		}
		else
		{
			rb_raise(rb_eArgError, "Symbols table is corrupted");
		}
	}
}

void resolve_lits_ords(VALUE data, NODEObjAddresses *relocs)
{
	VALUE tbl_val = rb_hash_aref(data, ID2SYM(rb_intern("literals")));
	if (tbl_val == Qnil)
	{
		rb_raise(rb_eArgError, "Cannot find literals table");
	}
	if (TYPE(tbl_val) != T_ARRAY)
	{
		rb_raise(rb_eArgError, "Literals table is not an array");
	}
	relocs->lits_adr = RARRAY_PTR(tbl_val);
	relocs->lits_len = RARRAY_LEN(tbl_val);
}

void resolve_gvars_ords(VALUE data, NODEObjAddresses *relocs)
{
	VALUE tbl_val = rb_hash_aref(data, ID2SYM(rb_intern("global_entries")));
	int i;

	if (tbl_val == Qnil)
	{
		rb_raise(rb_eArgError, "Cannot find global entries table");
	}
	if (TYPE(tbl_val) != T_ARRAY)
	{
		rb_raise(rb_eArgError, "Global entries table should be an array");
	}
	relocs->gvars_len = RARRAY_LEN(tbl_val);
	relocs->gvars_adr = ALLOC_N(struct rb_global_entry *, relocs->gvars_len);
	for (i = 0; i < relocs->gvars_len; i++)
	{
		int ind = FIX2INT(RARRAY_PTR(tbl_val)[i]);
		ID sym = relocs->syms_adr[ind];
		relocs->gvars_adr[i] = rb_global_entry(sym);
	}
}


void resolve_idtbls_ords(VALUE data, NODEObjAddresses *relocs)
{
	VALUE tbl_val = rb_hash_aref(data, ID2SYM(rb_intern("id_tables")));
	int i, j, idnum;
	
	if (tbl_val == Qnil)
	{
		rb_raise(rb_eArgError, "Cannot find id_tables entries");
	}
	relocs->idtbls_len = RARRAY_LEN(tbl_val);
	relocs->idtbls_adr = ALLOC_N(ID *, relocs->idtbls_len);
	for (i = 0; i < relocs->idtbls_len; i++)
	{
		VALUE idtbl = RARRAY_PTR(tbl_val)[i];
		idnum = RARRAY_LEN(idtbl);
		if (idnum == 0)
		{	// Empty table: NULL pointer in the address table
			relocs->idtbls_adr[i] = NULL;
		}
		else
		{	// Filled table: pointer to dynamic memory
			relocs->idtbls_adr[i] = ALLOC_N(ID, idnum + 1);
			relocs->idtbls_adr[i][0] = idnum;
			for (j = 0; j < idnum; j++)
			{
				int ind = FIX2INT(RARRAY_PTR(idtbl)[j]);
				relocs->idtbls_adr[i][j+1] = relocs->syms_adr[ind];
			}
		}
	}
}

void resolve_nodes_ords(VALUE data, int num_of_nodes, NODEObjAddresses *relocs)
{
	int i;
	VALUE tbl_val = rb_hash_aref(data, ID2SYM(rb_intern("nodes")));
	if (tbl_val == Qnil)
	{
		rb_raise(rb_eArgError, "Cannot find nodes entries");
	}
	if (TYPE(tbl_val) != T_STRING)
	{
		rb_raise(rb_eArgError, "Nodes description must be a string");
	}
	relocs->nodes_adr = ALLOC_N(NODE *, num_of_nodes);
	relocs->nodes_len = num_of_nodes;
	for (i = 0; i < num_of_nodes; i++)
	{
		relocs->nodes_adr[i] = (NODE *) NEW_NODE((enum node_type) 0, 0, 0, 0);
	}
}

#ifdef USE_RB_ARGS_INFO
void resolve_args_ords(VALUE data, NODEObjAddresses *relocs)
{
	int i;
	VALUE tbl_val = rb_hash_aref(data, ID2SYM(rb_intern("args")));

	if (tbl_val == Qnil)
	{
		rb_raise(rb_eArgError, "Cannot find args entries table");
	}
	if (TYPE(tbl_val) != T_ARRAY)
	{
		rb_raise(rb_eArgError, "args description must be an array");
	}
	relocs->args_len = RARRAY_LEN(tbl_val);
	relocs->args_adr = ALLOC_N(struct rb_args_info *, relocs->args_len);
	for (i = 0; i < relocs->args_len; i++)
	{
		int ord;
		VALUE ainfo_val, *aiptr;
		struct rb_args_info *ainfo;

		relocs->args_adr[i] = ALLOC(struct rb_args_info);
		ainfo_val = RARRAY_PTR(tbl_val)[i];
		aiptr = RARRAY_PTR(ainfo_val);
		ainfo = relocs->args_adr[i];

		if (TYPE(ainfo_val) != T_ARRAY || RARRAY_LEN(ainfo_val) != 10)
		{
			rb_raise(rb_eArgError, "args entry %d is corrupted", i);
		}
		// Load unresolved values
		ainfo->pre_init = (NODE *) FIX2LONG(aiptr[0]); // Node ordinal
		ainfo->post_init = (NODE *) FIX2LONG(aiptr[1]); // Node ordinal
		ainfo->pre_args_num = FIX2INT(aiptr[2]); // No ordinal resolving
		ainfo->post_args_num = FIX2INT(aiptr[3]); // No ordinal resolving
		ainfo->first_post_arg = FIX2INT(aiptr[4]); // Symbolic ordinal
		ainfo->rest_arg = FIX2INT(aiptr[5]); // Symbolic ordinal
		ainfo->block_arg = FIX2INT(aiptr[6]); // Symbolic ordinal
		ainfo->kw_args = (NODE *) FIX2LONG(aiptr[7]); // Node ordinal
		ainfo->kw_rest_arg = (NODE *) FIX2LONG(aiptr[8]); // Node ordinal
		ainfo->opt_args = (NODE *) FIX2LONG(aiptr[9]); // Node ordinal
		// Resolve nodes
		ord = (int) (((VALUE) ainfo->pre_init) & 0xFFFFFFFF);
		if (ord < -1 || ord >= relocs->nodes_len)
			rb_raise(rb_eArgError, "Invalid node ordinal %d", ord);
		ainfo->pre_init = (ord == -1) ? NULL : relocs->nodes_adr[ord];

		ord = (int) (((VALUE) ainfo->post_init) & 0xFFFFFFFF);
		if (ord < -1 || ord >= relocs->nodes_len)
			rb_raise(rb_eArgError, "Invalid node ordinal %d", ord);
		ainfo->post_init = (ord == -1) ? NULL : relocs->nodes_adr[ord];

		ord = (int) (((VALUE) ainfo->kw_args) & 0xFFFFFFFF);
		if (ord < -1 || ord >= relocs->nodes_len)
			rb_raise(rb_eArgError, "Invalid node ordinal %d", ord);
		ainfo->kw_args = (ord == -1) ? NULL : relocs->nodes_adr[ord];

		ord = (int) (((VALUE) ainfo->kw_rest_arg) & 0xFFFFFFFF);
		if (ord < -1 || ord >= relocs->nodes_len)
			rb_raise(rb_eArgError, "Invalid node ordinal %d", ord);
		ainfo->kw_rest_arg = (ord == -1) ? NULL : relocs->nodes_adr[ord];

		ord = (int) (((VALUE) ainfo->opt_args) & 0xFFFFFFFF);
		if (ord < -1 || ord >= relocs->nodes_len)
			rb_raise(rb_eArgError, "Invalid node ordinal %d", ord);
		ainfo->opt_args = (ord == -1) ? NULL : relocs->nodes_adr[ord];
		// Resolve symbolic ordinals
		ord = ainfo->first_post_arg;
		if (ord < -1 || ord >= relocs->syms_len)
			rb_raise(rb_eArgError, "1- Invalid symbol ID ordinal %d", ord);
		ainfo->first_post_arg = (ord == -1) ? 0 : relocs->syms_adr[ord];

		ord = ainfo->rest_arg;
		if (ord < -1 || ord >= relocs->syms_len)
			rb_raise(rb_eArgError, "2- Invalid symbol ID ordinal %d", ord);
		ainfo->rest_arg = (ord == -1) ? 0 : relocs->syms_adr[ord];

		ord = ainfo->block_arg;
		if (ord < -1 || ord >= relocs->syms_len)
			rb_raise(rb_eArgError, "3- Invalid symbol ID ordinal %d", ord);
		ainfo->block_arg = (ord == -1) ? 0 : relocs->syms_adr[ord];
	}
}
#endif

void load_nodes_from_str(VALUE data, NODEObjAddresses *relocs)
{
	int i, j;
	VALUE tbl_val = rb_hash_aref(data, ID2SYM(rb_intern("nodes")));
	unsigned char *bin = (unsigned char *) RSTRING_PTR(tbl_val);
	NODE *node = NULL;
	for (i = 0; i < relocs->nodes_len; i++)
	{
		int rtypes[4];
		VALUE u[3], flags;
		// Read data structure info
		for (j = 0; j < 4; j++)
			rtypes[j] = *bin++;
		flags = bin_to_value(bin, rtypes[3]); bin += rtypes[3];
		for (j = 0; j < 3; j++)
		{
			int val_len = (rtypes[j] & 0xF0) >> 4;
			u[j] = bin_to_value(bin, val_len);
			bin += val_len;
			rtypes[j] &= 0x0F;
			
		}
		if ((char *)bin - RSTRING_PTR(tbl_val) > RSTRING_LEN(tbl_val))
			rb_raise(rb_eArgError, "Nodes binary dump is too short");
		// Resolving all addresses
		for (j = 0; j < 3; j++)
		{
			switch(rtypes[j])
			{
			case VL_RAW: // Do nothing: it is raw data
				break;
			case VL_NODE:
				if (u[j] >= (unsigned int) relocs->nodes_len)
					rb_raise(rb_eArgError, "Cannot resolve VL_NODE entry %d", (int) u[j]);
				u[j] = (VALUE) relocs->nodes_adr[u[j]];
				if (TYPE(u[j]) != T_NODE)
					rb_raise(rb_eArgError, "load_nodes_from_str: nodes memory corrupted");
				break;
			case VL_ID:
				if (u[j] >= (unsigned int) relocs->syms_len)
					rb_raise(rb_eArgError, "Cannot resolve VL_ID entry %d", (int) u[j]);
				u[j] = relocs->syms_adr[u[j]];
				break;
			case VL_GVAR:
				if (u[j] >= (unsigned int) relocs->gvars_len)
					rb_raise(rb_eArgError, "Cannot resolve VL_GVAR entry %d", (int) u[j]);
				u[j] = (VALUE) relocs->gvars_adr[u[j]];
				break;
			case VL_IDTABLE:
				if (u[j] >= (unsigned int) relocs->idtbls_len)
					rb_raise(rb_eArgError, "Cannot resolve VL_IDTABLE entry %d", (int) u[j]);
				u[j] = (VALUE) relocs->idtbls_adr[u[j]];
				break;
#ifdef USE_RB_ARGS_INFO
			case VL_ARGS:
				if (u[j] >= (unsigned int) relocs->args_len)
					rb_raise(rb_eArgError, "Cannot resolve VL_ARGS entry %d", (int) u[j]);
				u[j] = (VALUE) relocs->args_adr[u[j]];
				break;
#endif
			case VL_LIT:
				if (u[j] >= (unsigned int) relocs->lits_len)
					rb_raise(rb_eArgError, "Cannot resolve VL_LIT entry %d", (int) u[j]);
				u[j] = (VALUE) relocs->lits_adr[u[j]];
				break;
			default:
				rb_raise(rb_eArgError, "Unknown RTYPE %d", rtypes[j]);
			}
		}

		// Fill classic node structure
		node = relocs->nodes_adr[i];
#ifdef RESET_GC_FLAGS
		flags = flags & (~0x3); // Ruby 1.9.x -- specific thing
#endif
		node->flags = (flags << 5) | T_NODE;
		node->nd_reserved = 0;
		node->u1.value = u[0];
		node->u2.value = u[1];
		node->u3.value = u[2];
	}	
}

/*
 * Returns the value of string hash field using symbolic key
 */
static VALUE get_hash_strfield(VALUE hash, const char *idtxt)
{
	VALUE str = rb_hash_aref(hash, ID2SYM(rb_intern(idtxt)));
	if (TYPE(str) != T_STRING)
	{
		rb_raise(rb_eArgError, "Hash field %s is not a string", idtxt);
		return Qnil;
	}
	else
	{
		return str;
	}
}

/* 
 * Check validity of node hash representation signatures ("magic" values)
 */
static VALUE check_hash_magic(VALUE data)
{
	VALUE val, refval;
	// MAGIC signature must be valid
	val = get_hash_strfield(data, "MAGIC");
	if (strcmp(NODEMARSHAL_MAGIC, RSTRING_PTR(val)))
		rb_raise(rb_eArgError, "Bad value of MAGIC signature");
	// RUBY_PLATFORM signature must match the current platform
	val = get_hash_strfield(data, "RUBY_PLATFORM"); 
	refval = rb_const_get(rb_cObject, rb_intern("RUBY_PLATFORM"));
	if (strcmp(RSTRING_PTR(refval), RSTRING_PTR(val)))
		rb_raise(rb_eArgError, "Incompatible RUBY_PLATFORM value %s", RSTRING_PTR(val));
	// RUBY_VERSION signature must match the used Ruby interpreter
	val = get_hash_strfield(data, "RUBY_VERSION");
	refval = rb_const_get(rb_cObject, rb_intern("RUBY_VERSION"));
	if (strcmp(RSTRING_PTR(refval), RSTRING_PTR(val)))
		rb_raise(rb_eArgError, "Incompatible RUBY_VERSION value %s", RSTRING_PTR(val));
	return Qtrue;
}

/*
 * Part 5. C-to-Ruby interface
 * 
 */

/*
 * Restore Ruby node from the binary blob (dump)
 */
static VALUE m_nodedump_from_memory(VALUE self, VALUE dump)
{
	VALUE cMarshal, data, val, val_relocs;
	int num_of_nodes;
	NODEObjAddresses *relocs;
	/* DISABLE GARBAGE COLLECTOR (required for stable loading
	   of large node trees */
	rb_gc_disable();
	/* Wrap struct for relocations */
	val_relocs = Data_Make_Struct(cNodeObjAddresses, NODEObjAddresses,
		NULL, NODEObjAddresses_free, relocs); // This data envelope cannot exist without NODE
	/* Load and unpack our dump */
	cMarshal = rb_const_get(rb_cObject, rb_intern("Marshal"));
	data = rb_funcall(cMarshal, rb_intern("load"), 1, dump);
	if (TYPE(data) != T_HASH)
	{
		rb_raise(rb_eArgError, "Input dump is corrupted");
	}
	val = rb_hash_aref(data, ID2SYM(rb_intern("num_of_nodes")));
	if (val == Qnil)
	{
		rb_raise(rb_eArgError, "num_of_nodes not found");
	}
	else
	{
		num_of_nodes = FIX2INT(val);
	}
	/* Check "magic" signature and platform identifiers */
	check_hash_magic(data);
	/* Get the information about the source file that was compiled to the node */
	// a) node name
	val = rb_hash_aref(data, ID2SYM(rb_intern("nodename")));
	if (val == Qnil || TYPE(val) == T_STRING)
		rb_iv_set(self, "@nodename", val);
	else
		rb_raise(rb_eArgError, "nodename value is corrupted");
	// b) file name
	val = rb_hash_aref(data, ID2SYM(rb_intern("filename")));
	if (val == Qnil || TYPE(val) == T_STRING)
		rb_iv_set(self, "@filename", val);
	else
		rb_raise(rb_eArgError, "filename value is corrupted");
	// c) file path
	val = rb_hash_aref(data, ID2SYM(rb_intern("filepath")));
	if (val == Qnil || TYPE(val) == T_STRING)
		rb_iv_set(self, "@filepath", val);
	else
		rb_raise(rb_eArgError, "filepath value is corrupted");
	/* Load all required data */
	resolve_syms_ords(data, relocs); // Symbols
	resolve_lits_ords(data, relocs); // Literals
	resolve_gvars_ords(data, relocs); // Global entries (with symbol ID resolving)
	resolve_idtbls_ords(data, relocs); // Identifiers tables (with symbol ID resolving)
	resolve_nodes_ords(data, num_of_nodes, relocs); // Allocate memory for all nodes
#ifdef USE_RB_ARGS_INFO
	resolve_args_ords(data, relocs); // Load args entries with symbols ID and nodes resolving
#endif
	load_nodes_from_str(data, relocs);
	/* Save the loaded node tree and collect garbage */
	rb_iv_set(self, "@node", (VALUE) relocs->nodes_adr[0]);
	rb_iv_set(self, "@num_of_nodes", INT2FIX(num_of_nodes));
	rb_iv_set(self, "@obj_addresses", val_relocs);
	rb_gc_enable();
	rb_gc_start();
	return self;
}


/*
 * call-seq:
 *   obj.symbols
 *
 * Return array with the list of symbols
 */
static VALUE m_nodedump_symbols(VALUE self)
{
	int i;
	VALUE val_relocs, val_nodeinfo, syms;
	// Variant 1: node loaded from file
	val_relocs = rb_iv_get(self, "@obj_addresses");
	if (val_relocs != Qnil)
	{
		NODEObjAddresses *relocs;
		Data_Get_Struct(val_relocs, NODEObjAddresses, relocs);
		syms = rb_ary_new();
		for (i = 0; i < relocs->syms_len; i++)
			rb_ary_push(syms, ID2SYM(relocs->syms_adr[i]));
		return syms;
	}
	// Variant 2: node saved to file (parsed from memory)
	val_nodeinfo = rb_iv_get(self, "@nodeinfo");
	if (val_nodeinfo != Qnil)
	{
		NODEInfo *ninfo;
		VALUE *ary;
		Data_Get_Struct(val_nodeinfo, NODEInfo, ninfo);
		syms = rb_funcall(ninfo->syms.vals, rb_intern("values"), 0);
		ary = RARRAY_PTR(syms);
		for (i = 0; i < RARRAY_LEN(syms); i++)
		{
			ary[i] = rb_funcall(ary[i], rb_intern("to_sym"), 0);
		}
		return syms;
	}
	rb_raise(rb_eArgError, "Symbol information not initialized. Run to_hash before reading.");
}

/*
 * call-seq:
 *   obj.change_symbol(old_sym, new_sym)
 *
 * Replace one symbol by another (to be used for code obfuscation)
 * - +old_sym+ -- String that contains symbol name to be replaced 
 * - +new_sym+ -- String that contains new name of the symbol
 */
static VALUE m_nodedump_change_symbol(VALUE self, VALUE old_sym, VALUE new_sym)
{
	VALUE val_nodehash = rb_iv_get(self, "@nodehash");
	VALUE syms, key;
	// Check if node is position-independent
	// (i.e. with initialized NODEInfo structure that contains
	// relocations for symbols)
	if (val_nodehash == Qnil)
		rb_raise(rb_eArgError, "This node is not preparsed into Hash");
	// Check data types of the input array
	if (TYPE(old_sym) != T_STRING)
	{
		rb_raise(rb_eArgError, "old_sym argument must be a string");
	}
	if (TYPE(new_sym) != T_STRING)
	{
		rb_raise(rb_eArgError, "new_sym argument must be a string");
	}
	// Get the symbol table from the Hash
	syms = rb_hash_aref(val_nodehash, ID2SYM(rb_intern("symbols")));
	if (syms == Qnil)
		rb_raise(rb_eArgError, "Preparsed hash has no :symbols field");
	// Check if new_sym is present in the symbol table
	key = rb_funcall(syms, rb_intern("find_index"), 1, new_sym);
	if (key != Qnil)
	{
		rb_raise(rb_eArgError, "new_sym value must be absent in table of symbols");
	}
	// Change the symbol in the preparsed Hash
	key = rb_funcall(syms, rb_intern("find_index"), 1, old_sym);
	if (key == Qnil)
		return Qnil;
	RARRAY_PTR(syms)[FIX2INT(key)] = new_sym;
	return self;
}

/*
 * Return array with the list of literals
 */
static VALUE m_nodedump_literals(VALUE self)
{
	int i;
	VALUE val_relocs, val_nodeinfo, lits;
	// Variant 1: node loaded from file. It uses NODEObjAddresses struct
	// with the results of Ruby NODE structure parsing.
	val_relocs = rb_iv_get(self, "@obj_addresses");
	if (val_relocs != Qnil)
	{
		NODEObjAddresses *relocs;

		Data_Get_Struct(val_relocs, NODEObjAddresses, relocs);
		lits = rb_ary_new();
		for (i = 0; i < relocs->lits_len; i++)
		{
			VALUE val = relocs->lits_adr[i];
			int t = TYPE(val);
			if (t != T_SYMBOL && t != T_FLOAT && t != T_FIXNUM)
				val = rb_funcall(val, rb_intern("dup"), 0);
			rb_ary_push(lits, val);
		}
		return lits;
	}
	// Variant 2: node saved to file (parsed from memory). It uses
	// NODEInfo struct that is initialized during node dump parsing.
	val_nodeinfo = rb_iv_get(self, "@nodeinfo");
	if (val_nodeinfo != Qnil)
	{
		NODEInfo *ninfo;
		VALUE *ary;
		Data_Get_Struct(val_nodeinfo, NODEInfo, ninfo);
		lits = rb_funcall(ninfo->lits.vals, rb_intern("values"), 0);
		ary = RARRAY_PTR(lits);
		for (i = 0; i < RARRAY_LEN(lits); i++)
		{
			int t = TYPE(ary[i]);
			if (t != T_SYMBOL && t != T_FLOAT && t != T_FIXNUM)
				ary[i] = rb_funcall(ary[i], rb_intern("dup"), 0);
		}
		return lits;
	}
	rb_raise(rb_eArgError, "Literals information not initialized. Run to_hash before reading.");
}

/*
 * Update the array with the list of literals
 * (to be used for code obfuscation)
 * Warning! This function is a stub!
 */
static VALUE m_nodedump_change_literal(VALUE self, VALUE old_lit, VALUE new_lit)
{
    /* TO BE IMPLEMENTED */
    return self;
}


/*
 * call-seq:
 *   obj.compile
 *
 * Creates the RubyVM::InstructionSequence object from the node
 */
static VALUE m_nodedump_compile(VALUE self)
{
	NODE *node = RNODE(rb_iv_get(self, "@node"));
	VALUE nodename = rb_iv_get(self, "@nodename");
	VALUE filename = rb_iv_get(self, "@filename");
	VALUE filepath = rb_iv_get(self, "@filepath");
#ifndef WITH_RB_ISEQW_NEW
	/* For Pre-2.3 */
	return rb_iseq_new_top(node, nodename, filename, filepath, Qfalse);
#else
	/* For Ruby 2.3 */
	return rb_iseqw_new(rb_iseq_new_top(node, nodename, filename, filepath, Qfalse));
#endif
}

/*
 * Parses Ruby file with the source code and saves the node
 */
static VALUE m_nodedump_from_source(VALUE self, VALUE file)
{
	VALUE line = INT2FIX(1), f, node, filepath;
	const char *fname;

	rb_gc_disable();
	rb_secure(1);
	FilePathValue(file);
	fname = StringValueCStr(file);
	/* Remember information about the file */
	rb_iv_set(self, "@nodename", rb_str_new2("<main>"));
	rb_iv_set(self, "@filename", file);
	filepath = rb_funcall(rb_cFile, rb_intern("realpath"), 1, file); // Envelope for rb_realpath_internal
	rb_iv_set(self, "@filepath", filepath);
	/* Create node from the source */
	f = rb_file_open_str(file, "r");
	node = (VALUE) rb_compile_file(fname, f, NUM2INT(line));
	rb_gc_enable();
	rb_iv_set(self, "@node", node);
	if ((void *) node == NULL)
	{
		rb_raise(rb_eArgError, "Error during string parsing");
	}
	return self;
}

/*
 * Parses Ruby string with the source code and saves the node
 */
static VALUE m_nodedump_from_string(VALUE self, VALUE str)
{
	VALUE line = INT2FIX(1), node;
	const char *fname = "STRING";
	Check_Type(str, T_STRING);
	rb_secure(1);
	/* Create empty information about the file */
	rb_iv_set(self, "@nodename", rb_str_new2("<main>"));
	if (RUBY_API_VERSION_MAJOR == 1)
	{	/* For Ruby 1.9.x */
		rb_iv_set(self, "@filename", Qnil);
		rb_iv_set(self, "@filepath", Qnil);
	}
	else
	{	/* For Ruby 2.x */
		rb_iv_set(self, "@filename", rb_str_new2("<compiled>"));
		rb_iv_set(self, "@filepath", rb_str_new2("<compiled>"));
	}
	/* Create node from the string */
	rb_gc_disable();
	node = (VALUE) rb_compile_string(fname, str, NUM2INT(line));
	rb_iv_set(self, "@node", node);
	rb_gc_enable();
	rb_gc_start();
	if ((void *) node == NULL)
	{
		rb_raise(rb_eArgError, "Error during string parsing");
	}
	return self;
}

/*
 * call-seq:
 *   obj.new(:srcfile, filename) # Will load source file from the disk
 *   obj.new(:binfile, filename) # Will load file with node binary dump from the disk
 *   obj.new(:srcmemory, srcstr) # Will load source code from the string
 *   obj.new(:binmemory, binstr) # Will load node binary dump from the string
 * 
 * Creates NodeMarshal class example from the source code or dumped
 * syntax tree (NODEs), i.e. preparsed and packed source code. Created
 * object can be used either for code execution or for saving it
 * in the preparsed form (useful for code obfuscation/protection)
 */
static VALUE m_nodedump_init(VALUE self, VALUE source, VALUE info)
{
	ID id_usr;
	rb_iv_set(self, "@show_offsets", Qfalse);
	Check_Type(source, T_SYMBOL);
	id_usr = SYM2ID(source);
	if (id_usr == rb_intern("srcfile"))
	{
		return m_nodedump_from_source(self, info);
	}
	else if (id_usr == rb_intern("srcmemory"))
	{
		return m_nodedump_from_string(self, info);
	}
	else if (id_usr == rb_intern("binmemory"))
	{
		return m_nodedump_from_memory(self, info);
	}
	else if (id_usr == rb_intern("binfile"))
	{
		VALUE cFile = rb_const_get(rb_cObject, rb_intern("File"));
		VALUE bin = rb_funcall(cFile, rb_intern("binread"), 1, info);
		return m_nodedump_from_memory(self, bin);
	}
	else
	{
		rb_raise(rb_eArgError, "Invalid source type (it must be :srcfile, :srcmemory, :binmemory of :binfile)");
	}
	return Qnil;
}

/*
 * call-seq:
 *   obj.dump_tree
 * 
 * Transforms Ruby syntax tree (NODE) to the String using
 * +rb_parser_dump_tree+ function from +node.c+ (see Ruby source code).
 */
static VALUE m_nodedump_parser_dump_tree(VALUE self)
{
	NODE *node = RNODE(rb_iv_get(self, "@node"));
	return rb_parser_dump_tree(node, 0);
}

/*
 * call-seq:
 *   obj.dump_tree_short
 *
 * Transforms Ruby syntax tree (NODE) to the String using custom function
 * instead of +rb_parser_dump_tree+ function.
 *
 * See also #show_offsets, #show_offsets=
 */
static VALUE m_nodedump_dump_tree_short(VALUE self)
{
	VALUE str = rb_str_new2(""); // Output string
	NODE *node = RNODE(rb_iv_get(self, "@node"));
	int show_offsets = (rb_iv_get(self, "@show_offsets") == Qtrue) ? 1 : 0;
	print_node(str, node, 0, show_offsets);
	return str;
}

/*
 * call-seq:
 *   obj.show_offsets
 *
 * Returns show_offsets property (used by NodeMarshal#dump_tree_short)
 * It can be either true or false
 */
static VALUE m_nodedump_show_offsets(VALUE self)
{
	return rb_iv_get(self, "@show_offsets");
}

/*
 * call-seq:
 *   obj.show_offsets=
 *
 * Sets show_offsets property (used by NodeMarshal#dump_tree_short)
 * It can be either true or false
 */
static VALUE m_nodedump_set_show_offsets(VALUE self, VALUE value)
{
	if (value != Qtrue && value != Qfalse)
	{
		rb_raise(rb_eArgError, "show_offsets property must be either true or false");
	}
	return rb_iv_set(self, "@show_offsets", value);
}


/*
 * call-seq:
 *   obj.to_hash
 * 
 * Converts NodeMarshal class example to the hash that contains full
 * and independent from data structures memory addresses information.
 * Format of the obtained hash depends on used platform (especially
 * size of the pointer) and Ruby version.
 *
 * <b>Format of the hash</b>
 * 
 * <i>Part 1: Signatures</i>
 *
 * - <tt>MAGIC</tt> -- NODEMARSHAL11
 * - <tt>RUBY_PLATFORM</tt> -- saved <tt>RUBY_PLATFORM</tt> constant value
 * - <tt>RUBY_VERSION</tt>  -- saved <tt>RUBY_VERSION</tt> constant value
 *
 * <i>Part 2: Program loadable elements.</i>
 *
 * All loadable elements are arrays. Index of the array element means
 * its identifier that is used in the node tree.
 *
 * - <tt>literals</tt> -- program literals (strings, ranges etc.)
 * - <tt>symbols</tt> -- program symbols (values have either String or Fixnum
 *   data type; numbers are used for symbols that cannot be represented as strings)
 * - <tt>global_entries</tt> -- global variables information
 * - <tt>id_tables</tt> -- array of arrays. Each array contains symbols IDs
 * - <tt>args</tt> -- information about code block argument(s)
 * 
 * <i>Part 3: Nodes information</i>
 * - <tt>nodes</tt> -- string that contains binary encoded information
 *   about the nodes
 * - <tt>num_of_nodes</tt> -- number of nodes in the <tt>nodes</tt> field
 * - <tt>nodename</tt> -- name of the node (usually "<main>")
 * - <tt>filename</tt> -- name (without path) of .rb file used for the node generation
 * - <tt>filepath</tt> -- name (with full path) of .rb file used for the node generation
 */
static VALUE m_nodedump_to_hash(VALUE self)
{
	NODE *node = RNODE(rb_iv_get(self, "@node"));
	NODEInfo *info;
	VALUE ans, num, val_info;
	// DISABLE GARBAGE COLLECTOR (important for dumping)
	rb_gc_disable();
	// Convert the node to the form with relocs (i.e. the information about node)
	// if such form is not present
	val_info = rb_iv_get(self, "@nodeinfo");
	if (val_info == Qnil)
	{
		val_info = Data_Make_Struct(cNodeInfo, NODEInfo,
			NODEInfo_mark, NODEInfo_free, info); // This data envelope cannot exist without NODE
		NODEInfo_init(info);
		rb_iv_set(self, "@nodeinfo", val_info);
		num = INT2FIX(count_num_of_nodes(node, node, info));
		rb_iv_set(self, "@nodeinfo_num_of_nodes", num);
		// Convert node to NODEInfo structure
		ans = NODEInfo_toHash(info);
		rb_hash_aset(ans, ID2SYM(rb_intern("num_of_nodes")), num);
		rb_hash_aset(ans, ID2SYM(rb_intern("nodename")), rb_iv_get(self, "@nodename"));
		rb_hash_aset(ans, ID2SYM(rb_intern("filename")), rb_iv_get(self, "@filename"));
		rb_hash_aset(ans, ID2SYM(rb_intern("filepath")), rb_iv_get(self, "@filepath"));
		rb_iv_set(self, "@nodehash", ans);
	}
	else
	{
		ans = rb_iv_get(self, "@nodehash");
	}
	// ENABLE GARBAGE COLLECTOR (important for dumping)
	rb_gc_enable();
	return ans;
}

/*
 * call-seq:
 *   obj.to_bin
 * 
 * Converts NodeMarshal class example to the binary string that
 * can be saved to the file and used for loading the node from the file.
 * Format of the obtained binary dump depends on used platform (especially
 * size of the pointer) and Ruby version.
 */
static VALUE m_nodedump_to_bin(VALUE self)
{
	VALUE hash = m_nodedump_to_hash(self);
	VALUE cMarshal = rb_const_get(rb_cObject, rb_intern("Marshal"));
	return rb_funcall(cMarshal, rb_intern("dump"), 1, hash);
}

/*
 * Gives the information about the node
 */
static VALUE m_nodedump_inspect(VALUE self)
{
	static char str[1024], buf[512];
	VALUE num_of_nodes, nodename, filepath, filename;
	VALUE val_obj_addresses, val_nodeinfo;
	// Get generic information about node
	num_of_nodes = rb_iv_get(self, "@num_of_nodes");
	nodename = rb_iv_get(self, "@nodename");
	filepath = rb_iv_get(self, "@filepath");
	filename = rb_iv_get(self, "@filename");
	// Generate string with generic information about node
	sprintf(str,
		"----- NodeMarshal:0x%"PRIxPTR"\n"
		"    num_of_nodes: %d\n    nodename: %s\n    filepath: %s\n    filename: %s\n",
		(uintptr_t) (self),
		(num_of_nodes == Qnil) ? -1 : FIX2INT(num_of_nodes),
		(nodename == Qnil) ? "nil" : RSTRING_PTR(nodename),
		(filepath == Qnil) ? "nil" : RSTRING_PTR(filepath),
		(filename == Qnil) ? "nil" : RSTRING_PTR(filename)
		);
	// Check if the information about node struct is available
	val_nodeinfo = rb_iv_get(self, "@nodeinfo");
	val_obj_addresses = rb_iv_get(self, "@obj_addresses");
	if (val_nodeinfo == Qnil && val_obj_addresses == Qnil)
	{
		m_nodedump_to_hash(self);
		val_nodeinfo = rb_iv_get(self, "@nodeinfo");
	}
	// Information about preparsed node
	// a) NODEInfo struct
	if (val_nodeinfo == Qnil)
	{
		sprintf(buf, "    NODEInfo struct is empty\n");
	}
	else
	{
		NODEInfo *ninfo;
		Data_Get_Struct(val_nodeinfo, NODEInfo, ninfo);
		sprintf(buf, 
			"    NODEInfo struct:\n"
			"      syms hash len (Symbols):         %d\n"
			"      lits hash len (Literals):        %d\n"
			"      idtabs hash len (ID tables):     %d\n"
			"      gentries hash len (Global vars): %d\n"
			"      nodes hash len (Nodes):          %d\n"
#ifdef USE_RB_ARGS_INFO
			"      args hash len (args info):       %d\n"
#endif
			,
			FIX2INT(rb_funcall(ninfo->syms.vals, rb_intern("length"), 0)),
			FIX2INT(rb_funcall(ninfo->lits.vals, rb_intern("length"), 0)),
			FIX2INT(rb_funcall(ninfo->idtabs.vals, rb_intern("length"), 0)),
			FIX2INT(rb_funcall(ninfo->gentries.vals, rb_intern("length"), 0)),
			FIX2INT(rb_funcall(ninfo->nodes.vals, rb_intern("length"), 0))
#ifdef USE_RB_ARGS_INFO
			,
			FIX2INT(rb_funcall(ninfo->args.vals, rb_intern("length"), 0))
#endif
		);
	}
	strcat(str, buf);
	// b) NODEObjAddresses struct
	if (val_obj_addresses == Qnil)
	{
		sprintf(buf, "    NODEObjAddresses struct is empty\n");
	}
	else
	{
		NODEObjAddresses *objadr;
		Data_Get_Struct(val_obj_addresses, NODEObjAddresses, objadr);
		sprintf(buf, 
			"    NODEObjAddresses struct:\n"
			"      syms_len (Num of symbols):      %d\n"
			"      lits_len (Num of literals):     %d\n"
			"      idtbls_len (Num of ID tables):  %d\n"
			"      gvars_len (Num of global vars): %d\n"
			"      nodes_len (Num of nodes):       %d\n"
#ifdef USE_RB_ARGS_INFO
			"      args_len: (Num of args info):   %d\n"
#endif
			, objadr->syms_len, objadr->lits_len,
			objadr->idtbls_len, objadr->gvars_len,
			objadr->nodes_len
#ifdef USE_RB_ARGS_INFO
			, objadr->args_len
#endif
		);
	}
	strcat(str, buf);
	strcat(str, "------------------\n");
	// Generate output string
	return rb_str_new2(str);
}

/*
 * Returns node name (usually <main>)
 */
static VALUE m_nodedump_nodename(VALUE self)
{
	return rb_funcall(rb_iv_get(self, "@nodename"), rb_intern("dup"), 0);
}

/*
 * Returns name of file that was used for node generation and will be used
 * by YARV (or nil/<compiled> if a string of code was used)
 */
static VALUE m_nodedump_filename(VALUE self)
{
	return rb_funcall(rb_iv_get(self, "@filename"), rb_intern("dup"), 0);
}

/*
 * Sets name of file that was used for node generation and will be used
 * by YARV (or nil/<compiled> if a string of code was used)
 */
static VALUE m_nodedump_set_filename(VALUE self, VALUE val)
{
	if (val != Qnil)
	{
		Check_Type(val, T_STRING);
		rb_iv_set(self, "@filename", rb_funcall(val, rb_intern("dup"), 0));	
	}
	else
	{
		rb_iv_set(self, "@filename", Qnil);
	}
	return self;
}

/*
 * Returns path of file that was used for node generation and will be used
 * by YARV (or nil/<compiled> if a string of code was used)
 */
static VALUE m_nodedump_filepath(VALUE self)
{
	return rb_funcall(rb_iv_get(self, "@filepath"), rb_intern("dup"), 0);
}

/*
 * call-seq:
 *   obj.filepath=value
 *
 * Sets the path of file that was used for node generation and will
 * be used by YARV (or nil/<compiled> if a string of code was used)
 */
static VALUE m_nodedump_set_filepath(VALUE self, VALUE val)
{
	if (val != Qnil)
	{
		Check_Type(val, T_STRING);
		rb_iv_set(self, "@filepath", rb_funcall(val, rb_intern("dup"), 0));
	}
	else
	{
		rb_iv_set(self, "@filepath", Qnil);
	}
	return self;
}

/*
 * call-seq:
 *   NodeMarshal.base85r_encode(input) -> output
 *
 * Encode arbitrary binary string to the ASCII string
 * using modified version of BASE85 (useful for obfuscation
 * of .rb source files)
 */
static VALUE m_base85r_encode(VALUE obj, VALUE input)
{
	return base85r_encode(input);
}

/*
 * call-seq:
 *   NodeMarshal.base85r_decode(input) -> output
 *
 * Decode ASCII string in the modified BASE85 format
 * to the binary string (useful for obfuscation of .rb
 * source files)
 */
static VALUE m_base85r_decode(VALUE obj, VALUE input)
{
	return base85r_decode(input);
}

/* call-seq:
 *   obj.to_text
 * 
 * Converts NodeMarshal class example to the text string (modified Base85 encoding) that
 * can be saved to the file and used for loading the node from the file.
 * Format of the obtained binary dump depends on used platform (especially
 * size of the pointer) and Ruby version.
 */
static VALUE m_nodedump_to_text(VALUE self)
{
	VALUE bin = m_nodedump_to_bin(self);
	return base85r_encode(bin);
}

/*
 * Returns node object
 */
static VALUE m_nodedump_node(VALUE self)
{
	return rb_iv_get(self, "@node");
}

/*
 * This class can load and save Ruby code in the form of the
 * platform-dependent syntax tree (made of NODEs). Such function
 * allows to hide the source code from users. Main features:
 *
 * - Irreversible transformation of Ruby source code to the syntax tree
 * - Representation of syntax tree in binary form dependent from the platform and Ruby version
 * - Simple options for node inspection
 * - Ruby 1.9.3, 2.2.x and 2.3.x support
 * - Subroutines for custom code obfuscation
 */
void Init_nodemarshal()
{
	static VALUE cNodeMarshal;
	init_nodes_table(nodes_ctbl, NODES_CTBL_SIZE);
	base85r_init_tables();

	cNodeMarshal = rb_define_class("NodeMarshal", rb_cObject);
	rb_define_singleton_method(cNodeMarshal, "base85r_encode", RUBY_METHOD_FUNC(m_base85r_encode), 1);
	rb_define_singleton_method(cNodeMarshal, "base85r_decode", RUBY_METHOD_FUNC(m_base85r_decode), 1);

	rb_define_method(cNodeMarshal, "initialize", RUBY_METHOD_FUNC(m_nodedump_init), 2);
	rb_define_method(cNodeMarshal, "to_hash", RUBY_METHOD_FUNC(m_nodedump_to_hash), 0);
	rb_define_method(cNodeMarshal, "to_bin", RUBY_METHOD_FUNC(m_nodedump_to_bin), 0);
	rb_define_method(cNodeMarshal, "to_text", RUBY_METHOD_FUNC(m_nodedump_to_text), 0);
	rb_define_method(cNodeMarshal, "dump_tree", RUBY_METHOD_FUNC(m_nodedump_parser_dump_tree), 0);
	rb_define_method(cNodeMarshal, "dump_tree_short", RUBY_METHOD_FUNC(m_nodedump_dump_tree_short), 0);
	rb_define_method(cNodeMarshal, "compile", RUBY_METHOD_FUNC(m_nodedump_compile), 0);
	rb_define_method(cNodeMarshal, "show_offsets", RUBY_METHOD_FUNC(m_nodedump_show_offsets), 0);
	rb_define_method(cNodeMarshal, "show_offsets=", RUBY_METHOD_FUNC(m_nodedump_set_show_offsets), 1);
	// Methods for working with the information about the node
	// a) literals, symbols, generic information
	rb_define_method(cNodeMarshal, "symbols", RUBY_METHOD_FUNC(m_nodedump_symbols), 0);
	rb_define_method(cNodeMarshal, "change_symbol", RUBY_METHOD_FUNC(m_nodedump_change_symbol), 2);
	rb_define_method(cNodeMarshal, "literals", RUBY_METHOD_FUNC(m_nodedump_literals), 0);
	rb_define_method(cNodeMarshal, "change_literal", RUBY_METHOD_FUNC(m_nodedump_change_literal), 2);
	rb_define_method(cNodeMarshal, "inspect", RUBY_METHOD_FUNC(m_nodedump_inspect), 0);
	rb_define_method(cNodeMarshal, "node", RUBY_METHOD_FUNC(m_nodedump_node), 0);
	// b) node and file names
	rb_define_method(cNodeMarshal, "nodename", RUBY_METHOD_FUNC(m_nodedump_nodename), 0);
	rb_define_method(cNodeMarshal, "filename", RUBY_METHOD_FUNC(m_nodedump_filename), 0);
	rb_define_method(cNodeMarshal, "filename=", RUBY_METHOD_FUNC(m_nodedump_set_filename), 1);
	rb_define_method(cNodeMarshal, "filepath", RUBY_METHOD_FUNC(m_nodedump_filepath), 0);
	rb_define_method(cNodeMarshal, "filepath=", RUBY_METHOD_FUNC(m_nodedump_set_filepath), 1);
	// C structure wrappers
	cNodeObjAddresses = rb_define_class("NodeObjAddresses", rb_cObject);
	cNodeInfo = rb_define_class("NodeInfo", rb_cObject);
}
