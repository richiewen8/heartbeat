#!/usr/bin/env python

'''CTS: Cluster Testing System: Lab environment module


 '''

__copyright__='''
Copyright (C) 2001 Alan Robertson <alanr@unix.sh>
Licensed under the GNU GPL.
'''

#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

from UserDict import UserDict
import sys, time, types, syslog, whrandom, os, struct
from CTS  import ClusterManager
from CM_hb import HeartbeatCM
from socket import gethostbyname_ex

class ResetMechanism:
    def reset(self, node):
        raise ValueError("Abstract class member (reset)")

class Stonith(ResetMechanism):
    def __init__(self, sttype="baytech", parm="10.10.10.100 admin admin"
    ,	path="/usr/sbin/stonith"):
        self.pathname=path
        self.configstring=parm
        self.stonithtype=sttype

    def reset(self, node):
        cmdstring = "%s -t '%s' -p '%s' '%s'" % (self.pathname
        ,	self.stonithtype, self.configstring, node)
        return (os.system(cmdstring) == 0)

class Logger:
    TimeFormat = "%Y/%m/%d_%H:%M:%S\t"

    def __call__(self, lines):
        raise ValueError("Abstract class member (__call__)")

class SysLog(Logger):
    defaultsource="CTS"
    defaultlevel=7

    def __init__(self, labinfo):

        if labinfo.has_key("syslogsource"):
            self.source=labinfo["syslogsource"]
        else:
            self.source=SysLog.defaultsource

        if labinfo.has_key("sysloglevel"):
            self.level=labinfo["sysloglevel"]
        else:
            self.level=SysLog.defaultlevel

        syslog.openlog(self.source, 0, self.level)

    def __call__(self, lines):
        if isinstance(lines, types.StringType):
            syslog.syslog(lines)
        else:
            for line in lines:
                syslog.syslog(line)

class StdErrLog(Logger):

    def __init__(self, labinfo):
        pass

    def __call__(self, lines):
        t = time.strftime(Logger.TimeFormat, time.localtime(time.time()))  
        if isinstance(lines, types.StringType):
            sys.__stderr__.writelines([t, lines, "\n"])
        else:
            for line in lines:
                sys.__stderr__.writelines([t, line, "\n"])
        sys.__stderr__.flush()


class FileLog(Logger):
    def __init__(self, labinfo, filename=None):

        if filename == None:
            filename=labinfo["logfile"]
        
        self.logfile=filename

    def __call__(self, lines):

        fd = open(self.logfile, "a")
        t = time.strftime(Logger.TimeFormat, time.localtime(time.time()))  

        if isinstance(lines, types.StringType):
            fd.writelines([t, lines, "\n"])
        else:
            for line in lines:
                fd.writelines([t, line, "\n"])
        fd.close()


class CtsLab(UserDict):
    '''This class defines the Lab Environment for the Cluster Test System.
    It defines those things which are expected to change from test
    environment to test environment for the same cluster manager.

    It is where you define the set of nodes that are in your test lab
    what kind of reset mechanism you use, etc.

    This class is derived from a UserDict because we hold many
    different parameters of different kinds, and this provides
    provide a uniform and extensible interface useful for any kind of
    communication between the user/administrator/tester and CTS.

    At this point in time, it is the intent of this class to model static
    configuration and/or environmental data about the environment which
    doesn't change as the tests proceed.

    Well-known names (keys) are an important concept in this class.
    The HasMinimalKeys member function knows the minimal set of
    well-known names for the class.

    The following names are standard (well-known) at this time:

	nodes		An array of the nodes in the cluster
	reset		A ResetMechanism object
	logger		An array of objects that log strings...
	CMclass		The type of ClusterManager we are running
			(This is a class object, not a class instance)
        RandSeed	Random seed.  It is a triple of bytes. (optional)

    The CTS code ignores names it doesn't know about/need.
    The individual tests have access to this information, and it is
    perfectly acceptable to provide hints, tweaks, fine-tuning
    directions or other information to the tests through this mechanism.
    '''

    def __init__(self, nodes):
        self.data = {}
        self["nodes"] = nodes
        self.MinimalKeys=["nodes", "reset", "logger", "CMclass"]

    def HasMinimalKeys(self):
        'Return TRUE if our object has the minimal set of keys/values in it'
        result = 1
        for key in self.MinimalKeys:
            if not self.has_key(key):
                result = None
        return result

    def SupplyDefaults(self):
        if not self.has_key("logger"):
            self["logger"] = (SysLog(self), StdErrLog(self))
        if not self.has_key("reset"):
            self["reset"] = Stonith()
        if not self.has_key("CMclass"):
            self["CMclass"] = HeartbeatCM

        #
        #  Now set up our random number generator...
        #
        self.RandomGen = whrandom.whrandom()

        #  Get a random seed for the random number generator.

        if self.has_key("RandSeed"):
            randseed = self["RandSeed"]
        else:
            f=open("/dev/urandom", "r")
            string=f.read(3)
            f.close()
            randseed=struct.unpack("BBB", string)

        self.log("Random seed is: " + str(randseed))
        self.randseed=randseed

        self.RandomGen.seed(randseed[0], randseed[1], randseed[2]) 

    def log(self, args):
        "Log using each of the supplied logging methods"
        for logfcn in self._logfunctions:
            logfcn(args)

    def __setitem__(self, key, value):
        '''Since this function gets called whenever we modify the
        dictionary (object), we can (and do) validate those keys that we
	know how to validate.  For the most part, we know how to validate
        the "MinimalKeys" elements.
        '''

	#
	#	List of nodes in the system
	#
        if key == "nodes":
            self.Nodes = {}
            for node in value:
                # I don't think I need the IP address, etc. but this validates
                # the node name against /etc/hosts and/or DNS, so it's a
                # GoodThing(tm).
                self.Nodes[node] = gethostbyname_ex(node)
                if len(value) < 2:
                    raise ValueError("Must have at least two nodes in system")
            
	#
	#	Reset Mechanism
	#
        elif key == "reset":
            if not issubclass(value.__class__, ResetMechanism):
                raise ValueError("'reset' Value must be a subclass"
                " of ResetMechanism") 
	#
	#	List of Logging Mechanism(s)
	#
        elif key == "logger":
            if len(value) < 1:
                raise ValueError("Must have at least one logging mechanism")
            for logger in value:
                if not callable(logger):
                    raise ValueError("'logger' elements must be callable")
            self._logfunctions = value
	#
	#	Cluster Manager Class
	#
        elif key == "CMclass":
            if not issubclass(value, ClusterManager):
                raise ValueError("'CMclass' must be a subclass of"
                " ClusterManager")
	#
	#	Initial Random seed...
	#
        elif key == "RandSeed":
            if len(value) != 3:
                raise ValueError("'Randseed' must be a 3-element list/tuple")
            for elem in value:
                if not isinstance(elem, types.IntType):
                    raise ValueError("'Randseed' list must all be ints")
              
        self.data[key] = value

    def IsValidNode(self, node):
        'Return TRUE if the given node is valid'
        return self.Nodes.has_key(node)

    def __CheckNode(self, node):
        "Raise a ValueError if the given node isn't valid"

        if not self.IsValidNode(node):
            raise ValueError("Invalid node [%s] in CheckNode" % node)

    def RandomNode(self):
        '''Choose a random node from the cluster'''
        return self.RandomGen.choice(self["nodes"])

    def ResetNode(self, node):
        "Reset a node, (normally) using a hardware mechanism"
        self.__CheckNode(node)
        return self["reset"].reset(node)

    
#
#   A little test code...
#
if __name__ == '__main__': 

    from CTSaudits import AuditList
    from CTStests import TestList
    from CTS import RandomTests, Scenario, InitClusterManager, PingFest

    Environment = CtsLab(["sgi1", "sgi2"])
    #Environment["RandSeed"] = (1,2,3)
    Environment["DoStonith"] = 0
    Environment.SupplyDefaults()

    print Environment

    # Your basic start up the world type of test scenario...

    #scenario = Scenario(
    #[	InitClusterManager(Environment)
    #,	PingFest(Environment)])
    scenario = Scenario(
    [	InitClusterManager(Environment)])

    # Create the Cluster Manager object

    cm = Environment['CMclass'](Environment)

    cm.log("Hi There")
    cm.log(">>>>>>>>>>>>>>>> BEGINNING TESTS")

    Audits = AuditList(cm)
    Tests = TestList(cm)

    tests = RandomTests(scenario, cm, Tests, Audits)
    overall, detailed = tests.run(5000)
 
    cm.log("****************")
    cm.log("Overall Results:" + repr(overall))
    cm.log("****************")
    cm.log("Detailed Results")
    for test in detailed.keys():
        cm.log("Test %s:" % test + repr(detailed[test]))
    cm.log("<<<<<<<<<<<<<<<< TESTS COMPLETED")
