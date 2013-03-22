/*
Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
*/

/******************************************************************************
 ***                                                                        ***
 ***                   Configuration parameter calculations                 ***
 ***                                                                        ***
 ******************************************************************************
 *
 *  Module: 
 *      Name: mcc.configuration.calculations
 *
 *  Description:
 *      Calculating environment dependent parameters for processes and types
 *
 *  External interface:
 *      mcc.configuration.calculations.autoConfigure: Auto add processes
 *      mcc.configuration.calculations.instanceSetup: Predef instance params
 *      mcc.configuration.calculations.typeSetup: Predef type params
 *
 *  External data: 
 *      None
 *
 *  Internal interface: 
 *      hwDepParams: Calculate hw dependent data node parameters
 *      ndb_mgmd_setup: ndb_mgmd process specific parameter assignments
 *      ndbd_setup: ndbd process specific parameter assignments
 *      mysqld_setup: mysqld process specific parameter assignments
 *
 *  Internal data: 
 *      None
 *
 *  Unit test interface: 
 *      None
 *
 *  Todo:
        Implement unit tests.
 * 
 ******************************************************************************/

/****************************** Import/export  ********************************/

dojo.provide("mcc.configuration.calculations");

dojo.require("mcc.util");
dojo.require("mcc.storage");
dojo.require("mcc.configuration");

/**************************** External interface  *****************************/

mcc.configuration.calculations.autoConfigure = autoConfigure;
mcc.configuration.calculations.instanceSetup = instanceSetup;
mcc.configuration.calculations.typeSetup = typeSetup;

/****************************** Implementation  *******************************/

// Add processes to the cluster if noone exists already
function autoConfigure() {
    var waitCondition = new dojo.Deferred();
    // If no processes, add
    mcc.storage.processStorage().getItems({}).then(function (items) {
        // Shortcut if we have processes already
        if (items && items.length > 0) {
            mcc.util.dbg("Processes already exist, not adding default")
            waitCondition.resolve();
            return;
        }
        mcc.storage.hostStorage().getItems({}).then(function (hosts) {
            var anyHost = null;

            // Remove the wildcard host from the list
            for (var i in hosts) {
                if (hosts[i].getValue("anyHost")) {
                    anyHost = hosts[i];
                    hosts.splice(i, 1);
                    break;
                }
            }

            // First sort by name to get repeatable allocation (needed for tests)
            hosts.sort(function (h1, h2) {
                if (h1.getValue("name") < h2.getValue("name")) {
                    return -1;
                } else {
                    return 1;
                }
            });

            // Shortcut if we have no hosts, or only wildcard host
            if (!hosts || hosts.length == 0 || 
                    (hosts.length == 1 && hosts[0].getValue("anyHost"))) {
                alert("No hosts - unable to add default processes")
                mcc.util.dbg("No hosts - unable tocall add default processes")
                waitCondition.resolve();
                return;
            }
            
            var typeIds = [];
            var names = [];
            var familyHead = [];    // Ptype hashed on family name
            var typeHead = [];      // Ptype hashed on type name
            var dataNodeId = 1; 
            var otherNodeId = 49; 

            // Get ids of all process types
            mcc.storage.processTypeStorage().forItems({}, function (pType) {
                if (!familyHead[pType.getValue("family")]) {
                    familyHead[pType.getValue("family")] = pType;
                }
                typeHead[pType.getValue("name")] = 
                        familyHead[pType.getValue("family")];
                typeIds[pType.getValue("name")] = pType.getId();
                names[pType.getValue("name")] = pType.getValue("nodeLabel");
            },
            function () {
                // Add new process
                function newProcess(pname, host) {
                    mcc.storage.processStorage().newItem({
                        name: names[pname] + " " + 
                                typeHead[pname].getValue("currSeq"),
                        host: host.getId(),
                        processtype: typeIds[pname],
                        NodeId: (pname == "ndbd" || pname == "ndbmtd") ? 
                                dataNodeId++ : otherNodeId++,
                        seqno: typeHead[pname].getValue("currSeq")
                    });
                }

                // Sort host array on RAM
                hosts.sort(function (a, b) {
                    // Treat unefined ram as smallest
                    if (!a.getValue("ram") && !b.getValue("ram")) {
                        return 0;
                    }
                    if (!a.getValue("ram")) {
                        return -1;
                    }
                    if (!b.getValue("ram")) {
                        return 1;
                    }

                    // Put largest ram at end where ndbds are allocated
                    if (+a.getValue("ram") < +b.getValue("ram")) {
                        return -1;
                    } else if (+a.getValue("ram") > +b.getValue("ram")) {
                        return 1;
                    } else {
                        return 0;
                    }
                });
                
                if (hosts.length == 1) {
                    // One host: 1*mgmd + 3*api + 2*mysqld + 2*ndbd
                    newProcess("ndb_mgmd", hosts[0]);
                    for (var i = 0; i < 3; ++i) {
                        newProcess("api", hosts[0]);
                    }
                    for (var i = 0; i < 2; ++i) {
                        newProcess("mysqld", hosts[0]);
                    }
                    for (var i = 0; i < 2; ++i) {
                        newProcess("ndbmtd", hosts[0]);
                    }
                } else if (hosts.length == 2) {
                    // Two hosts: 1*mgmd + 2*api + 1*mysqld + 1*ndbd, 
                    //            2*api + 1*mysqld + 1*ndbd 
                    newProcess("ndb_mgmd", hosts[0]);
                    for (var i = 0; i < 2; ++i) {
                        newProcess("api", hosts[0]);
                    }
                    newProcess("mysqld", hosts[0]);
                    newProcess("ndbmtd", hosts[0]);

                    for (var i = 0; i < 2; ++i) {
                        newProcess("api", hosts[1]);
                    }
                    newProcess("mysqld", hosts[1]);
                    newProcess("ndbmtd", hosts[1]);
                } else if (hosts.length == 3) {
                    // Three hosts: 1*mgmd + 3*api + 2*mysqld, 
                    //              1*ndbd, 
                    //              1*ndbd
                    newProcess("ndb_mgmd", hosts[0]);
                    for (var i = 0; i < 3; ++i) {
                        newProcess("api", hosts[0]);
                    }
                    for (var i = 0; i < 2; ++i) {
                        newProcess("mysqld", hosts[0]);
                    }
                    for (var i = 0; i < 2; ++i) {
                        newProcess("ndbmtd", hosts[i + 1]);
                    }

                } else if (hosts.length > 3) {
                    // N>3 hosts: First, divide hosts into groups
                    var nNDBD = Math.floor(hosts.length / 4) * 2;
                    var nSQL = hosts.length - nNDBD;

                    // Use 2 hosts for 1*mgmds + 2*api on each
                    for (var i = 0; i < 2; ++i) {
                        if (otherNodeId <= 255) {
                            newProcess("ndb_mgmd", hosts[i]);
                        }
                        if (otherNodeId <= 255) {
                            newProcess("api", hosts[i]);
                        }
                        if (otherNodeId <= 255) {
                            newProcess("api", hosts[i]);
                        }
                    }
                    // Possibly two more api on third host
                    if (hosts.length > 4) {
                        if (otherNodeId <= 255) {
                            newProcess("api", hosts[2]);
                        }
                        if (otherNodeId <= 255) {
                            newProcess("api", hosts[2]);
                        }
                    }
                    // Use N - (N DIV 4)*2 hosts for mysqlds, two on each
                    for (var i = 0; i < nSQL; ++i) {
                        if (otherNodeId <= 255) {
                            newProcess("mysqld", hosts[i]);
                        }
                        if (otherNodeId <= 255) {
                            newProcess("mysqld", hosts[i]);
                        }
                    }
                    // Use (N DIV 4)*2 hosts for data nodes, one on each
                    for (var i = nSQL; i < nSQL + nNDBD; ++i) {
                        if (dataNodeId <= 48) {
                            newProcess("ndbmtd", hosts[i]);
                        }
                    }
                }
                mcc.util.dbg("Default processes added")
                waitCondition.resolve();
            });
        });
    });
    return waitCondition;
}

// Calculate hw dependent data node parameters
function hwDepParams(processTypeName) {

    // Single deferred to callback
    var waitCondition = new dojo.Deferred();

    // Array of deferreds to wait for
    var waitConditions= [];
    var waitList; 

    // Fetch processes
    mcc.storage.processTypeStorage().getItems({name: processTypeName}).
            then(function (ptypes) {
        mcc.storage.processStorage().getItems({
                processtype: ptypes[0].getId()}).then(function (nodes) {
            for (var i in nodes) {
                // Run instance setup
                waitConditions[i] = instanceSetup(ptypes[0].
                        getValue("family"), nodes[i]);
            }

            // After looping over all processes, wait for DeferredList
            waitList = new dojo.DeferredList(waitConditions);
            waitList.then(function () {
                waitCondition.resolve();
            });
        });
    });

    return waitCondition;
}

// Calculate process type parameters depending on environment and external input
function typeSetup(processItem) {

    var processtype = processItem.getValue("family");
    var waitCondition = new dojo.Deferred();
    mcc.util.dbg("Setup process type defaults for " + processtype);

    // Process type specific assignments
    if (processtype == "management") {
        // Get portbase, set default port
        var pbase = processItem.getValue("Portbase");
        if (pbase === undefined) {
            pbase = mcc.configuration.getPara(processtype, null, 
                    "Portbase", "defaultValueType");
        }
        mcc.configuration.setPara(processtype, null, "Portnumber",
                "defaultValueType", pbase);
        // Leave process type level datadir undefined
        waitCondition.resolve();
    } else if (processtype == "data") {

        // Check parameters that depend on cluster defaults
        mcc.storage.clusterStorage().getItem(0).then(function (cluster) {
            // Leave process type level datadir undefined

            // Check real time or web mode
            if (cluster.getValue("apparea") != "realtime") {
                mcc.configuration.setPara(processtype, null, 
                        "HeartbeatIntervalDbDb", "defaultValueType", 15000);
                mcc.configuration.setPara(processtype, null, 
                        "HeartbeatIntervalDbApi", "defaultValueType", 15000);
            } else {
                mcc.configuration.setPara(processtype, null, 
                        "HeartbeatIntervalDbDb", "defaultValueType", 1500);
                mcc.configuration.setPara(processtype, null, 
                        "HeartbeatIntervalDbApi", "defaultValueType", 1500);
            }

            // Check read/write load
            if (cluster.getValue("writeload") == "high") {
                mcc.configuration.setPara(processtype, null, 
                        "SendBufferMemory", "defaultValueType", 8);
                mcc.configuration.setPara(processtype, null, 
                        "ReceiveBufferMemory", "defaultValueType", 8);
                mcc.configuration.setPara(processtype, null, 
                        "RedoBuffer", "defaultValueType", 64);
            } else if (cluster.getValue("writeload") == "medium") {
                mcc.configuration.setPara(processtype, null, 
                        "SendBufferMemory", "defaultValueType", 4);
                mcc.configuration.setPara(processtype, null, 
                        "ReceiveBufferMemory", "defaultValueType", 4);
                mcc.configuration.setPara(processtype, null, 
                        "RedoBuffer", "defaultValueType", 32);
            } else {
                mcc.configuration.setPara(processtype, null, 
                        "SendBufferMemory", "defaultValueType", 2);
                mcc.configuration.setPara(processtype, null, 
                        "ReceiveBufferMemory", "defaultValueType", 2);
                mcc.configuration.setPara(processtype, null, 
                        "RedoBuffer", "defaultValueType", 32);
            }

            // Get disk page buffer memory, assign shared global memory
            var diskBuf = processItem.getValue(
                    mcc.configuration.getPara(processtype, null, 
                            "DiskPageBufferMemory", "attribute"));
            if (!diskBuf) {
                diskBuf = mcc.configuration.getPara(processtype, null, 
                    "DiskPageBufferMemory", "defaultValueType");
            }

            if (diskBuf > 8192) {
                mcc.configuration.setPara(processtype, null, 
                        "SharedGlobalMemory", "defaultValueType", 1024);
            } else if (diskBuf > 64) {
                mcc.configuration.setPara(processtype, null, 
                        "SharedGlobalMemory", "defaultValueType", 384);
            } else {
                mcc.configuration.setPara(processtype, null, 
                        "SharedGlobalMemory", "defaultValueType", 20);
            }

            // Restrict MaxNoOfTables
            var maxTab = processItem.getValue(
                    mcc.configuration.getPara(processtype, null, 
                            "MaxNoOfTables", "attribute"));
            if (maxTab) {
                if (maxTab > 20320) {
                    processItem.setValue(
                        mcc.configuration.getPara(processtype, null, 
                            "MaxNoOfTables", "attribute"), 20320);
                } else if (maxTab < 128) {
                    processItem.setValue(
                        mcc.configuration.getPara(processtype, null, 
                            "MaxNoOfTables", "attribute"), 128);
                }
                mcc.storage.processStorage().save();
            }

            // Calculate datamem, indexmem, and maxexecthreads
            hwDepParams("ndbd").then(function () {
                hwDepParams("ndbmtd").then(function () {

                    // Get predefined data node parameters
                    var params = mcc.configuration.getAllPara("data");

                    function setLow(param) {
                        var low = undefined;
                        // Loop over instance values, collect min, set
                        for (var i in params[param].defaultValueInstance) {
                            var curr = params[param].defaultValueInstance[i];
                            if (low === undefined || 
                                    (curr !== undefined && curr < low)) {
                                low = curr;
                            }
                        }
                        mcc.util.dbg("Lowest value for " + param + 
                                " now: " + low);
                        if (low !== undefined) {
                            mcc.configuration.setPara(processtype, null, 
                                    param, "defaultValueType", low);
                        }
                    }

                    setLow("DataMemory");
                    setLow("IndexMemory");
                    setLow("MaxNoOfExecutionThreads");

                    // Get overridden redo log file size
                    var fileSz = processItem.getValue("FragmentLogFileSize");
                    
                    // If not overridden, set value depending on app area
                    if (!fileSz) {
                        // Lower value if simple testing, easier on resources
                        if (cluster.getValue("apparea") == "simple testing") {
                            fileSz = 64;
                        } else {
                            fileSz = 256;                        
                        }
                        mcc.configuration.setPara(processtype, null, 
                                "FragmentLogFileSize", "defaultValueType", fileSz);
                    }
                    mcc.util.dbg("FragmentLogFileSize=" + fileSz);

                    // Caclulate and set number of files
                    var dataMem = mcc.configuration.getPara(processtype, null, 
                                    "DataMemory", "defaultValueType");
                    var noOfFiles = 16;

                    // Use def value unless not simple testing and DataMem defined
                    if (cluster.getValue("apparea") != "simple testing" && dataMem) {
                        noOfFiles = Math.floor(6 * dataMem / fileSz / 4);
                    }
                        
                    // At least three files in each set
                    if (noOfFiles < 3) {
                        noOfFiles = 3;
                    }
                    mcc.util.dbg("NoOfFragmentLogFiles=" + noOfFiles);
                    mcc.configuration.setPara(processtype, null, 
                            "NoOfFragmentLogFiles", "defaultValueType", 
                            noOfFiles);

		    // Get number of data nodes
		    mcc.util.getNodeDistribution().then(function (nNodes) {
                        mcc.configuration.setPara(processtype, null, 
                                "NoOfReplicas", "defaultValueType", 
                                2 - (nNodes['ndbd'] + nNodes['ndbmtd']) % 2);
			waitCondition.resolve();
		    });
                });
            });
        });
    } else if (processtype == "sql") {
        // Get portbase, set default port
        var pbase = processItem.getValue("Portbase");
        if (pbase === undefined) {
            pbase = mcc.configuration.getPara(processtype, null, 
                    "Portbase", "defaultValueType");
        }
        mcc.configuration.setPara(processtype, null, "Port",
                "defaultValueType", pbase);
        // Leave process type level socket and datadir undefined
        waitCondition.resolve();
    } else if (processtype == "api") {
        waitCondition.resolve();
    }
    return waitCondition;
}

// ndb_mgmd process specific parameter assignments
function ndb_mgmd_setup(processitem, processtype, host, waitCondition) {
    var id = processitem.getId();
    var datadir = host.getValue("datadir");
    var dirSep = mcc.util.dirSep(datadir);

    // Set datadir
    mcc.configuration.setPara(processtype, id, "DataDir",
            "defaultValueInstance", datadir +
            processitem.getValue("NodeId") + dirSep);

    // Get colleague nodes, find own index on host
    mcc.util.getColleagueNodes(processitem).then(function (colleagues) {
        var myIdx = dojo.indexOf(colleagues, processitem.getId());

        // Get process type
        mcc.storage.processTypeStorage().getItem(
                processitem.getValue("processtype")).then(function (ptype) {
            // Get type's overridden port base
            var pbase = ptype.getValue("Portbase");

            // If not overridden, use type default
            if (pbase === undefined) {
                pbase = mcc.configuration.getPara(processtype, null, 
                        "Portbase", "defaultValueType");
            }
            // Set port using retrieved portbase and node index on host
            mcc.configuration.setPara(processtype, id, "Portnumber",
                    "defaultValueInstance", myIdx + pbase);

            waitCondition.resolve();
        });
    });
}

// ndbXd process specific parameter assignments
function ndbd_setup(processitem, processtype, host, waitCondition) {
    var id = processitem.getId();
    var datadir = host.getValue("datadir");
    var dirSep = mcc.util.dirSep(datadir);

    // Set datadir
    mcc.configuration.setPara(processtype, id, "DataDir",
            "defaultValueInstance", datadir +
            processitem.getValue("NodeId") + dirSep);

    // Get cluster attributes
    mcc.storage.clusterStorage().getItem(0).then(function (cluster) {
        // Get node distribution (deferred)
        mcc.util.getNodeDistribution().then(function(nNodes) {
            var noOfMysqld= nNodes["mysqld"];
            var noOfNdbd= nNodes["ndbd"] + nNodes["ndbmtd"];
        
            // Need these for calculations below
            var MaxNoOfTables= mcc.configuration.getPara(processtype, null,
                    "MaxNoOfTables", "defaultValueType");
            var sendreceive=  mcc.configuration.getPara(processtype, null,
                    "SendBufferMemory", "defaultValueType");
            var DiskPageBufferMemory=  mcc.configuration.getPara(processtype,
                    null, "DiskPageBufferMemory", "defaultValueType");
            var SharedGlobalMemory=  mcc.configuration.getPara(processtype, 
                    null, "SharedGlobalMemory", "defaultValueType");
            var RedoBuffer=  mcc.configuration.getPara(processtype, null,
                    "RedoBuffer", "defaultValueType");
    
            // Change this setting if we support managing connection pooling
            var connectionPool= 1;
    
            // Temporary variables used in memory calculations
            var reserveMemoryToOS = 1024 * 1;
            var buffers = 300 * 1;
            var tableObjectMemory = MaxNoOfTables * 20 / 1024; // each ~ 20kB
            var attrsObjectMemory = tableObjectMemory * 6 * 200 / 1024 / 1024;
            var backup = 20;
            var indexes = (tableObjectMemory / 2) * 15 / 1024;
            var ops = 100000 / 1024;
            var connectionMemory= noOfMysqld * sendreceive * 2 * connectionPool
                    + 2 * 2 * sendreceive +
                    (noOfNdbd * (noOfNdbd - 1) * 2 * sendreceive);
            var multiplier = 800;
    
            // Get host ram and cores
            mcc.storage.hostStorage().getItem(processitem.getValue("host")).
                    then(function (host) {
                        
                var machineRAM = host.getValue("ram");
                var machineCores = host.getValue("cores");

                // Get number of data nodes on this host (deferred)
                mcc.util.getNodeDistribution(host.getId()).
                        then(function(nNodesOnHost) {
                    var nNdbdOnHost = nNodesOnHost["ndbd"] + 
                            nNodesOnHost["ndbmtd"];

                    // Set number of cores
                    if (!isNaN(machineCores)) {
                        var nExecThreads = 2;

                        // Lower value if simple testing, easier on resources
                        if (cluster.getValue("apparea") != "simple testing") {
                            // Divide by number of data nodes
                            machineCores = machineCores / nNdbdOnHost; 
                            if (machineCores > 6) {
                                nExecThreads = 8;
                            } else if (machineCores > 3) {
                                nExecThreads = 4;
                            }
                        }
                        
                        mcc.configuration.setPara(processtype, id, 
                                "MaxNoOfExecutionThreads",
                                "defaultValueInstance", nExecThreads);
                    }

                    // Set IndexMemory
                    if (!isNaN(machineRAM)) {
                        var indexMemory = Math.floor((machineRAM - 
                                reserveMemoryToOS - buffers - 
                                DiskPageBufferMemory - connectionMemory - 
                                tableObjectMemory - attrsObjectMemory - 
                                indexes - RedoBuffer - ops - backup - 
                                SharedGlobalMemory) / (8 * nNdbdOnHost));

                        // Lower value if simple testing, easier on resources
                        if (cluster.getValue("apparea") == "simple testing") {
                            indexMemory = Math.floor(indexMemory / 4);
                        }

                        // Obey constraints
                        var indexConstraints = mcc.configuration.
                                getPara(processtype, null,
                                "IndexMemory", "constraints");
                        if (indexMemory < indexConstraints.min) {
                            indexMemory = indexConstraints.min;
                        } else if (indexMemory > indexConstraints.max) {
                            indexMemory = indexConstraints.max;
                        }

                        mcc.configuration.setPara(processtype, id, 
                                "IndexMemory",
                                "defaultValueInstance", indexMemory);

                        // Use overridden indexMemory for dataMemory calc
                        var realIndexMemory = processitem.
                                getValue("IndexMemory");
                        if (!realIndexMemory) {
                            realIndexMemory = indexMemory;
                        }

                        // Set DataMemory
                        var dataMemory= Math.floor(multiplier * 
                                (machineRAM - reserveMemoryToOS - buffers -
                                DiskPageBufferMemory - connectionMemory -
                                tableObjectMemory - attrsObjectMemory - indexes-
                                RedoBuffer - ops - backup - SharedGlobalMemory -
                                realIndexMemory) / (1000 * nNdbdOnHost));

                        // Lower value if simple testing, easier on resources
                        if (cluster.getValue("apparea") == "simple testing") {
                            dataMemory = Math.floor(dataMemory / 4);
                        }

                        // Obey constraints
                        var dataConstraints = mcc.configuration.
                                getPara(processtype, null,
                                "DataMemory", "constraints");
                        if (dataMemory < dataConstraints.min) {
                            dataMemory = dataConstraints.min;
                        } else if (dataMemory > dataConstraints.max) {
                            dataMemory = dataConstraints.max;
                        }
                        
                        mcc.configuration.setPara(processtype, id, "DataMemory",
                                "defaultValueInstance", dataMemory);
                    }
                    waitCondition.resolve();
                });
            });
        });
    });
}

// mysqld process specific parameter assignments
function mysqld_setup(processitem, processtype, host, waitCondition) {
    var id = processitem.getId();
    var datadir = host.getValue("datadir");
    var dirSep = mcc.util.dirSep(datadir);

    // Set datadir and socket
    mcc.configuration.setPara(processtype, id, "DataDir",
            "defaultValueInstance", datadir +
            processitem.getValue("NodeId") + dirSep);

    mcc.configuration.setPara(processtype, id, "Socket",
            "defaultValueInstance", datadir +
            processitem.getValue("NodeId") + dirSep + 
            "mysql.socket");

    // Get colleague nodes, find own index on host
    mcc.util.getColleagueNodes(processitem).then(function (colleagues) {
        var myIdx = dojo.indexOf(colleagues, processitem.getId());

        // Get process type
        mcc.storage.processTypeStorage().getItem(
                processitem.getValue("processtype")).then(function (ptype) {
            // Get type's overridden port base
            var pbase = ptype.getValue("Portbase");

            // If not overridden, use type default
            if (pbase === undefined) {
                pbase = mcc.configuration.getPara(processtype, null, 
                        "Portbase", "defaultValueType");
            }

            // Set port using retrieved portbase and node index on host
            mcc.configuration.setPara(processtype, id, "Port",
                    "defaultValueInstance", myIdx + pbase);

            waitCondition.resolve();
        });
    });
}

// Calculate predefined values for a given process instance
function instanceSetup(processfam, processitem) {
    // Wait condition to return
    var waitCondition = new dojo.Deferred();
    var id = processitem.getId();
    
    mcc.util.dbg("Setup process instance defaults for " + 
            processitem.getValue("name"));

    // For any process, set HostName and datadir, unless wildcard host
    mcc.storage.hostStorage().getItem(processitem.getValue("host")).then(
            function (host) {
        if (host.getValue("anyHost")) {
            mcc.configuration.setPara(processfam, id, "HostName",
                    "defaultValueInstance", null);
        } else {
            mcc.configuration.setPara(processfam, id, "HostName",
                    "defaultValueInstance", host.getValue("name"));
        }

        // Process specific assignments
        if (processfam == "management") {
            ndb_mgmd_setup(processitem, processfam, host, waitCondition);
        } else if (processfam == "data") {
            ndbd_setup(processitem, processfam, host, waitCondition);
        } else if (processfam == "sql") {
            mysqld_setup(processitem, processfam, host, waitCondition);
        } else if (processfam == "api") {
            waitCondition.resolve();
        }
    });
    return waitCondition;
}

/******************************** Initialize  *********************************/

dojo.ready(function () {
    mcc.util.dbg("Configuration calulations module initialized");
});

