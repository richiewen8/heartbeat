/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm/common/crm.h>

#include <portability.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <libxml/tree.h>
#include <cibio.h>
#include <crm/common/crmutils.h>
#include <crm/common/xmltags.h>
#include <crm/common/xmlutils.h>

const char * local_resource_path[] =
{
    XML_CIB_TAG_STATUS,
};

const char * resource_path[] =
{
    XML_CIB_TAG_RESOURCES,
};

const char * node_path[] =
{
    XML_CIB_TAG_NODES,
};

const char * constraint_path[] =
{
    XML_CIB_TAG_CONSTRAINTS,
};

gboolean initialized = FALSE;
xmlNodePtr the_cib = NULL;
xmlNodePtr node_search = NULL;
xmlNodePtr resource_search = NULL;
xmlNodePtr constraint_search = NULL;
xmlNodePtr status_search = NULL;

xmlNodePtr
createEmptyCib(void)
{
    // real code...
    xmlDocPtr cib = xmlNewDoc("1.0");
//    xmlNodePtr tree, subtree;

    cib->children = xmlNewDocNode(cib, NULL, "cib", NULL);
    xmlSetProp(cib->children, "version", "1");
    xmlSetProp(cib->children, "generated", "true");
    xmlSetProp(cib->children, XML_ATTR_TSTAMP, getNow());        

    xmlAddChild(cib->children, xmlNewNode(NULL, XML_CIB_TAG_NODES));
    xmlAddChild(cib->children, xmlNewNode(NULL, XML_CIB_TAG_RESOURCES));
    xmlAddChild(cib->children, xmlNewNode(NULL, XML_CIB_TAG_CONSTRAINTS));
    xmlAddChild(cib->children, xmlNewNode(NULL, XML_CIB_TAG_STATUS));

    xmlNodePtr root = xmlDocGetRootElement(cib);
    if(verifyCibXml(root)) return root;
    cl_log(LOG_CRIT, "The generated CIB did not pass integrity testing!!  All hope is lost.");
    return NULL;
}

gboolean
verifyCibXml(xmlNodePtr cib)
{
    if(cib == NULL)
    {
	cl_log(LOG_INFO, "XML Buffer was empty.");
	return FALSE;
    }

    xmlNodePtr tmp1 = findNode(cib, XML_CIB_TAG_NODES);
    xmlNodePtr tmp2 = findNode(cib, XML_CIB_TAG_RESOURCES);
    xmlNodePtr tmp3 = findNode(cib, XML_CIB_TAG_CONSTRAINTS);
    xmlNodePtr tmp4 = findNode(cib, XML_CIB_TAG_STATUS);
    
    if(tmp1 == NULL || tmp2 == NULL || tmp3 == NULL || tmp4 == NULL)
    {
	xmlChar *mem = NULL;
	int size = 0;
	xmlDocDumpMemory(cib->doc, &mem, &size);
	cl_log(LOG_CRIT, "Not all required sections were present. Sections [%s, %s, %s, %s]\nCib was: %s", tmp1 == NULL? "ok":"null", tmp2 == NULL? "ok":"null", tmp3 == NULL? "ok":"null", tmp4 == NULL? "ok":"null", (char*)mem);
	return FALSE;
    }

    // more integrity tests

    return TRUE;
}


xmlNodePtr
readCibXml(char *buffer)
{
   xmlDocPtr doc = xmlParseMemory(buffer, strlen(buffer));
   if(doc == NULL)
   {
       cl_log(LOG_INFO, "XML Buffer was not valid...\n Buffer: %s", buffer);
   }
   xmlNodePtr root = xmlDocGetRootElement(doc);
   if(verifyCibXml(root) == FALSE)
       return createEmptyCib();
   return root;
}


xmlNodePtr
readCibXmlFile(const char *filename)
{
    struct stat buf;
    int s_res = stat(CIB_FILENAME, &buf);
    
    cl_log(LOG_DEBUG, "Stat of (%s) was (%d).", CIB_FILENAME, s_res);
    
    xmlDocPtr doc = NULL;
    if(s_res == 0)
    {
	doc = xmlParseFile(filename);
	xmlSetProp(xmlDocGetRootElement(doc), "generated", "false");
    }
   xmlNodePtr root = xmlDocGetRootElement(doc);
   if(verifyCibXml(root) == FALSE)
       return createEmptyCib();
   return root;
}

xmlNodePtr
theCib(void)
{
    return the_cib;
}

xmlNodePtr
getCibSection(const char *section)
{
    CRM_DEBUG2("Looking for section (%s) of the CIB", section);

    xmlNodePtr res = findNode(the_cib, section);

    // make sure the siblings dont turn up as well
    if(res != NULL) res = xmlLinkedCopyNoSiblings(res, 1);
    else if(the_cib == NULL)
    {
	cl_log(LOG_CRIT, "The CIB has not been initialized!");
	
    }
    else cl_log(LOG_ERR, "Section (%s) not found.", section);
    
    return res;
}

gboolean
initializeCib(xmlNodePtr cib)
{
    if(verifyCibXml(cib))
    {
	the_cib = cib;
	node_search = updatedSearchPath(cib, XML_CIB_TAG_NODES);
	resource_search = updatedSearchPath(cib, XML_CIB_TAG_RESOURCES);
	constraint_search = updatedSearchPath(cib, XML_CIB_TAG_CONSTRAINTS);
	status_search = updatedSearchPath(cib, XML_CIB_TAG_STATUS);
	initialized = TRUE;
	return TRUE;
    }
    return FALSE;
    
}


xmlNodePtr
updatedSearchPath(xmlNodePtr cib, const char *path)
{
//    const char *last_path = path[DIMOF(path)-1];
    cl_log(LOG_INFO, "Updating (%s) search path.", path);
    xmlNodePtr parent = findNode(cib, path);
    if(parent == NULL)
	cl_log(LOG_CRIT, "Updating %s search path failed.", path);
    cl_log(LOG_INFO, "Updating (%s) search path to (%s).", path, xmlGetNodePath(parent) );
    return parent;
}


int
moveFile(const char *oldname, const char *newname, gboolean backup, char *ext)
{
    /* move 'oldname' to 'newname' by creating a hard link to it
     *  and then removing the original hard link
     */
    int res = 0;
    struct stat tmp;
    int s_res = stat(newname, &tmp);

    cl_log(LOG_INFO, "Stat of %s (code: %d).", newname, s_res);
    if(s_res >= 0)
    {
	if(backup == TRUE)
	{
	    char backname[1024];
	    static const char *back_ext = "bak";
	    if(ext != NULL) back_ext = (char*)ext;
	    
	    snprintf(backname, sizeof(backname)-1, "%s.%s", newname, back_ext);
	    moveFile(newname, backname, FALSE, NULL);
	}
	else
	{
	    res = unlink(newname);
	    if(res < 0)
	    {
		perror("Could not remove the current backup of Cib");
		return -1;
	    }
	}
    }
    
    s_res = stat(oldname, &tmp);
    cl_log(LOG_INFO, "Stat of %s (code: %d).", oldname, s_res);

    if(s_res >= 0)
    {
	res = link(oldname, newname);
	if(res < 0)
	{
	    perror("Could not create backup of current Cib");
	    return -2;
	}
	res = unlink(oldname);
	if(res < 0)
	{
	    perror("Could not unlink the current Cib");
	    return -3;
	}
    }
    
    return 0;
    
}


int
activateCibBuffer(char *buffer)
{
    xmlNodePtr cib = readCibXml(buffer);
    return activateCibXml(cib);
    
}

int
activateCibXml(xmlNodePtr cib)
{
    if(cib != NULL)
    {
	if(initializeCib(cib) == FALSE) return -5;
	
	int res = moveFile(CIB_FILENAME, CIB_BACKUP, FALSE, NULL);
	
	if(res  < 0)
	{
	    cl_log(LOG_INFO, "Could not make backup of the current Cib (code: %d)... aborting update.", res);
	    return -1;
	}
	
 	// modify the timestamp
	xmlSetProp(cib, XML_ATTR_TSTAMP, getNow());        
	
	// save it
	res = xmlSaveFile(CIB_FILENAME, cib->doc);
	// res == num bytes saved
	cl_log(LOG_INFO, "Saved %d bytes to the Cib as XML", res);
	
	if(res < 0) // assume 0 is good
	{
	    if(moveFile(CIB_BACKUP, CIB_FILENAME, FALSE, NULL) < -1)
	    {
		cl_log(LOG_CRIT, "Could not restore the backup of the current Cib (code: %d)... panic!", res);
		return -2;
		// should probably exit here 
	    }
	    
	    cl_log(LOG_CRIT, "Update of Cib failed (code: %d)... reverted to last known valid version", res);
	    return -3;
	}
    }
    else
    {
	cl_log(LOG_INFO, "Ignoring invalid Cib XML");
	return -4;
    }

    return 0;
    
}
