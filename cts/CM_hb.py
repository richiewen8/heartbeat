#!/usr/bin/env python

'''CTS: Cluster Testing System: heartbeat dependent modules...

Classes related to testing high-availability clusters...

Lots of things are implemented.

Lots of things are not implemented.

We have many more ideas of what to do than we've implemented.
 '''

__copyright__='''
Copyright (C) 2000,2001 Alan Robertson <alanr@unix.sh>
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

from CTS import *
from CTStests import *
from CTSaudits import *


class HeartbeatCM(ClusterManager):
    '''
    The heartbeat cluster manager class.
    It implements the things we need to talk to and manipulate
    heartbeat clusters
    '''

    def __init__(self, Environment, randseed=None):

        self.ResourceDirs = ["/etc/ha.d/resource.d", "/etc/rc.d/init.d"]
        self.ResourceFile = "/etc/ha.d/haresources"
        ClusterManager.__init__(self, Environment, randseed=randseed)
        self.update({
            "Name"	     : "heartbeat",
            "DeadTime"	     : 5,
            "StartCmd"	     : "/usr/lib/heartbeat/heartbeat ",
            "StopCmd"	     : "/usr/lib/heartbeat/heartbeat -k",
            "StatusCmd"	     : "/usr/lib/heartbeat/heartbeat -s",
            "RereadCmd"	     : "/usr/lib/heartbeat/heartbeat -r",
            "TestConfigDir"  : "/etc/ha.d/testconfigs",
            "LogFileName"    : "/var/log/ha-log",

            # Patterns to look for in the log files for various occasions...
            "Pat:We_started"   : "Local status now set to: 'active'",
            "Pat:They_started" : "%s: status active",
            "Pat:We_stopped"   : "Heartbeat shutdown complete",
            "Pat:They_stopped" : "node %s: is dead",
            "Pat:All_stopped"  : " %s heartbeat.*Heartbeat shutdown complete",

            # Bad news Regexes.  Should never occur.
            "BadRegexes"   : (
                r"Shutting down\.",
             	r"Forcing shutdown\.",
             	r"Both machines own .* resources!",
             	r"No one owns .* resources!",
             	r", exiting\.",
            ),
        })
        self._finalConditions()

    def SetClusterConfig(self, configpath="default", nodelist=None):
        '''Activate the named test configuration throughout the cluster.
        This code is specialized to heartbeat.
        '''
        rc=1
        Command='''
        cd %s%s%s;		: cd to test configuration directory
        for j in *
        do
          if
            [ -f "/etc/ha.d/$j" ];
          then
            if
              cmp $j /etc/ha.d/$j >/dev/null 2>&1;
            then
              : Config file $j is already up to correct.
            else
              echo "Touching $j"
              cp $j /etc/ha.d/$j
            fi
          fi
        done
        ''' % (self["TestConfigDir"], os.sep, configpath)

        if nodelist == None:
            nodelist=self.Env["nodes"]
        for node in nodelist:
            if not self.rsh(node, Command): rc=None

        self.rereadall()
        return rc


    def ResourceGroups(self):
        '''
        Return the list of resources groups defined in this configuration.

        This code is specialized to heartbeat.
        We make the assumption that the resource file on the local machine
        is the same as that of a cluster member.

        We aren't necessarily a member of the cluster
        (In fact, we usually aren't).
        '''
        RscGroups=[]
        file = open(self.ResourceFile, "r")
        while (1):

            line = file.readline()
            if line == "":   break

            idx=string.find(line, '#')
            if idx >= 0:
                line=line[:idx]
            if line == "":    continue
            line = string.strip(line)

            # Is this wrong?
            tokens = re.split("[ \t]+", line)

            # Ignore the default server for this resource group
            del tokens[0]

            Group=[]
            for token in tokens:
                if token != "":
                    idx=string.find(token, "::")
                    if idx > 0:
                        tuple=string.split(token, "::")
                    else:
                        #
                        # Is this an IPaddr default resource type?
                        #
                        if re.match("^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$"
                        ,	token):
                            tuple=["IPaddr", token]
                        else:
                            tuple = [token, None]
                    Resource = self.hbResource(tuple[0], tuple[1])
                    Group.append(Resource)

            RscGroups.append(Group)
        file.close()
        return RscGroups

    def HasQuorum(self):
        (
'''Return TRUE if the cluster currently has quorum.  According to
current heartbeat code this means one node is up.
''')
        return self.upcount() >= 1

    def hbResource(self, type, instance):
        '''
        Our job is to create the right kind of resource.  For most
        resources, we just create an HBResource object, but for
        IP addresses, we create an HBipResource instead.
        Some other types of resources may also be added as special
        cases.
        '''

        if type == "IPaddr":
            return HBipResource(self, type, instance)

        return HBResource(self, type, instance)


class HBResource(Resource):

    def IsRunningOn(self, nodename):
        '''
        This member function returns true if our resource is running
        on the given node in the cluster.
        We call the status operation for the resource script.
        '''

        return self._ResourceOperation("status", "OK|running", nodename)

    def _ResourceOperation(self, operation, pattern, nodename):
        '''
        We call the requested operation for the resource script.
        We don't care what kind of operation we were called to do
        particularly.
        When we were created, we were bound to a cluster manager, which
        has its own remote execution method (which we use here).
        '''
        if self.Instance == None:
            instance = ""
        else:
            instance = self.Instance

        Rlist = 'LIST="'
        for dir in self.CM.ResourceDirs:
          Rlist = Rlist + " " + dir
        Rlist = Rlist + '"; '


        Script= Rlist + '''
        T="''' + self.ResourceType + '''";
        I="''' + instance + '''";
        for dir in $LIST;
        do
          if
            [ -f "$dir/$T" -a  -x "$dir/$T" ]
          then
            "$dir/$T" $I ''' + operation + '''
            exit 0
          fi
        done;
        exit 1;'''

        line = self.CM.rsh.readaline(nodename, Script)
        return re.search(pattern, line)

        
    def IsWorkingCorrectly(self, nodename):
        "We default to returning TRUE for this one..."
        self.CM.log("Faking out: " + self.Instance)
        return 1

class HBipResource(HBResource):
    '''
    We are a specialized IP address resource which knows how to test
    to see if our resource type is actually being served.
    We are cheat and run the IPaddr resource script on
    the current machine, because it's a more interesting case.
    '''

    def IsWorkingCorrectly(self, nodename):
        return self._ResourceOperation("monitor", "OK", "localhost")

#
#   A little test code...
#
if __name__ == '__main__':


    #cm = HeartbeatCM(randseed=(88, 246, 228))
    #cm = HeartbeatCM(randseed=(108, 10, 174)) 
    cm = HeartbeatCM()
    cm.log(">>>>>>>>>>>>>>>> BEGINNING TESTS")

    cm.setnodes(["sgi1", "sgi2"])

    # We need to make sure all machines are up before starting the tests
    # A ping -c1 -s nodename >/dev/null might be a suitable command to use

    cm.SyncTestConfigs()
    cm.log("Setting Cluster Config")
    cm.SetClusterConfig()

    cm.log("Resource Groups: " + repr(cm.ResourceGroups()))

#	Set up the test scenario

    cm.TruncLogs()
    print "Calling stopall\n"
    cm.stopall()
    print "Calling startall\n"
    cm.startall()
    time.sleep(10)

    Audits = AuditList(cm)

    ListOfTests = TestList(cm)

    tests = RandomTests(cm, ListOfTests, Audits)
    overall, detailed = tests.run(5)
    cm.log("****************")
    cm.log("Overall Results:" + repr(overall))
    cm.log("****************")
    cm.log("Detailed Results")
    for test in detailed.keys():
        cm.log("Test %s:" % test + repr(detailed[test]))
    cm.log("<<<<<<<<<<<<<<<< TESTS COMPLETED")
