/* This file was generated by mib2c and is intended for use as a mib module
   for the ucd-snmp snmpd agent. */


#ifdef IN_UCD_SNMP_SOURCE
/* If we're compiling this file inside the ucd-snmp source tree */


/* This should always be included first before anything else */
#include <config.h>


/* minimal include directives */
#include "mibincl.h"
#include "util_funcs.h"


#else /* !IN_UCD_SNMP_SOURCE */

#include "config.h" /* linux-ha/config.h */

#include <ucd-snmp/ucd-snmp-config.h>
#include <ucd-snmp/ucd-snmp-includes.h>
#include <ucd-snmp/ucd-snmp-agent-includes.h>

#ifdef HAVE_NET_SNMP
#ifdef HAVE_NET_SNMP_UTIL_FUNCS_H
	#include <ucd-snmp/util_funcs.h>
#else /* !HAVE_NET_SNMP_UTIL_FUNCS_H */
	#include "ucd_util_funcs.h"
#endif /* HAVE_NET_SNMP_UTIL_FUNCS_H */ 

#else /* !HAVE_NET_SNMP */
	#include <ucd-snmp/util_funcs.h>
#endif /* HAVE_NET_SNMP */ 

#endif /* !IN_UCD_SNMP_SOURCE */


#include "NodeTable.h"
#include <clplumbing/cl_log.h>
#include "haclient.h"


/* 
 * NodeTable_variables_oid:
 *   this is the top level oid that we want to register under.  This
 *   is essentially a prefix, with the suffix appearing in the
 *   variable below.
 */


oid NodeTable_variables_oid[] = { 1,3,6,1,4,1,4682,2 };


/* 
 * variable2 NodeTable_variables:
 *   this variable defines function callbacks and type return information 
 *   for the NodeTable mib section 
 */


struct variable2 NodeTable_variables[] = {
/*  magic number        , variable type , ro/rw , callback fn  , L, oidsuffix */
#define   NODENAME              4
  { NODENAME            , ASN_OCTET_STR , RONLY , var_NodeTable, 2, { 1,2 } },
#define   NODESTATUS            5
  { NODESTATUS          , ASN_OCTET_STR , RONLY , var_NodeTable, 2, { 1,3 } },
#define   NODEHAIFCOUNT         6
  { NODEHAIFCOUNT       , ASN_INTEGER   , RONLY , var_NodeTable, 2, { 1,4 } },

};
/*    (L = length of the oidsuffix) */


/*
 * init_NodeTable():
 *   Initialization routine.  This is called when the agent starts up.
 *   At a minimum, registration of your variables should take place here.
 */
void init_NodeTable(void) {


	/* register ourselves with the agent to handle our mib tree */
	REGISTER_MIB("NodeTable", NodeTable_variables, variable2,
               NodeTable_variables_oid);


	/* place any other initialization junk you need here */
	(void) _ha_msg_h_Id;
}


/*
 * var_NodeTable():
 *   Handle this table separately from the scalar value case.
 *   The workings of this are basically the same as for var_NodeTable above.
 */
unsigned char *
var_NodeTable(struct variable *vp,
    	    oid     *name,
    	    size_t  *length,
    	    int     exact,
    	    size_t  *var_len,
    	    WriteMethod **write_method)
{
	
	
	/* variables we may use later */
	static long long_ret;
	static unsigned char string[SPRINT_MAX_LEN];
	unsigned long count, id;
	const struct hb_node_t * node = NULL;

	if (get_node_count(&count) != HA_OK) 
		return NULL;
	if (header_simple_table(vp,name,length,exact,var_len,write_method, count) 
			== MATCH_FAILED)
		return NULL;
	
	id = name[*length - 1] - 1;
	if (get_node_info(id, &node) != HA_OK)
		return NULL;
	
	/* 
	 * this is where we do the value assignments for the mib results.
	 */
	switch(vp->magic) {
	
	
		case NODENAME:
		    
		    *string = 0;
		    strncpy(string, node->name, SPRINT_MAX_LEN);
		    *var_len = strlen(string);
		    return (unsigned char *) string;
		
		case NODESTATUS:
		    
		    *string = 0;
		    strncpy(string, node->status, SPRINT_MAX_LEN);
		    *var_len = strlen(string);
		    return (unsigned char *) string;
		
		case NODEHAIFCOUNT:
		    
		    long_ret = 0;
		    return (unsigned char *) &long_ret;
		
		
		default:
		  ERROR_MSG("");
	}
	return NULL;
}






