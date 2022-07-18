#!/usr/bin/bash
#
# This script contains DPU utility functions
#


# Start the DPU daemons
#
# Param Comma separated list of hosts on which to start DPU daemons
#
function dpu_start_daemons
{
	local dpulist="$1"
	local daemondir="$2"
	local conf_file="$3"

	if [ -z "$dpulist" -o -z "$daemondir" ]; then
		echo "Usage: $0 <list of dpus> <daemon exec dir> <configuration file path>"
		exit 1
	else
		echo "DEBUG: dpulist=$dpulist"
		echo "DEBUG: daemondir=$daemondir"
	fi

	# For each BF setup the environment and start it
	daemonexe="${daemondir}/bin/ucc_offload_dpu_daemon"
	daemonenv="UCX_NET_DEVICES=mlx5_0:1 \
		UCX_ZCOPY_THRESH=0 \
		UCX_TLS=rc_x \
		UCX_RC_TIMEOUT=inf \
		UCX_LOG_LEVEL=warn \
		UCX_LOG_PRINT_ENABLE=n \
		DPU_OFFLOAD_DBG_VERBOSE=1 \
		OFFLOAD_CONFIG_FILE_PATH=${conf_file} \
		DPU_OFFLOAD_LIST_DPUS=${dpulist} \
		LD_LIBRARY_PATH=${DPU_SWHOME}/lib"

	echo "DEBUG: Config file: ${conf_file}"
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
