#!/usr/bin/python

'''CTS: Cluster Testing System: Main module

Classes related to testing high-availability clusters...

Lots of things are implemented.

Lots of things are not implemented.

We have many more ideas of what to do than we've implemented.
 '''

__copyright__='''
Copyright (C) 2000, 2001 Alan Robertson <alanr@unix.sh>
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

import types, string, select, sys, time, re, os, struct, os, signal
from UserDict import UserDict
from syslog import *
from popen2 import Popen3
import CTStests


class RemoteExec:
    '''This is an abstract remote execution class.  It runs a command on another
       machine - somehow.  The somehow is up to us.  This particular
       class uses ssh.
       Most of the work is done by fork/exec of ssh or scp.
    '''

    def __init__(self):
        #	-n: no stdin, -x: no X11
        self.Command = "/usr/bin/ssh -l root -n -x"
	#	-B: batch mode, -q: no stats (quiet)
        self.CpCommand = "/usr/bin/scp -B -q"

    def setcmd(self, rshcommand):

        '''Set the name of the remote shell command'''

        self.Command = rshcommand

    def _cmd(self, *args):

        '''Compute the string that will run the given command on the
        given remote system'''

        args= args[0]
        sysname = args[0]
        command = args[1]
        return self.Command + " " + sysname + " '" + command + "'"

    def __call__(self, *args):
        '''Run the given command on the given remote system
        If you call this class like a function, this is the function that gets
        called.  It just runs it roughly as though it were a system() call
        on the remote machine.  The first argument is name of the machine to
        run it on.
        '''
        #print "Now running %s\n" % self._cmd(args)
        count=0;
        rc = 0;
        while count < 3:
           rc = os.system(self._cmd(args))
           if rc == 0: return rc
           print "Retrying command %s" % self._cmd(args)
           count=count+1
        return rc


    def popen(self, *args):
        '''popen the given remote command on the remote system.
        As in __call__, the first argument is name of the machine to run it on.
        '''
        #print "Now running %s\n" % self._cmd(args)
        return Popen3(self._cmd(args), None)

    def readaline(self, *args):
        '''Run a command on the remote machine and capture 1 line of
        stdout from the given remote command
        As in __call__, the first argument is name of the machine to run it on.
        '''
        p = self.popen(args[0], args[1])
        p.tochild.close()
        result = p.fromchild.readline()
        p.fromchild.close()
        self.lastrc = p.wait()
        return result

    def cp(self, *args):
        '''Perform a remote copy'''
        cpstring=self.CpCommand
        for arg in args:
            cpstring = cpstring + " \'" + arg + "\'"
        return os.system(cpstring) == 0



class LogWatcher:

    '''This class watches logs for messages that fit certain regular
       expressions.  Watching logs for events isn't the ideal way
       to do business, but it's better than nothing :-)

       On the other hand, this class is really pretty cool ;-)

       The way you use this class is as follows:
          Construct a LogWatcher object
          Call setwatch() when you want to start watching the log
          Call look() to scan the log looking for the patterns
    '''

    def __init__(self, log, regexes, timeout=10, debug=0):
        '''This is the constructor for the LogWatcher class.  It takes a
        log name to watch, and a list of regular expressions to watch for."
        '''

        #  Validate our arguments.  Better sooner than later ;-)
        for regex in regexes:
            assert re.compile(regex)
        self.regexes = regexes
        self.filename = log
        self.debug=debug
        if self.debug:
            print "Debug now on for for log", log
        self.Timeout = int(timeout)
        if not os.access(log, os.R_OK):
            raise ValueError("File [" + log + "] not accessible (r)")

    def setwatch(self, frombeginning=None):
        '''Mark the place to start watching the log from.
        '''
        self.file = open(self.filename, "r")
        self.size = os.path.getsize(self.filename)
        if not frombeginning:
            self.file.seek(0,2)

    def look(self, timeout=None):
        '''Examine the log looking for the given patterns.
        It starts looking from the place marked by setwatch().
        This function looks in the file in the fashion of tail -f.
        It properly recovers from log file truncation, but not from
        removing and recreating the log.  It would be nice if it
        recovered from this as well :-)

        We return the first line which matches any of our patterns.
        '''

        if timeout == None: timeout = self.Timeout

        done=time.time()+timeout+1
        while (time.time() <= done):
            newsize=os.path.getsize(self.filename)
            if self.debug: print "newsize = %d" % newsize
            if newsize < self.size:
                # Somebody truncated the log!
                if self.debug: print "Log truncated!"
                self.watch(frombeginning=1, timeout=timeout)
                continue
            if newsize > self.file.tell():
                line=self.file.readline()
                if self.debug: print "Looking at line:", line
                if line:
                    for regex in self.regexes:
                        if self.debug: print "Comparing line to ", regex
                        if re.search(regex, line):
                            return line
            newsize=os.path.getsize(self.filename)
            if self.file.tell() == newsize:
                if timeout > 0:
                    time.sleep(1)
                else:
                    return None
        return None

class ClusterManager(UserDict):
    '''The Cluster Manager class.
    This is an subclass of the Python dictionary class.
    (this is because it contains lots of {name,value} pairs,
    not because it's behavior is that terribly similar to a
    dictionary in other ways.)

    This is an abstract class which class implements high-level
    operations on the cluster and/or its cluster managers.
    Actual cluster managers classes are subclassed from this type.

    One of the things we do is track the state we think every node should
    be in.
    '''


    def __InitialConditions(self):
        #if os.geteuid() != 0:
        #  raise ValueError("Must Be Root!")
	None

    def _finalConditions(self):
        for key in self.keys():
            if self[key] == None:
                raise ValueError("Improper derivation: self[" + key
                +   "] must be overridden by subclass.")

    def __init__(self, Environment, randseed=None):
        self.Env = Environment
        self.__InitialConditions()
        self.data = {
            "up"	     : "up",	# Status meaning up
            "down"	     : "down",  # Status meaning down
            "StonithCmd"     : "/usr/sbin/stonith -t baytech -p '10.10.10.100 admin admin' %s",
            "DeadTime"	     : 30,	# Max time to detect dead node...
            "StartTime"	     : 90,	# Max time to start up
    #
    # These next values need to be overridden in the derived class.
    #
            "Name"	     : None,
            "StartCmd"	     : None,
            "StopCmd"	     : None,
            "StatusCmd"	     : None,
            "RereadCmd"	     : None,
            "TestConfigDir"  : None,
            "LogFileName"    : None,

            "Pat:We_started"   : None,
            "Pat:They_started" : None,
            "Pat:We_stopped"   : None,
            "Pat:They_stopped" : None,

            "BadRegexes"     : None,	# A set of "bad news" regexes
                                        # to apply to the log
        }
        self.rsh = RemoteExec()

        self.ShouldBeStatus={}
        self.OurNode=os.uname()[1]
        self.ShouldBeStatus={}

    def log(self, args):
        self.Env.log(args)


    def prepare(self):
        '''Finish the Initialization process. Prepare to test...'''

        for node in self.Env["nodes"]:
          if self.StataCM(node):
              self.ShouldBeStatus[node]=self["up"]
          else:
              self.ShouldBeStatus[node]=self["down"]

    def upcount(self):
        '''How many nodes are up?'''
        count=0
        for node in self.Env["nodes"]:
          if self.ShouldBeStatus[node]==self["up"]:
            count=count+1
        return count

    def TruncLogs(self):
        '''Truncate the log for the cluster manager so we can start clean'''
        if self["LogFileName"] != None:
            os.system("cp /dev/null " + self["LogFileName"])

    def StartaCM(self, node):

        '''Start up the cluster manager on a given node'''

        rc=self.rsh(node, self["StartCmd"])
        if rc == 0:
            self.ShouldBeStatus[node]=self["up"]
            return 1
        else:
            self.log ("Could not start %s on node %s"
            %	(self["Name"], node))
        return None

    def StopaCM(self, node):

        '''Stop the cluster manager on a given node'''

        rc=self.rsh(node, self["StopCmd"])
        if rc == 0:
            self.ShouldBeStatus[node]=self["down"]
            return 1
        else:
            self.log ("Could not stop %s on node %s"
            %	(self["Name"], node))
        return None

    def RereadCM(self, node):

        '''Force the cluster manager on a given node to reread its config
           This may be a no-op on certain cluster managers.
        '''

        rc=self.rsh(node, self["RereadCmd"])
        if rc == 0:
            return 1
        else:
            self.log ("Could not force %s on node %s to reread its config"
            %	(self["Name"], node))
        return None


    def StataCM(self, node):

        '''Report the status of the cluster manager on a given node'''

        out=self.rsh.readaline(node, self["StatusCmd"])
        ret= (string.find(out, 'stopped') == -1)

        try:
            if ret:
                if self.ShouldBeStatus[node] != self["up"]:
                    self.log(
                    "Node status for %s is %s but we think it should be %s"
                    %	(node, self["up"], self.ShouldBeStatus[node]))
            else:
                if self.ShouldBeStatus[node] != self["down"]:
                    self.log(
                    "Node status for %s is %s but we think it should be %s"
                    %	(node, self["down"], self.ShouldBeStatus[node]))
        except KeyError:	pass

        if ret:	self.ShouldBeStatus[node]=self["up"]
        else:	self.ShouldBeStatus[node]=self["down"]
        return ret

    def startall(self, nodelist=None):

        '''Start the cluster manager on every node in the cluster.
        We can do it on a subset of the cluster if nodelist is not None.
        '''

        map = {}
        if not nodelist:
            nodelist=self.Env["nodes"]
        for node in nodelist:
            if self.ShouldBeStatus[node] == self["down"]:
                self.StartaCM(node)

    def stopall(self, nodelist=None):

        '''Stop the cluster managers on every node in the cluster.
        We can do it on a subset of the cluster if nodelist is not None.
        '''

        map = {}
        if not nodelist:
            nodelist=self.Env["nodes"]
        for node in self.Env["nodes"]:
            if self.ShouldBeStatus[node] == self["up"]:
                self.StopaCM(node)


    def rereadall(self, nodelist=None):

        '''Force the cluster managers on every node in the cluster
        to reread their config files.  We can do it on a subset of the
        cluster if nodelist is not None.
        '''

        map = {}
        if not nodelist:
            nodelist=self.Env["nodes"]
        for node in self.Env["nodes"]:
            if self.ShouldBeStatus[node] == self["up"]:
                self.RereadCM(node)


    def statall(self, nodelist=None):

        '''Return the status of the cluster managers in the cluster.
        We can do it on a subset of the cluster if nodelist is not None.
        '''

        result={}
        if not nodelist:
            nodelist=self.Env["nodes"]
        for node in nodelist:
            if self.StataCM(node):
                result[node] = self["up"]
            else:
                result[node] = self["down"]
        return result

    def SyncTestConfigs(self):
        '''Synchronize test configurations throughout the cluster.
        This one's a no-op for FailSafe, since it does that by itself.
        '''

        fromdir=self["TestConfigDir"]

        if not os.access(fromdir, os.F_OK | os.R_OK | os.W_OK):
            raise ValueError("Directory [" + fromdir + "] not accessible (rwx)")

        for node in self.Env["nodes"]:
            if node == self.OurNode:	continue
            self.log("Syncing test configurations on " + node)
            # Perhaps I ought to use rsync...
            self.rsh.cp("-r", fromdir, node + ":" + fromdir)

    def SetClusterConfig(self, configpath="default", nodelist=None):
        '''Activate the named test configuration throughout the cluster.
        It would be useful to implement this :-)
        '''
        pass
        return 1


    def ResourceGroups(self):
         "Return a list of resource type/instance pairs for the cluster"
         raise ValueError("Abstract Class member (ResourceGroups)")

    def HasQuorum(self):
         "Return TRUE if the cluster currently has quorum"
         raise ValueError("Abstract Class member (HasQuorum)")

class Resource:
    '''
    This is an HA resource (not a resource group).
    A resource group is just an ordered list of Resource objects.
    '''

    def __init__(self, cm, rsctype=None, instance=None):
        self.CM = cm
        self.ResourceType = rsctype
        self.Instance = instance

    def Type(self, nodename):
        return self.ResourceType

    def Instance(self, nodename):
        return self.Instance

    def IsRunningOn(self, nodename):
        '''
        This member function returns true if our resource is running
        on the given node in the cluster.
        It is analagous to the "status" operation on SystemV init scripts and
        heartbeat scripts.  FailSafe calls it the "exclusive" operation.
        '''
        raise ValueError("Abstract Class member (IsRunningOn)")
        return None

    def IsWorkingCorrectly(self, nodename):
        '''
        This member function returns true if our resource is operating
        correctly on the given node in the cluster.
        on the given node in the cluster.
        Heartbeat does not require this operation, but it might be called
        the Monitor operation, which is what FailSafe calls it.
        For remotely monitorable resources (like IP addresses), they *should*
        be monitored remotely for testing.
        '''
        raise ValueError("Abstract Class member (IsWorkingCorrectly)")
        return None

    def __repr__(self):
        return "{" + self.ResourceType + "::" + self.Instance + "}"


class ScenarioComponent:

    def __init__(self, Env):
	self.Env = Env

    def IsApplicable(self):
        '''Return TRUE if the current ScenarioComponent is applicable
	in the given LabEnvironment given to the constructor.
	'''

        raise ValueError("Abstract Class member (IsApplicable)")

    def SetUp(self, CM):
        '''Set up the given ScenarioComponent'''
        raise ValueError("Abstract Class member (Setup)")

    def TearDown(self, CM):
        '''Tear down (undo) the given ScenarioComponent'''
        raise ValueError("Abstract Class member (Setup)")
	
	

class Scenario:
    (
'''The basic idea of a scenario is that of an ordered list of
ScenarioComponent objects.  Each ScenarioComponent is SetUp() in turn,
and then after the tests have been run, they are torn down using TearDown()
(in reverse order).

A Scenario is applicable to a particular cluster manager iff each
ScenarioComponent is applicable.

A partially set up scenario is torn down if it fails during setup.
''')

    def __init__(self, Components):

        "Initialize the Scenario from the list of ScenarioComponents"

        for comp in Components:

            if not issubclass(comp.__class__, ScenarioComponent):
                raise ValueError("Init value must be subclass of"
		" ScenarioComponent")
        self.Components = Components


    def IsApplicable(self):
        (
'''A Scenario IsApplicable() iff each of its ScenarioComponents IsApplicable()
'''
        )

        for comp in self.Components:
            if not comp.IsApplicable():
                return None
        return 1

    def SetUp(self, CM):
        '''Set up the Scenario. Return TRUE on success.'''

        j=0
        while j < len(self.Components):
            if not self.Components[j].SetUp(CM):
                # OOPS!  We failed.  Tear partial setups down.
                CM.log("Tearing down partial setup")
                self.TearDown(CM, j)
                return None
            j=j+1
        return 1

    def TearDown(self, CM, max=None):

        '''Tear Down the Scenario - in reverse order.'''

        if max == None:
            max = len(self.Components)-1
        j=max
        while j >= 0:
            self.Components[j].TearDown(CM)
            j=j-1


class InitClusterManager(ScenarioComponent):
    (
'''InitClusterManager is the most basic of ScenarioComponents.
This ScenarioComponent simply starts the cluster manager on all the nodes.
It is fairly robust as it waits for all nodes to come up before starting
as they might have been rebooted or crashed for some reason beforehand.
''')

    def IsApplicable(self):
        '''InitClusterManager is so generic it is always Applicable'''
        return 1


    def _IsNodeBooted(self, node):
	'''Return TRUE if the given node is booted (responds to pings'''
        return os.system("ping -nq -c1 -w1 %s >/dev/null 2>&1" % node) == 0


    def _WaitForNodeToComeUp(self, node, Timeout=300):

	'''Return TRUE when given node comes up, or FALSE if timeout'''

        timeout=Timeout
        anytimeouts=0
	while timeout > 0:
            if self._IsNodeBooted(node):
                if anytimeouts:
                     # Fudge to wait for the system to finish coming up
                     time.sleep(30)
                     self.Env.log("Node %s now up" % node)
                return 1

            time.sleep(1)
            if (not anytimeouts):
                self.Env.log("Waiting for node %s to come up" % node)
                
            anytimeouts=1
            timeout = timeout - 1

        self.Env.log("%s did not come up within %d tries" % (node, Timeout))
        return None

    def _WaitForAllNodesToComeUp(self, nodes, timeout=300):
	'''Return TRUE when all nodes come up, or FALSE if timeout'''

        for node in nodes:
            if not self._WaitForNodeToComeUp(node, timeout):
                return None
        return 1



    def SetUp(self, CM):
        '''Basic Cluster Manager startup.  Start everything'''

        if not self._WaitForAllNodesToComeUp(CM.Env["nodes"]):
            return None
        CM.prepare()

        #	Clear out the cobwebs ;-)

        self.TearDown(CM)

        # Now start the Cluster Manager on all the nodes.

        CM.log("Starting Cluster Manager on all nodes")
        CM.startall()
        return 1


    def TearDown(self, CM):
        '''Set up the given ScenarioComponent'''

        self._WaitForAllNodesToComeUp(CM.Env["nodes"])

        # Stop the cluster manager everywhere

        CM.log("Stopping Cluster Manager on all nodes")
        CM.stopall()

class PingFest(ScenarioComponent):
    (
'''PingFest does a flood ping to each node in the cluster from the test machine.

If the LabEnvironment Parameter PingSize is set, it will be used as the size
of ping packet requested (via the -s option).  If it is not set, it defaults
to 1024 bytes.

According to the manual page for ping:
    Outputs packets as fast as they come back or one hundred times per
    second, whichever is more.  For every ECHO_REQUEST sent a period ``.''
    is printed, while for every ECHO_REPLY received a backspace is printed.
    This provides a rapid display of how many packets are being dropped.
    Only the super-user may use this option.  This can be very hard on a net�
    work and should be used with caution.            

''' )

    def __init__(self, Env):
	self.Env = Env

    def IsApplicable(self):
        '''PingFests are always applicable ;-)
	'''

        return 1

    def SetUp(self, CM):
        '''Start the PingFest!'''

        self.PingSize=1024
        if CM.Env.has_key("PingSize"):
		self.PingSize=CM.Env["PingSize"]

        CM.log("Starting %d byte flood pings" % self.PingSize)

        self.PingPids=[]
        for node in CM.Env["nodes"]:
            self.PingPids.append(self._pingchild(node))

        CM.log("Ping PIDs: " + repr(self.PingPids))
        return 1

    def TearDown(self, CM):
        '''Stop it right now!  My ears are pinging!!'''

        for pid in self.PingPids:
            if pid != None:
                CM.log("Stopping ping process %d" % pid)
	        os.kill(pid, signal.SIGKILL)

    def _pingchild(self, node):

        Args = ["ping", "-qfn", "-s", str(self.PingSize), node]


        sys.stdin.flush()
        sys.stdout.flush()
        sys.stderr.flush()
        pid = os.fork()

        if pid < 0:
            self.Env.log("Cannot fork ping child")
            return None
        if pid > 0:
            return pid


        # Otherwise, we're the child process.

   
        os.execvp("ping", Args)
        self.Env.log("Cannot execvp ping: " + repr(Args))
        sys.exit(1)


        
        
	


class RandomTests:
    '''
    A collection of tests which are run at random.
    '''
    def __init__(self, scenario, cm, tests, Audits):

        self.CM = cm
        self.Env = cm.Env
        self.Scenario = scenario
        self.Tests = []

        for test in tests:
            if not issubclass(test.__class__, CTStests.CTSTest):
                raise ValueError("Init value must be a subclass of CTSTest")
            if test.is_applicable():
                self.Tests.append(test)

        if not scenario.IsApplicable():
                raise ValueError("Scenario not applicable in"
                " given Environment")

       
        self.Stats = {"success":0, "failure":0, "BadNews":0}
        self.IndividualStats= {}

        self.Audits = Audits

    def incr(self, name):
        '''Increment (or initialize) the value associated with the given name'''
        if not self.Stats.has_key(name):
            self.Stats[name]=0
        self.Stats[name] = self.Stats[name]+1

    def run(self, max=1):
        (
'''
Set up the given scenario, then run the selected tests at
random for the selected number of iterations.
''')

        if not self.Scenario.SetUp(self.CM):
            return None

        BadNews=LogWatcher(self.CM["LogFileName"], self.CM["BadRegexes"]
        ,	timeout=0)
        BadNews.setwatch()
        testcount=1

	# This makes sure everything is stabilized before starting...
        for audit in self.Audits:
            if not audit():
                self.CM.log("Audit " + audit.name() + " Failed.")
                self.incr("auditfail")
        while testcount <= max:
            test = self.Env.RandomGen.choice(self.Tests)

            # Some tests need a node as an argument.

            self.CM.log("Running test %s [%d]" % (test.name, testcount))
            testcount = testcount + 1
            try:
                ret=test()
            except TypeError:
                ret=test(self.Env.RandomNode())

            if ret:
                self.incr("success");
            else:
                self.incr("failure");
        	# Better get the current info from the cluster...
                self.CM.statall()

            for tries in 1,2,3,4,5,6,7,8,9,10:
                match=BadNews.look()
                if match:
                   if not re.search(" CTS: ", match):
                      self.CM.log(match)
                      self.incr("BadNews");
                else:
                  break
            else:
              self.CM.log("Big problems.  Shutting down.")
              self.CM.stopall()
              raise ValueError("Looks like we hit the jackpot!	:-)")

            for audit in self.Audits:
                if not audit():
                    self.CM.log("Audit " + audit.name() + " Failed.")
                    test.incr("auditfail")
                    self.incr("auditfail")
	    #os.system("ssh -l root sgi1 uname -a")
	    #os.system("ssh -l root sgi1 /sbin/ifconfig eth0:0")
	    #os.system("ssh -l root sgi1 /sbin/ifconfig eth0:1")
	    #os.system("ssh -l root sgi2 uname -a")
	    #os.system("ssh -l root sgi2 /sbin/ifconfig eth0:0")
	    #os.system("ssh -l root sgi2 /sbin/ifconfig eth0:1")

        self.Scenario.TearDown(self.CM)

        for test in self.Tests:
            self.IndividualStats[test.name] = test.Stats

        return self.Stats, self.IndividualStats
