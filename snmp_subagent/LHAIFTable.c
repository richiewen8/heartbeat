/*
 * Note: this file originally auto-generated by mib2c using
 *        : mib2c.iterate.conf,v 5.9 2003/06/04 00:14:41 hardaker Exp $
 */

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
#include "LHAIFTable.h"

#include "hbagent.h"

static GArray * gIFInfo = NULL;

int LHAIFTable_load(netsnmp_cache *cache, void *vmagic); 
void LHAIFTable_free(netsnmp_cache *cache, void *vmagic); 

/** Initialize the LHAIFTable table by defining its contents and how it's structured */
void
initialize_table_LHAIFTable(void)
{
    static oid LHAIFTable_oid[] = {1,3,6,1,4,1,4682,3};
    netsnmp_table_registration_info *table_info;
    netsnmp_handler_registration *my_handler;
    netsnmp_iterator_info *iinfo;

    /** create the table registration information structures */
    table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);
    iinfo = SNMP_MALLOC_TYPEDEF(netsnmp_iterator_info);

    /** if your table is read only, it's easiest to change the
        HANDLER_CAN_RWRITE definition below to HANDLER_CAN_RONLY */
    my_handler = netsnmp_create_handler_registration("LHAIFTable",
                                             LHAIFTable_handler,
                                             LHAIFTable_oid,
                                             OID_LENGTH(LHAIFTable_oid),
                                             HANDLER_CAN_RONLY);
            
    if (!my_handler || !table_info || !iinfo) {
        snmp_log(LOG_ERR, "malloc failed in initialize_table_LHAIFTable");
        return; /* Serious error. */
    }

    /***************************************************
     * Setting up the table's definition
     */
    netsnmp_table_helper_add_indexes(table_info,
                                  ASN_INTEGER, /* index: LHANodeIndex */
                                  ASN_INTEGER, /* index: LHAIFIndex */
                             0);

    /** Define the minimum and maximum accessible columns.  This
        optimizes retrival. */
    table_info->min_column = 2;
    table_info->max_column = 3;

    /* iterator access routines */
    iinfo->get_first_data_point = LHAIFTable_get_first_data_point;
    iinfo->get_next_data_point = LHAIFTable_get_next_data_point;

    /** tie the two structures together */
    iinfo->table_reginfo = table_info;

    /***************************************************
     * registering the table with the master agent
     */
    DEBUGMSGTL(("initialize_table_LHAIFTable",
                "Registering table LHAIFTable as a table iterator\n"));		 
    netsnmp_register_table_iterator(my_handler, iinfo);

    /*
     * .... with a local cache
     */
    netsnmp_inject_handler(my_handler,
	 netsnmp_get_cache_handler(CACHE_TIME_OUT, 
				   LHAIFTable_load,
				   LHAIFTable_free,
				   LHAIFTable_oid,
				   OID_LENGTH(LHAIFTable_oid)));
}

/** Initializes the LHAIFTable module */
void
init_LHAIFTable(void)
{

  /** here we initialize all the tables we're planning on supporting */
    initialize_table_LHAIFTable();
}

/** returns the first data point within the LHAIFTable table data.

    Set the my_loop_context variable to the first data point structure
    of your choice (from which you can find the next one).  This could
    be anything from the first node in a linked list, to an integer
    pointer containing the beginning of an array variable.

    Set the my_data_context variable to something to be returned to
    you later (in your main LHAIFTable_handler routine) that will provide
    you with the data to return in a given row.  This could be the
    same pointer as what my_loop_context is set to, or something
    different.

    The put_index_data variable contains a list of snmp variable
    bindings, one for each index in your table.  Set the values of
    each appropriately according to the data matching the first row
    and return the put_index_data variable at the end of the function.
*/
netsnmp_variable_list *
LHAIFTable_get_first_data_point(void **my_loop_context, void **my_data_context,
                          netsnmp_variable_list *put_index_data,
                          netsnmp_iterator_info *mydata)
{
    if (gIFInfo && gIFInfo->len == 0) 
	return NULL;

    *my_loop_context = NULL;
    return LHAIFTable_get_next_data_point(my_loop_context, 
	    				my_data_context,
					put_index_data,
					mydata);
}

/** functionally the same as LHAIFTable_get_first_data_point, but
   my_loop_context has already been set to a previous value and should
   be updated to the next in the list.  For example, if it was a
   linked list, you might want to cast it to your local data type and
   then return my_loop_context->next.  The my_data_context pointer
   should be set to something you need later (in your main
   LHAIFTable_handler routine) and the indexes in put_index_data updated
   again. */
netsnmp_variable_list *
LHAIFTable_get_next_data_point(void **my_loop_context, void **my_data_context,
                         netsnmp_variable_list *put_index_data,
                         netsnmp_iterator_info *mydata)
{
    static size_t i = 0;
    netsnmp_variable_list *vptr;
    struct hb_ifinfo * info;

    if (*my_loop_context != NULL) {
	i = *((size_t *) *my_loop_context);
    } else {
	i = 0;
    }

    if (gIFInfo && i >= gIFInfo->len) 
	return NULL;

    vptr = put_index_data;
    info = & g_array_index(gIFInfo, struct hb_ifinfo, i);

    snmp_set_var_value(vptr, (u_char *) &(info->nodeid), sizeof(size_t));
    vptr = vptr->next_variable;

    snmp_set_var_value(vptr, (u_char *) &(info->id), sizeof(size_t));
    vptr = vptr->next_variable;

    i++; 
    *my_loop_context = (void *) &i;
    *my_data_context = (void *) info;

    return put_index_data;
}

/** handles requests for the LHAIFTable table, if anything else needs to be done */
int
LHAIFTable_handler(
    netsnmp_mib_handler               *handler,
    netsnmp_handler_registration      *reginfo,
    netsnmp_agent_request_info        *reqinfo,
    netsnmp_request_info              *requests) {

    netsnmp_request_info *request;
    netsnmp_table_request_info *table_info;
    netsnmp_variable_list *var;

    struct hb_ifinfo * entry;
    
    for(request = requests; request; request = request->next) {
        var = request->requestvb;
        if (request->processed != 0)
            continue;

        /** perform anything here that you need to do before each
           request is processed. */

        /** the following extracts the my_data_context pointer set in
           the loop functions above.  You can then use the results to
           help return data for the columns of the LHAIFTable table in question */
        entry = (struct hb_ifinfo *) netsnmp_extract_iterator_context(request);
        if (entry == NULL) {
            if (reqinfo->mode == MODE_GET) {
                netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE);
                continue;
            }
            /** XXX: no row existed, if you support creation and this is a
               set, start dealing with it here, else continue */
        }

        /** extracts the information about the table from the request */
        table_info = netsnmp_extract_table_info(request);
        /** table_info->colnum contains the column number requested */
        /** table_info->indexes contains a linked list of snmp variable
           bindings for the indexes of the table.  Values in the list
           have been set corresponding to the indexes of the
           request */
        if (table_info==NULL) {
            continue;
        }

        switch(reqinfo->mode) {
            /** the table_iterator helper should change all GETNEXTs
               into GETs for you automatically, so you don't have to
               worry about the GETNEXT case.  Only GETs and SETs need
               to be dealt with here */
            case MODE_GET:
                switch(table_info->colnum) {
                    case COLUMN_LHAIFNAME:
                        snmp_set_var_typed_value(var, 
				ASN_OCTET_STR, 
				(u_char *) entry->name, 
				strlen(entry->name) + 1);
                        break;

                    case COLUMN_LHAIFSTATUS:
                        snmp_set_var_typed_value(var, 
				ASN_OCTET_STR, 
				(u_char *) entry->status, 
				strlen(entry->status) + 1);
                        break;

                    default:
                        /** We shouldn't get here */
                        snmp_log(LOG_ERR, "problem encountered in LHAIFTable_handler: unknown column\n");
                }
                break;

            case MODE_SET_RESERVE1:
                /** set handling... */

            default:
                snmp_log(LOG_ERR, "problem encountered in LHAIFTable_handler: unsupported mode\n");
        }
    }
    return SNMP_ERR_NOERROR;
}

int
LHAIFTable_load(netsnmp_cache *cache, void *vmagic) 
{
    LHAIFTable_free(cache, vmagic);

    gIFInfo = get_hb_info(LHA_IFINFO);

    return 0;
}

void
LHAIFTable_free(netsnmp_cache *cache, void *vmagic)
{
    return;
}

