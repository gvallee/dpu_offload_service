# Getting Started with the DPU Offload Service

## Prerequisites:
1. Access to the following repositories:
  - UCX (the most up-to-date dpu_offload branch)
  - UCC-priv (the most up-to-date dpu_offload branch)
  - DPU Offload Service (v0.0.4 or better)
  - OpenMPI

2. A machine with DPUs. At NVIDIA, the following machines will work:
  - thor
  - helios

Note: You must use the Santa Clara VPN at NVIDIA to access the gateway to these clusters.

## Theory of Operation
The primary challenge in integrating the DPU-offloaded MPI stack is the requirement to have multiple binary types (aarch64 and x86_64) active and up-to-date at the same time. Mechanisms like Spack do not currently support this in a way that makes life easier rather than harder. To that end, we are going to use a mix of environment variables, shell scripts, and module files to support keeping these binaries straight and up-to-date, but this is simply one mechanism for accomplishing this task and other methods may be preferred. We suggest following these instructions precisely before modifying your methods to use a better approach.

These instructions will create several environment variables (PLATFORM, SWHOME), several directories ($SWHOME, $SWHOME/modulefiles, ~/bin, ~/srun, ~/workspace), module files and shell scripts. As you proceed through these instructions you will understand why each step is required to construct the environment. It will likely be useful to read the entire instructions once before proceeding.

## Planning for Capacity Constraints
Many home spaces have insufficient capacity to handle the very large build environments needed to support the large number of code repositories we will simultaneously have active. To move these spaces to a larger scratch file system we will use symbolic links. Please confirm with your administrator that the scratch file system you are moving files to is not automatically purged.

Execute the following commands to create the symbolic links:
```
echo $USER #confirm this is your userid
cd /global/scratch/users/$USER
mkdir sw workspace
cd
mkdir -p ~/srun
ln -s /global/scratch/users/$USER scratch
ln -s /global/scratch/users/$USER/sw sw
ln -s /global/scratch/users/$USER/workspace workspace
```

## Creating a Multi-Platform Build Environment
The first step in building an environment is creating environment variables that describe the current platform. In general, we need the instruction set and the binary versions captured in this environment.

Add the following to your shell resource file to set an environment variable describing the platform (usually add this to .bashrc):
```
#
# Platform detection
#
os=$(uname -s)
arch=$(uname -m)
if [ -d /etc/susehelp.d ]; then
  os='sles'
elif [ '1' = "$(uname -r |cut -f 6 -d '.' |grep -c chaos)" ]; then
  os='toss'
elif [ 'Linux' = "$os" ]; then
  os=$(uname -r |grep -o -E '[a-z]+.' |head -n 1)
elif [ 'Darwin' = "$os" ]; then
  os='osx'
fi
export PLATFORM=${os}-${arch}
```

After execution of the above your PLATFORM environment variable should be set similarly to: `el8-x86_64`

Second, we use this platform value to set the platform default binary location.

Add the following to your shell’s sourced environment (usually .bashrc):

```
export SWHOME=~/sw/$PLATFORM
mkdir -p $SWHOME
```

Now we setup some other variables that will be useful as a software developer:

```
export MODULEPATH=~/sw/modulefiles:$MODULEPATH
export PKG_CONFIG_PATH="$SWHOME/lib:$PKG_CONFIG_PATH"
```

Finally, be sure to reload your current environment with the variables modified above.
```
exec $SHELL
```

At this point you should have a valid build environment to begin code development. See Appendix A.1 for a complete version of this shell environment.

# Creating a Proof-of-Concept (POC) Sandbox
As a research software developer, it may be necessary to have multiple POC builds available that are simple to activate at any particular time. This means we need to have separate sandboxes for building and installing each of these POC demonstrations. To keep this straight, we will use modulefiles. The general naming of the pieces is `poc-<user supplied name>`. In this case the POC will be the dpu-alltoallv, and thus the POC will be called `poc-dpu-alltoallv`.
To create the POC sandbox execute the following commands:
```
mkdir -p ~/workspace/poc-dpu-alltoallv
mkdir -p ~/sw/modulefiles/poc-dpu-alltoallv
cd ~/sw/modulefiles/poc-dpu-alltoallv
```

Now create a file named 1.0 with the contents shown in Appendix A.2.

To activate the sandbox, type the following:
```
module load poc-dpu-alltoallv
```

# Building the POC Software Stack
Note that you will have to build the software stack on all architectures you are using (e.g. in the case of a DPU offload you will need to build for the host (x86) and the DPU (aarch64).
First, retrieve all the required repositories as shown below (Yong will grant access to UCX and UCC-priv, Geoffroy will grant access to DPU Offload service and MPI tests):

```
module load poc-dpu-alltoallv
poc # this will switch you to the POC build directory

git clone -b topic/dpu_offload_v3 git@github.com:yqin/ucx
git clone git@github.com:gvallee/dpu_offload_service.git
git clone -b topic/dpu_offload git@github.com:yqin/ucc-priv
git clone --recurse-submodules https://github.com/open-mpi/ompi.git
git clone git@github.com:gvallee/mpi_tests
```

Now you need to build these repositories. While this is simple enough to do, our build script can make it easier. It is important to note that we are going to use out of tree builds – this is important so that the same code base can be built for two different target architectures. For each repository, we will build a version into build_x86 and a version into build_aarch64. In the beginning you may lose track of this detail multiple times and it may be a source of difficulty. Just get used to this and try to remain diligent.
Also note that UCC requires OpenMPI and OpenMPI requires UCC. You will need to build OpenMPI at least once without UCC in order to bootstrap the UCC build the first time. (Running the build script without specifying a particular target will build and link the entire infrastructure from scratch and take care of this for you).

Create a script called build.sh with the contents shown in Appendix A.3. (Note: You may wish to remove the -–enable-debug flags)

You will undoubtedly edit this script many times during the development of proof-of-concept code, it’s just a starting point. To build the POC execute the following command once:
```
chmod +x ./build.sh
```
And run the script on each platform type that you require (likely an x86 host and a DPU):
```
./build.sh
```

# Running the POC Software Stack
Note that to run the DPU prototype on a Slurm-based system we will need an allocation that includes both hosts and DPUs. There is nothing tricky to doing this, but we also provide a utility script that will help you formulate these commands.

Create a file in ~/bin named dpu-salloc-bfdev.sh with contents as shown in Appendix A.4.

This script will ensure you get an allocation of hosts and their DPUs. To print an salloc command to run type the following:
```
dpu-salloc-bfdev.sh -v <n>
```
where n is the first host to use. To identify available hosts type the following:
```
sinfo -p thor
sinfo -p helios
```

Note that nodes in the idle state which you can request. You need to get both a host AND its DPU. You can ssh to the DPU and build the DPU versions of the software by executing:

```
ssh thorbf00n
module load poc-dpu-alltoallv
poc
./build.sh
```

As you gain familiarity with the stack you will come to understand which things must be rebuilt for the changes you have made.
Now it is necessary to run the software stack. For this, you will need a SLURM script to run which will create all of the DPU config files and start/stop daemons as required.

In your srun directory, first create ~/srun/dpu_utils.sh with contents as shown in Appendix A.5.

Next, create ~/srun/dpu_ucc-perf-alltoallv.sh with contents as shown in Appendix A.6.

Now, we are ready to run an example Alltoallv collective test.

First, build the example using:
```
cd $HOME/workspace/poc-dpu-alltoallv/mpi_tests/alltoallv && make && poc
```

Next, submit the job using:
```
sbatch -N 2 -p thor --nodelist=<n> ~/srun/dpu_alltoallv.sbatch
```
where `<n>` is an available host/dpu pair of the form “thor001, thorbf001”

# Appendix A: Files
## A.1 Shell File Contents
```
#
# Platform stuff
#
os=$(uname -s)
arch=$(uname -m)
if [ -d /etc/susehelp.d ]; then
  os='sles'
elif [ '1' = "$(uname -r |cut -f 6 -d '.' |grep -c chaos)" ]; then
  os='toss'
elif [ 'Linux' = "$os" ]; then
  os=$(uname -r |grep -o -E '[a-z]+.' |head -n 1)
elif [ 'Darwin' = "$os" ]; then
  os='osx'
fi
export PLATFORM=${os}-${arch}

#
# Set binary location
#
export SWHOME=~/sw/$PLATFORM
mkdir -p $SWHOME

#
# Set sw location vars
#
export MODULEPATH=~/sw/modulefiles:$MODULEPATH
export PKG_CONFIG_PATH="$SWHOME/lib:$PKG_CONFIG_PATH"

#
# Add to PATH
#
export PATH="$HOME/bin:$SWHOME/bin:$PATH"
```

## A.2 Module File Contents
```
#%Module

proc ModulesHelp { } {
  puts stderr "This module switches the SW environment to use the poc-dpu-alltoallv environment"
}

module-whatis "This module enables the poc-dpu-alltoallv environment"

# Retrieve environment variables
set home [getenv HOME]
set orig_swhome [getenv SWHOME]
set platform [getenv PLATFORM]

# Setup the environment
set pocname "poc-dpu-alltoallv"
set basedir "${home}/sw/${pocname}"
set swhome "${basedir}/${platform}"
setenv SWHOME "${swhome}"
prepend-path PATH "${swhome}/bin"
prepend-path LD_LIBRARY_PATH "${swhome}/lib"

# Create aliases
set-alias poc "cd ${home}/workspace/${pocname}"
set-alias sw "echo ${swhome}"
```

## A.3 Build Script
```
#!/bin/sh
# Environment setup stuff
module load poc-dpu-alltoallv
os=$(uname -r |grep -o -E '[a-z]+.' |head -n 1)
mtype=$(uname -m)
platform=${os}-${mtype}
export SWHOME=${HOME}/sw/poc-dpu-alltoallv/${platform}
export WORKSPACE=${HOME}/workspace/poc-dpu-alltoallv

# Identify what to build
if [ "x86_64" = "$mtype" ]; then
	builddir="build_x86"
elif [ "aarch64" = "$mtype" ]; then
	builddir="build_aarch64"
else
	echo "Error: Unknown machine type."
	exit 1
fi

if [ ! -z "${1}" ]; then
	echo "Requested targets for $SWHOME are $@"
	targets="$@"
else
	echo "Full build requested, deleting $SWHOME contents ..."
	sleep 4s
	rm -r $SWHOME/bin $SWHOME/lib $SWHOME/include $SWHOME/etc $SWHOME/share
	targets="ucx dpu_offload_service ompi ucc ompi+ucc"
fi

# Perform builds
for t in $targets; do
	echo "Building ${t} ..."
	case $t in
		ucx)
			dir=ucx
			cd ${dir} && ./autogen.sh >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		dpu_offload_service)
			dir=dpu_offload_service
			cd ${dir} && ./autogen.sh >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --with-ucx=$SWHOME --enable-debug >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		ompi)
			dir=ompi
			cd ${dir} && ./autogen.pl >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --with-ucx=$SWHOME --with-pmix=internal >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		ucc)
			dir=ucc-priv
			cd ${dir} && ./autogen.sh >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --enable-debug --with-ucx=$SWHOME --with-dpu-offload=$SWHOME --with-mpi=$SWHOME >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1

			# Build the daemon
			set -x
			UCC_SRC_DIR=${WORKSPACE}/ucc-priv
			DPU_DAEMON_BUILD=${UCC_SRC_DIR}/${builddir}
			DPU_DAEMON_SRC_DIR=${UCC_SRC_DIR}/src/components/tl/ucp/offload_dpu_daemon
			echo "Daemon dirs: <$UCC_SRC_DIR> <${DPU_DAEMON_BUILD}> <${DPU_DAEMON_SRC_DIR}>"
			cd ${DPU_DAEMON_BUILD}
			gcc ${DPU_DAEMON_SRC_DIR}/offload_dpu_daemon.c -O0 -g -I${DPU_DAEMON_SRC_DIR} -I${SWHOME}/include -L${SWHOME}/lib -ldpuoffloaddaemon -l ucp -l ucs -o ucc_offload_dpu_daemon
			[ $? != 0] && exit 1
			cp ucc_offload_dpu_daemon $SWHOME/bin
			cd ${WORKSPACE}
			;;
		ompi+ucc)
			dir=ompi
			cd ${dir} && ./autogen.pl >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --with-ucx=$SWHOME --with-ucc=$SWHOME --with-pmix=internal >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		*)
			echo "Unknown target ${t}!"
			echo "Available targets are 'ucx', 'dpu_offload_service', 'ompi', 'ucc', & 'ompi+ucc'"
			exit 2
			;;
	esac
done

echo "Completed building targets:${targets}"
```

## A.4 salloc Helper Script
```
# Get an allocation for hosts and their DPUs
#

# Print usage
function usage {
	echo "Usage: $0 <-v> <-p platform> <-n nnodes> <specific node>"
	exit 1
}

#
# Main
#
# Determine if the user requested a specific platform, otherwise set default
nhosts="1"
platform="thor"
verbose="0"
while getopts "n:p:v" o; do
	case ${o} in
		p)
			platoform=${OPTARG}
			;;
		n)
			nhosts=${OPTARG}
			;;
		v)
			echo "Dry run enabled"
			verbose=1
			;;
		*)
			usage
			;;
	esac
done
shift $((OPTIND-1))

# Determine if the user requested a specific first node
if [ -z "$1" ]; then
	begin=1
else
	begin="$1"
fi
echo "$platform count=$nhosts first=$begin"

# Hosts aren't named consistently, so we have to build the names differently
for n in $(seq $begin $((begin + nhosts - 1)) ); do
	if [ "thor" == "$platform" ]; then
		hostnum=$(seq -w $n 999 999)
		bfnum=$hostnum
		nodes="${platform}${hostnum},${platform}bf${bfnum}"
	elif [ "helios" == "$platform" ]; then
		hostnum=$(seq -w $n 999 999)
		bfnum=$hostnum
		nodes="${platform}${hostnum},${platform}bf${bfnum}"
	fi

	if [ -z "${nodelist}" ]; then
		nodelist="$nodes"
	else
		nodelist="${nodelist},${nodes}"
	fi
done

cmd="salloc -N $((nhosts*2)) -p ${platform} --nodelist ${nodelist} -t 120"
if [ "1" == "$verbose" ]; then
	echo "$cmd"
else
	$cmd
fi
```

## A.5 Slurm DPU Utility Script
```
#!/usr/bin/bash
#
# This script DPU utility functions
#

# Global settings (not used yet)
DPU_PORT1=9999
DPU_PORT2=11112
DPU_CONFIGFILE_PREFIX="/tmp/bws-dpu-cgf"

# Create the config file for the DPU offload engine
#
# Param dpulist Comma seperated list of hosts with DPUs
#
# Uses SLURM_JOB_PARTITION SLURM_NODELIST
# Exports OFFLOAD_CONFIG_FILE_PATH path to the DPU daemon config file
# Exports DPU_OFFLOAD_LIST_DPUS Comma separated list of hosts with DPUs
#
function dpu_create_configfile
{
	local dpulist="$1"
	if [ -z "$dpulist" ]; then
		echo "Usage: $0 <list of dpus>"
		exit 1
	else
		echo "DEBUG: dpulist=$dpulist"
	fi

	local domain=".hpcadvisorycouncil.com"
	if [ "thor" == "${SLURM_JOB_PARTITION}" ]; then
		local bf_ip_prefix="192.168.131.1"
		local hosts=$(scontrol show hostname ${SLURM_NODELIST} |grep -v 'bf' |sed -e "s/\$/${domain}/g")
		local bfs=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/\$/${domain}/g")
		local bf_ips=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/thorbf0/${bf_ip_prefix}/g")
		local cfg_names=$(paste <(echo "$hosts") <(echo "$bfs") -d ,)
		local cfg_names_ips=$(paste <(echo "$cfg_names") <(echo "$bf_ips") -d :)
		local dpu_cfg=$(sed -e "s/\$/:${DPU_PORT1}:${DPU_PORT2}\\n/g" <(echo "$cfg_names_ips") )
	elif [ "helios" == "${SLURM_JOB_PARTITION}" ]; then
		local bf_ip_prefix="192.168.129.1"
		local hosts=$(scontrol show hostname ${SLURM_NODELIST} |grep -v 'bf' |sed -e "s/\$/${domain}/g")
		local bfs=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/\$/${domain}/g")
		local bf_ips=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/heliosbf0/${bf_ip_prefix}/g")
		local cfg_names=$(paste <(echo "$hosts") <(echo "$bfs") -d ,)
		local cfg_names_ips=$(paste <(echo "$cfg_names") <(echo "$bf_ips") -d :)
		local dpu_cfg=$(sed -e "s/\$/:${DPU_PORT1}:${DPU_PORT2}\\n/g" <(echo "$cfg_names_ips") )
	else
		echo "Unknown BF Testbed platform: ${SLURM_JOB_PARTITION}"
		exit 1
	fi

	# Create the config file
	rm -f /tmp/bws-dpu-cfg.*
	DPU_OFFLOAD_LIST_DPUS="$dpulist"
	OFFLOAD_CONFIG_FILE_PATH=$(mktemp /tmp/bws-dpu-cfg.XXXXXX)
	echo "# Format version: 1" > $OFFLOAD_CONFIG_FILE_PATH
	echo "# <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port:rank-conn-port>,..." >> $OFFLOAD_CONFIG_FILE_PATH
	for line in $dpu_cfg; do
		echo $line >> $OFFLOAD_CONFIG_FILE_PATH
	done

	export OFFLOAD_CONFIG_FILE_PATH
	export DPU_OFFLOAD_LIST_DPUS
}

# Distribute the DPU config file to al hosts
#
# Param configfile
#
# Uses SLURM_NODELIST
#
function dpu_send_configfile
{
	local configfile="$1"
	local hostlist=$(scontrol show hostname ${SLURM_NODELIST})
	for host in $hostlist; do
	        scp $OFFLOAD_CONFIG_FILE_PATH $host:$OFFLOAD_CONFIG_FILE_PATH
		[ $? != 0 ] && exit 1
	done
}

# Start the DPU daemons
#
# Param Comma separated list of hosts on which to start DPU daemons
#
function dpu_start_daemons
{
	local dpulist="$1"
	local daemondir="$2"

	if [ -z "$dpulist" -o -z "$daemondir" ]; then
		echo "Usage: $0 <list of dpus> <daemon exec dir>"
		exit 1
	else
		echo "DEBUG: dpulist=$dpulist"
		echo "DEBUG: daemondir=$daemondir"
	fi

	dpu_create_configfile "$dpulist"
	dpu_send_configfile "$OFFLOAD_CONFIG_FILE_PATH"

	# For each BF setup the environment and start it
	daemonexe="${daemondir}/bin/ucc_offload_dpu_daemon"
	daemonenv="UCX_NET_DEVICES=mlx5_0:1 \
		UCX_ZCOPY_THRESH=0 \
		UCX_TLS=rc_x \
		UCX_RC_TIMEOUT=inf \
		UCX_LOG_LEVEL=warn \
		UCX_LOG_PRINT_ENABLE=n \
		DPU_OFFLOAD_DBG_VERBOSE=1 \
		OFFLOAD_CONFIG_FILE_PATH=${OFFLOAD_CONFIG_FILE_PATH} \
		DPU_OFFLOAD_LIST_DPUS=${DPU_OFFLOAD_LIST_DPUS} \
		LD_LIBRARY_PATH=${DPU_SWHOME}/lib"

	echo "DEBUG: Config file: ${OFFLOAD_CONFIG_FILE_PATH}"
	for dpu in $(echo $dpulist |sed "s/,/ /g"); do
		daemonlog="$HOME/daemonlog-${SLURM_JOBID}-${dpu}.out"
	        ssh "$dpu" "${daemonenv} nohup $daemonexe &> $daemonlog &"
		echo "Daemon ($daemonexe) start status: $?"
	done

	local time=5s
	echo "Wait $time for daemon wireup to complete"
	sleep $time
}

# Stop the DPU daemons
#
# param Comma separated list of hosts with DPU
#
function dpu_stop_daemons {
	local dpulist=$1

	# Kill all the daemons
	for dpu in $(echo $dpulist |sed "s/,/ /g"); do
        	ssh $dpu "pkill -f ucc_offload_dpu_daemon; [ "\$?" == "0" ] && echo \"$dpu: Daemon stopped\""
	done
}
```

## A.6 Slurm DPU Command Script
```
#!/usr/bin/bash
#
# This script runs a set of benchmarks using DPUs
#
#SBATCH --job-name=dpu-alltoallv
##SBATCH --output=$HOME/jobs/%x-%j.out
#SBATCH --nodes=2
#SBATCH --time=00:60:00
#SBATCH --exclusive
#SBATCH -d singleton

### Platform Settings
nodes=$((SLURM_NNODES/2))
slots=${SLURM_CPUS_ON_NODE}
sockets=0
hostib="mlx5_100:100"
case "${SLURM_JOB_PARTITION}" in
	thor)
		sockets=1
		hostib=mlx5_2:1
		;;
	helios)
		sockets=1
		hostib=mlx5_4:1
		;;
	*)
		echo "This script has not been tested on partition: ${SLURM_JOB_PARTITION}"
		exit 1
		;;
esac

### Job Settings
JOB_PPN=$slots
JOB_NPROCS=$((nodes*slots))
JOB_ITER=1
JOB_NPROCS=$((2))

### Platform/DPU software locations
export PRTE_MCA_plm=ssh
export SWHOME="${HOME}/sw/poc-dpu-alltoallv/el8-x86_64"
export DPU_SWHOME="${HOME}/sw/poc-dpu-alltoallv/bluefield-aarch64"
echo "SWHOME: $SWHOME"
echo "DPU_SWHOME: ${DPU_SWHOME}"

# Create a list of just the hosts and a list of just the DPUs
hostlist=$(scontrol show hostname ${SLURM_NODELIST} |grep -v 'bf' |sed -e "s/\$/:${slots}/g" |paste -d , -s)
bflist=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/\$//g" |paste -d , -s)
echo "Hostname list: $hostlist"
echo "Bluefield list: $bflist"

# Import the DPU utility functions
if [ -f ~/srun/dpu_utils.sh ]; then
	source ~/srun/dpu_utils.sh
else
	echo "Unable to import: ~/srun/dpu_utils.sh"
	exit 1
fi

dpu_stop_daemons "$bflist"
dpu_start_daemons "$bflist" "${DPU_SWHOME}"

# Run the dpu assisted MPI command
export LD_LIBRARY_PATH=$SWHOME/lib
echo "Offload Config file: $OFFLOAD_CONFIG_FILE_PATH:"
cat $OFFLOAD_CONFIG_FILE_PATH
which $SWHOME/bin/mpirun
unset SLURM_JOBID

# Run collective through an mpi test program (set coll_ucc_enable=1 and coll_ucc_priority=100)
time $SWHOME/bin/mpirun \
    --np ${JOB_NPROCS} \
    --map-by ppr:${JOB_PPN}:node:oversubscribe \
    -H ${hostlist} \
    --bind-to core \
    --rank-by core \
    --mca pml ucx \
        -x UCX_NET_DEVICES=$hostib \
        -x UCX_TLS=rc_x \
        -x UCX_LOG_LEVEL=warn \
        -x UCX_LOG_PRINT_ENABLE=n \
    --mca coll_hcoll_enable 0 \
    --mca coll_ucc_enable 1 \
    --mca coll_ucc_priority 100 \
        -x UCC_CL_BASIC_TLS=ucp \
        -x UCC_LOG_LEVEL=debug \
        -x OFFLOAD_CONFIG_FILE_PATH=$OFFLOAD_CONFIG_FILE_PATH \
        -x DPU_OFFLOAD_DBG_VERBOSE=1 \
    stdbuf -e0 -o0 \
    $HOME/workspace/poc-dpu-alltoallv/mpi_tests/alltoallv/simple_alltoallv 2>&1 | tee alltoallv-log


dpu_stop_daemons "$bflist"
```
